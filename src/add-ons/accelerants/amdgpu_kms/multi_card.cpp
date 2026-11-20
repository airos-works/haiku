/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	One desktop over several GPUs (see accelerant.h): app_server draws into
	its back buffer and flushes through a frame buffer source, which is host
	memory here; copying a source to the frame buffer copies what was drawn
	into the frame buffer of every card whose monitors show it, the first
	card's (app_server's own) always. On nouveau cards the GPU's 2D engine
	copies (nouveau_copy.cpp), from a buffer object of the source's pages;
	otherwise, and for small updates, the CPU copies, into each card's
	scanout buffer through its PCI aperture (write-combined). The file
	~/config/settings/amdgpu_kms_cpu_copies keeps the copies on the CPU.
*/


#include "accelerant.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>


#define TRACE(x...) debug_printf("amdgpu_kms: " x)


static const uint32 kMaxSources = 4;
static const uint32 kMaxRects = 256;
static const uint64 kCpuCopyPixels = 32 * 1024;
	// fewer changed pixels than this the CPU copies itself: the GPU's round
	// trip takes longer than writing them through the aperture


struct HostSource {
	uint32		id;
	area_id		area;
	uint8*		bits;
	size_t		size;
	uint32		bytesPerRow;
	uint16		width;
	uint16		height;
	// the source's buffer object on each card with a copier, or 0
	uint32		handles[kMaxCards];
	uint64		vas[kMaxCards];
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


/*!	The card's 2D engine, started the first time it is asked for. */
static NouveauCopier*
card_copier(Card& card)
{
	if (!card.copierTried) {
		card.copierTried = true;
		struct stat ignored;
		if (strcmp(card.driver, "nouveau") == 0 && stat(
				"/boot/home/config/settings/amdgpu_kms_cpu_copies", &ignored)
					!= 0)
			card.copier = nouveau_copy_init(card.fd);
	}
	return card.copier;
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
	entry->size = size;
	entry->bytesPerRow = bytesPerRow;
	entry->width = width;
	entry->height = height;
	uint32 gpuCards = 0;
	for (uint32 c = 0; c < gInfo->cardCount; c++) {
		NouveauCopier* copier = card_copier(gInfo->cards[c]);
		if (copier == NULL)
			continue;
		status_t status = nouveau_copy_import(copier, bits, size,
			entry->handles[c], entry->vas[c]);
		if (status == B_OK)
			gpuCards++;
		else {
			entry->handles[c] = 0;
			TRACE("card %" B_PRIu32 ": no buffer of the flush source: %s\n",
				c + 1, strerror(status));
		}
	}
	unlock_device_copies();

	source->id = entry->id;
	source->bits = bits;
	source->bytes_per_row = bytesPerRow;
	source->width = width;
	source->height = height;
	source->space = space;
	TRACE("%ux%u flush source in host memory for %" B_PRIu32 " cards, %"
		B_PRIu32 " of them copying with the GPU\n", width, height,
		gInfo->cardCount, gpuCards);
	return B_OK;
}


void
multi_card_delete_source(uint32 id)
{
	lock_device_copies();
	HostSource* source = find_source(id);
	if (source != NULL) {
		// the buffer objects first: they keep the pages wired, and freeing
		// the memory would wait for them
		for (uint32 c = 0; c < gInfo->cardCount; c++)
			nouveau_copy_release(gInfo->cards[c].copier, source->handles[c]);
		delete_area(source->area);
		memset(source, 0, sizeof(HostSource));
	}
	unlock_device_copies();
}


/*!	Copies the rectangle [left, right) x [top, bottom) from \a from into
	\a to, at the same place. */
static void
copy_pixels(const uint8* from, uint32 fromBytesPerRow, uint8* to,
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
			copy_pixels(bits, bytesPerRow, (uint8*)buffer.address, buffer.pitch,
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
			copy_pixels(bits, bytesPerRow, (uint8*)buffer.address, buffer.pitch,
				l, t, r, b);
		}
	}
}


/*!	The rectangles of \a rects that \a cardIndex's monitors show, clipped to
	them and to its frame buffer (all of each on the first card). */
static uint32
card_rects(uint32 cardIndex, const copy_rect* rects, uint32 count,
	copy_rect* clipped, uint64& _pixels)
{
	const Kms::Buffer& buffer = gInfo->cards[cardIndex].frameBuffer;
	uint32 clippedCount = 0;
	uint64 pixels = 0;
	for (uint32 i = 0; i < count && clippedCount < kMaxRects; i++) {
		copy_rect rect = rects[i];
		rect.right = min_c(rect.right, (int32)buffer.width);
		rect.bottom = min_c(rect.bottom, (int32)buffer.height);
		if (cardIndex == 0) {
			if (rect.left < rect.right && rect.top < rect.bottom) {
				clipped[clippedCount++] = rect;
				pixels += (uint64)(rect.right - rect.left)
					* (rect.bottom - rect.top);
			}
			continue;
		}
		for (uint32 o = 0; o < gInfo->outputCount
				&& clippedCount < kMaxRects; o++) {
			const Output& output = gInfo->outputs[o];
			if (!output.enabled || output.card != cardIndex)
				continue;
			copy_rect part;
			part.left = max_c(rect.left, output.x);
			part.top = max_c(rect.top, output.y);
			part.right = min_c(rect.right, (int32)(output.x + output.width));
			part.bottom = min_c(rect.bottom,
				(int32)(output.y + output.height));
			if (part.left < part.right && part.top < part.bottom) {
				clipped[clippedCount++] = part;
				pixels += (uint64)(part.right - part.left)
					* (part.bottom - part.top);
			}
		}
	}
	_pixels = pixels;
	return clippedCount;
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

	copy_rect rects[kMaxRects];
	copy_rect clipped[kMaxRects];
	uint32 done = 0;
	while (done < count) {
		uint32 rectCount = 0;
		for (; done < count && rectCount < kMaxRects; done++) {
			const fill_rect_params& rect = list[done];
			copy_rect& to = rects[rectCount];
			to.left = max_c((int32)rect.left, 0);
			to.top = max_c((int32)rect.top, 0);
			to.right = min_c((int32)rect.right + 1, (int32)source->width);
			to.bottom = min_c((int32)rect.bottom + 1, (int32)source->height);
			if (to.left < to.right && to.top < to.bottom)
				rectCount++;
		}

		for (uint32 c = 0; c < gInfo->cardCount; c++) {
			Card& card = gInfo->cards[c];
			if (card.frameBuffer.address == NULL)
				continue;
			uint64 pixels;
			uint32 clippedCount = card_rects(c, rects, rectCount, clipped,
				pixels);
			if (clippedCount == 0)
				continue;
			if (card.copier != NULL && source->handles[c] != 0
				&& pixels >= kCpuCopyPixels
				&& nouveau_copy_rects(card.copier, source->handles[c],
					source->vas[c], source->bytesPerRow, source->width,
					source->height, card.frameBuffer, clipped, clippedCount)
						== B_OK) {
				continue;
			}
			for (uint32 i = 0; i < clippedCount; i++) {
				copy_pixels(source->bits, source->bytesPerRow,
					(uint8*)card.frameBuffer.address, card.frameBuffer.pitch,
					clipped[i].left, clipped[i].top, clipped[i].right,
					clipped[i].bottom);
			}
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


/*!	B_FRAME_BUFFER_IS_PARTIAL: with more than one card, the first card's
	frame buffer is not what all the monitors show; direct windows are kept
	out of it (app_server stops their direct access). */
extern "C" bool
amdgpu_frame_buffer_is_partial(void)
{
	return multi_card_copies();
}
