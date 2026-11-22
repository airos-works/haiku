/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "accelerant.h"

#include <stdlib.h>
#include <string.h>


// The display engine's cursor: premultiplied ARGB on every CRTC, positioned
// in each monitor's own pixels; 128 x 128 on amdgpu, 64 x 64 on nouveau's
// older display engines (gInfo->cursorSize, the cards' smallest).
#define kCursorSize (gInfo->cursorSize)


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
		Card& card = card_of(output);
		// a monitor shown upside down needs its cursor turned as well
		if (output_is_rotated(output)
			&& card.cursorRotation[output.crtcIndex] == 0) {
			status = B_UNSUPPORTED;
			break;
		}
		status = Kms::SetCursor(card.fd,
			card.resources.crtcs[output.crtcIndex], card.cursor.handle,
			kCursorSize, kCursorSize, gInfo->cursorHotX, gInfo->cursorHotY);
	}
	if (status != B_OK) {
		for (uint32 i = 0; i < gInfo->outputCount; i++) {
			const Output& output = gInfo->outputs[i];
			if (output.enabled && output.crtcIndex >= 0) {
				Card& card = card_of(output);
				Kms::SetCursor(card.fd,
					card.resources.crtcs[output.crtcIndex], 0, 0, 0, 0, 0);
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
	// a cursor buffer on every card
	for (uint32 c = 0; c < gInfo->cardCount; c++) {
		Card& card = gInfo->cards[c];
		if (card.cursor.handle != 0)
			continue;
		status_t status = Kms::CreateBuffer(card.fd, kCursorSize,
			kCursorSize, DRM_FORMAT_ARGB8888, card.cursor, false);
		if (status != B_OK)
			return status;
	}
	// B_RGBA32 is ARGB8888 in memory; app_server premultiplies it
	if (gInfo->cursorImage == NULL) {
		gInfo->cursorImage = (uint32*)malloc(kCursorSize * kCursorSize * 4);
		if (gInfo->cursorImage == NULL)
			return B_NO_MEMORY;
	}
	for (uint32 y = 0; y < kCursorSize; y++) {
		uint32* row = gInfo->cursorImage + y * kCursorSize;
		if (y < height) {
			memcpy(row, data + y * bytesPerRow, width * 4);
			memset(row + width, 0, (kCursorSize - width) * 4);
		} else
			memset(row, 0, kCursorSize * 4);
	}
	restore_cursor_image();
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


/*!	Writes the cursor image into the cursor's buffer (in video memory). */
void
restore_cursor_image(void)
{
	if (gInfo->cursorImage == NULL)
		return;
	for (uint32 c = 0; c < gInfo->cardCount; c++) {
		const Kms::Buffer& cursor = gInfo->cards[c].cursor;
		if (cursor.address == NULL)
			continue;
		uint8* bits = (uint8*)cursor.address;
		for (uint32 y = 0; y < kCursorSize; y++) {
			memcpy(bits + y * cursor.pitch,
				gInfo->cursorImage + y * kCursorSize, kCursorSize * 4);
		}
	}
}


/*!	Video memory is lost when amdgpu resets the GPU (after a hang): the
	frame buffer and the cursor have to be drawn again. Returns B_OK when that
	happened (app_server then redraws everything), B_TIMED_OUT when it did not
	within \a timeout. */
extern "C" status_t
amdgpu_wait_for_display_restore(bigtime_t timeout)
{
	if (gInfo->isClone)
		return B_NOT_ALLOWED;
	bigtime_t end = system_time() + timeout;
	do {
		uint32 lost;
		if (gInfo->isAmdgpu && Kms::VramLostCounter(gInfo->device, lost) == B_OK
			&& lost != gInfo->vramLost) {
			gInfo->vramLost = lost;
			debug_printf("amdgpu_kms: video memory was lost (GPU reset); "
				"the screen is drawn again\n");
			restore_cursor_image();
			return B_OK;
		}
		if (!gInfo->isAmdgpu
			&& Kms::DisplayLostCounter(gInfo->device, lost) == B_OK
			&& lost != gInfo->displayLost) {
			gInfo->displayLost = lost;
			debug_printf("amdgpu_kms: the display lost its contents (suspend "
				"to RAM); the screen is drawn again\n");
			restore_cursor_image();
			return B_OK;
		}
		snooze(100000);
	} while (system_time() < end);
	return B_TIMED_OUT;
}


/*!	Puts the cursor where app_server has it on every monitor, in that
	monitor's pixels: its region of the frame buffer may be scaled. */
void
update_cursor(void)
{
	if (gInfo == NULL || gInfo->isClone)
		return;
	bool show = gInfo->cursorVisible && gInfo->cards[0].cursor.handle != 0
		&& gInfo->dpmsMode == B_DPMS_ON;
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		const Output& output = gInfo->outputs[i];
		if (!output.enabled || output.crtcIndex < 0)
			continue;
		Card& card = card_of(output);
		uint32 crtc = card.resources.crtcs[output.crtcIndex];
		if (output_is_rotated(output)
			&& card.cursorRotation[output.crtcIndex] == 0) {
			// it cannot turn with the picture: app_server draws it
			Kms::SetCursor(card.fd, crtc, 0, 0, 0, 0, 0);
			continue;
		}
		if (!show || card.cursor.handle == 0) {
			if (!gInfo->cursorSet)
				Kms::SetCursor(card.fd, crtc, 0, 0, 0, 0, 0);
			continue;
		}
		if (!gInfo->cursorSet) {
			Kms::SetCursor(card.fd, crtc, card.cursor.handle,
				kCursorSize, kCursorSize, gInfo->cursorHotX, gInfo->cursorHotY);
		}
		int32 x = gInfo->cursorX - output.x;
		int32 y = gInfo->cursorY - output.y;
		if (output.width != 0 && output.height != 0) {
			x = x * (int32)output.timing.hdisplay / (int32)output.width;
			y = y * (int32)output.timing.vdisplay / (int32)output.height;
		}
		x -= gInfo->cursorHotX;
		y -= gInfo->cursorHotY;
		if (output_is_rotated(output)) {
			// The plane's picture is turned around its own middle: the
			// cursor's hot spot is as far from its bottom right corner now,
			// and the panel's pixels count from the other corner.
			x = (int32)output.timing.hdisplay - x - (int32)kCursorSize;
			y = (int32)output.timing.vdisplay - y - (int32)kCursorSize;
		}
		Kms::MoveCursor(card.fd, crtc, x, y);
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
