/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The accelerant of the Raspberry Pi 4's HDMI outputs (rpi_display).

	The firmware holds each output at its native video mode and
	scales whatever region of the frame buffer the driver gives it to that
	mode. So an output's "resolution" and its scale are both the size of its
	region; app_server arranges the regions through the display layout hooks.
	There is no acceleration; the cursor is a plane the firmware composites
	over the frame buffer. */


#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <Accelerant.h>
#include <OS.h>

#include <edid.h>

#include <rpi_display.h>


struct accelerant_info {
	int						device;
	bool					is_clone;
	area_id					shared_area;
	rpi_display_shared_info* shared;
	area_id					frame_buffer_area;
	void*					frame_buffer;
	uint32					generation;

	// the hardware cursor's buffer, NULL where the driver has none
	area_id					cursor_area;
	void*					cursor_buffer;

	// the layout app_server asked for, until its mode is set
	bool					pending;
	rpi_display_layout		pending_layout;
};

static accelerant_info* gInfo;

// the resolutions offered below an output's own
static const uint16 kSizes[][2] = {
	{3840, 2160}, {2560, 1440}, {1920, 1200}, {1920, 1080}, {1680, 1050},
	{1600, 900}, {1440, 900}, {1366, 768}, {1280, 1024}, {1280, 800},
	{1280, 720}, {1024, 768}, {800, 600}, {640, 480}
};


//	#pragma mark - helpers


/*!	A 60 Hz timing for a picture of this size. The firmware has the real
	timings; these only have to read as a plausible mode.
*/
static display_timing
make_timing(uint32 width, uint32 height)
{
	display_timing timing;
	memset(&timing, 0, sizeof(timing));
	timing.h_display = width;
	timing.h_sync_start = width + width / 16;
	timing.h_sync_end = width + width / 8;
	timing.h_total = width + width / 4;
	timing.v_display = height;
	timing.v_sync_start = height + 3;
	timing.v_sync_end = height + 8;
	timing.v_total = height + height / 24 + 8;
	timing.pixel_clock = (uint32)((uint64)timing.h_total * timing.v_total
		* 60 / 1000);
	timing.flags = B_POSITIVE_HSYNC | B_POSITIVE_VSYNC;
	return timing;
}


static display_mode
make_mode(uint32 width, uint32 height)
{
	display_mode mode;
	memset(&mode, 0, sizeof(mode));
	mode.timing = make_timing(width, height);
	mode.space = B_RGB32;
	mode.virtual_width = width;
	mode.virtual_height = height;
	return mode;
}


static display_mode
current_mode()
{
	return make_mode(gInfo->shared->width, gInfo->shared->height);
}


static display_mode
preferred_mode()
{
	if (gInfo->pending) {
		return make_mode(gInfo->pending_layout.width,
			gInfo->pending_layout.height);
	}
	return current_mode();
}


/*!	The frame buffer region of a picture \a pixels large shown at \a scale
	percent while everything is drawn at \a renderScale percent.
*/
static uint16
region_size(uint32 pixels, uint16 scale, uint16 renderScale)
{
	if (renderScale == scale)
		return pixels;
	uint32 logical = (pixels * 100 + scale / 2) / scale;
	return (uint16)((logical * renderScale + 50) / 100);
}


static status_t
read_state(rpi_display_shared_info& state)
{
	if (ioctl(gInfo->device, RPI_DISPLAY_GET_STATE, &state, sizeof(state)) != 0)
		return errno != 0 ? errno : B_ERROR;
	return B_OK;
}


static status_t
rpi_set_display_change_port(port_id port, int32 code)
{
	rpi_display_change_port request = {port, code};
	if (ioctl(gInfo->device, RPI_DISPLAY_SET_CHANGE_PORT, &request,
			sizeof(request)) != 0)
		return errno != 0 ? errno : B_ERROR;
	return B_OK;
}


static status_t
map_frame_buffer()
{
	area_info info;
	if (ioctl(gInfo->device, RPI_DISPLAY_CLONE_FRAME_BUFFER, &info,
			sizeof(info)) != 0) {
		return errno != 0 ? errno : B_ERROR;
	}

	if (gInfo->frame_buffer_area >= 0)
		delete_area(gInfo->frame_buffer_area);
	gInfo->frame_buffer_area = info.area;
	gInfo->frame_buffer = info.address;
	gInfo->generation = gInfo->shared->generation;
	return B_OK;
}


