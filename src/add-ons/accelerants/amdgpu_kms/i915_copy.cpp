/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Copies by an Intel GPU's blitter (the BCS engine) on i915_drm, for
	app_server's flush: what device_copy.cpp does with amdgpu's SDMA engine.

	B_CREATE_FRAME_BUFFER_SOURCE, B_COPY_TO_FRAME_BUFFER: app_server draws
	into a buffer object of system memory (shmem pages, mapped here cached:
	the GPU snoops them through the LLC, or they are made snooped), and the
	blitter copies the rectangles that changed into the frame buffer with
	XY_SRC_COPY_BLT, so the CPU does not write each frame into the
	uncached scanout memory itself. Small updates are still copied by the
	CPU, for which the round trip to the GPU takes longer than the copy.

	Everything goes through i915's own ioctls on the accelerant's file:
	GEM_CREATE, GEM_MMAP_OFFSET (the mapping itself through
	DRM_HAIKU_IOCTL_MAP), and GEM_EXECBUFFER2 on the blitter with
	relocations, so it works with any GTT layout. A copy is waited for
	before its caller goes on (GEM_WAIT on the batch).

	Shadows (B_CREATE_FRAME_BUFFER_SHADOW) are not offered: on an
	integrated GPU they would live in the same memory as the back buffer. */


#include "accelerant.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "i915_drm.h"


#define TRACE(x...) debug_printf("amdgpu_kms: i915: " x)


// XY_SRC_COPY_BLT for Gen 8 and later (64-bit addresses: 10 dwords)
static const uint32 kXySrcCopyBlt = (2u << 29) | (0x53u << 22) | (3u << 20)
	| (10 - 2);
static const uint32 kRopCopy = 0xccu << 16;
static const uint32 kDepth32 = 3u << 24;
static const uint32 kMiBatchBufferEnd = 0x0au << 23;
static const uint32 kBlitDwords = 10;

static const uint32 kMaxSources = 8;
static const uint32 kBatchSize = B_PAGE_SIZE * 4;
static const uint32 kCopiesPerBatch = 64;
	// two relocations each
static const uint32 kMaxExtent = 1 << 15;
	// the blitter's coordinates and pitch are 16 bits, signed
static const uint64 kCpuCopyPixels = 64 * 1024;
static const int64 kCopyTimeout = 1000000000LL;


struct GemBuffer {
	uint32	handle;
	uint64	size;
	void*	address;
	area_id	area;
};

struct Source {
	uint32		id;			// 0 when the slot is free
	GemBuffer	buffer;
	uint32		width;
	uint32		height;
	uint32		bytesPerRow;
};


static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static bool sInitialized;
static bool sAvailable;
static GemBuffer sBatch;
static Source sSources[kMaxSources];
static uint32 sNextSourceId = 1;


struct Locker {
	Locker() { pthread_mutex_lock(&sLock); }
	~Locker() { pthread_mutex_unlock(&sLock); }
};


static status_t
ioctl_i915(unsigned long request, void* argument)
{
	return Kms::Ioctl(gInfo->device, request, argument);
}


static void
destroy_buffer(GemBuffer& buffer)
{
	if (buffer.address != NULL) {
		drm_haiku_unmap unmap = {};
		unmap.address = (addr_t)buffer.address;
		ioctl_i915(DRM_HAIKU_IOCTL_UNMAP, &unmap);
	}
	if (buffer.handle != 0) {
		drm_gem_close close = {};
		close.handle = buffer.handle;
		ioctl_i915(DRM_IOCTL_GEM_CLOSE, &close);
	}
	memset(&buffer, 0, sizeof(buffer));
}


/*!	A buffer object of system memory, mapped cached for this team (and
	cloneable, as renderers import app_server's back buffer). */
static status_t
create_buffer(uint64 size, GemBuffer& buffer)
{
	memset(&buffer, 0, sizeof(buffer));
	size = (size + B_PAGE_SIZE - 1) & ~(uint64)(B_PAGE_SIZE - 1);

	drm_i915_gem_create create = {};
	create.size = size;
	status_t status = ioctl_i915(DRM_IOCTL_I915_GEM_CREATE, &create);
	if (status != B_OK)
		return status;
	buffer.handle = create.handle;
	buffer.size = create.size;

	// snooped by the GPU (on GPUs with an LLC it already is)
	drm_i915_gem_caching caching = {};
	caching.handle = buffer.handle;
	caching.caching = I915_CACHING_CACHED;
	ioctl_i915(DRM_IOCTL_I915_GEM_SET_CACHING, &caching);

	drm_i915_gem_mmap_offset offset = {};
	offset.handle = buffer.handle;
	offset.flags = I915_MMAP_OFFSET_WB;
	status = ioctl_i915(DRM_IOCTL_I915_GEM_MMAP_OFFSET, &offset);
	if (status != B_OK) {
		destroy_buffer(buffer);
		return status;
	}
	drm_haiku_map map = {};
	map.offset = offset.offset;
	map.size = buffer.size;
	map.flags = DRM_HAIKU_MAP_WRITE | DRM_HAIKU_MAP_CLONEABLE;
	status = ioctl_i915(DRM_HAIKU_IOCTL_MAP, &map);
	if (status != B_OK) {
		destroy_buffer(buffer);
		return status;
	}
	buffer.address = (void*)(addr_t)map.address;
	buffer.area = map.area;
	return B_OK;
}


