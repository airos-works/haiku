/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The accelerant of the Allwinner A733's display outputs (sunxi_display),
	a copy of the Raspberry Pi's (rpi_display) for the same protocol.

	The driver holds each output at its native video mode and has the
	display engine scale whatever region of the frame buffer it is given to
	that mode. So an output's "resolution" and its scale are both the size of
	its region; app_server arranges the regions through the display layout
	hooks. There is no acceleration and no hardware cursor yet. */


#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <Accelerant.h>
#include <OS.h>

#include <create_display_modes.h>
#include <edid.h>

#include <sunxi_display.h>


struct accelerant_info {
	int						device;
	bool					is_clone;
	area_id					shared_area;
	sunxi_display_shared_info* shared;
	area_id					frame_buffer_area;
	void*					frame_buffer;
	uint32					generation;

	// the layout app_server asked for, until its mode is set
	bool					pending;
	sunxi_display_layout		pending_layout;
};

static accelerant_info* gInfo;

// the resolutions a headless output offers up to its own
static const uint16 kSizes[][2] = {
	{3840, 2160}, {2560, 1440}, {1920, 1200}, {1920, 1080}, {1680, 1050},
	{1600, 900}, {1440, 900}, {1366, 768}, {1280, 1024}, {1280, 800},
	{1280, 720}, {1024, 768}, {800, 600}, {640, 480}
};


//	#pragma mark - helpers