static status_t
apply_layout(const rpi_display_layout& layout)
{
	rpi_display_layout request = layout;
	if (ioctl(gInfo->device, RPI_DISPLAY_SET_LAYOUT, &request,
			sizeof(request)) != 0) {
		return errno != 0 ? errno : B_ERROR;
	}

	if (gInfo->frame_buffer_area < 0
		|| gInfo->generation != gInfo->shared->generation) {
		return map_frame_buffer();
	}
	return B_OK;
}


static status_t
init_common(int device, bool isClone)
{
	gInfo = (accelerant_info*)calloc(1, sizeof(accelerant_info));
	if (gInfo == NULL)
		return B_NO_MEMORY;
	gInfo->device = device;
	gInfo->is_clone = isClone;
	gInfo->frame_buffer_area = -1;
	gInfo->cursor_area = -1;

	area_id sharedArea;
	status_t status = B_OK;
	if (ioctl(device, RPI_DISPLAY_GET_SHARED_AREA, &sharedArea,
			sizeof(sharedArea)) != 0) {
		status = errno != 0 ? errno : B_ERROR;
	}
	if (status == B_OK) {
		gInfo->shared_area = clone_area("rpi display shared info",
			(void**)&gInfo->shared, B_ANY_ADDRESS, B_READ_AREA,
			sharedArea);
		if (gInfo->shared_area < 0)
			status = gInfo->shared_area;
		else if (gInfo->shared->version != RPI_DISPLAY_VERSION) {
			delete_area(gInfo->shared_area);
			status = B_MISMATCHED_VALUES;
		}
	}

	if (status != B_OK) {
		free(gInfo);
		gInfo = NULL;
	}
	return status;
}


static void
uninit_common()
{
	if (!gInfo->is_clone)
		rpi_set_display_change_port(-1, 0);
	if (gInfo->frame_buffer_area >= 0)
		delete_area(gInfo->frame_buffer_area);
	if (gInfo->cursor_area >= 0)
		delete_area(gInfo->cursor_area);
	delete_area(gInfo->shared_area);
	if (gInfo->is_clone)
		close(gInfo->device);
	free(gInfo);
	gInfo = NULL;
}


//	#pragma mark - general hooks


static status_t
rpi_init_accelerant(int device)
{
	status_t status = init_common(device, false);
	if (status != B_OK)
		return status;

	// Until app_server arranges them: the first output at its own size and
	// the other one showing the same.
	rpi_display_shared_info shared;
	status = read_state(shared);
	if (status != B_OK) {
		uninit_common();
		return status;
	}
	rpi_display_layout layout = {};
	layout.version = RPI_DISPLAY_VERSION;
	uint32 first = 0;
	while (first < shared.output_count
		&& (shared.outputs[first].flags & RPI_DISPLAY_OUTPUT_CONNECTED) == 0)
		first++;
	if (first == shared.output_count) {
		uninit_common();
		return B_DEVICE_NOT_FOUND;
	}
	layout.width = shared.outputs[first].native_width;
	layout.height = shared.outputs[first].native_height;
	for (uint32 i = 0; i < shared.output_count; i++) {
		if ((shared.outputs[i].flags & RPI_DISPLAY_OUTPUT_CONNECTED) == 0)
			continue;
		rpi_display_output& output = layout.outputs[i];
		output.flags = RPI_DISPLAY_OUTPUT_ENABLED
			| (i != first ? RPI_DISPLAY_OUTPUT_MIRROR : 0);
		output.width = output.mode_width = layout.width;
		output.height = output.mode_height = layout.height;
		output.scale = output.render_scale = 100;
	}

	status = apply_layout(layout);
	if (status != B_OK)
		uninit_common();
	return status;
}


static void
rpi_uninit_accelerant()
{
	uninit_common();
}


static ssize_t
rpi_accelerant_clone_info_size()
{
	return B_PATH_NAME_LENGTH;
}


static void
rpi_get_accelerant_clone_info(void* info)
{
	strlcpy((char*)info, "/dev/" RPI_DISPLAY_DEVICE, B_PATH_NAME_LENGTH);
}


static status_t
rpi_clone_accelerant(void* info)
{
	int device = open((const char*)info, B_READ_WRITE);
	if (device < 0)
		return errno;

	status_t status = init_common(device, true);
	if (status == B_OK) {
		status = map_frame_buffer();
		if (status != B_OK)
			uninit_common();
	} else
		close(device);
	return status;
}


