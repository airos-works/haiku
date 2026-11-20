/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	One desktop over several GPUs (see accelerant.h): app_server draws into
	its back buffer and flushes through a frame buffer source, which is host
	memory here; copying a source to the frame buffer copies what was drawn
	into the frame buffer of every card whose monitors show it, the first
	card's (app_server's own) always. The CPU copies, into each card's
	scanout buffer through its PCI aperture (write-combined).
*/


#include "accelerant.h"

#include <stdlib.h>
#include <string.h>


#define TRACE(x...) debug_printf("amdgpu_kms: " x)


static const uint32 kMaxSources = 4;


struct HostSource {
	uint32		id;
	area_id		area;
	uint8*		bits;
	uint32		bytesPerRow;
	uint16		width;
	uint16		height;
};


static HostSource sSources[kMaxSources];
static uint32 sNextId = 0x10000;
	// apart from the device copies' ids


/*!	Whether the flushes come here: more than one card. */
bool
multi_card_copies(void)
{
	return gInfo != NULL && !gInfo->isClone && gInfo->cardCount > 1;
}


static HostSource*
find_source(uint32 id)
{
	for (uint32 i = 0; i < kMaxSources; i++) {
		if (sSources[i].id == id && id != 0)
			return &sSources[i];
	}
	return NULL;
}


status_t
multi_card_create_source(uint16 width, uint16 height, color_space space,
	frame_buffer_source* source)
{
	if (space != B_RGB32 && space != B_RGBA32)
		return B_BAD_VALUE;
	if (width == 0 || height == 0)
		return B_BAD_VALUE;
	lock_device_copies();
	HostSource* entry = NULL;
	for (uint32 i = 0; entry == NULL && i < kMaxSources; i++) {
		if (sSources[i].id == 0)
			entry = &sSources[i];
	}
	if (entry == NULL) {
		unlock_device_copies();
		return B_NO_MEMORY;
	}
	uint32 bytesPerRow = ((uint32)width * 4 + 63) & ~63u;
	size_t size = ((size_t)bytesPerRow * height + B_PAGE_SIZE - 1)
		& ~(size_t)(B_PAGE_SIZE - 1);
	void* bits;
	area_id area = create_area("amdgpu_kms flush source", &bits,
		B_ANY_ADDRESS, size, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (area < 0) {
		unlock_device_copies();
		return area;
	}
	entry->id = sNextId++;
	entry->area = area;
	entry->bits = (uint8*)bits;
	entry->bytesPerRow = bytesPerRow;
	entry->width = width;
	entry->height = height;
	unlock_device_copies();

	source->id = entry->id;
	source->bits = bits;
	source->bytes_per_row = bytesPerRow;
	source->width = width;
	source->height = height;
	source->space = space;
	TRACE("%ux%u flush source in host memory for %" B_PRIu32 " cards\n",
		width, height, gInfo->cardCount);
	return B_OK;
}


void
multi_card_delete_source(uint32 id)
{
	lock_device_copies();
	HostSource* source = find_source(id);
	if (source != NULL) {
		delete_area(source->area);
		memset(source, 0, sizeof(HostSource));
	}
	unlock_device_copies();
}


/*!	Copies the rectangle [left, right) x [top, bottom) from \a from into
	\a to, at the same place. */
static void
copy_rect(const uint8* from, uint32 fromBytesPerRow, uint8* to,
	uint32 toBytesPerRow, int32 left, int32 top, int32 right, int32 bottom)
{
	size_t length = (size_t)(right - left) * 4;
	const uint8* source = from + (size_t)top * fromBytesPerRow + left * 4;
	uint8* target = to + (size_t)top * toBytesPerRow + left * 4;
	for (int32 y = top; y < bottom; y++) {
		memcpy(target, source, length);
		source += fromBytesPerRow;
		target += toBytesPerRow;
	}
}


/*!	Copies the rectangle (exclusive right and bottom) into the frame buffer
	of \a card, as far as its monitors show it: the first card's frame buffer
	is the screen app_server knows, it gets all of it. */
static void
copy_to_card(uint32 cardIndex, const uint8* bits, uint32 bytesPerRow,
	uint32 width, uint32 height, int32 left, int32 top, int32 right,
	int32 bottom)
{
	const Kms::Buffer& buffer = gInfo->cards[cardIndex].frameBuffer;
	if (buffer.address == NULL)
		return;
	right = min_c(right, (int32)min_c(width, buffer.width));
	bottom = min_c(bottom, (int32)min_c(height, buffer.height));
	if (cardIndex == 0) {
		if (left < right && top < bottom) {
			copy_rect(bits, bytesPerRow, (uint8*)buffer.address, buffer.pitch,
				left, top, right, bottom);
		}
		return;
	}
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		const Output& output = gInfo->outputs[i];
		if (!output.enabled || output.card != cardIndex)
			continue;
		int32 l = max_c(left, output.x);
		int32 t = max_c(top, output.y);
		int32 r = min_c(right, (int32)(output.x + output.width));
		int32 b = min_c(bottom, (int32)(output.y + output.height));
		if (l < r && t < b) {
			copy_rect(bits, bytesPerRow, (uint8*)buffer.address, buffer.pitch,
				l, t, r, b);
		}
	}
}


status_t
multi_card_copy(uint32 id, const fill_rect_params* list, uint32 count)
{
	lock_device_copies();
	HostSource* source = find_source(id);
	if (source == NULL) {
		unlock_device_copies();
		return B_BAD_VALUE;
	}
	for (uint32 i = 0; i < count; i++) {
		const fill_rect_params& rect = list[i];
		int32 left = max_c((int32)rect.left, 0);
		int32 top = max_c((int32)rect.top, 0);
		int32 right = min_c((int32)rect.right + 1, (int32)source->width);
		int32 bottom = min_c((int32)rect.bottom + 1, (int32)source->height);
		if (left >= right || top >= bottom)
			continue;
		for (uint32 c = 0; c < gInfo->cardCount; c++) {
			copy_to_card(c, source->bits, source->bytesPerRow, source->width,
				source->height, left, top, right, bottom);
		}
	}
	unlock_device_copies();
	return B_OK;
}


/*!	After a layout change, until app_server has drawn everything again: the
	other cards get what the first card's frame buffer has (read through its
	aperture, slowly, but only once). The device copies' lock is held. */
void
multi_card_fill_from_first(void)
{
	const Kms::Buffer& first = gInfo->cards[0].frameBuffer;
	if (first.address == NULL)
		return;
	for (uint32 c = 1; c < gInfo->cardCount; c++) {
		copy_to_card(c, (const uint8*)first.address, first.pitch, first.width,
			first.height, 0, 0, first.width, first.height);
	}
}
