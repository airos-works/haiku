/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "accelerant.h"

#include <errno.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <edid.h>


#define TRACE(x...) debug_printf("amdgpu_kms: " x)


static const uint32 kMinPixelClock = 25000;		// kHz
static const uint32 kMaxPixelClock = 2400000;	// layout modes are wide


void
mode_to_timing(const drm_mode_modeinfo& mode, display_timing& timing)
{
	memset(&timing, 0, sizeof(timing));
	timing.pixel_clock = mode.clock;
	timing.h_display = mode.hdisplay;
	timing.h_sync_start = mode.hsync_start;
	timing.h_sync_end = mode.hsync_end;
	timing.h_total = mode.htotal;
	timing.v_display = mode.vdisplay;
	timing.v_sync_start = mode.vsync_start;
	timing.v_sync_end = mode.vsync_end;
	timing.v_total = mode.vtotal;
	if ((mode.flags & DRM_MODE_FLAG_PHSYNC) != 0)
		timing.flags |= B_POSITIVE_HSYNC;
	if ((mode.flags & DRM_MODE_FLAG_PVSYNC) != 0)
		timing.flags |= B_POSITIVE_VSYNC;
	if ((mode.flags & DRM_MODE_FLAG_INTERLACE) != 0)
		timing.flags |= B_TIMING_INTERLACED;
}


void
timing_to_mode(const display_timing& timing, drm_mode_modeinfo& mode)
{
	memset(&mode, 0, sizeof(mode));
	mode.clock = timing.pixel_clock;
	mode.hdisplay = timing.h_display;
	mode.hsync_start = timing.h_sync_start;
	mode.hsync_end = timing.h_sync_end;
	mode.htotal = timing.h_total;
	mode.vdisplay = timing.v_display;
	mode.vsync_start = timing.v_sync_start;
	mode.vsync_end = timing.v_sync_end;
	mode.vtotal = timing.v_total;
	mode.flags = ((timing.flags & B_POSITIVE_HSYNC) != 0
			? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC)
		| ((timing.flags & B_POSITIVE_VSYNC) != 0
			? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC)
		| ((timing.flags & B_TIMING_INTERLACED) != 0
			? DRM_MODE_FLAG_INTERLACE : 0);
	if (mode.htotal != 0 && mode.vtotal != 0) {
		mode.vrefresh = (uint32)((uint64)mode.clock * 1000
			/ ((uint64)mode.htotal * mode.vtotal));
	}
	mode.type = DRM_MODE_TYPE_USERDEF;
	snprintf(mode.name, sizeof(mode.name), "%ux%u", mode.hdisplay,
		mode.vdisplay);
}


static uint32
refresh_millihertz(const drm_mode_modeinfo& mode)
{
	if (mode.htotal == 0 || mode.vtotal == 0)
		return 0;
	return (uint32)((uint64)mode.clock * 1000000
		/ ((uint64)mode.htotal * mode.vtotal));
}


/*!	The monitor's own mode of \a timing's size whose refresh rate is
	nearest to its, or -1. app_server asks for a size and a rate only (its
	timings have no blanking), so pixel clocks cannot be compared: a 60 Hz
	request has a clock nearer a panel's 48 Hz mode than its 60 Hz one.
*/
int32
find_output_mode(const Output& output, const display_timing& timing)
{
	uint32 wanted = 0;
	if (timing.h_total != 0 && timing.v_total != 0) {
		wanted = (uint32)((uint64)timing.pixel_clock * 1000000
			/ ((uint64)timing.h_total * timing.v_total));
	}
	int32 best = -1;
	for (uint32 m = 0; m < output.modeCount; m++) {
		const drm_mode_modeinfo& candidate = output.modes[m];
		if (candidate.hdisplay != timing.h_display
			|| candidate.vdisplay != timing.v_display
			|| (candidate.flags & DRM_MODE_FLAG_INTERLACE) != 0)
			continue;
		if (best < 0 || abs((int32)refresh_millihertz(candidate)
					- (int32)wanted)
				< abs((int32)refresh_millihertz(output.modes[best])
					- (int32)wanted))
			best = m;
	}
	return best;
}


/*!	Connectors come and go at run time: DisplayPort MST hubs (in docks and
	daisy chained monitors) get one for every monitor behind them. */
static void
update_connector_list(void)
{
	Kms::Resources* resources
		= (Kms::Resources*)malloc(sizeof(Kms::Resources));
	if (resources == NULL)
		return;
	for (uint32 c = 0; c < gInfo->cardCount; c++) {
		Card& card = gInfo->cards[c];
		if (Kms::GetResources(card.fd, *resources) == B_OK) {
			memcpy(card.resources.connectors, resources->connectors,
				sizeof(resources->connectors));
			card.resources.connectorCount = resources->connectorCount;
		}
	}
	free(resources);
}


/*!	How many hot plug events amdgpu_drm has counted. */
uint32
hotplug_generation(void)
{
	drm_haiku_wait_hotplug wait = {};
	if (Kms::Ioctl(gInfo->device, DRM_HAIKU_IOCTL_WAIT_HOTPLUG, &wait) != B_OK)
		return 0;
	return wait.generation;
}