static status_t
rpi_get_accelerant_device_info(accelerant_device_info* info)
{
	info->version = B_ACCELERANT_VERSION;
	strlcpy(info->name, "Raspberry Pi HDMI", sizeof(info->name));
	strlcpy(info->chipset, "Broadcom BCM2711 (firmware)",
		sizeof(info->chipset));
	strlcpy(info->serial_no, "None", sizeof(info->serial_no));
	info->memory = gInfo->shared->bytes_per_row * gInfo->shared->height;
	info->dac_speed = 0;
	return B_OK;
}


/*!	Released at every vertical blank of the first output shown; app_server's
	own, which it gives to its clients.
*/
static sem_id
rpi_accelerant_retrace_semaphore()
{
	sem_id semaphore;
	if (ioctl(gInfo->device, RPI_DISPLAY_GET_RETRACE_SEMAPHORE, &semaphore,
			sizeof(semaphore)) != 0) {
		return -1;
	}
	return semaphore;
}


//	#pragma mark - cursor


static status_t
map_cursor_buffer()
{
	if (gInfo->cursor_area >= 0)
		return B_OK;
	area_info info;
	if (ioctl(gInfo->device, RPI_DISPLAY_CLONE_CURSOR_BUFFER, &info,
			sizeof(info)) != 0) {
		return errno != 0 ? errno : B_ERROR;
	}
	gInfo->cursor_area = info.area;
	gInfo->cursor_buffer = info.address;
	return B_OK;
}


/*!	The firmware composites the cursor as a plane of its own over the frame
	buffer, so the pointer never meets the pixels it points at: no software
	cursor's backup and restore over what a direct window drew there.
	Returns B_UNSUPPORTED where the bitmap cannot ride a plane; the
	app_server then draws the cursor itself.
*/
static status_t
rpi_set_cursor_bitmap(uint16 width, uint16 height, uint16 hotX, uint16 hotY,
	color_space colorSpace, uint16 bytesPerRow, const uint8* bitmapData)
{
	if (colorSpace != B_RGBA32 || width == 0 || width > RPI_DISPLAY_CURSOR_MAX
		|| height == 0 || height > RPI_DISPLAY_CURSOR_MAX)
		return B_UNSUPPORTED;

	status_t status = map_cursor_buffer();
	if (status != B_OK)
		return B_UNSUPPORTED;

	uint8* buffer = (uint8*)gInfo->cursor_buffer;
	const uint32 bufferRow = RPI_DISPLAY_CURSOR_MAX * 4;
	memset(buffer, 0, bufferRow * RPI_DISPLAY_CURSOR_MAX);
	for (uint16 y = 0; y < height; y++) {
		const uint8* src = bitmapData + y * bytesPerRow;
		uint8* dst = buffer + y * bufferRow;
		for (uint16 x = 0; x < width; x++, src += 4, dst += 4) {
			// Haiku keeps its cursors premultiplied, alpha 255 opaque; the
			// firmware blends straight alpha
			// (out = rgb * a + background * (1 - a)), so un-premultiply.
			uint8 alpha = src[3];
			dst[3] = alpha;
			if (alpha == 0) {
				dst[0] = dst[1] = dst[2] = 0;
			} else if (alpha == 255) {
				dst[0] = src[0];
				dst[1] = src[1];
				dst[2] = src[2];
			} else {
				dst[0] = (uint8)((src[0] * 255 + alpha / 2) / alpha);
				dst[1] = (uint8)((src[1] * 255 + alpha / 2) / alpha);
				dst[2] = (uint8)((src[2] * 255 + alpha / 2) / alpha);
			}
		}
	}

	rpi_display_cursor cursor = {width, height, (int16)hotX, (int16)hotY};
	if (ioctl(gInfo->device, RPI_DISPLAY_DEFINE_CURSOR, &cursor,
			sizeof(cursor)) != 0) {
		return errno != 0 ? errno : B_ERROR;
	}
	return B_OK;
}


static void
rpi_move_cursor(uint16 x, uint16 y)
{
	rpi_display_cursor_position position = {x, y};
	ioctl(gInfo->device, RPI_DISPLAY_MOVE_CURSOR, &position, sizeof(position));
}


static void
rpi_show_cursor(bool visible)
{
	ioctl(gInfo->device, RPI_DISPLAY_SHOW_CURSOR, &visible, sizeof(visible));
}


