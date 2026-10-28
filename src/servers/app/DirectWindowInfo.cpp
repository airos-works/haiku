/*
 * Copyright 2008-2009, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Stefano Ceccherini <stefano.ceccherini@gmail.com>
 *		Axel Dörfler, axeld@pinc-software.de
 */


#include "DirectWindowInfo.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <Autolock.h>
#include <kernel.h>

#include "RenderingBuffer.h"
#include "clipping.h"


DirectWindowInfo::DirectWindowInfo()
	:
	fBufferInfo(NULL),
	fSem(-1),
	fAcknowledgeSem(-1),
	fBufferArea(-1),
	fOriginalFeel(B_NORMAL_WINDOW_FEEL),
	fFullScreen(false)
{
	fBufferArea = create_area("direct area", (void**)&fBufferInfo,
		B_ANY_ADDRESS, DIRECT_BUFFER_INFO_AREA_SIZE,
		B_NO_LOCK, B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);

	memset(fBufferInfo, 0, DIRECT_BUFFER_INFO_AREA_SIZE);
	fBufferInfo->buffer_state = B_DIRECT_STOP;
	fBufferInfo->bits_area = -1;
	fBufferInfo->drawing_bits_area = -1;

	fSem = create_sem(0, "direct sem");
	fAcknowledgeSem = create_sem(0, "direct sem ack");
}


DirectWindowInfo::~DirectWindowInfo()
{
	// this should make the client die in case it's still running
	fBufferInfo->bits = NULL;
	fBufferInfo->bytes_per_row = 0;

	delete_area(fBufferArea);
	delete_sem(fSem);
	delete_sem(fAcknowledgeSem);
}


status_t
DirectWindowInfo::InitCheck() const
{
	if (fBufferArea < B_OK)
		return fBufferArea;
	if (fSem < B_OK)
		return fSem;
	if (fAcknowledgeSem < B_OK)
		return fAcknowledgeSem;

	return B_OK;
}


status_t
DirectWindowInfo::GetSyncData(direct_window_sync_data& data) const
{
	data.area = fBufferArea;
	data.disable_sem = fSem;
	data.disable_sem_ack = fAcknowledgeSem;

	return B_OK;
}


/*!	A rectangle in the window system's coordinates in frame buffer pixels,
	rounded the way the Painter rounds (DrawingEngine::_ScaleRegion()), so
	that what a direct window draws meets what app_server draws next to it.
*/
static clipping_rect
to_device_pixels(clipping_rect rect, float scale)
{
	rect.left = (int32)floorf(rect.left * scale);
	rect.top = (int32)floorf(rect.top * scale);
	rect.right = (int32)floorf((rect.right + 1) * scale) - 1;
	rect.bottom = (int32)floorf((rect.bottom + 1) * scale) - 1;
	return rect;
}