status_t
probe_outputs(void)
{
	// (before looking: an event during the probe is looked at again)
	gInfo->probedGeneration = hotplug_generation();
	update_connector_list();
	gInfo->outputCount = 0;
	for (uint32 c = 0; c < gInfo->cardCount; c++) {
	Card& card = gInfo->cards[c];
	int fd = card.fd;
	for (uint32 i = 0; i < card.resources.connectorCount
			&& gInfo->outputCount < kMaxOutputs; i++) {
		Kms::Connector* connector
			= (Kms::Connector*)malloc(sizeof(Kms::Connector));
		if (connector == NULL)
			return B_NO_MEMORY;
		if (Kms::GetConnector(fd, card.resources.connectors[i], *connector)
				!= B_OK) {
			free(connector);
			continue;
		}
		Output& output = gInfo->outputs[gInfo->outputCount++];
		memset(&output, 0, sizeof(output));
		output.card = c;
		output.connector = connector->id;
		output.id = output_id(c, connector->id);
		// the second GPU's "DP-1" is another connector than the first's
		if (c == 0) {
			snprintf(output.name, sizeof(output.name), "%s-%" B_PRIu32,
				Kms::ConnectorTypeName(connector->type), connector->typeId);
		} else {
			snprintf(output.name, sizeof(output.name), "%s-%" B_PRIu32
				" (GPU %" B_PRIu32 ")", Kms::ConnectorTypeName(connector->type),
				connector->typeId, c + 1);
		}
		output.connected = connector->connected && connector->modeCount > 0;
		output.internal = connector->type == DRM_MODE_CONNECTOR_eDP
			|| connector->type == DRM_MODE_CONNECTOR_LVDS
			|| connector->type == DRM_MODE_CONNECTOR_DSI;
		output.widthMM = connector->widthMM;
		output.heightMM = connector->heightMM;
		for (uint32 e = 0; e < connector->encoderCount; e++) {
			uint32 crtcs = 0;
			if (Kms::GetEncoderCrtcs(fd, connector->encoders[e], crtcs) == B_OK)
				output.possibleCrtcs |= crtcs;
		}
		output.modeCount = connector->modeCount;
		memcpy(output.modes, connector->modes,
			sizeof(drm_mode_modeinfo) * connector->modeCount);
		output.preferred = connector->modeCount > 0 ? 0 : -1;
		for (uint32 m = 0; m < connector->modeCount; m++) {
			if ((connector->modes[m].type & DRM_MODE_TYPE_PREFERRED) != 0) {
				output.preferred = m;
				break;
			}
		}
		output.edidLength = connector->edidLength;
		memcpy(output.edid, connector->edid, connector->edidLength);
		output.crtcIndex = -1;
		output.scale = output.renderScale = 100;
		TRACE("%s: %s, %" B_PRIu32 " mode(s)%s\n", output.name,
			output.connected ? "connected" : "disconnected", output.modeCount,
			output.edidLength > 0 ? ", EDID" : "");
		free(connector);
	}
	}
	return gInfo->outputCount > 0 ? B_OK : B_ENTRY_NOT_FOUND;
}


/*!	The layout app_server asked for (and is about to set as a mode) goes
	along with a new probe: its outputs are in the order of the connectors
	as they were, and a dock's MST hub adds and removes connectors. Left as
	it was, the layout would hand one connector's region to another, or to
	none (connector 0), and setting it would fail (ENOENT); a monitor of it
	that is gone or was exchanged makes it one to forget.
	\a previousCount is the number of outputs before the probe.
*/
static void
remap_pending(uint32 previousCount)
{
	if (!gInfo->hasPending)
		return;
	Layout* old = (Layout*)malloc(sizeof(Layout));
	if (old == NULL) {
		gInfo->hasPending = false;
		return;
	}
	memcpy(old, &gInfo->pending, sizeof(Layout));

	bool valid = true;
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		Output& output = gInfo->pending.outputs[i];
		output = gInfo->outputs[i];
		output.enabled = false;
		output.mirror = false;
		output.crtcIndex = -1;
		for (uint32 j = 0; j < previousCount; j++) {
			const Output& was = old->outputs[j];
			if (was.id != output.id || !was.enabled)
				continue;
			if (!output.connected || output.edidLength != was.edidLength
				|| memcmp(output.edid, was.edid, was.edidLength) != 0) {
				valid = false;
				break;
			}
			output.enabled = true;
			output.mirror = was.mirror;
			output.x = was.x;
			output.y = was.y;
			output.width = was.width;
			output.height = was.height;
			output.scale = was.scale;
			output.renderScale = was.renderScale;
			output.timing = was.timing;
		}
	}
	// a monitor of the layout whose connector went away
	for (uint32 j = 0; valid && j < previousCount; j++) {
		if (!old->outputs[j].enabled)
			continue;
		bool found = false;
		for (uint32 i = 0; i < gInfo->outputCount && !found; i++)
			found = gInfo->outputs[i].id == old->outputs[j].id;
		valid = found;
	}
	if (!valid) {
		debug_printf("amdgpu_kms: the monitors of the layout to set changed,"
			" forgetting it\n");
		gInfo->hasPending = false;
	}
	free(old);
}


/*!	Probes the outputs again after monitors came or went; what drives the
	ones still there stays as it is (app_server lays them out anew). */