//	#pragma mark - mode hooks


// The one mode is the desktop of the layout: the one app_server asked for
// last, until it is set.
static uint32
rpi_accelerant_mode_count()
{
	return 1;
}


static status_t
rpi_get_mode_list(display_mode* list)
{
	list[0] = preferred_mode();
	return B_OK;
}


static status_t
rpi_get_preferred_display_mode(display_mode* mode)
{
	*mode = preferred_mode();
	return B_OK;
}


static status_t
rpi_set_display_mode(display_mode* mode)
{
	if (mode == NULL || mode->space != B_RGB32)
		return B_UNSUPPORTED;

	if (gInfo->pending && mode->virtual_width == gInfo->pending_layout.width
		&& mode->virtual_height == gInfo->pending_layout.height) {
		status_t status = apply_layout(gInfo->pending_layout);
		if (status == B_OK)
			gInfo->pending = false;
		return status;
	}

	if (mode->virtual_width == gInfo->shared->width
		&& mode->virtual_height == gInfo->shared->height) {
		return B_OK;
	}
	return B_UNSUPPORTED;
}


static status_t
rpi_get_display_mode(display_mode* mode)
{
	*mode = current_mode();
	return B_OK;
}


static status_t
rpi_get_frame_buffer_config(frame_buffer_config* config)
{
	if (gInfo->generation != gInfo->shared->generation) {
		// the driver replaced the buffer under a clone
		status_t status = map_frame_buffer();
		if (status != B_OK)
			return status;
	}

	config->frame_buffer = (uint8*)gInfo->frame_buffer;
	config->frame_buffer_dma = (uint8*)(addr_t)gInfo->shared->physical_address;
	config->bytes_per_row = gInfo->shared->bytes_per_row;
	return B_OK;
}


static status_t
rpi_get_pixel_clock_limits(display_mode* mode, uint32* _low, uint32* _high)
{
	*_low = *_high = mode->timing.pixel_clock;
	return B_OK;
}


static status_t
rpi_get_edid_info(void* info, size_t size, uint32* _version)
{
	rpi_display_shared_info state;
	status_t status = read_state(state);
	if (status != B_OK)
		return status;
	uint32 first = 0;
	while (first < state.output_count
		&& (state.outputs[first].flags & RPI_DISPLAY_OUTPUT_CONNECTED) == 0)
		first++;
	if (first == state.output_count)
		return B_DEVICE_NOT_FOUND;
	const rpi_display_output& output = state.outputs[first];
	if (output.edid_length < 128 || size < sizeof(edid1_info))
		return B_ERROR;

	edid_decode((edid1_info*)info, (const edid1_raw*)output.edid);
	*_version = EDID_VERSION_1;
	return B_OK;
}


//	#pragma mark - display layout hooks


static uint32
rpi_display_output_count()
{
	return gInfo->shared->output_count;
}


static const char*
output_name(const rpi_display_output& output, uint32 index)
{
	if (output.id == 2)
		return "HDMI-1";
	if (output.id == 7)
		return "HDMI-2";
	return index == 0 ? "Display-1" : "Display-2";
}


static status_t
rpi_get_display_outputs(display_output* outputs, uint32* _count)
{
	rpi_display_shared_info shared;
	status_t status = read_state(shared);
	if (status != B_OK)
		return status;
	uint32 count = 0;
	for (uint32 i = 0; i < shared.output_count && count < *_count; i++) {
		const rpi_display_output& source = shared.outputs[i];
		display_output& output = outputs[count++];
		memset(&output, 0, sizeof(output));
		output.version = B_DISPLAY_OUTPUT_VERSION;
		output.id = i + 1;
		strlcpy(output.name, output_name(source, i), sizeof(output.name));
		output.flags = B_DISPLAY_OUTPUT_SCALABLE;
		if ((source.flags & RPI_DISPLAY_OUTPUT_CONNECTED) != 0)
			output.flags |= B_DISPLAY_OUTPUT_CONNECTED;
		output.scale = output.render_scale = 100;
		output.native_timing = make_timing(source.native_width,
			source.native_height);
		output.timing = output.native_timing;
		if ((source.flags & RPI_DISPLAY_OUTPUT_ENABLED) != 0) {
			output.flags |= B_DISPLAY_OUTPUT_ENABLED;
			if ((source.flags & RPI_DISPLAY_OUTPUT_MIRROR) != 0)
				output.flags |= B_DISPLAY_OUTPUT_MIRROR;
			output.x = source.x;
			output.y = source.y;
			output.width = source.width;
			output.height = source.height;
			if (source.scale != 0) {
				output.scale = source.scale;
				output.render_scale = source.render_scale;
			}
			if (source.mode_width != 0) {
				output.timing = make_timing(source.mode_width,
					source.mode_height);
			}
		}
		if (source.edid_length > 0) {
			output.edid_length = source.edid_length;
			memcpy(output.edid, source.edid, source.edid_length);
		}
	}
	*_count = count;
	return B_OK;
}