//! sets up on the first use; the lock must be held
static bool
init_copies(void)
{
	if (sInitialized)
		return sAvailable;
	sInitialized = true;
	if (gInfo->isClone || !gInfo->cards[0].isI915)
		return false;
	if (getenv("I915_KMS_NO_BLITTER") != NULL)
		return false;

	int value = 0;
	drm_i915_getparam param = {};
	param.param = I915_PARAM_HAS_BLT;
	param.value = &value;
	if (ioctl_i915(DRM_IOCTL_I915_GETPARAM, &param) != B_OK || value == 0) {
		TRACE("no blitter, the CPU copies\n");
		return false;
	}
	value = 0;
	param.param = I915_PARAM_CHIPSET_ID;
	ioctl_i915(DRM_IOCTL_I915_GETPARAM, &param);
	int deviceId = value;

	if (create_buffer(kBatchSize, sBatch) != B_OK) {
		TRACE("no batch buffer, the CPU copies\n");
		return false;
	}
	sAvailable = true;
	TRACE("copies by the blitter (device %#x)\n", deviceId);
	return true;
}


static Source*
find_source(uint32 id)
{
	for (uint32 i = 0; i < kMaxSources; i++) {
		if (sSources[i].id == id && id != 0)
			return &sSources[i];
	}
	return NULL;
}


static bool
clip(const fill_rect_params& rect, uint32 width, uint32 height,
	uint32& left, uint32& top, uint32& right, uint32& bottom)
{
	left = rect.left;
	top = rect.top;
	right = min_c((uint32)rect.right + 1, width);
	bottom = min_c((uint32)rect.bottom + 1, height);
	return left < right && top < bottom;
}


static status_t
wait_for_batch(void)
{
	drm_i915_gem_wait wait = {};
	wait.bo_handle = sBatch.handle;
	wait.timeout_ns = kCopyTimeout;
	status_t status = ioctl_i915(DRM_IOCTL_I915_GEM_WAIT, &wait);
	if (status != B_OK)
		TRACE("waiting for a copy failed: %s\n", strerror(status));
	return status;
}


/*!	Copies the rectangles from the source to the same place in the frame
	buffer; the lock is held. */
static status_t
copy_rects(const Source& source, const fill_rect_params* list, uint32 count)
{
	const Kms::Buffer& target = gInfo->cards[0].frameBuffer;
	if (target.handle == 0)
		return B_NO_INIT;
	uint32 width = min_c(source.width, target.width);
	uint32 height = min_c(source.height, target.height);
	uint32 left, top, right, bottom;

	uint64 pixels = 0;
	for (uint32 i = 0; i < count; i++) {
		if (clip(list[i], width, height, left, top, right, bottom))
			pixels += (uint64)(right - left) * (bottom - top);
	}
	if (pixels == 0)
		return B_OK;
	if (pixels < kCpuCopyPixels && target.address != NULL) {
		for (; count > 0; list++, count--) {
			if (!clip(*list, width, height, left, top, right, bottom))
				continue;
			size_t rowBytes = (right - left) * 4;
			for (uint32 y = top; y < bottom; y++) {
				memcpy((uint8*)target.address + y * target.pitch + left * 4,
					(uint8*)source.buffer.address + y * source.bytesPerRow
						+ left * 4, rowBytes);
			}
		}
		return B_OK;
	}

	status_t status = B_OK;
	while (count > 0 && status == B_OK) {
		uint32* batch = (uint32*)sBatch.address;
		drm_i915_gem_relocation_entry relocations[kCopiesPerBatch * 2];
		uint32 dwords = 0;
		uint32 copies = 0;
		for (; count > 0 && copies < kCopiesPerBatch; list++, count--) {
			if (!clip(*list, width, height, left, top, right, bottom))
				continue;
			batch[dwords++] = kXySrcCopyBlt;
			batch[dwords++] = kRopCopy | kDepth32 | target.pitch;
			batch[dwords++] = top << 16 | left;
			batch[dwords++] = bottom << 16 | right;
			// destination address, relocated: buffer 1 (the frame buffer)
			drm_i915_gem_relocation_entry& to = relocations[copies * 2];
			memset(&to, 0, sizeof(to));
			to.target_handle = 1;
			to.offset = dwords * 4;
			to.read_domains = I915_GEM_DOMAIN_RENDER;
			to.write_domain = I915_GEM_DOMAIN_RENDER;
			batch[dwords++] = 0;
			batch[dwords++] = 0;
			batch[dwords++] = top << 16 | left;
			batch[dwords++] = source.bytesPerRow;
			// source address, relocated: buffer 0 (the source)
			drm_i915_gem_relocation_entry& from = relocations[copies * 2 + 1];
			memset(&from, 0, sizeof(from));
			from.target_handle = 0;
			from.offset = dwords * 4;
			from.read_domains = I915_GEM_DOMAIN_RENDER;
			batch[dwords++] = 0;
			batch[dwords++] = 0;
			copies++;
		}
		if (copies == 0)
			break;
		batch[dwords++] = kMiBatchBufferEnd;
		if (dwords % 2 != 0)
			batch[dwords++] = 0;

		drm_i915_gem_exec_object2 objects[3] = {};
		objects[0].handle = source.buffer.handle;
		objects[1].handle = target.handle;
		objects[1].flags = EXEC_OBJECT_WRITE;
		objects[2].handle = sBatch.handle;
		objects[2].relocation_count = copies * 2;
		objects[2].relocs_ptr = (uint64)(addr_t)relocations;

		drm_i915_gem_execbuffer2 execbuffer = {};
		execbuffer.buffers_ptr = (uint64)(addr_t)objects;
		execbuffer.buffer_count = 3;
		execbuffer.batch_len = dwords * 4;
		execbuffer.flags = I915_EXEC_BLT | I915_EXEC_HANDLE_LUT;
		status = ioctl_i915(DRM_IOCTL_I915_GEM_EXECBUFFER2, &execbuffer);
		if (status != B_OK) {
			TRACE("submitting a copy failed: %s\n", strerror(status));
			break;
		}
		// the batch is written again next: wait for this one
		status = wait_for_batch();
	}
	return status;
}