void
refresh_outputs(void)
{
	if (!gInfo->outputsChanged)
		return;
	gInfo->outputsChanged = false;
	Output* previous = (Output*)malloc(sizeof(Output) * kMaxOutputs);
	if (previous == NULL)
		return;
	uint32 previousCount = gInfo->outputCount;
	memcpy(previous, gInfo->outputs, sizeof(Output) * previousCount);
	if (probe_outputs() == B_OK) {
		for (uint32 i = 0; i < gInfo->outputCount; i++) {
			Output& output = gInfo->outputs[i];
			for (uint32 j = 0; j < previousCount; j++) {
				const Output& old = previous[j];
				if (old.id != output.id)
					continue;
				output.enabled = old.enabled;
				output.mirror = old.mirror;
				output.x = old.x;
				output.y = old.y;
				output.width = old.width;
				output.height = old.height;
				output.scale = old.scale;
				output.renderScale = old.renderScale;
				output.timing = old.timing;
				output.crtcIndex = old.crtcIndex;
			}
		}
		remap_pending(previousCount);
		create_mode_list();
	}
	free(previous);
}


/*!	Every connected monitor at its preferred mode, side by side, as its
	connectors are numbered. */
void
default_layout(Layout& layout)
{
	memset(&layout, 0, sizeof(layout));
	int32 x = 0;
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		Output& output = layout.outputs[i];
		output = gInfo->outputs[i];
		output.enabled = output.connected && output.preferred >= 0;
		output.mirror = false;
		output.crtcIndex = -1;
		if (!output.enabled)
			continue;
		output.timing = output.modes[output.preferred];
		output.scale = output.renderScale = 100;
		output.x = x;
		output.y = 0;
		output.width = output.timing.hdisplay;
		output.height = output.timing.vdisplay;
		x += output.width;
		layout.width = max_c(layout.width, (uint32)(output.x + output.width));
		layout.height = max_c(layout.height, (uint32)(output.y + output.height));
	}
}


static const Output*
layout_primary(const Layout& layout)
{
	const Output* primary = NULL;
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		const Output& output = layout.outputs[i];
		if (!output.enabled || output.mirror)
			continue;
		if (primary == NULL || output.y < primary->y
			|| (output.y == primary->y && output.x < primary->x))
			primary = &output;
	}
	return primary;
}


Output*
primary_output(void)
{
	Output* primary = NULL;
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		Output& output = gInfo->outputs[i];
		if (!output.enabled || output.mirror || output.crtcIndex < 0)
			continue;
		if (primary == NULL || output.y < primary->y
			|| (output.y == primary->y && output.x < primary->x))
			primary = &output;
	}
	return primary;
}


/*!	The mode app_server sees for a layout: the frame buffer, with the
	primary monitor's timing stretched to its size at the same refresh rate.
*/
display_mode
layout_mode(const Layout& layout)
{
	display_mode mode;
	memset(&mode, 0, sizeof(mode));
	mode.space = B_RGB32;
	mode.virtual_width = layout.width;
	mode.virtual_height = layout.height;
	const Output* primary = layout_primary(layout);
	if (primary == NULL)
		return mode;
	drm_mode_modeinfo timing = primary->timing;
	uint32 refresh = refresh_millihertz(timing);
	uint32 hBlank = timing.htotal - timing.hdisplay;
	uint32 vBlank = timing.vtotal - timing.vdisplay;
	int32 hSync = timing.hsync_start - timing.hdisplay;
	int32 vSync = timing.vsync_start - timing.vdisplay;
	timing.hsync_start = layout.width + hSync;
	timing.hsync_end = timing.hsync_start + (timing.hsync_end
		- primary->timing.hsync_start);
	timing.htotal = layout.width + hBlank;
	timing.hdisplay = layout.width;
	timing.vsync_start = layout.height + vSync;
	timing.vsync_end = timing.vsync_start + (timing.vsync_end
		- primary->timing.vsync_start);
	timing.vtotal = layout.height + vBlank;
	timing.vdisplay = layout.height;
	if (refresh != 0) {
		timing.clock = (uint32)((uint64)timing.htotal * timing.vtotal
			* refresh / 1000000);
	}
	mode_to_timing(timing, mode.timing);
	return mode;
}


bool
is_layout_mode(const Layout& layout, const display_mode& mode)
{
	return layout.width != 0 && mode.virtual_width == layout.width
		&& mode.virtual_height == layout.height
		&& mode.timing.h_display == layout.width
		&& mode.timing.v_display == layout.height;
}


/*!	Where on its monitor an output shows its region: all of it, but a
	mirror keeps the region's shape, centered.
*/
static void
destination(const Output& output, uint32& x, uint32& y, uint32& width,
	uint32& height)
{
	x = y = 0;
	width = output.timing.hdisplay;
	height = output.timing.vdisplay;
	if (!output.mirror || output.width == 0 || output.height == 0)
		return;
	uint64 scaledWidth = (uint64)output.width * height / output.height;
	if (scaledWidth <= width) {
		x = (width - scaledWidth) / 2;
		width = scaledWidth;
	} else {
		uint64 scaledHeight = (uint64)output.height * width / output.width;
		y = (height - scaledHeight) / 2;
		height = scaledHeight;
	}
}


static uint32
round_up(uint32 value, uint32 multiple)
{
	return (value + multiple - 1) / multiple * multiple;
}