static status_t
rpi_get_display_output_modes(uint32 id, display_mode* modes, uint32* _count)
{
	rpi_display_shared_info shared;
	status_t status = read_state(shared);
	if (status != B_OK)
		return status;
	if (id < 1 || id > shared.output_count)
		return B_ENTRY_NOT_FOUND;
	const rpi_display_output& output = shared.outputs[id - 1];

	// the output's own size first, then what the firmware scales up to it
	uint32 count = 0;
	if (count < *_count)
		modes[count++] = make_mode(output.native_width, output.native_height);
	for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++) {
		if (count == *_count)
			break;
		if (kSizes[i][0] > output.native_width
			|| kSizes[i][1] > output.native_height
			|| (kSizes[i][0] == output.native_width
				&& kSizes[i][1] == output.native_height)) {
			continue;
		}
		modes[count++] = make_mode(kSizes[i][0], kSizes[i][1]);
	}
	*_count = count;
	return B_OK;
}


/*!	Takes the layout app_server wants; it shows when the mode returned here
	is set. Positions come in logical pixels, the regions are in frame
	buffer pixels.
*/
static status_t
rpi_set_display_layout(const display_output_config* configs, uint32 count,
	display_mode* _mode)
{
	rpi_display_shared_info shared;
	status_t status = read_state(shared);
	if (status != B_OK)
		return status;
	rpi_display_layout layout = {};
	layout.version = RPI_DISPLAY_VERSION;

	uint32 enabled = 0;
	for (uint32 i = 0; i < count; i++) {
		const display_output_config& config = configs[i];
		if (config.id < 1 || config.id > shared.output_count)
			return B_ENTRY_NOT_FOUND;
		const rpi_display_output& source = shared.outputs[config.id - 1];
		rpi_display_output& output = layout.outputs[config.id - 1];
		if ((config.flags & B_DISPLAY_OUTPUT_ENABLED) == 0)
			continue;
		if ((source.flags & RPI_DISPLAY_OUTPUT_CONNECTED) == 0)
			return B_ENTRY_NOT_FOUND;

		bool mirror = (config.flags & B_DISPLAY_OUTPUT_MIRROR) != 0;
		uint16 renderScale = config.render_scale != 0
			? config.render_scale : 100;
		uint16 scale = config.scale;
		if (mirror) {
			// A mirror shows its source's region whatever its own size:
			// the scale app_server worked out for it means nothing here.
			if (scale < renderScale)
				scale = renderScale;
		} else if (scale < 100 || scale > 400 || renderScale < 100
			|| renderScale > scale) {
			return B_BAD_VALUE;
		}

		uint32 width = source.native_width;
		uint32 height = source.native_height;
		if (config.timing.h_display != 0) {
			width = config.timing.h_display;
			height = config.timing.v_display;
			if (width > source.native_width || height > source.native_height
				|| width < 320 || height < 200) {
				return B_BAD_VALUE;
			}
		}

		output.flags = RPI_DISPLAY_OUTPUT_ENABLED;
		if (mirror)
			output.flags |= RPI_DISPLAY_OUTPUT_MIRROR;
		output.mode_width = width;
		output.mode_height = height;
		output.scale = config.scale != 0 ? config.scale : 100;
		output.render_scale = renderScale;
		output.width = region_size(width, scale, renderScale);
		output.height = region_size(height, scale, renderScale);
		output.x = (config.x * renderScale + 50) / 100;
		output.y = (config.y * renderScale + 50) / 100;
		enabled++;
	}
	if (enabled == 0)
		return B_BAD_VALUE;

	// A mirror shows its source's region: the output without the flag that
	// starts at the same place.
	for (uint32 i = 0; i < shared.output_count; i++) {
		rpi_display_output& output = layout.outputs[i];
		if ((output.flags & RPI_DISPLAY_OUTPUT_ENABLED) == 0
			|| (output.flags & RPI_DISPLAY_OUTPUT_MIRROR) == 0) {
			continue;
		}
		const rpi_display_output* source = NULL;
		for (uint32 k = 0; k < shared.output_count; k++) {
			const rpi_display_output& candidate = layout.outputs[k];
			if (k != i && (candidate.flags & RPI_DISPLAY_OUTPUT_ENABLED) != 0
				&& (candidate.flags & RPI_DISPLAY_OUTPUT_MIRROR) == 0
				&& candidate.x == output.x && candidate.y == output.y) {
				source = &candidate;
			}
		}
		if (source == NULL)
			return B_BAD_VALUE;
		output.width = source->width;
		output.height = source->height;
	}

	// the regions' bounding box, moved to the frame buffer's origin
	int32 minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN,
		maxY = INT32_MIN;
	for (uint32 i = 0; i < shared.output_count; i++) {
		const rpi_display_output& output = layout.outputs[i];
		if ((output.flags & RPI_DISPLAY_OUTPUT_ENABLED) == 0)
			continue;
		if (output.x < minX)
			minX = output.x;
		if (output.y < minY)
			minY = output.y;
		if (output.x + output.width > maxX)
			maxX = output.x + output.width;
		if (output.y + output.height > maxY)
			maxY = output.y + output.height;
	}
	for (uint32 i = 0; i < shared.output_count; i++) {
		layout.outputs[i].x -= minX;
		layout.outputs[i].y -= minY;
	}
	layout.width = maxX - minX;
	layout.height = maxY - minY;
	if (layout.width > 8192 || layout.height > 8192)
		return B_BAD_VALUE;

	gInfo->pending_layout = layout;
	gInfo->pending = true;
	*_mode = make_mode(layout.width, layout.height);
	return B_OK;
}


