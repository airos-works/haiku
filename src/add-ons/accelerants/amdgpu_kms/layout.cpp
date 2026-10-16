/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "accelerant.h"

#include <stdlib.h>
#include <string.h>


static Output*
find_output(Layout& layout, uint32 id)
{
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		if (layout.outputs[i].connector == id)
			return &layout.outputs[i];
	}
	return NULL;
}


extern "C" uint32
amdgpu_display_output_count(void)
{
	refresh_outputs();
	return gInfo->outputCount;
}


extern "C" status_t
amdgpu_get_display_outputs(display_output* outputs, uint32* _count)
{
	refresh_outputs();
	uint32 count = min_c(*_count, gInfo->outputCount);
	for (uint32 i = 0; i < count; i++) {
		const Output& source = gInfo->outputs[i];
		display_output& output = outputs[i];
		memset(&output, 0, sizeof(output));
		output.version = B_DISPLAY_OUTPUT_VERSION;
		output.id = source.connector;
		strlcpy(output.name, source.name, sizeof(output.name));
		output.flags = B_DISPLAY_OUTPUT_SCALABLE;
		if (source.connected)
			output.flags |= B_DISPLAY_OUTPUT_CONNECTED;
		if (source.enabled)
			output.flags |= B_DISPLAY_OUTPUT_ENABLED;
		if (source.mirror)
			output.flags |= B_DISPLAY_OUTPUT_MIRROR;
		output.x = source.x;
		output.y = source.y;
		output.width = source.width;
		output.height = source.height;
		output.scale = source.scale != 0 ? source.scale : 100;
		output.render_scale = source.renderScale != 0 ? source.renderScale
			: output.scale;
		if (source.preferred >= 0)
			mode_to_timing(source.modes[source.preferred], output.native_timing);
		mode_to_timing(source.enabled ? source.timing
			: source.preferred >= 0 ? source.modes[source.preferred]
				: source.timing, output.timing);
		output.edid_length = min_c(source.edidLength,
			(uint32)B_DISPLAY_OUTPUT_EDID_LENGTH);
		memcpy(output.edid, source.edid, output.edid_length);
	}
	*_count = count;
	return B_OK;
}


extern "C" status_t
amdgpu_get_display_output_modes(uint32 id, display_mode* modes,
	uint32* _count)
{
	const Output* output = NULL;
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		if (gInfo->outputs[i].connector == id)
			output = &gInfo->outputs[i];
	}
	if (output == NULL)
		return B_ENTRY_NOT_FOUND;
	uint32 count = 0;
	for (uint32 m = 0; m < output->modeCount && count < *_count; m++) {
		if ((output->modes[m].flags & DRM_MODE_FLAG_INTERLACE) != 0)
			continue;
		display_mode& mode = modes[count++];
		memset(&mode, 0, sizeof(mode));
		mode_to_timing(output->modes[m], mode.timing);
		mode.space = B_RGB32;
		mode.virtual_width = output->modes[m].hdisplay;
		mode.virtual_height = output->modes[m].vdisplay;
	}
	*_count = count;
	return B_OK;
}


/*!	The monitor's own mode closest to \a timing, else \a timing itself. */
static drm_mode_modeinfo
output_timing(const Output& output, const display_timing& timing)
{
	if (timing.h_display == 0)
		return output.modes[output.preferred >= 0 ? output.preferred : 0];
	int32 best = find_output_mode(output, timing);
	if (best >= 0)
		return output.modes[best];
	drm_mode_modeinfo mode;
	timing_to_mode(timing, mode);
	return mode;
}


/*!	Takes the layout app_server wants: each monitor shows a region of the
	frame buffer, its logical size (its timing at its scale) times the
	render scale; positions come in logical pixels. A layout the display
	engine cannot drive is refused, which makes app_server try a lower
	render scale. The monitors change when the returned mode is set.
*/
/*!	The first frame buffer pixel of logical pixel \a logical at
	\a renderScale percent: app_server's floor(logical * scale).
*/
static int32
buffer_pixel(int32 logical, uint16 renderScale)
{
	int64 scaled = (int64)logical * renderScale;
	return (int32)(scaled >= 0 ? scaled / 100 : -((-scaled + 99) / 100));
}