/*!	Whether \a buffer can show \a layout: at least as large, and not so
	much larger that keeping it wastes memory (after a monitor went away).
*/
static bool
buffer_fits(const Kms::Buffer& buffer, const Layout& layout)
{
	return buffer.handle != 0 && buffer.framebuffer != 0
		&& buffer.width >= layout.width && buffer.height >= layout.height
		&& (uint64)buffer.width * buffer.height
			<= (uint64)round_up(layout.width, 64) * round_up(layout.height, 16)
				* 5 / 4;
}


/*!	Whether \a card shows anything of \a layout. The first card always
	gets a frame buffer: it is app_server's.
*/
static bool
card_in_layout(const Layout& layout, uint32 card)
{
	if (card == 0)
		return true;
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		if (layout.outputs[i].enabled && layout.outputs[i].card == card)
			return true;
	}
	return false;
}


/*!	Adds what \a card does for \a layout to \a request: its monitors on the
	frame buffer \a buffer, its cursors off, the CRTCs it does not use off.
	Creates the mode blobs into \a blobs.
*/
static status_t
add_card_layout(Kms::AtomicRequest& request, uint32 cardIndex,
	const Layout& layout, const Kms::Buffer& buffer, uint32* blobs)
{
	Card& card = gInfo->cards[cardIndex];
	uint32 usedCrtcs = 0;
	status_t status = B_OK;
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		const Output& output = layout.outputs[i];
		if (output.card != cardIndex)
			continue;
		if (!output.enabled) {
			request.Add(output.connector, card.connectorCrtcId, 0);
			continue;
		}
		usedCrtcs |= 1 << output.crtcIndex;
		uint32 crtc = card.resources.crtcs[output.crtcIndex];
		uint32 plane = card.crtcPrimaryPlane[output.crtcIndex];
		status = Kms::CreateBlob(card.fd, &output.timing, sizeof(output.timing),
			blobs[output.crtcIndex]);
		if (status != B_OK)
			return status;
		request.Add(output.connector, card.connectorCrtcId, crtc);
		request.Add(crtc, card.crtcModeId, blobs[output.crtcIndex]);
		request.Add(crtc, card.crtcActive,
			gInfo->dpmsMode == B_DPMS_ON ? 1 : 0);
		request.Add(plane, card.planeFbId, buffer.framebuffer);
		request.Add(plane, card.planeCrtcId, crtc);
		uint32 width = min_c(output.width, layout.width - output.x);
		uint32 height = min_c(output.height, layout.height - output.y);
		request.Add(plane, card.planeSrcX, (uint64)output.x << 16);
		request.Add(plane, card.planeSrcY, (uint64)output.y << 16);
		request.Add(plane, card.planeSrcW, (uint64)width << 16);
		request.Add(plane, card.planeSrcH, (uint64)height << 16);
		uint32 x, y, w, h;
		destination(output, x, y, w, h);
		request.Add(plane, card.planeCrtcX, x);
		request.Add(plane, card.planeCrtcY, y);
		request.Add(plane, card.planeCrtcW, w);
		request.Add(plane, card.planeCrtcH, h);
	}
	// The cursors go, and come back when the monitors are on (update_
	// cursor()): a cursor left on a CRTC without its primary plane, or over
	// a plane scaled differently, would need an overlay cursor, which DCE
	// display engines do not have (amdgpu_dm refuses the commit).
	for (uint32 c = 0; c < card.resources.crtcCount; c++) {
		if (card.crtcCursorPlane[c] != 0) {
			request.Add(card.crtcCursorPlane[c], card.planeFbId, 0);
			request.Add(card.crtcCursorPlane[c], card.planeCrtcId, 0);
		}
	}
	for (uint32 c = 0; c < card.resources.crtcCount; c++) {
		if ((usedCrtcs & (1 << c)) != 0)
			continue;
		uint32 crtc = card.resources.crtcs[c];
		request.Add(crtc, card.crtcActive, 0);
		request.Add(crtc, card.crtcModeId, 0);
		if (card.crtcPrimaryPlane[c] != 0) {
			request.Add(card.crtcPrimaryPlane[c], card.planeFbId, 0);
			request.Add(card.crtcPrimaryPlane[c], card.planeCrtcId, 0);
		}
	}
	return B_OK;
}