/*!	A 60 Hz timing for a picture of this size, for what has no real one:
	the frame buffer as a whole, a headless output.
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


static display_timing
from_shared(const sunxi_display_timing& shared)
{
	display_timing timing;
	memset(&timing, 0, sizeof(timing));
	timing.pixel_clock = shared.pixel_clock;
	timing.h_display = shared.h_display;
	timing.h_sync_start = shared.h_sync_start;
	timing.h_sync_end = shared.h_sync_end;
	timing.h_total = shared.h_total;
	timing.v_display = shared.v_display;
	timing.v_sync_start = shared.v_sync_start;
	timing.v_sync_end = shared.v_sync_end;
	timing.v_total = shared.v_total;
	timing.flags = shared.flags & (B_POSITIVE_HSYNC | B_POSITIVE_VSYNC);
	return timing;
}


static sunxi_display_timing
to_shared(const display_timing& timing)
{
	sunxi_display_timing shared = {};
	shared.pixel_clock = timing.pixel_clock;
	shared.h_display = timing.h_display;
	shared.h_sync_start = timing.h_sync_start;
	shared.h_sync_end = timing.h_sync_end;
	shared.h_total = timing.h_total;
	shared.v_display = timing.v_display;
	shared.v_sync_start = timing.v_sync_start;
	shared.v_sync_end = timing.v_sync_end;
	shared.v_total = timing.v_total;
	shared.flags = timing.flags & (B_POSITIVE_HSYNC | B_POSITIVE_VSYNC);
	return shared;
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
read_state(sunxi_display_shared_info& state)
{
	if (ioctl(gInfo->device, SUNXI_DISPLAY_GET_STATE, &state, sizeof(state)) != 0)
		return errno != 0 ? errno : B_ERROR;
	return B_OK;
}


static status_t
sunxi_set_display_change_port(port_id port, int32 code)
{
	sunxi_display_change_port request = {port, code};
	if (ioctl(gInfo->device, SUNXI_DISPLAY_SET_CHANGE_PORT, &request,
			sizeof(request)) != 0)
		return errno != 0 ? errno : B_ERROR;
	return B_OK;
}


static status_t
map_frame_buffer()
{
	area_info info;
	if (ioctl(gInfo->device, SUNXI_DISPLAY_CLONE_FRAME_BUFFER, &info,
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
apply_layout(const sunxi_display_layout& layout)
{
	sunxi_display_layout request = layout;
	if (ioctl(gInfo->device, SUNXI_DISPLAY_SET_LAYOUT, &request,
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

	area_id sharedArea;
	status_t status = B_OK;
	if (ioctl(device, SUNXI_DISPLAY_GET_SHARED_AREA, &sharedArea,
			sizeof(sharedArea)) != 0) {
		status = errno != 0 ? errno : B_ERROR;
	}
	if (status == B_OK) {
		gInfo->shared_area = clone_area("sunxi display shared info",
			(void**)&gInfo->shared, B_ANY_ADDRESS, B_READ_AREA,
			sharedArea);
		if (gInfo->shared_area < 0)
			status = gInfo->shared_area;
		else if (gInfo->shared->version != SUNXI_DISPLAY_VERSION) {
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
		sunxi_set_display_change_port(-1, 0);
	if (gInfo->frame_buffer_area >= 0)
		delete_area(gInfo->frame_buffer_area);
	delete_area(gInfo->shared_area);
	if (gInfo->is_clone)
		close(gInfo->device);
	free(gInfo);
	gInfo = NULL;
}


//	#pragma mark - general hooks


static status_t
sunxi_init_accelerant(int device)
{
	status_t status = init_common(device, false);
	if (status != B_OK)
		return status;

	// Until app_server arranges them: the first output at its own size and
	// the other one showing the same.
	sunxi_display_shared_info shared;
	status = read_state(shared);
	if (status != B_OK) {
		uninit_common();
		return status;
	}
	sunxi_display_layout layout = {};
	layout.version = SUNXI_DISPLAY_VERSION;
	uint32 first = 0;
	while (first < shared.output_count
		&& (shared.outputs[first].flags & SUNXI_DISPLAY_OUTPUT_CONNECTED) == 0)
		first++;
	if (first == shared.output_count) {
		uninit_common();
		return B_DEVICE_NOT_FOUND;
	}
	// the mode the output shows now: the driver may have kept one a layout
	// asked for earlier
	const sunxi_display_timing& timing = shared.outputs[first].timing;
	layout.width = timing.h_display;
	layout.height = timing.v_display;
	for (uint32 i = 0; i < shared.output_count; i++) {
		if ((shared.outputs[i].flags & SUNXI_DISPLAY_OUTPUT_CONNECTED) == 0)
			continue;
		sunxi_display_output& output = layout.outputs[i];
		output.flags = SUNXI_DISPLAY_OUTPUT_ENABLED
			| (i != first ? SUNXI_DISPLAY_OUTPUT_MIRROR : 0);
		output.width = output.mode_width = layout.width;
		output.height = output.mode_height = layout.height;
		output.scale = output.render_scale = 100;
		if (i == first)
			output.timing = timing;
	}

	status = apply_layout(layout);
	if (status != B_OK)
		uninit_common();
	return status;
}


static void
sunxi_uninit_accelerant()
{
	uninit_common();
}


static ssize_t
sunxi_accelerant_clone_info_size()
{
	return B_PATH_NAME_LENGTH;
}


static void
sunxi_get_accelerant_clone_info(void* info)
{
	strlcpy((char*)info, "/dev/" SUNXI_DISPLAY_DEVICE, B_PATH_NAME_LENGTH);
}


static status_t
sunxi_clone_accelerant(void* info)
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
sunxi_get_accelerant_device_info(accelerant_device_info* info)
{
	info->version = B_ACCELERANT_VERSION;
	strlcpy(info->name, "Allwinner display engine", sizeof(info->name));
	strlcpy(info->chipset, "Allwinner A733 DE 3.5",
		sizeof(info->chipset));
	strlcpy(info->serial_no, "None", sizeof(info->serial_no));
	info->memory = gInfo->shared->bytes_per_row * gInfo->shared->height;
	info->dac_speed = 0;
	return B_OK;
}


/*!	Released at every vertical blank of the DisplayPort output (at 60 Hz
	while it shows nothing); app_server's own, which it gives to its
	clients.
*/
static sem_id
sunxi_accelerant_retrace_semaphore()
{
	sem_id semaphore;
	if (ioctl(gInfo->device, SUNXI_DISPLAY_GET_RETRACE_SEMAPHORE, &semaphore,
			sizeof(semaphore)) != 0) {
		return -1;
	}
	return semaphore;
}


//	#pragma mark - mode hooks


