/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	The framebuffer accelerant, pretending that its frame buffer is shown by
	several monitors, which are plugged in and pulled out by writing a file.
	This lets the app_server's handling of monitors coming and going be
	tried in a virtual machine.

	The monitors are listed in ~/config/settings/fake_outputs, one per line:
	"name width height". Without it there are two, sharing the frame buffer
	from left to right, the first one less tall. The names listed in
	/var/tmp/fake_outputs_unplugged are not connected; the file is read
	twice a second. Monitors have no EDID; each shows a region of its
	logical size at the layout's render scale, as hardware that scales
	would.
	A layout's frame buffer is the top left part of the real one.
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Accelerant.h>
#include <OS.h>

#include <compute_display_timing.h>

#include "accelerant.h"
#include "accelerant_protos.h"


#undef get_accelerant_hook
extern "C" void* framebuffer_get_accelerant_hook(uint32 feature, void* data);


static const int32 kMaxOutputs = 4;
static const char* kSettingsPath
	= "/boot/home/config/settings/fake_outputs";
static const char* kUnpluggedPath = "/var/tmp/fake_outputs_unplugged";


struct fake_output {
	char			name[B_DISPLAY_OUTPUT_NAME_LENGTH];
	uint16			width;
	uint16			height;
	bool			connected;
	bool			enabled;
	bool			mirror;
	int32			x;
	int32			y;
	uint16			regionWidth;
	uint16			regionHeight;
	uint16			scale;
	uint16			renderScale;
};


static fake_output sOutputs[kMaxOutputs];
static int32 sOutputCount;
static display_mode sFrameBufferMode;
static display_mode sMode;
static port_id sChangePort = -1;
static int32 sChangeCode;
static thread_id sWatcher = -1;
static volatile bool sQuitWatcher;


static bool
is_unplugged(const char* list, const char* name)
{
	size_t length = strlen(name);
	const char* at = list;
	while ((at = strstr(at, name)) != NULL) {
		bool start = at == list || at[-1] == ' ' || at[-1] == '\n'
			|| at[-1] == '\t';
		char end = at[length];
		if (start && (end == '\0' || end == ' ' || end == '\n'
				|| end == '\t' || end == '\r'))
			return true;
		at += length;
	}
	return false;
}


static void
read_unplugged(char* list, size_t size)
{
	list[0] = '\0';
	FILE* file = fopen(kUnpluggedPath, "r");
	if (file == NULL)
		return;
	size_t bytes = fread(list, 1, size - 1, file);
	list[bytes] = '\0';
	fclose(file);
}


/*!	Connects and disconnects the monitors as the file says; returns whether
	any changed.
*/
static bool
apply_unplugged(const char* list)
{
	bool changed = false;
	for (int32 i = 0; i < sOutputCount; i++) {
		bool connected = !is_unplugged(list, sOutputs[i].name);
		if (connected == sOutputs[i].connected)
			continue;
		sOutputs[i].connected = connected;
		if (!connected)
			sOutputs[i].enabled = false;
		changed = true;
	}
	return changed;
}


static status_t
watch_unplugged(void*)
{
	char last[512];
	read_unplugged(last, sizeof(last));

	while (!sQuitWatcher) {
		snooze(500000);

		char list[512];
		read_unplugged(list, sizeof(list));
		if (strcmp(list, last) == 0)
			continue;
		strcpy(last, list);

		if (apply_unplugged(list) && sChangePort >= 0)
			write_port(sChangePort, sChangeCode, NULL, 0);
	}
	return B_OK;
}