extern "C" status_t
amdgpu_set_display_layout(const display_output_config* configs, uint32 count,
	display_mode* _mode)
{
	if (gInfo->isClone)
		return B_NOT_ALLOWED;

	Layout* layout = (Layout*)calloc(1, sizeof(Layout));
	if (layout == NULL)
		return B_NO_MEMORY;
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		layout->outputs[i] = gInfo->outputs[i];
		layout->outputs[i].enabled = false;
		layout->outputs[i].mirror = false;
	}

	status_t status = B_OK;
	bool any = false;
	for (uint32 i = 0; i < count && status == B_OK; i++) {
		const display_output_config& config = configs[i];
		Output* output = find_output(*layout, config.id);
		if (output == NULL) {
			status = B_ENTRY_NOT_FOUND;
			break;
		}
		if ((config.flags & B_DISPLAY_OUTPUT_ENABLED) == 0)
			continue;
		if (!output->connected || output->modeCount == 0) {
			status = B_BAD_VALUE;
			break;
		}
		output->enabled = true;
		output->mirror = (config.flags & B_DISPLAY_OUTPUT_MIRROR) != 0;
		output->timing = output_timing(*output, config.timing);
		output->scale = config.scale != 0 ? config.scale : 100;
		output->renderScale = config.render_scale != 0 ? config.render_scale
			: 100;
		uint32 logicalWidth = (output->timing.hdisplay * 100 + output->scale / 2)
			/ output->scale;
		uint32 logicalHeight = (output->timing.vdisplay * 100
			+ output->scale / 2) / output->scale;
		// The region starts where app_server draws the display's first
		// logical pixel, and ends where it draws the one after its last.
		output->x = buffer_pixel(config.x, output->renderScale);
		output->y = buffer_pixel(config.y, output->renderScale);
		if (output->scale == output->renderScale) {
			// Drawn one to one: the monitor's own pixels. The logical size
			// is rounded, so its scaled size can be a fraction of a pixel
			// off (1503.25 rows for a 1504 row panel at 175%), and a region
			// that size would be stretched by a pixel over the whole panel.
			output->width = output->timing.hdisplay;
			output->height = output->timing.vdisplay;
		} else {
			output->width = buffer_pixel(config.x + logicalWidth,
				output->renderScale) - output->x;
			output->height = buffer_pixel(config.y + logicalHeight,
				output->renderScale) - output->y;
		}
		any = true;
	}

	if (status == B_OK && !any)
		status = B_BAD_VALUE;
	if (status == B_OK) {
		// the regions start at the origin
		int32 left = INT32_MAX, top = INT32_MAX;
		for (uint32 i = 0; i < gInfo->outputCount; i++) {
			const Output& output = layout->outputs[i];
			if (output.enabled && !output.mirror) {
				left = min_c(left, output.x);
				top = min_c(top, output.y);
			}
		}
		if (left == INT32_MAX)
			status = B_BAD_VALUE;
		for (uint32 i = 0; status == B_OK && i < gInfo->outputCount; i++) {
			Output& output = layout->outputs[i];
			if (!output.enabled)
				continue;
			output.x -= left;
			output.y -= top;
		}
		// a mirror shows the region of the output it mirrors
		for (uint32 i = 0; status == B_OK && i < gInfo->outputCount; i++) {
			Output& output = layout->outputs[i];
			if (!output.enabled || !output.mirror)
				continue;
			const Output* source = NULL;
			for (uint32 j = 0; j < gInfo->outputCount; j++) {
				const Output& other = layout->outputs[j];
				if (other.enabled && !other.mirror && other.x == output.x
					&& other.y == output.y)
					source = &other;
			}
			if (source == NULL) {
				// nothing there to mirror: a monitor of its own
				output.mirror = false;
				continue;
			}
			output.width = source->width;
			output.height = source->height;
		}
		for (uint32 i = 0; status == B_OK && i < gInfo->outputCount; i++) {
			const Output& output = layout->outputs[i];
			if (!output.enabled || output.mirror)
				continue;
			layout->width = max_c(layout->width,
				(uint32)(output.x + output.width));
			layout->height = max_c(layout->height,
				(uint32)(output.y + output.height));
		}
	}
	if (status == B_OK)
		status = apply_layout(*layout, true);
	if (status == B_OK) {
		gInfo->pending = *layout;
		gInfo->hasPending = true;
		*_mode = layout_mode(*layout);
		create_mode_list();
	}
	free(layout);
	return status;
}


/*!	Tells app_server when monitors come and go (amdgpu_drm counts its hot
	plug events); the outputs are probed again when it asks for them. */
static status_t
hotplug_thread(void*)
{
	drm_haiku_wait_hotplug wait = {};
	// the current count
	if (Kms::Ioctl(gInfo->device, DRM_HAIKU_IOCTL_WAIT_HOTPLUG, &wait) != B_OK)
		return B_OK;
	while (!gInfo->quitHotplug) {
		uint32 seen = wait.generation;
		wait.timeout_ms = 1000;
		if (Kms::Ioctl(gInfo->device, DRM_HAIKU_IOCTL_WAIT_HOTPLUG, &wait)
				!= B_OK) {
			snooze(1000000);
			continue;
		}
		if (wait.generation != seen && !gInfo->quitHotplug) {
			if (gInfo->dpmsMode != B_DPMS_ON) {
				// Monitors asleep drop their hot plug line now and then (a
				// Dell P2415Q five minutes into DPMS off, and raises it
				// again): looked at when they are woken.
				debug_printf("amdgpu_kms: hot plug event while asleep\n");
				gInfo->changedAsleep = true;
				continue;
			}
			// Links that are trained again (waking up, for one) raise hot
			// plug events too: only a different set of monitors matters.
			if (!connected_outputs_changed()) {
				debug_printf("amdgpu_kms: hot plug event, the same monitors\n");
				continue;
			}
			debug_printf("amdgpu_kms: monitors changed\n");
			gInfo->outputsChanged = true;
			write_port_etc(gInfo->changePort, gInfo->changeCode, NULL, 0,
				B_RELATIVE_TIMEOUT, 0);
		}
	}
	return B_OK;
}


void
stop_hotplug(void)
{
	if (gInfo->hotplugThread < 0)
		return;
	gInfo->quitHotplug = true;
	status_t result;
	wait_for_thread(gInfo->hotplugThread, &result);
	gInfo->hotplugThread = -1;
}


extern "C" status_t
amdgpu_set_display_change_port(port_id port, uint32 code)
{
	if (gInfo->isClone)
		return B_NOT_ALLOWED;
	gInfo->changePort = port;
	gInfo->changeCode = code;
	if (gInfo->hotplugThread < 0) {
		gInfo->quitHotplug = false;
		gInfo->hotplugThread = spawn_thread(hotplug_thread,
			"amdgpu_kms hot plug", B_NORMAL_PRIORITY, NULL);
		if (gInfo->hotplugThread >= 0)
			resume_thread(gInfo->hotplugThread);
	}
	return B_OK;
}