/*!	Drives the monitors as \a layout says, in one atomic commit per card,
	each card on its current frame buffer when it is large enough and on a
	new one otherwise; with \a testOnly, only asks whether that would work.
	Assigns the outputs' CRTCs. Every card's frame buffer is the size of the
	whole layout, its monitors show their regions of it.
*/
status_t
apply_layout(Layout& layout, bool testOnly)
{
	if (layout.width == 0 || layout.height == 0)
		return B_BAD_VALUE;
	for (uint32 c = 0; c < gInfo->cardCount; c++) {
		const Kms::Resources& resources = gInfo->cards[c].resources;
		if (card_in_layout(layout, c)
			&& ((resources.maxWidth != 0 && layout.width > resources.maxWidth)
				|| (resources.maxHeight != 0
					&& layout.height > resources.maxHeight)))
			return B_BAD_VALUE;
	}

	// CRTCs: a monitor keeps the one it has (moving it is a mode set on a
	// monitor that did not change), the others get free ones of their card
	uint32 usedCrtcs[kMaxCards] = {};
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		Output& output = layout.outputs[i];
		const Output& current = gInfo->outputs[i];
		const Card& card = card_of(output);
		int32 c = current.crtcIndex;
		output.crtcIndex = -1;
		if (output.enabled && current.id == output.id
			&& current.enabled && c >= 0
			&& (output.possibleCrtcs & (1 << c)) != 0
			&& (usedCrtcs[output.card] & (1 << c)) == 0
			&& card.crtcPrimaryPlane[c] != 0) {
			output.crtcIndex = c;
			usedCrtcs[output.card] |= 1 << c;
		}
	}
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		Output& output = layout.outputs[i];
		if (!output.enabled || output.crtcIndex >= 0)
			continue;
		const Card& card = card_of(output);
		for (uint32 c = 0; c < card.resources.crtcCount; c++) {
			if ((output.possibleCrtcs & (1 << c)) != 0
				&& (usedCrtcs[output.card] & (1 << c)) == 0
				&& card.crtcPrimaryPlane[c] != 0) {
				output.crtcIndex = c;
				usedCrtcs[output.card] |= 1 << c;
				break;
			}
		}
		if (output.crtcIndex < 0)
			return B_NOT_SUPPORTED;
	}

	// The frame buffers: the current one when it is large enough, else a new
	// one with a little room to spare. With 4K monitors one is tens of
	// megabytes, and an APU has little VRAM to pin it in (512 MB on the
	// Framework 13's 760M, shared with everything else the GPU does):
	// pinning a second 9935x2160 buffer next to the current 9936x2160 one
	// failed with ENOMEM when the scale went from 200 to 175%.
	Kms::Buffer buffers[kMaxCards];
	bool reused[kMaxCards] = {};
	bool used[kMaxCards] = {};
	uint32 blobs[kMaxCards][Kms::kMaxObjects];
	memset(blobs, 0, sizeof(blobs));
	status_t status = B_OK;
	for (uint32 c = 0; c < gInfo->cardCount && status == B_OK; c++) {
		Card& card = gInfo->cards[c];
		used[c] = card_in_layout(layout, c);
		memset(&buffers[c], 0, sizeof(Kms::Buffer));
		buffers[c].area = -1;
		if (!used[c]) {
			// its monitors go dark; the buffer is freed once that is done
			continue;
		}
		reused[c] = buffer_fits(card.frameBuffer, layout);
		if (reused[c])
			buffers[c] = card.frameBuffer;
		else {
			status = Kms::CreateBuffer(card.fd, round_up(layout.width, 64),
				round_up(layout.height, 16), DRM_FORMAT_XRGB8888, buffers[c]);
			if (status != B_OK) {
				TRACE("no %" B_PRIu32 "x%" B_PRIu32 " frame buffer on card %"
					B_PRIu32 ": %s\n", layout.width, layout.height, c,
					strerror(status));
			}
		}
	}

	// every card is asked first; only when all of them can, they are set
	// (on the heap: one is several KB; no libstdc++ in an accelerant)
	Kms::AtomicRequest* requests = (Kms::AtomicRequest*)malloc(
		sizeof(Kms::AtomicRequest) * gInfo->cardCount);
	if (requests == NULL)
		status = B_NO_MEMORY;
	for (uint32 c = 0; requests != NULL && c < gInfo->cardCount; c++)
		new(&requests[c]) Kms::AtomicRequest;
	for (uint32 c = 0; c < gInfo->cardCount && status == B_OK; c++) {
		status = add_card_layout(requests[c], c, layout, buffers[c],
			blobs[c]);
		if (status == B_OK) {
			status = requests[c].Commit(gInfo->cards[c].fd,
				DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_TEST_ONLY);
		}
		if (status != B_OK && gInfo->cardCount > 1)
			TRACE("card %" B_PRIu32 " cannot have the layout\n", c);
	}
	for (uint32 c = 0; c < gInfo->cardCount && status == B_OK && !testOnly;
			c++) {
		status = requests[c].Commit(gInfo->cards[c].fd,
			DRM_MODE_ATOMIC_ALLOW_MODESET);
		if (status != B_OK && c > 0) {
			// the cards before it show the new layout already: what is left
			// is this card's monitors staying as they were
			TRACE("card %" B_PRIu32 " refused the layout it accepted in the "
				"test: %s\n", c, strerror(status));
		}
	}
	free(requests);
	if (status != B_OK || testOnly) {
		if (status != B_OK) {
			TRACE("%s %" B_PRIu32 "x%" B_PRIu32 " layout: %s\n",
				testOnly ? "cannot have the" : "setting the", layout.width,
				layout.height, strerror(status));
		}
		for (uint32 c = 0; c < gInfo->cardCount; c++) {
			for (uint32 b = 0; b < Kms::kMaxObjects; b++)
				Kms::DestroyBlob(gInfo->cards[c].fd, blobs[c][b]);
			if (used[c] && !reused[c])
				Kms::DestroyBuffer(gInfo->cards[c].fd, buffers[c]);
		}
		return status;
	}

	// it is on screen
	int fd = gInfo->device;
	const Kms::Buffer& buffer = buffers[0];
	// Renderers that copy their frames straight into the screen (Summit)
	// take it from the driver. It is offered again with every layout, also
	// when the buffer stayed the same, so that one refused offer does not
	// leave them without it until the next resize (haiku#43); a refusal is
	// worth a line in the syslog, as nothing else shows it. An older driver
	// does not know the call (B_DEV_INVALID_IOCTL / ENOTTY). With more cards,
	// such a renderer would draw on the first one only.
	if (gInfo->cardCount == 1) {
		status_t published = Kms::PublishScanout(fd, buffer,
			DRM_FORMAT_XRGB8888);
		if (published != B_OK && published != B_DEV_INVALID_IOCTL
			&& published != ENOTTY) {
			syslog(LOG_WARNING, "amdgpu_kms: the %" B_PRIu32 "x%" B_PRIu32
				" frame buffer (pitch %" B_PRIu32 ") was not offered to "
				"renderers: %s\n", buffer.width, buffer.height, buffer.pitch,
				strerror(published));
		}
	}
	// no copy (by the GPU or to the other cards) runs into the old buffers
	// meanwhile
	lock_device_copies();
	for (uint32 c = 0; c < gInfo->cardCount; c++) {
		Card& card = gInfo->cards[c];
		if (!reused[c] && (used[c] || card.frameBuffer.handle != 0)) {
			Kms::DestroyBuffer(card.fd, card.frameBuffer);
			card.frameBuffer = buffers[c];
			if (c == 0)
				gInfo->frameBufferGeneration++;
		}
		for (uint32 b = 0; b < Kms::kMaxObjects; b++) {
			Kms::DestroyBlob(card.fd, card.modeBlobs[b]);
			card.modeBlobs[b] = blobs[c][b];
		}
	}
	if (gInfo->cardCount > 1) {
		// the other cards show what the first one has until app_server
		// draws everything again
		multi_card_fill_from_first();
	}
	unlock_device_copies();
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		Output& output = gInfo->outputs[i];
		const Output& applied = layout.outputs[i];
		output.enabled = applied.enabled;
		output.mirror = applied.mirror;
		output.x = applied.x;
		output.y = applied.y;
		output.width = applied.width;
		output.height = applied.height;
		output.scale = applied.scale;
		output.renderScale = applied.renderScale;
		output.timing = applied.timing;
		output.crtcIndex = applied.crtcIndex;
		if (output.enabled) {
			TRACE("%s: %ux%u@%u, region %" B_PRId32 ",%" B_PRId32 " %" B_PRIu32
				"x%" B_PRIu32 "%s, CRTC %" B_PRId32 "\n", output.name,
				output.timing.hdisplay, output.timing.vdisplay,
				output.timing.vrefresh, output.x, output.y, output.width,
				output.height, output.mirror ? " (mirror)" : "",
				output.crtcIndex);
		}
	}
	gInfo->mode = layout_mode(layout);
	gInfo->modeSet = true;
	gInfo->shared->frameBufferArea = buffer.area;
	gInfo->shared->frameBufferOffset = 0;
	gInfo->shared->mode = gInfo->mode;
	gInfo->shared->bytesPerRow = buffer.pitch;
	gInfo->cursorSet = false;
	update_cursor();
	return B_OK;
}


