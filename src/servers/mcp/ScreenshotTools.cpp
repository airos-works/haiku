/*
 * airos_mcp - screenshots straight from app_server, encoded as PNG here
 * (the arm64 image has no PNG translator).
 * Copyright 2026 air/OS contributors. MIT license.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include <Bitmap.h>
#include <Screen.h>

#include "Mcp.h"
#include "Tools.h"
#include "Util.h"


namespace {

uint32_t
Crc32(const uint8_t* data, size_t length, uint32_t crc = 0)
{
	return crc32(crc, data, length);
}


void
AppendChunk(std::string& out, const char* type, const std::string& data)
{
	uint32_t length = data.size();
	uint8_t header[8] = {
		(uint8_t)(length >> 24), (uint8_t)(length >> 16), (uint8_t)(length >> 8),
		(uint8_t)length, (uint8_t)type[0], (uint8_t)type[1], (uint8_t)type[2],
		(uint8_t)type[3]
	};
	out.append((const char*)header, 8);
	out.append(data);
	uint32_t crc = Crc32(header + 4, 4);
	crc = Crc32((const uint8_t*)data.data(), data.size(), crc);
	uint8_t crcBytes[4] = {
		(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc
	};
	out.append((const char*)crcBytes, 4);
}


// rgb: width*height*3 bytes, rows top to bottom.
std::string
EncodePng(const std::vector<uint8_t>& rgb, int width, int height, int level)
{
	std::string raw;
	raw.reserve((size_t)height * (width * 3 + 1));
	for (int y = 0; y < height; y++) {
		raw += (char)0;	// filter: none
		raw.append((const char*)&rgb[(size_t)y * width * 3], width * 3);
	}
	uLongf compressedSize = compressBound(raw.size());
	std::string compressed;
	compressed.resize(compressedSize);
	if (compress2((Bytef*)&compressed[0], &compressedSize, (const Bytef*)raw.data(),
			raw.size(), level) != Z_OK)
		return std::string();
	compressed.resize(compressedSize);

	std::string png("\x89PNG\r\n\x1a\n", 8);
	uint8_t ihdr[13] = {
		(uint8_t)(width >> 24), (uint8_t)(width >> 16), (uint8_t)(width >> 8),
		(uint8_t)width, (uint8_t)(height >> 24), (uint8_t)(height >> 16),
		(uint8_t)(height >> 8), (uint8_t)height, 8, 2, 0, 0, 0
	};
	AppendChunk(png, "IHDR", std::string((const char*)ihdr, 13));
	AppendChunk(png, "IDAT", compressed);
	AppendChunk(png, "IEND", std::string());
	return png;
}


// Converts any bitmap to packed RGB via a B_RGB32 copy (ImportBits does the
// color space conversion), cropping to frame and scaling down by an integer
// box filter plus a final bilinear-free step: we average scale*scale pixels.
bool
BitmapToRgb(BBitmap* source, BRect crop, int targetWidth, std::vector<uint8_t>& rgb,
	int& outWidth, int& outHeight, std::string& error)
{
	BRect bounds = source->Bounds();
	crop = crop & bounds;
	if (!crop.IsValid()) {
		error = "region is outside the screen";
		return false;
	}
	int cropX = (int)crop.left;
	int cropY = (int)crop.top;
	int cropWidth = (int)crop.IntegerWidth() + 1;
	int cropHeight = (int)crop.IntegerHeight() + 1;

	BBitmap* rgb32 = source;
	BBitmap* converted = NULL;
	if (source->ColorSpace() != B_RGB32 && source->ColorSpace() != B_RGBA32) {
		converted = new BBitmap(bounds, B_RGB32);
		if (converted->InitCheck() != B_OK
				|| converted->ImportBits(source) != B_OK) {
			delete converted;
			error = "cannot convert the screen bitmap to RGB32";
			return false;
		}
		rgb32 = converted;
	}

	int factor = 1;
	if (targetWidth > 0 && cropWidth > targetWidth)
		factor = (cropWidth + targetWidth - 1) / targetWidth;
	outWidth = cropWidth / factor;
	outHeight = cropHeight / factor;
	if (outWidth < 1 || outHeight < 1) {
		error = "region too small";
		delete converted;
		return false;
	}
	rgb.resize((size_t)outWidth * outHeight * 3);
	const uint8_t* bits = (const uint8_t*)rgb32->Bits();
	int32 bpr = rgb32->BytesPerRow();
	for (int y = 0; y < outHeight; y++) {
		for (int x = 0; x < outWidth; x++) {
			uint32_t r = 0, g = 0, b = 0;
			for (int sy = 0; sy < factor; sy++) {
				const uint8_t* row = bits + (size_t)(cropY + y * factor + sy) * bpr
					+ (size_t)(cropX + x * factor) * 4;
				for (int sx = 0; sx < factor; sx++) {
					b += row[sx * 4];
					g += row[sx * 4 + 1];
					r += row[sx * 4 + 2];
				}
			}
			uint32_t n = factor * factor;
			uint8_t* out = &rgb[((size_t)y * outWidth + x) * 3];
			out[0] = r / n;
			out[1] = g / n;
			out[2] = b / n;
		}
	}
	delete converted;
	return true;
}


ToolResult
Screenshot(const JsonValue& args)
{
	const char* reason = NULL;
	if (!EnsureApplication(&reason))
		return ToolResult::Error(std::string("screenshot needs app_server: ") + reason);

	int maxWidth = (int)args.GetInt("max_width", 1600);
	bool cursor = args.GetBool("cursor", false);
	std::string saveTo = ExpandPath(args.GetString("save_to"));
	bool returnImage = args.GetBool("return_image", saveTo.empty());
	int level = (int)args.GetInt("compression", 6);

	BScreen screen(B_MAIN_SCREEN_ID);
	if (!screen.IsValid())
		return ToolResult::Error("no main screen (app_server not driving a display?)");
	BRect frame = screen.Frame();
	BBitmap* bitmap = NULL;
	status_t error = screen.GetBitmap(&bitmap, cursor);
	if (error != B_OK || bitmap == NULL)
		return ToolResult::Error("BScreen::GetBitmap failed: " + StrError(error));

	BRect crop = bitmap->Bounds();
	const JsonValue* region = args.Get("region");
	if (region != NULL && region->IsObject()) {
		float x = region->GetDouble("x", 0);
		float y = region->GetDouble("y", 0);
		float w = region->GetDouble("width", crop.Width() + 1 - x);
		float h = region->GetDouble("height", crop.Height() + 1 - y);
		crop = BRect(x, y, x + w - 1, y + h - 1);
	}

	std::vector<uint8_t> rgb;
	int width = 0, height = 0;
	std::string convertError;
	bool ok = BitmapToRgb(bitmap, crop, maxWidth, rgb, width, height, convertError);
	color_space space = bitmap->ColorSpace();
	BRect bounds = bitmap->Bounds();
	delete bitmap;
	if (!ok)
		return ToolResult::Error(convertError);

	std::string png = EncodePng(rgb, width, height, level);
	if (png.empty())
		return ToolResult::Error("PNG encoding failed");

	std::string caption = Format("screen %dx%d (frame %gx%g, color space 0x%x), "
		"image %dx%d PNG, %zu bytes", (int)bounds.IntegerWidth() + 1,
		(int)bounds.IntegerHeight() + 1, frame.Width() + 1, frame.Height() + 1,
		(unsigned)space, width, height, png.size());
	if (!saveTo.empty()) {
		if (!WriteStringToFile(saveTo, png))
			return ToolResult::Error("cannot write " + saveTo);
		caption += ", saved to " + saveTo;
	}
	if (!returnImage)
		return ToolResult::Text(caption);
	return ToolResult::Image(Base64Encode(png.data(), png.size()), "image/png",
		caption);
}


ToolResult
ScreenInfo(const JsonValue& args)
{
	const char* reason = NULL;
	if (!EnsureApplication(&reason))
		return ToolResult::Error(std::string("needs app_server: ") + reason);
	BScreen screen(B_MAIN_SCREEN_ID);
	if (!screen.IsValid())
		return ToolResult::Error("no main screen");
	JsonValue result = JsonValue::Object();
	BRect frame = screen.Frame();
	result.Set("width", (int64_t)(frame.Width() + 1));
	result.Set("height", (int64_t)(frame.Height() + 1));
	result.Set("color_space", Format("0x%x", (unsigned)screen.ColorSpace()));
	display_mode mode;
	if (screen.GetMode(&mode) == B_OK) {
		JsonValue m = JsonValue::Object();
		m.Set("virtual_width", mode.virtual_width);
		m.Set("virtual_height", mode.virtual_height);
		m.Set("pixel_clock_khz", (int64_t)mode.timing.pixel_clock);
		m.Set("h_total", mode.timing.h_total);
		m.Set("v_total", mode.timing.v_total);
		if (mode.timing.h_total > 0 && mode.timing.v_total > 0)
			m.Set("refresh_hz", mode.timing.pixel_clock * 1000.0
				/ (mode.timing.h_total * mode.timing.v_total));
		m.Set("flags", Format("0x%x", (unsigned)mode.flags));
		result.Set("mode", m);
	}
	monitor_info info;
	if (screen.GetMonitorInfo(&info) == B_OK) {
		JsonValue mon = JsonValue::Object();
		mon.Set("vendor", info.vendor);
		mon.Set("name", info.name);
		mon.Set("serial", info.serial_number);
		mon.Set("produced", Format("%d-%02d", info.produced.year, info.produced.week));
		mon.Set("width_cm", info.width);
		mon.Set("height_cm", info.height);
		result.Set("monitor", mon);
	}
	accelerant_device_info device;
	if (screen.GetDeviceInfo(&device) == B_OK) {
		JsonValue dev = JsonValue::Object();
		dev.Set("name", device.name);
		dev.Set("chipset", device.chipset);
		dev.Set("serial", device.serial_no);
		dev.Set("memory", (int64_t)device.memory);
		dev.Set("dac_speed", (int64_t)device.dac_speed);
		result.Set("device", dev);
	}
	return ToolResult::Json(result);
}

} // namespace


void
RegisterScreenshotTools(McpServer& server)
{
	server.AddTool("screenshot",
		"Capture the screen through app_server (BScreen::GetBitmap, which works "
		"where the screenshot command and VNC captures do not) and return it as a "
		"PNG image, scaled down to max_width (default 1600) by a box filter. "
		"Optional region crop and save_to a file on the device instead of or in "
		"addition to returning it. Menus and tooltips are captured too.",
		"{\"type\":\"object\",\"properties\":{"
		"\"max_width\":{\"type\":\"integer\",\"description\":\"scale down so the "
		"image is at most this wide (default 1600; 0 = full size)\"},"
		"\"region\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"number\"},"
		"\"y\":{\"type\":\"number\"},\"width\":{\"type\":\"number\"},"
		"\"height\":{\"type\":\"number\"}},\"description\":\"crop in frame buffer "
		"pixels before scaling\"},"
		"\"cursor\":{\"type\":\"boolean\",\"description\":\"draw the mouse cursor\"},"
		"\"save_to\":{\"type\":\"string\",\"description\":\"also/instead write the "
		"PNG to this path on the device\"},"
		"\"return_image\":{\"type\":\"boolean\",\"description\":\"return the image "
		"content (default true, false when save_to is given)\"},"
		"\"compression\":{\"type\":\"integer\",\"description\":\"zlib level 0-9 "
		"(default 6)\"}}}", Screenshot);
	server.AddTool("screen_info",
		"Main screen geometry, display mode (refresh rate), monitor EDID name and "
		"the accelerant's device info.",
		"{\"type\":\"object\",\"properties\":{}}", ScreenInfo);
}