static void
set_up_outputs()
{
	uint16 width = sFrameBufferMode.virtual_width;
	uint16 height = sFrameBufferMode.virtual_height;

	sOutputCount = 0;
	FILE* file = fopen(kSettingsPath, "r");
	if (file != NULL) {
		char line[256];
		while (sOutputCount < kMaxOutputs
			&& fgets(line, sizeof(line), file) != NULL) {
			fake_output& output = sOutputs[sOutputCount];
			unsigned outputWidth, outputHeight;
			if (line[0] == '#' || sscanf(line, "%31s %u %u", output.name,
					&outputWidth, &outputHeight) != 3)
				continue;
			output.width = outputWidth;
			output.height = outputHeight;
			sOutputCount++;
		}
		fclose(file);
	}

	if (sOutputCount == 0) {
		uint16 first = (width * 2 / 5) & ~7;
		strcpy(sOutputs[0].name, "FAKE-1");
		sOutputs[0].width = first;
		sOutputs[0].height = height * 3 / 4;
		strcpy(sOutputs[1].name, "FAKE-2");
		sOutputs[1].width = width - first;
		sOutputs[1].height = height;
		sOutputCount = 2;
	}

	// side by side, as far as they fit
	int32 x = 0;
	for (int32 i = 0; i < sOutputCount; i++) {
		fake_output& output = sOutputs[i];
		output.connected = true;
		output.enabled = x + output.width <= width && output.height <= height;
		output.mirror = false;
		output.x = x;
		output.y = 0;
		output.regionWidth = output.width;
		output.regionHeight = output.height;
		output.scale = 100;
		output.renderScale = 100;
		if (output.enabled)
			x += output.width;
	}

	char list[512];
	read_unplugged(list, sizeof(list));
	apply_unplugged(list);

	sMode = sFrameBufferMode;
}


static void
fill_timing(display_timing& timing, uint16 width, uint16 height)
{
	compute_display_timing(width, height, 60, false, &timing);
}


// #pragma mark - hooks


static status_t
fake_init_accelerant(int device)
{
	status_t status = framebuffer_init_accelerant(device);
	if (status != B_OK)
		return status;

	sFrameBufferMode = gInfo->shared_info->current_mode;
	set_up_outputs();
	return B_OK;
}


static void
fake_uninit_accelerant()
{
	if (sWatcher >= 0) {
		sQuitWatcher = true;
		status_t result;
		wait_for_thread(sWatcher, &result);
		sWatcher = -1;
	}
	framebuffer_uninit_accelerant();
}


static status_t
fake_set_display_mode(display_mode* mode)
{
	if (mode == NULL || mode->space != sFrameBufferMode.space
		|| mode->virtual_width > sFrameBufferMode.virtual_width
		|| mode->virtual_height > sFrameBufferMode.virtual_height
		|| mode->virtual_width < 320 || mode->virtual_height < 200)
		return B_BAD_VALUE;

	sMode = *mode;
	sMode.h_display_start = 0;
	sMode.v_display_start = 0;
	return B_OK;
}


static status_t
fake_get_display_mode(display_mode* mode)
{
	*mode = sMode;
	return B_OK;
}


static uint32
fake_get_display_output_count()
{
	return sOutputCount;
}


static status_t
fake_get_display_outputs(display_output* outputs, uint32* _count)
{
	uint32 count = 0;
	for (int32 i = 0; i < sOutputCount && count < *_count; i++, count++) {
		const fake_output& fake = sOutputs[i];
		display_output& output = outputs[count];
		memset(&output, 0, sizeof(output));
		output.version = B_DISPLAY_OUTPUT_VERSION;
		output.id = i + 1;
		strlcpy(output.name, fake.name, sizeof(output.name));
		output.flags = B_DISPLAY_OUTPUT_SCALABLE;
		if (fake.connected) {
			output.flags |= B_DISPLAY_OUTPUT_CONNECTED;
			if (fake.enabled)
				output.flags |= B_DISPLAY_OUTPUT_ENABLED;
			if (fake.mirror)
				output.flags |= B_DISPLAY_OUTPUT_MIRROR;
		}
		output.x = fake.x;
		output.y = fake.y;
		output.width = fake.regionWidth;
		output.height = fake.regionHeight;
		output.scale = fake.scale;
		output.render_scale = fake.renderScale;
		fill_timing(output.native_timing, fake.width, fake.height);
		output.timing = output.native_timing;
	}
	*_count = count;
	return B_OK;
}