// The one mode is the desktop of the layout: the one app_server asked for
// last, until it is set.
static uint32
sunxi_accelerant_mode_count()
{
	return 1;
}


static status_t
sunxi_get_mode_list(display_mode* list)
{
	list[0] = preferred_mode();
	return B_OK;
}


static status_t
sunxi_get_preferred_display_mode(display_mode* mode)
{
	*mode = preferred_mode();
	return B_OK;
}


static status_t
sunxi_set_display_mode(display_mode* mode)
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
sunxi_get_display_mode(display_mode* mode)
{
	*mode = current_mode();
	return B_OK;
}


static status_t
sunxi_get_frame_buffer_config(frame_buffer_config* config)
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
sunxi_get_pixel_clock_limits(display_mode* mode, uint32* _low, uint32* _high)
{
	*_low = *_high = mode->timing.pixel_clock;
	return B_OK;
}


static status_t
sunxi_get_edid_info(void* info, size_t size, uint32* _version)
{
	sunxi_display_shared_info state;
	status_t status = read_state(state);
	if (status != B_OK)
		return status;
	uint32 first = 0;
	while (first < state.output_count
		&& (state.outputs[first].flags & SUNXI_DISPLAY_OUTPUT_CONNECTED) == 0)
		first++;
	if (first == state.output_count)
		return B_DEVICE_NOT_FOUND;
	const sunxi_display_output& output = state.outputs[first];
	if (output.edid_length < 128 || size < sizeof(edid1_info))
		return B_ERROR;

	edid_decode((edid1_info*)info, (const edid1_raw*)output.edid);
	*_version = EDID_VERSION_1;
	return B_OK;
}


//	#pragma mark - display layout hooks


static uint32
sunxi_display_output_count()
{
	return gInfo->shared->output_count;
}


static const char*
output_name(const sunxi_display_output& output, uint32 index)
{
	if (output.id == SUNXI_DISPLAY_OUTPUT_DP0)
		return "DP-1";
	return index == 0 ? "Display-1" : "Display-2";
}