status_t
i915_create_frame_buffer_source(uint16 width, uint16 height,
	color_space space, frame_buffer_source* source)
{
	if (space != B_RGB32 && space != B_RGBA32)
		return B_BAD_VALUE;
	if (width == 0 || height == 0 || width >= kMaxExtent
		|| height >= kMaxExtent)
		return B_BAD_VALUE;

	Locker _;
	if (!init_copies())
		return B_NOT_SUPPORTED;
	Source* entry = NULL;
	for (uint32 i = 0; entry == NULL && i < kMaxSources; i++) {
		if (sSources[i].id == 0)
			entry = &sSources[i];
	}
	if (entry == NULL)
		return B_NO_MEMORY;

	// rows of whole 64 byte units (the blitter wants the pitch aligned)
	uint32 bytesPerRow = ((uint32)width * 4 + 63) & ~63u;
	if (bytesPerRow >= kMaxExtent)
		return B_BAD_VALUE;
	status_t status = create_buffer((uint64)bytesPerRow * height,
		entry->buffer);
	if (status != B_OK)
		return status;
	entry->id = sNextSourceId++;
	if (sNextSourceId == 0)
		sNextSourceId = 1;
	entry->width = width;
	entry->height = height;
	entry->bytesPerRow = bytesPerRow;

	source->id = entry->id;
	source->bits = entry->buffer.address;
	source->bytes_per_row = bytesPerRow;
	source->width = width;
	source->height = height;
	source->space = space;
	return B_OK;
}


void
i915_delete_frame_buffer_source(uint32 id)
{
	Locker _;
	Source* source = find_source(id);
	if (source == NULL)
		return;
	// closing the handle keeps the object until the GPU is done with it
	destroy_buffer(source->buffer);
	source->id = 0;
}


status_t
i915_copy_to_frame_buffer(uint32 id, const fill_rect_params* list,
	uint32 count)
{
	Locker _;
	Source* source = sAvailable ? find_source(id) : NULL;
	if (source == NULL)
		return sAvailable ? B_BAD_VALUE : B_NOT_SUPPORTED;
	status_t status = copy_rects(*source, list, count);
	if (status != B_OK)
		TRACE("copy to the frame buffer failed: %s\n", strerror(status));
	return status;
}


//! app_server is going away (the primary accelerant only)
void
uninit_i915_copies(void)
{
	Locker _;
	if (sAvailable)
		wait_for_batch();
	for (uint32 i = 0; i < kMaxSources; i++) {
		if (sSources[i].id != 0)
			destroy_buffer(sSources[i].buffer);
		sSources[i].id = 0;
	}
	destroy_buffer(sBatch);
	sAvailable = false;
	sInitialized = false;
}


//! held while the frame buffer is replaced: no copy runs then
void
lock_i915_copies(void)
{
	pthread_mutex_lock(&sLock);
}


void
unlock_i915_copies(void)
{
	pthread_mutex_unlock(&sLock);
}