//	#pragma mark -


extern "C" void*
get_accelerant_hook(uint32 feature, void* data)
{
	switch (feature) {
		case B_INIT_ACCELERANT:
			return (void*)rpi_init_accelerant;
		case B_UNINIT_ACCELERANT:
			return (void*)rpi_uninit_accelerant;
		case B_CLONE_ACCELERANT:
			return (void*)rpi_clone_accelerant;
		case B_ACCELERANT_CLONE_INFO_SIZE:
			return (void*)rpi_accelerant_clone_info_size;
		case B_GET_ACCELERANT_CLONE_INFO:
			return (void*)rpi_get_accelerant_clone_info;
		case B_GET_ACCELERANT_DEVICE_INFO:
			return (void*)rpi_get_accelerant_device_info;
		case B_ACCELERANT_RETRACE_SEMAPHORE:
			return (void*)rpi_accelerant_retrace_semaphore;

		case B_SET_CURSOR_BITMAP:
			return (void*)rpi_set_cursor_bitmap;
		case B_MOVE_CURSOR:
			return (void*)rpi_move_cursor;
		case B_SHOW_CURSOR:
			return (void*)rpi_show_cursor;

		case B_ACCELERANT_MODE_COUNT:
			return (void*)rpi_accelerant_mode_count;
		case B_GET_MODE_LIST:
			return (void*)rpi_get_mode_list;
		case B_SET_DISPLAY_MODE:
			return (void*)rpi_set_display_mode;
		case B_GET_DISPLAY_MODE:
			return (void*)rpi_get_display_mode;
		case B_GET_PREFERRED_DISPLAY_MODE:
			return (void*)rpi_get_preferred_display_mode;
		case B_GET_EDID_INFO:
			return (void*)rpi_get_edid_info;
		case B_GET_FRAME_BUFFER_CONFIG:
			return (void*)rpi_get_frame_buffer_config;
		case B_GET_PIXEL_CLOCK_LIMITS:
			return (void*)rpi_get_pixel_clock_limits;

		case B_GET_DISPLAY_OUTPUT_COUNT:
			return (void*)rpi_display_output_count;
		case B_GET_DISPLAY_OUTPUTS:
			return (void*)rpi_get_display_outputs;
		case B_GET_DISPLAY_OUTPUT_MODES:
			return (void*)rpi_get_display_output_modes;
		case B_SET_DISPLAY_LAYOUT:
			return (void*)rpi_set_display_layout;
		case B_SET_DISPLAY_CHANGE_PORT:
			return (void*)rpi_set_display_change_port;
	}
	return NULL;
}