static void
add_mode(display_mode* list, uint32& count, uint32 max,
	const display_mode& mode)
{
	for (uint32 i = 0; i < count; i++) {
		if (list[i].virtual_width == mode.virtual_width
			&& list[i].virtual_height == mode.virtual_height
			&& list[i].timing.pixel_clock == mode.timing.pixel_clock)
			return;
	}
	if (count < max)
		list[count++] = mode;
}


status_t
create_mode_list(void)
{
	const uint32 max = Kms::kMaxModes + 2;
	display_mode* list = (display_mode*)calloc(max, sizeof(display_mode));
	if (list == NULL)
		return B_NO_MEMORY;
	uint32 count = 0;

	// the layout first: what app_server should keep
	Layout layout;
	if (gInfo->hasPending)
		add_mode(list, count, max, layout_mode(gInfo->pending));
	if (gInfo->modeSet)
		add_mode(list, count, max, gInfo->mode);
	default_layout(layout);
	add_mode(list, count, max, layout_mode(layout));

	// and the first monitor's modes, for one screen
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		const Output& output = gInfo->outputs[i];
		if (!output.connected)
			continue;
		for (uint32 m = 0; m < output.modeCount; m++) {
			if ((output.modes[m].flags & DRM_MODE_FLAG_INTERLACE) != 0)
				continue;
			display_mode mode;
			memset(&mode, 0, sizeof(mode));
			mode_to_timing(output.modes[m], mode.timing);
			mode.space = B_RGB32;
			mode.virtual_width = output.modes[m].hdisplay;
			mode.virtual_height = output.modes[m].vdisplay;
			add_mode(list, count, max, mode);
		}
		break;
	}
	free(gInfo->modeList);
	gInfo->modeList = list;
	gInfo->modeCount = count;
	return B_OK;
}


/*!	A layout for a mode that is not one: the first monitor in it, every
	other one mirroring it at its own preferred mode. */
static status_t
single_mode_layout(const display_mode& mode, Layout& layout)
{
	default_layout(layout);
	bool primaryDone = false;
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		Output& output = layout.outputs[i];
		if (!output.enabled)
			continue;
		output.x = output.y = 0;
		output.width = mode.virtual_width;
		output.height = mode.virtual_height;
		if (!primaryDone) {
			// the monitor's own mode of that size, else the timing asked for
			int32 best = find_output_mode(output, mode.timing);
			if (best >= 0)
				output.timing = output.modes[best];
			else
				timing_to_mode(mode.timing, output.timing);
			output.mirror = false;
			primaryDone = true;
		} else
			output.mirror = true;
	}
	layout.width = mode.virtual_width;
	layout.height = mode.virtual_height;
	return primaryDone ? B_OK : B_ENTRY_NOT_FOUND;
}