/*!	The area that holds \a buffer, made cloneable, if the buffer is an area
	of its own; -1 otherwise. A heap allocation is never handed out: the
	area around it would hold other things too.
*/
static area_id
area_of_buffer(RenderingBuffer* buffer)
{
	if (buffer == NULL || !IS_USER_ADDRESS(buffer->Bits()))
		return -1;

	area_id area = area_for(buffer->Bits());
	area_info info;
	if (area < 0 || get_area_info(area, &info) != B_OK
		|| info.address != buffer->Bits()
		|| info.size < (size_t)buffer->BytesPerRow() * buffer->Height())
		return -1;

	set_area_protection(area, B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
	return area;
}


status_t
DirectWindowInfo::SetState(direct_buffer_state bufferState,
	direct_driver_state driverState, RenderingBuffer* buffer,
	const BRect& windowFrame, const BRegion& clipRegion, uint16 deviceScale,
	RenderingBuffer* drawingBuffer)
{
	if ((fBufferInfo->buffer_state & B_DIRECT_MODE_MASK) == B_DIRECT_STOP
		&& (bufferState & B_DIRECT_MODE_MASK) != B_DIRECT_START)
		return B_OK;

	fBufferInfo->buffer_state = bufferState;

	if ((int)driverState != -1)
		fBufferInfo->driver_state = driverState;

	if ((bufferState & B_BUFFER_RESET) != 0 || fBufferInfo->bits_area < 0) {
		void* bits = buffer->Bits();
		if (IS_USER_ADDRESS(bits)) {
			fBufferInfo->bits = NULL;

			area_id area = area_for(bits);
			fBufferInfo->bits_area = area;

			// make sure the area is cloneable
			set_area_protection(area, B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
		} else {
			// framebuffer is in kernel address space
			// TODO: update all drivers and then drop this case!
			fBufferInfo->bits = bits;
		}
		fBufferInfo->bytes_per_row = buffer->BytesPerRow();

		switch (buffer->ColorSpace()) {
			case B_RGBA64:
			case B_RGBA64_BIG:
				fBufferInfo->bits_per_pixel = 64;
				break;
			case B_RGB48:
			case B_RGB48_BIG:
				fBufferInfo->bits_per_pixel = 48;
				break;
			case B_RGB32:
			case B_RGBA32:
			case B_RGB32_BIG:
			case B_RGBA32_BIG:
				fBufferInfo->bits_per_pixel = 32;
				break;
			case B_RGB24:
			case B_RGB24_BIG:
				fBufferInfo->bits_per_pixel = 24;
				break;
			case B_RGB16:
			case B_RGB16_BIG:
			case B_RGB15:
			case B_RGB15_BIG:
				fBufferInfo->bits_per_pixel = 16;
				break;
			case B_CMAP8:
			case B_GRAY8:
				fBufferInfo->bits_per_pixel = 8;
				break;
			default:
				syslog(LOG_ERR,
					"unknown colorspace in DirectWindowInfo::SetState()!\n");
				fBufferInfo->bits_per_pixel = 0;
				break;
		}

		fBufferInfo->pixel_format = buffer->ColorSpace();
		fBufferInfo->layout = B_BUFFER_NONINTERLEAVED;
		fBufferInfo->orientation = B_BUFFER_TOP_TO_BOTTOM;
			// TODO

		fBufferInfo->drawing_bits_area = area_of_buffer(drawingBuffer);
		fBufferInfo->drawing_bytes_per_row
			= fBufferInfo->drawing_bits_area >= 0
				? drawingBuffer->BytesPerRow() : 0;
	}

	// Only windows that draw in frame buffer pixels are told the density;
	// for the others the coordinates are their own and it stays 0.
	fBufferInfo->device_scale = deviceScale;
	float scale = deviceScale > 100 ? deviceScale / 100.0f : 1;

	if ((bufferState & B_DIRECT_MODE_MASK) != B_DIRECT_STOP) {
		fBufferInfo->window_bounds = to_clipping_rect(windowFrame);

		const int32 kMaxClipRectsCount = (DIRECT_BUFFER_INFO_AREA_SIZE
			- sizeof(direct_buffer_info)) / sizeof(clipping_rect);

		fBufferInfo->clip_list_count = min_c(clipRegion.CountRects(),
			kMaxClipRectsCount);
		fBufferInfo->clip_bounds = clipRegion.FrameInt();

		for (uint32 i = 0; i < fBufferInfo->clip_list_count; i++)
			fBufferInfo->clip_list[i] = clipRegion.RectAtInt(i);

		if (scale != 1) {
			fBufferInfo->window_bounds = to_device_pixels(
				fBufferInfo->window_bounds, scale);
			fBufferInfo->clip_bounds = to_device_pixels(
				fBufferInfo->clip_bounds, scale);
			for (uint32 i = 0; i < fBufferInfo->clip_list_count; i++) {
				fBufferInfo->clip_list[i] = to_device_pixels(
					fBufferInfo->clip_list[i], scale);
			}
		}
	}

	return _SynchronizeWithClient();
}


void
DirectWindowInfo::GetClipping(BRegion& region) const
{
	region.MakeEmpty();
	if (fBufferInfo == NULL
		|| (fBufferInfo->buffer_state & B_DIRECT_MODE_MASK) == B_DIRECT_STOP)
		return;
	for (uint32 i = 0; i < fBufferInfo->clip_list_count; i++)
		region.Include(fBufferInfo->clip_list[i]);
}


void
DirectWindowInfo::EnableFullScreen(const BRect& frame, window_feel feel)
{
	fOriginalFrame = frame;
	fOriginalFeel = feel;
	fFullScreen = true;
}


void
DirectWindowInfo::DisableFullScreen()
{
	fFullScreen = false;
}


status_t
DirectWindowInfo::_SynchronizeWithClient()
{
	// Releasing this semaphore causes the client to call
	// BDirectWindow::DirectConnected()
	status_t status = release_sem(fSem);
	if (status != B_OK)
		return status;

	// Wait with a timeout of half a second until the client exits
	// from its DirectConnected() implementation
	do {
		status = acquire_sem_etc(fAcknowledgeSem, 1, B_TIMEOUT, 500000);
	} while (status == B_INTERRUPTED);

	return status;
}