static status_t
fake_get_display_output_modes(uint32 id, display_mode* modes, uint32* _count)
{
	if (id < 1 || id > (uint32)sOutputCount)
		return B_BAD_VALUE;
	if (*_count < 1)
		return B_BUFFER_OVERFLOW;

	const fake_output& fake = sOutputs[id - 1];
	memset(&modes[0], 0, sizeof(display_mode));
	fill_timing(modes[0].timing, fake.width, fake.height);
	modes[0].space = sFrameBufferMode.space;
	modes[0].virtual_width = fake.width;
	modes[0].virtual_height = fake.height;
	*_count = 1;
	return B_OK;
}


static status_t
fake_set_display_layout(const display_output_config* configs, uint32 count,
	display_mode* _mode)
{
	fake_output outputs[kMaxOutputs];
	memcpy(outputs, sOutputs, sizeof(outputs));
	for (int32 i = 0; i < sOutputCount; i++)
		outputs[i].enabled = false;

	int32 right = 0, bottom = 0;
	for (uint32 i = 0; i < count; i++) {
		const display_output_config& config = configs[i];
		if (config.id < 1 || config.id > (uint32)sOutputCount)
			return B_BAD_VALUE;
		fake_output& output = outputs[config.id - 1];
		if ((config.flags & B_DISPLAY_OUTPUT_ENABLED) == 0
			|| !output.connected)
			continue;

		uint16 scale = config.scale != 0 ? config.scale : 100;
		uint16 renderScale = config.render_scale != 0
			? config.render_scale : 100;

		output.enabled = true;
		output.mirror = (config.flags & B_DISPLAY_OUTPUT_MIRROR) != 0;
		output.scale = scale;
		output.renderScale = renderScale;
		output.x = config.x * renderScale / 100;
		output.y = config.y * renderScale / 100;
		uint32 logicalWidth = (output.width * 100 + scale / 2) / scale;
		uint32 logicalHeight = (output.height * 100 + scale / 2) / scale;
		output.regionWidth = logicalWidth * renderScale / 100;
		output.regionHeight = logicalHeight * renderScale / 100;
		if (output.x + output.regionWidth > right)
			right = output.x + output.regionWidth;
		if (output.y + output.regionHeight > bottom)
			bottom = output.y + output.regionHeight;
	}

	if (right == 0 || bottom == 0)
		return B_BAD_VALUE;
	if (right > sFrameBufferMode.virtual_width
		|| bottom > sFrameBufferMode.virtual_height)
		return B_NO_MEMORY;

	memcpy(sOutputs, outputs, sizeof(outputs));

	display_mode mode = sFrameBufferMode;
	mode.virtual_width = right;
	mode.virtual_height = bottom;
	mode.h_display_start = 0;
	mode.v_display_start = 0;
	fill_timing(mode.timing, right, bottom);
	*_mode = mode;
	return B_OK;
}


static status_t
fake_set_display_change_port(port_id port, int32 code)
{
	sChangePort = port;
	sChangeCode = code;
	if (port >= 0 && sWatcher < 0) {
		sQuitWatcher = false;
		sWatcher = spawn_thread(watch_unplugged, "fake outputs watcher",
			B_LOW_PRIORITY, NULL);
		if (sWatcher >= 0)
			resume_thread(sWatcher);
	}
	return B_OK;
}


extern "C" void*
get_accelerant_hook(uint32 feature, void* data)
{
	switch (feature) {
		case B_INIT_ACCELERANT:
			return (void*)fake_init_accelerant;
		case B_UNINIT_ACCELERANT:
			return (void*)fake_uninit_accelerant;
		case B_SET_DISPLAY_MODE:
			return (void*)fake_set_display_mode;
		case B_GET_DISPLAY_MODE:
			return (void*)fake_get_display_mode;
		case B_GET_DISPLAY_OUTPUT_COUNT:
			return (void*)fake_get_display_output_count;
		case B_GET_DISPLAY_OUTPUTS:
			return (void*)fake_get_display_outputs;
		case B_GET_DISPLAY_OUTPUT_MODES:
			return (void*)fake_get_display_output_modes;
		case B_SET_DISPLAY_LAYOUT:
			return (void*)fake_set_display_layout;
		case B_SET_DISPLAY_CHANGE_PORT:
			return (void*)fake_set_display_change_port;
	}

	return framebuffer_get_accelerant_hook(feature, data);
}