extern "C" uint32
amdgpu_accelerant_mode_count(void)
{
	return gInfo->modeCount;
}


extern "C" status_t
amdgpu_get_mode_list(display_mode* list)
{
	memcpy(list, gInfo->modeList, gInfo->modeCount * sizeof(display_mode));
	return B_OK;
}


extern "C" status_t
amdgpu_propose_display_mode(display_mode* target, const display_mode* low,
	const display_mode* high)
{
	if (target->virtual_width < 320 || target->virtual_height < 200)
		return B_BAD_VALUE;
	const Kms::Resources& resources = gInfo->cards[0].resources;
	if ((resources.maxWidth != 0 && target->virtual_width > resources.maxWidth)
		|| (resources.maxHeight != 0
			&& target->virtual_height > resources.maxHeight))
		return B_BAD_VALUE;
	return B_OK;
}


extern "C" status_t
amdgpu_set_display_mode(display_mode* mode)
{
	if (gInfo->isClone)
		return B_NOT_ALLOWED;
	if (mode->space != B_RGB32 && mode->space != B_RGBA32)
		return B_BAD_VALUE;

	Layout layout;
	status_t status;
	if (gInfo->hasPending && is_layout_mode(gInfo->pending, *mode)) {
		status = apply_layout(gInfo->pending, false);
		// applied or not, it is done with: a layout that failed is not the
		// one to report as preferred afterwards
		gInfo->hasPending = false;
	} else {
		default_layout(layout);
		if (!is_layout_mode(layout, *mode)) {
			status = single_mode_layout(*mode, layout);
			if (status != B_OK)
				return status;
		}
		status = apply_layout(layout, false);
	}
	if (status == B_OK)
		create_mode_list();
	return status;
}


extern "C" status_t
amdgpu_get_display_mode(display_mode* mode)
{
	if (gInfo->modeSet || gInfo->isClone) {
		*mode = gInfo->isClone ? gInfo->shared->mode : gInfo->mode;
		return B_OK;
	}
	return amdgpu_get_preferred_display_mode(mode);
}


extern "C" status_t
amdgpu_get_preferred_display_mode(display_mode* mode)
{
	if (gInfo->hasPending) {
		*mode = layout_mode(gInfo->pending);
		return B_OK;
	}
	if (gInfo->modeSet) {
		*mode = gInfo->mode;
		return B_OK;
	}
	Layout layout;
	default_layout(layout);
	if (layout.width == 0)
		return B_ERROR;
	*mode = layout_mode(layout);
	return B_OK;
}


extern "C" status_t
amdgpu_get_edid_info(void* info, size_t size, uint32* _version)
{
	if (size < sizeof(edid1_info))
		return B_BUFFER_OVERFLOW;
	const Output* primary = primary_output();
	if (primary == NULL) {
		for (uint32 i = 0; i < gInfo->outputCount; i++) {
			if (gInfo->outputs[i].edidLength >= sizeof(edid1_raw)) {
				primary = &gInfo->outputs[i];
				break;
			}
		}
	}
	if (primary == NULL || primary->edidLength < sizeof(edid1_raw))
		return B_ERROR;
	edid_decode((edid1_info*)info, (const edid1_raw*)primary->edid);
	*_version = EDID_VERSION_1;
	return B_OK;
}


extern "C" status_t
amdgpu_get_frame_buffer_config(frame_buffer_config* config)
{
	if (gInfo->isClone) {
		config->frame_buffer = gInfo->cloneFrameBuffer;
		config->frame_buffer_dma = NULL;
		config->bytes_per_row = gInfo->shared->bytesPerRow;
		return gInfo->cloneFrameBuffer != NULL ? B_OK : B_ERROR;
	}
	const Kms::Buffer& frameBuffer = gInfo->cards[0].frameBuffer;
	if (frameBuffer.address == NULL)
		return B_ERROR;
	config->frame_buffer = frameBuffer.address;
	config->frame_buffer_dma = NULL;
	config->bytes_per_row = frameBuffer.pitch;
	return B_OK;
}


extern "C" status_t
amdgpu_get_pixel_clock_limits(display_mode* mode, uint32* _low, uint32* _high)
{
	*_low = kMinPixelClock;
	*_high = kMaxPixelClock;
	return B_OK;
}


extern "C" uint32
amdgpu_dpms_capabilities(void)
{
	return B_DPMS_ON | B_DPMS_STAND_BY | B_DPMS_SUSPEND | B_DPMS_OFF;
}


extern "C" uint32
amdgpu_dpms_mode(void)
{
	return gInfo->dpmsMode;
}


/*!	The built-in panel's backlight, from 0 to 1. All the way down it still
	glows a little: a dark panel with nothing to tell why would be worse. */
