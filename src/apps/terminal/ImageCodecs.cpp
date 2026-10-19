/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


//!	Decoding the images programs send (kitty's graphics protocol).


#include "ImageCodecs.h"

#include <string.h>

#include <Bitmap.h>
#include <DataIO.h>
#include <TranslationUtils.h>

#include <zlib.h>

#include "TerminalGraphics.h"


static bool
decode_png(const uint8* data, size_t length, int32& width, int32& height,
	std::vector<uint8>& rgba)
{
	BMemoryIO stream(data, length);
	BBitmap* bitmap = BTranslationUtils::GetBitmap(&stream);
	if (bitmap == NULL)
		return false;

	// whatever the translator made of it, as 32 bits with alpha
	if (bitmap->ColorSpace() != B_RGBA32 && bitmap->ColorSpace() != B_RGB32) {
		BBitmap* converted = new(std::nothrow) BBitmap(bitmap->Bounds(), 0,
			B_RGBA32);
		if (converted == NULL || converted->InitCheck() != B_OK
			|| converted->ImportBits(bitmap) != B_OK) {
			delete converted;
			delete bitmap;
			return false;
		}
		delete bitmap;
		bitmap = converted;
	}

	bool hasAlpha = bitmap->ColorSpace() == B_RGBA32;
	width = bitmap->Bounds().IntegerWidth() + 1;
	height = bitmap->Bounds().IntegerHeight() + 1;
	rgba.resize((size_t)width * height * 4);

	const uint8* bits = (const uint8*)bitmap->Bits();
	int32 bytesPerRow = bitmap->BytesPerRow();
	for (int32 y = 0; y < height; y++) {
		const uint8* source = bits + (size_t)y * bytesPerRow;
		uint8* target = rgba.data() + (size_t)y * width * 4;
		for (int32 x = 0; x < width; x++) {
			// BGRA in memory
			target[0] = source[2];
			target[1] = source[1];
			target[2] = source[0];
			target[3] = hasAlpha ? source[3] : 255;
			source += 4;
			target += 4;
		}
	}

	delete bitmap;
	return true;
}


static bool
inflate_zlib(const uint8* data, size_t length, size_t expectedLength,
	std::vector<uint8>& output)
{
	output.resize(expectedLength);
	uLongf size = expectedLength;
	if (uncompress(output.data(), &size, data, length) != Z_OK)
		return false;
	output.resize(size);
	return size == expectedLength;
}


void
register_image_codecs()
{
	TerminalGraphics::SetCodecs(&decode_png, &inflate_zlib);
}
