/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "accelerant.h"

#include <string.h>


// The display engine's cursor: 128 x 128 premultiplied ARGB on every
// CRTC, positioned in each monitor's own pixels.
static const uint32 kCursorSize = 128;


/*!	Whether every monitor can show the cursor: amdgpu_dm wants an overlay
	cursor over a region that is scaled, which DCE display engines (Polaris
	and older) do not have. app_server draws the cursor itself when it is
	refused, and asks again when the layout changes. */
static status_t
try_cursor(void)
{
	if (gInfo->dpmsMode != B_DPMS_ON)
		return B_OK;
	status_t status = B_OK;
	for (uint32 i = 0; i < gInfo->outputCount && status == B_OK; i++) {
		const Output& output = gInfo->outputs[i];
		if (!output.enabled || output.crtcIndex < 0)
			continue;
		status = Kms::SetCursor(gInfo->device,
			gInfo->resources.crtcs[output.crtcIndex], gInfo->cursor.handle,
			kCursorSize, kCursorSize, gInfo->cursorHotX, gInfo->cursorHotY);
	}
	if (status != B_OK) {
		for (uint32 i = 0; i < gInfo->outputCount; i++) {
			const Output& output = gInfo->outputs[i];
			if (output.enabled && output.crtcIndex >= 0) {
				Kms::SetCursor(gInfo->device,
					gInfo->resources.crtcs[output.crtcIndex], 0, 0, 0, 0, 0);
			}
		}
	}
	return status;
}


extern "C" status_t
amdgpu_set_cursor_bitmap(uint16 width, uint16 height, uint16 hotX,
	uint16 hotY, color_space space, uint16 bytesPerRow, const uint8* data)
{
	if (gInfo->isClone)
		return B_NOT_ALLOWED;
	if (space != B_RGBA32 || width > kCursorSize || height > kCursorSize
		|| hotX >= width || hotY >= height)
		return B_BAD_VALUE;
	if (gInfo->cursor.handle == 0) {
		status_t status = Kms::CreateBuffer(gInfo->device, kCursorSize,
			kCursorSize, DRM_FORMAT_ARGB8888, gInfo->cursor, false);
		if (status != B_OK)
			return status;
	}
	// B_RGBA32 is ARGB8888 in memory; app_server premultiplies it
	uint8* bits = (uint8*)gInfo->cursor.address;
	for (uint32 y = 0; y < kCursorSize; y++) {
		uint32* row = (uint32*)(bits + y * gInfo->cursor.pitch);
		if (y < height) {
			memcpy(row, data + y * bytesPerRow, width * 4);
			memset(row + width, 0, (kCursorSize - width) * 4);
		} else
			memset(row, 0, kCursorSize * 4);
	}
	gInfo->cursorHotX = hotX;
	gInfo->cursorHotY = hotY;
	gInfo->cursorSet = false;
	status_t status = try_cursor();
	if ((status != B_OK) != gInfo->cursorRefused) {
		gInfo->cursorRefused = status != B_OK;
		debug_printf("amdgpu_kms: %s\n", status != B_OK
			? "no hardware cursor on these monitors, app_server draws it"
			: "hardware cursor");
	}
	if (status != B_OK) {
		gInfo->cursorSet = true;
		return status;
	}
	update_cursor();
	return B_OK;
}


/*!	Puts the cursor where app_server has it on every monitor, in that
	monitor's pixels: its region of the frame buffer may be scaled. */
void
update_cursor(void)
{
	if (gInfo == NULL || gInfo->isClone)
		return;
	bool show = gInfo->cursorVisible && gInfo->cursor.handle != 0
		&& gInfo->dpmsMode == B_DPMS_ON;
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		const Output& output = gInfo->outputs[i];
		if (!output.enabled || output.crtcIndex < 0)
			continue;
		uint32 crtc = gInfo->resources.crtcs[output.crtcIndex];
		if (!show) {
			if (!gInfo->cursorSet)
				Kms::SetCursor(gInfo->device, crtc, 0, 0, 0, 0, 0);
			continue;
		}
		if (!gInfo->cursorSet) {
			Kms::SetCursor(gInfo->device, crtc, gInfo->cursor.handle,
				kCursorSize, kCursorSize, gInfo->cursorHotX, gInfo->cursorHotY);
		}
		int32 x = gInfo->cursorX - output.x;
		int32 y = gInfo->cursorY - output.y;
		if (output.width != 0 && output.height != 0) {
			x = x * (int32)output.timing.hdisplay / (int32)output.width;
			y = y * (int32)output.timing.vdisplay / (int32)output.height;
		}
		Kms::MoveCursor(gInfo->device, crtc, x - gInfo->cursorHotX,
			y - gInfo->cursorHotY);
	}
	gInfo->cursorSet = true;
}


extern "C" void
amdgpu_move_cursor(uint16 x, uint16 y)
{
	gInfo->cursorX = x;
	gInfo->cursorY = y;
	update_cursor();
}


extern "C" void
amdgpu_show_cursor(bool visible)
{
	if (gInfo->cursorVisible != visible) {
		gInfo->cursorVisible = visible;
		gInfo->cursorSet = false;
	}
	update_cursor();
}