extern "C" status_t
amdgpu_set_brightness(float brightness)
{
	if (gInfo->isClone)
		return B_NOT_ALLOWED;

	drm_haiku_backlight request = {};
	request.brightness = -1;
	status_t status = Kms::Ioctl(gInfo->device, DRM_HAIKU_IOCTL_BACKLIGHT,
		&request);
	if (status != B_OK)
		return status;

	int32 maximum = request.max_brightness;
	int32 level = (int32)(brightness * maximum + 0.5f);
	request.brightness = max_c(max_c(level, maximum / 100), 1);
	request.brightness = min_c(request.brightness, maximum);
	return Kms::Ioctl(gInfo->device, DRM_HAIKU_IOCTL_BACKLIGHT, &request);
}


extern "C" status_t
amdgpu_get_brightness(float* brightness)
{
	drm_haiku_backlight request = {};
	request.brightness = -1;
	status_t status = Kms::Ioctl(gInfo->device, DRM_HAIKU_IOCTL_BACKLIGHT,
		&request);
	if (status != B_OK)
		return status;
	if (request.max_brightness == 0)
		return B_ERROR;

	*brightness = (float)request.brightness / request.max_brightness;
	return B_OK;
}


/*!	Whether a monitor came, went or was exchanged for another since the
	outputs were last probed. */
bool
connected_outputs_changed(void)
{
	Kms::Connector* connector = (Kms::Connector*)malloc(sizeof(Kms::Connector));
	if (connector == NULL)
		return true;
	bool changed = false;

	// an MST hub added or removed connectors
	Kms::Resources* resources
		= (Kms::Resources*)malloc(sizeof(Kms::Resources));
	uint32 total = 0;
	for (uint32 c = 0; resources != NULL && c < gInfo->cardCount && !changed;
			c++) {
		if (Kms::GetResources(gInfo->cards[c].fd, *resources) != B_OK)
			continue;
		for (uint32 i = 0; i < resources->connectorCount && !changed; i++) {
			if (total >= kMaxOutputs)
				break;
			const Output& output = gInfo->outputs[total++];
			if (total > gInfo->outputCount || output.card != c
				|| resources->connectors[i] != output.connector)
				changed = true;
		}
	}
	if (resources != NULL && total != gInfo->outputCount)
		changed = true;
	free(resources);

	for (uint32 i = 0; i < gInfo->outputCount && !changed; i++) {
		const Output& output = gInfo->outputs[i];
		if (Kms::GetConnector(card_of(output).fd, output.connector, *connector)
				!= B_OK) {
			changed = true;
			break;
		}
		bool connected = connector->connected && connector->modeCount > 0;
		changed = connected != output.connected
			|| (connected && (connector->edidLength != output.edidLength
				|| memcmp(connector->edid, output.edid, output.edidLength)
					!= 0));
	}
	free(connector);
	return changed;
}


extern "C" status_t
amdgpu_set_dpms_mode(uint32 dpmsMode)
{
	if (gInfo->isClone)
		return B_NOT_ALLOWED;
	uint32 active = dpmsMode == B_DPMS_ON ? 1 : 0;
	Kms::AtomicRequest requests[kMaxCards];
	uint32 cardCrtcs[kMaxCards] = {};
	uint32 crtcs = 0;
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		const Output& output = gInfo->outputs[i];
		if (output.enabled && output.crtcIndex >= 0) {
			const Card& card = card_of(output);
			requests[output.card].Add(card.resources.crtcs[output.crtcIndex],
				card.crtcActive, active);
			cardCrtcs[output.card]++;
			crtcs++;
		}
	}
	// Both ends of a switch go to the syslog (time stamped): displays that
	// would not wake (haiku#40) leave a request without its result there
	// when the commit never comes back.
	debug_printf("amdgpu_kms: displays %s (%" B_PRIu32 " CRTCs)...\n",
		active ? "on" : "off", crtcs);
	bigtime_t started = system_time();
	status_t status = B_OK;
	for (uint32 c = 0; c < gInfo->cardCount; c++) {
		if (cardCrtcs[c] == 0 && c > 0)
			continue;
		status_t cardStatus = requests[c].Commit(gInfo->cards[c].fd,
			DRM_MODE_ATOMIC_ALLOW_MODESET);
		if (cardStatus != B_OK)
			status = cardStatus;
	}
	debug_printf("amdgpu_kms: displays %s: %s in %.1f ms\n",
		active ? "on" : "off", status == B_OK ? "done" : strerror(status),
		(system_time() - started) / 1000.0);
	if (status == B_OK) {
		gInfo->dpmsMode = dpmsMode;
		if (active) {
			// Set the cursor again, not only move it: one shown while the
			// display was off (the screen saver quits on input before the
			// display is switched on) was set to nothing then.
			gInfo->cursorSet = false;
			update_cursor();
		}
	}
	if (status == B_OK && active && gInfo->changedAsleep) {
		// Only a monitor that is really gone, or new, changes the layout;
		// one that dropped its hot plug line while asleep and is back has
		// had its link trained again by being switched on.
		gInfo->changedAsleep = false;
		if (connected_outputs_changed()) {
			debug_printf("amdgpu_kms: monitors changed while asleep\n");
			gInfo->outputsChanged = true;
			write_port_etc(gInfo->changePort, gInfo->changeCode, NULL, 0,
				B_RELATIVE_TIMEOUT, 0);
		} else
			debug_printf("amdgpu_kms: the same monitors after waking\n");
	}
	return status;
}