static status_t
sunxi_get_display_outputs(display_output* outputs, uint32* _count)
{
	sunxi_display_shared_info shared;
	status_t status = read_state(shared);
	if (status != B_OK)
		return status;
	uint32 count = 0;
	for (uint32 i = 0; i < shared.output_count && count < *_count; i++) {
		const sunxi_display_output& source = shared.outputs[i];
		display_output& output = outputs[count++];
		memset(&output, 0, sizeof(output));
		output.version = B_DISPLAY_OUTPUT_VERSION;
		output.id = i + 1;
		strlcpy(output.name, output_name(source, i), sizeof(output.name));
		output.flags = B_DISPLAY_OUTPUT_SCALABLE;
		if ((source.flags & SUNXI_DISPLAY_OUTPUT_CONNECTED) != 0)
			output.flags |= B_DISPLAY_OUTPUT_CONNECTED;
		output.scale = output.render_scale = 100;
		output.native_timing = from_shared(source.native_timing);
		output.timing = from_shared(source.timing);
		if ((source.flags & SUNXI_DISPLAY_OUTPUT_ENABLED) != 0) {
			output.flags |= B_DISPLAY_OUTPUT_ENABLED;
			if ((source.flags & SUNXI_DISPLAY_OUTPUT_MIRROR) != 0)
				output.flags |= B_DISPLAY_OUTPUT_MIRROR;
			output.x = source.x;
			output.y = source.y;
			output.width = source.width;
			output.height = source.height;
			if (source.scale != 0) {
				output.scale = source.scale;
				output.render_scale = source.render_scale;
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


static uint32 sMaxPixelClock;


static bool
is_mode_supported(display_mode* mode)
{
	const display_timing& timing = mode->timing;
	return timing.h_display >= 320 && timing.h_display <= 4096
		&& timing.v_display >= 200 && timing.v_display <= 4096
		&& timing.pixel_clock >= 20000
		&& (timing.flags & B_TIMING_INTERLACED) == 0
		&& (sMaxPixelClock == 0 || timing.pixel_clock <= sMaxPixelClock);
}


/*!	A display's modes come from its EDID (preferred, detailed, standard and
	established timings, those the link carries); a headless output offers
	the common sizes up to its own. The preferred mode comes first.
*/
static uint32
output_modes(const sunxi_display_output& output, display_mode* modes,
	uint32 maxCount)
{
	display_mode native;
	memset(&native, 0, sizeof(native));
	native.timing = from_shared(output.native_timing);
	native.space = B_RGB32;
	native.virtual_width = native.timing.h_display;
	native.virtual_height = native.timing.v_display;

	uint32 count = 0;
	if (count < maxCount)
		modes[count++] = native;

	if ((output.flags & SUNXI_DISPLAY_OUTPUT_VIRTUAL) == 0
		&& output.edid_length >= 128) {
		edid1_info edid;
		edid_decode(&edid, (const edid1_raw*)output.edid);
		static const color_space kSpaces[] = { B_RGB32 };
		sMaxPixelClock = output.max_pixel_clock;
		display_mode* list;
		uint32 listCount;
		area_id area = create_display_modes("sunxi display modes", &edid,
			&native, 1, kSpaces, 1, is_mode_supported, &list, &listCount);
		if (area >= 0) {
			// the largest first
			for (uint32 i = listCount; i-- > 0 && count < maxCount;) {
				const display_timing& timing = list[i].timing;
				if (timing.h_display != native.timing.h_display
					|| timing.v_display != native.timing.v_display
					|| timing.pixel_clock != native.timing.pixel_clock) {
					modes[count++] = list[i];
				}
			}
			delete_area(area);
			return count;
		}
	}

	for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++) {
		if (count == maxCount)
			break;
		if (kSizes[i][0] > output.native_width
			|| kSizes[i][1] > output.native_height
			|| (kSizes[i][0] == output.native_width
				&& kSizes[i][1] == output.native_height)) {
			continue;
		}
		modes[count++] = make_mode(kSizes[i][0], kSizes[i][1]);
	}
	return count;
}


static float
refresh_rate(const display_timing& timing)
{
	if (timing.h_total == 0 || timing.v_total == 0)
		return 0;
	return timing.pixel_clock * 1000.0f / timing.h_total / timing.v_total;
}


/*!	The real timing for what app_server asks for: it names a mode by its
	size and refresh rate only. The output's mode of that size with the
	closest refresh rate.
*/
static status_t
find_timing(const sunxi_display_output& output, const display_timing& request,
	display_timing& _timing)
{
	display_mode modes[64];
	uint32 count = output_modes(output, modes, 64);
	float wanted = refresh_rate(request);
	float bestDistance = -1;
	for (uint32 i = 0; i < count; i++) {
		const display_timing& timing = modes[i].timing;
		if (timing.h_display != request.h_display
			|| timing.v_display != request.v_display) {
			continue;
		}
		float distance = wanted > 0 ? fabsf(refresh_rate(timing) - wanted) : 0;
		if (bestDistance < 0 || distance < bestDistance) {
			bestDistance = distance;
			_timing = timing;
		}
	}
	return bestDistance < 0 ? B_BAD_VALUE : B_OK;
}


static status_t
sunxi_get_display_output_modes(uint32 id, display_mode* modes, uint32* _count)
{
	sunxi_display_shared_info shared;
	status_t status = read_state(shared);
	if (status != B_OK)
		return status;
	if (id < 1 || id > shared.output_count)
		return B_ENTRY_NOT_FOUND;
	*_count = output_modes(shared.outputs[id - 1], modes, *_count);
	return B_OK;
}


/*!	Takes the layout app_server wants; it shows when the mode returned here
	is set. Positions come in logical pixels, the regions are in frame
	buffer pixels.
*/
static status_t
sunxi_set_display_layout(const display_output_config* configs, uint32 count,
	display_mode* _mode)
{
	sunxi_display_shared_info shared;
	status_t status = read_state(shared);
	if (status != B_OK)
		return status;
	sunxi_display_layout layout = {};
	layout.version = SUNXI_DISPLAY_VERSION;

	uint32 enabled = 0;
	for (uint32 i = 0; i < count; i++) {
		const display_output_config& config = configs[i];
		if (config.id < 1 || config.id > shared.output_count)
			return B_ENTRY_NOT_FOUND;
		const sunxi_display_output& source = shared.outputs[config.id - 1];
		sunxi_display_output& output = layout.outputs[config.id - 1];
		if ((config.flags & B_DISPLAY_OUTPUT_ENABLED) == 0)
			continue;
		if ((source.flags & SUNXI_DISPLAY_OUTPUT_CONNECTED) == 0)
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

		// the mode: a real one the display offers (the driver checks it
		// again), the preferred one when none is given
		display_timing timing = from_shared(source.native_timing);
		if (config.timing.h_display != 0) {
			status = find_timing(source, config.timing, timing);
			if (status != B_OK)
				return status;
		}
		uint32 width = timing.h_display;
		uint32 height = timing.v_display;

		output.flags = SUNXI_DISPLAY_OUTPUT_ENABLED;
		output.timing = to_shared(timing);
		if (mirror)
			output.flags |= SUNXI_DISPLAY_OUTPUT_MIRROR;
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
		sunxi_display_output& output = layout.outputs[i];
		if ((output.flags & SUNXI_DISPLAY_OUTPUT_ENABLED) == 0
			|| (output.flags & SUNXI_DISPLAY_OUTPUT_MIRROR) == 0) {
			continue;
		}
		const sunxi_display_output* source = NULL;
		for (uint32 k = 0; k < shared.output_count; k++) {
			const sunxi_display_output& candidate = layout.outputs[k];
			if (k != i && (candidate.flags & SUNXI_DISPLAY_OUTPUT_ENABLED) != 0
				&& (candidate.flags & SUNXI_DISPLAY_OUTPUT_MIRROR) == 0
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
		const sunxi_display_output& output = layout.outputs[i];
		if ((output.flags & SUNXI_DISPLAY_OUTPUT_ENABLED) == 0)
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
			return (void*)sunxi_init_accelerant;
		case B_UNINIT_ACCELERANT:
			return (void*)sunxi_uninit_accelerant;
		case B_CLONE_ACCELERANT:
			return (void*)sunxi_clone_accelerant;
		case B_ACCELERANT_CLONE_INFO_SIZE:
			return (void*)sunxi_accelerant_clone_info_size;
		case B_GET_ACCELERANT_CLONE_INFO:
			return (void*)sunxi_get_accelerant_clone_info;
		case B_GET_ACCELERANT_DEVICE_INFO:
			return (void*)sunxi_get_accelerant_device_info;
		case B_ACCELERANT_RETRACE_SEMAPHORE:
			return (void*)sunxi_accelerant_retrace_semaphore;

		case B_ACCELERANT_MODE_COUNT:
			return (void*)sunxi_accelerant_mode_count;
		case B_GET_MODE_LIST:
			return (void*)sunxi_get_mode_list;
		case B_SET_DISPLAY_MODE:
			return (void*)sunxi_set_display_mode;
		case B_GET_DISPLAY_MODE:
			return (void*)sunxi_get_display_mode;
		case B_GET_PREFERRED_DISPLAY_MODE:
			return (void*)sunxi_get_preferred_display_mode;
		case B_GET_EDID_INFO:
			return (void*)sunxi_get_edid_info;
		case B_GET_FRAME_BUFFER_CONFIG:
			return (void*)sunxi_get_frame_buffer_config;
		case B_GET_PIXEL_CLOCK_LIMITS:
			return (void*)sunxi_get_pixel_clock_limits;

		case B_GET_DISPLAY_OUTPUT_COUNT:
			return (void*)sunxi_display_output_count;
		case B_GET_DISPLAY_OUTPUTS:
			return (void*)sunxi_get_display_outputs;
		case B_GET_DISPLAY_OUTPUT_MODES:
			return (void*)sunxi_get_display_output_modes;
		case B_SET_DISPLAY_LAYOUT:
			return (void*)sunxi_set_display_layout;
		case B_SET_DISPLAY_CHANGE_PORT:
			return (void*)sunxi_set_display_change_port;
	}
	return NULL;
}
