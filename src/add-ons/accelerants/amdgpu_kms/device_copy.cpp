/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Copies to the screen by the GPU's SDMA engine, for app_server's flush
	(B_CREATE_FRAME_BUFFER_SOURCE, B_COPY_TO_FRAME_BUFFER): app_server keeps
	the screen's contents in buffers of system memory the GPU reads (GTT,
	cached for the CPU), and the engine copies the rectangles that changed
	into the frame buffer, so the CPU does not write each frame through the
	PCI aperture or into an APU's carve-out (summit#119). Updates of a few
	small rectangles (typing, a cursor's trail) are copied by the CPU here
	still: for them the GPU's round trip takes longer than the copy.

	Everything goes through amdgpu's own ioctls on the accelerant's file: a
	context, buffers and the GPU addresses (its VM is this file's alone) and
	one command submission per call, waited for. The copies are SDMA
	LINEAR_SUB_WINDOW packets, the same from CIK to RDNA 3; SI's DMA engine
	has other packets and gets no copies. */


#include "accelerant.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amdgpu_drm.h"


#define TRACE(x...) debug_printf("amdgpu_kms: " x)


#define SDMA_PACKET(op, subOp, extra) \
	((((uint32)(extra) & 0xffff) << 16) | (((uint32)(subOp) & 0xff) << 8) \
		| ((uint32)(op) & 0xff))
static const uint32 kSdmaOpcodeCopy = 0x1;
static const uint32 kSdmaCopyLinearSubWindow = 0x4;
static const uint32 kSdmaCopyDwords = 13;
static const uint32 kMaxSubWindow = 1 << 14;
	// the packet's x, y, width and height have 14 bits

static const uint32 kMaxSources = 8;
static const uint32 kCommandBufferSize = B_PAGE_SIZE * 4;
static const uint32 kCopiesPerSubmit
	= (kCommandBufferSize / 4 - 8) / kSdmaCopyDwords;
static const uint64 kVaAlignment = 2 * 1024 * 1024;
static const bigtime_t kCopyTimeout = 1000000;
static const uint64 kCpuCopyPixels = 64 * 1024;
	// a submission and its wait take 70 µs on the Framework 13's 760M, a
	// whole 4K frame 1.2 ms: less than this the CPU copies itself (a 120x20
	// rectangle takes it 1 µs on that APU, 64K pixels some tens through a
	// discrete card's aperture)


struct GpuBuffer {
	uint32	handle;
	uint64	va;
	uint64	size;
	void*	address;
};

struct Source {
	uint32		id;			// 0 when the slot is free
	GpuBuffer	buffer;
	uint32		width;
	uint32		height;
	uint32		bytesPerRow;
};


static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static bool sInitialized;
static bool sAvailable;
static uint32 sContext;
static uint64 sNextVa;
static uint64 sVaEnd;
static GpuBuffer sCommands;
static Source sSources[kMaxSources];
static uint32 sNextSourceId = 1;
// the frame buffer's mapping into the VM, for the generation it was made for
static uint64 sTargetVa;
static uint32 sTargetGeneration;
static bool sTargetMapped;


struct Locker {
	Locker() { pthread_mutex_lock(&sLock); }
	~Locker() { pthread_mutex_unlock(&sLock); }
};


static status_t
ioctl_amdgpu(unsigned long request, void* argument)
{
	return Kms::Ioctl(gInfo->device, request, argument);
}


/*!	The VM's addresses are handed out from the bottom up and not reused:
	there are terabytes of them, a mode change takes tens of megabytes. */
static status_t
map_va(uint32 handle, uint64 size, uint64& _va)
{
	uint64 va = (sNextVa + kVaAlignment - 1) & ~(kVaAlignment - 1);
	size = (size + B_PAGE_SIZE - 1) & ~(uint64)(B_PAGE_SIZE - 1);
	if (va + size > sVaEnd)
		return B_NO_MEMORY;

	drm_amdgpu_gem_va map = {};
	map.handle = handle;
	map.operation = AMDGPU_VA_OP_MAP;
	map.flags = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE
		| AMDGPU_VM_PAGE_EXECUTABLE;
	map.va_address = va;
	map.map_size = size;
	status_t status = ioctl_amdgpu(DRM_IOCTL_AMDGPU_GEM_VA, &map);
	if (status != B_OK)
		return status;
	sNextVa = va + size;
	_va = va;
	return B_OK;
}


static void
destroy_gpu_buffer(GpuBuffer& buffer)
{
	// closing the handle also ends its mapping into the VM
	if (buffer.address != NULL) {
		drm_haiku_unmap unmap = {};
		unmap.address = (addr_t)buffer.address;
		ioctl_amdgpu(DRM_HAIKU_IOCTL_UNMAP, &unmap);
	}
	if (buffer.handle != 0) {
		drm_gem_close close = {};
		close.handle = buffer.handle;
		ioctl_amdgpu(DRM_IOCTL_GEM_CLOSE, &close);
	}
	memset(&buffer, 0, sizeof(buffer));
}


//! system memory the GPU reads and the CPU caches, mapped for both
static status_t
create_gpu_buffer(uint64 size, GpuBuffer& buffer)
{
	memset(&buffer, 0, sizeof(buffer));
	size = (size + B_PAGE_SIZE - 1) & ~(uint64)(B_PAGE_SIZE - 1);

	drm_amdgpu_gem_create create = {};
	create.in.bo_size = size;
	create.in.alignment = B_PAGE_SIZE;
	create.in.domains = AMDGPU_GEM_DOMAIN_GTT;
	create.in.domain_flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
	status_t status = ioctl_amdgpu(DRM_IOCTL_AMDGPU_GEM_CREATE, &create);
	if (status != B_OK)
		return status;
	buffer.handle = create.out.handle;
	buffer.size = size;

	drm_amdgpu_gem_mmap mmap = {};
	mmap.in.handle = buffer.handle;
	status = ioctl_amdgpu(DRM_IOCTL_AMDGPU_GEM_MMAP, &mmap);
	if (status == B_OK) {
		drm_haiku_map map = {};
		map.offset = mmap.out.addr_ptr;
		map.size = size;
		map.flags = DRM_HAIKU_MAP_WRITE;
		status = ioctl_amdgpu(DRM_HAIKU_IOCTL_MAP, &map);
		if (status == B_OK)
			buffer.address = (void*)(addr_t)map.address;
	}
	if (status == B_OK)
		status = map_va(buffer.handle, size, buffer.va);
	if (status != B_OK)
		destroy_gpu_buffer(buffer);
	return status;
}


static status_t
create_context(void)
{
	drm_amdgpu_ctx context = {};
	context.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
	status_t status = ioctl_amdgpu(DRM_IOCTL_AMDGPU_CTX, &context);
	if (status == B_OK)
		sContext = context.out.alloc.ctx_id;
	return status;
}


static void
free_context(void)
{
	drm_amdgpu_ctx context = {};
	context.in.op = AMDGPU_CTX_OP_FREE_CTX;
	context.in.ctx_id = sContext;
	ioctl_amdgpu(DRM_IOCTL_AMDGPU_CTX, &context);
	sContext = 0;
}


//! sets up on the first use; the lock must be held
static bool
init_device_copies(void)
{
	if (sInitialized)
		return sAvailable;
	sInitialized = true;
	if (gInfo->isClone)
		return false;

	drm_amdgpu_info_device device = {};
	drm_amdgpu_info info = {};
	info.return_pointer = (uint64)(addr_t)&device;
	info.return_size = sizeof(device);
	info.query = AMDGPU_INFO_DEV_INFO;
	status_t status = ioctl_amdgpu(DRM_IOCTL_AMDGPU_INFO, &info);
	if (status != B_OK || device.family == AMDGPU_FAMILY_SI) {
		TRACE("no copies by the GPU: %s\n", status != B_OK
			? strerror(status) : "an SI GPU");
		return false;
	}
	sNextVa = device.virtual_address_offset;
	sVaEnd = device.virtual_address_max;

	status = create_context();
	if (status == B_OK)
		status = create_gpu_buffer(kCommandBufferSize, sCommands);
	if (status != B_OK) {
		TRACE("no copies by the GPU: %s\n", strerror(status));
		if (sContext != 0)
			free_context();
		return false;
	}
	sAvailable = true;
	return true;
}


static Source*
find_source(uint32 id)
{
	for (uint32 i = 0; i < kMaxSources; i++) {
		if (id != 0 && sSources[i].id == id)
			return &sSources[i];
	}
	return NULL;
}


/*!	Maps the frame buffer into the VM when it is not, or not any more: a new
	one (a mode change) has its own mapping, the old one's went with it. */
static status_t
map_target(void)
{
	if (sTargetMapped && sTargetGeneration == gInfo->frameBufferGeneration)
		return B_OK;
	sTargetMapped = false;
	if (gInfo->frameBuffer.handle == 0)
		return B_NO_INIT;
	status_t status = map_va(gInfo->frameBuffer.handle,
		gInfo->frameBuffer.size, sTargetVa);
	if (status != B_OK)
		return status;
	sTargetGeneration = gInfo->frameBufferGeneration;
	sTargetMapped = true;
	return B_OK;
}


static status_t
submit(uint32 dwords, uint32 sourceHandle)
{
	drm_amdgpu_bo_list_entry buffers[3] = {};
	buffers[0].bo_handle = sCommands.handle;
	buffers[1].bo_handle = sourceHandle;
	buffers[2].bo_handle = gInfo->frameBuffer.handle;
	drm_amdgpu_bo_list_in list = {};
	list.bo_number = 3;
	list.bo_info_size = sizeof(drm_amdgpu_bo_list_entry);
	list.bo_info_ptr = (uint64)(addr_t)buffers;

	drm_amdgpu_cs_chunk_ib ib = {};
	ib.va_start = sCommands.va;
	ib.ib_bytes = dwords * 4;
	ib.ip_type = AMDGPU_HW_IP_DMA;

	drm_amdgpu_cs_chunk chunks[2] = {};
	chunks[0].chunk_id = AMDGPU_CHUNK_ID_IB;
	chunks[0].length_dw = sizeof(ib) / 4;
	chunks[0].chunk_data = (uint64)(addr_t)&ib;
	chunks[1].chunk_id = AMDGPU_CHUNK_ID_BO_HANDLES;
	chunks[1].length_dw = sizeof(list) / 4;
	chunks[1].chunk_data = (uint64)(addr_t)&list;
	uint64 chunkPointers[2] = {
		(uint64)(addr_t)&chunks[0], (uint64)(addr_t)&chunks[1]
	};

	drm_amdgpu_cs cs = {};
	cs.in.ctx_id = sContext;
	cs.in.num_chunks = 2;
	cs.in.chunks = (uint64)(addr_t)chunkPointers;
	status_t status = ioctl_amdgpu(DRM_IOCTL_AMDGPU_CS, &cs);
	if (status != B_OK)
		return status;

	// the timeout is absolute, in the kernel's monotonic clock: system_time()
	drm_amdgpu_wait_cs wait = {};
	wait.in.handle = cs.out.handle;
	wait.in.timeout = (uint64)(system_time() + kCopyTimeout) * 1000;
	wait.in.ip_type = AMDGPU_HW_IP_DMA;
	wait.in.ctx_id = sContext;
	status = ioctl_amdgpu(DRM_IOCTL_AMDGPU_WAIT_CS, &wait);
	if (status == B_OK && wait.out.status != 0)
		status = B_TIMED_OUT;
	return status;
}


extern "C" status_t
amdgpu_create_frame_buffer_source(uint16 width, uint16 height,
	color_space space, frame_buffer_source* source)
{
	if (space != B_RGB32 && space != B_RGBA32)
		return B_BAD_VALUE;
	if (width == 0 || height == 0 || width > kMaxSubWindow
		|| height > kMaxSubWindow)
		return B_BAD_VALUE;

	Locker _;
	if (!init_device_copies())
		return B_NOT_SUPPORTED;
	Source* entry = NULL;
	for (uint32 i = 0; entry == NULL && i < kMaxSources; i++) {
		if (sSources[i].id == 0)
			entry = &sSources[i];
	}
	if (entry == NULL)
		return B_NO_MEMORY;

	// rows of whole 256 byte units, as the engine reads best
	uint32 bytesPerRow = ((uint32)width * 4 + 255) & ~255u;
	status_t status = create_gpu_buffer((uint64)bytesPerRow * height,
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


extern "C" void
amdgpu_delete_frame_buffer_source(uint32 id)
{
	Locker _;
	Source* source = find_source(id);
	if (source == NULL)
		return;
	// no copy from it runs: they are waited for under the lock
	destroy_gpu_buffer(source->buffer);
	source->id = 0;
}


/*!	Copies the rectangles (inclusive) to the same place in the frame buffer,
	clipped to both, and returns when they are there. */
extern "C" status_t
amdgpu_copy_to_frame_buffer(uint32 id, const fill_rect_params* list,
	uint32 count)
{
	Locker _;
	if (!sAvailable)
		return B_NOT_SUPPORTED;
	Source* source = find_source(id);
	if (source == NULL)
		return B_BAD_VALUE;
	status_t status = map_target();
	if (status != B_OK)
		return status;

	const Kms::Buffer& target = gInfo->frameBuffer;
	uint32 width = min_c(source->width, target.width);
	uint32 height = min_c(source->height, target.height);
	uint32 sourcePitch = source->bytesPerRow / 4;
	uint32 targetPitch = target.pitch / 4;
	if (width > kMaxSubWindow || height > kMaxSubWindow)
		return B_NOT_SUPPORTED;

	uint64 pixels = 0;
	for (uint32 i = 0; i < count; i++) {
		uint32 right = min_c((uint32)list[i].right + 1, width);
		uint32 bottom = min_c((uint32)list[i].bottom + 1, height);
		if (list[i].left < right && list[i].top < bottom) {
			pixels += (uint64)(right - list[i].left)
				* (bottom - list[i].top);
		}
	}
	if (pixels < kCpuCopyPixels) {
		// no copy by the GPU is under way: each one is waited for
		for (; count > 0; list++, count--) {
			uint32 right = min_c((uint32)list->right + 1, width);
			uint32 bottom = min_c((uint32)list->bottom + 1, height);
			if (list->left >= right || list->top >= bottom)
				continue;
			size_t rowBytes = (right - list->left) * 4;
			for (uint32 y = list->top; y < bottom; y++) {
				memcpy((uint8*)target.address + y * target.pitch
						+ list->left * 4,
					(uint8*)source->buffer.address + y * source->bytesPerRow
						+ list->left * 4, rowBytes);
			}
		}
		return B_OK;
	}

	while (count > 0) {
		uint32* commands = (uint32*)sCommands.address;
		uint32 dwords = 0;
		uint32 copies = 0;
		for (; count > 0 && copies < kCopiesPerSubmit; list++, count--) {
			uint32 left = list->left;
			uint32 top = list->top;
			uint32 right = min_c((uint32)list->right + 1, width);
			uint32 bottom = min_c((uint32)list->bottom + 1, height);
			if (left >= right || top >= bottom)
				continue;
			// 4 byte elements; pitches and slices in elements, minus one
			commands[dwords++] = SDMA_PACKET(kSdmaOpcodeCopy,
				kSdmaCopyLinearSubWindow, 0) | (2u << 29);
			commands[dwords++] = (uint32)source->buffer.va;
			commands[dwords++] = (uint32)(source->buffer.va >> 32);
			commands[dwords++] = left | top << 16;
			commands[dwords++] = (sourcePitch - 1) << 13;
			commands[dwords++] = sourcePitch * source->height - 1;
			commands[dwords++] = (uint32)sTargetVa;
			commands[dwords++] = (uint32)(sTargetVa >> 32);
			commands[dwords++] = left | top << 16;
			commands[dwords++] = (targetPitch - 1) << 13;
			commands[dwords++] = targetPitch * target.height - 1;
			commands[dwords++] = (right - left - 1)
				| (bottom - top - 1) << 16;
			commands[dwords++] = 0;
			copies++;
		}
		if (copies == 0)
			break;
		// SDMA command buffers are padded with NOPs to 8 dwords
		while (dwords % 8 != 0)
			commands[dwords++] = 0;

		status = submit(dwords, source->buffer.handle);
		if (status == ECANCELED) {
			// a GPU reset ended the context: once more with a new one
			free_context();
			status = create_context();
			if (status == B_OK)
				status = submit(dwords, source->buffer.handle);
		}
		if (status != B_OK) {
			TRACE("copy to the frame buffer failed: %s\n", strerror(status));
			return status;
		}
	}
	return B_OK;
}


//! app_server is going away (the primary accelerant only)
void
uninit_device_copies(void)
{
	Locker _;
	for (uint32 i = 0; i < kMaxSources; i++) {
		if (sSources[i].id != 0)
			destroy_gpu_buffer(sSources[i].buffer);
		sSources[i].id = 0;
	}
	if (sAvailable) {
		destroy_gpu_buffer(sCommands);
		free_context();
	}
	sAvailable = false;
	sInitialized = false;
	sTargetMapped = false;
}


//! held while the frame buffer is replaced (mode.cpp)
void
lock_device_copies(void)
{
	pthread_mutex_lock(&sLock);
}


void
unlock_device_copies(void)
{
	pthread_mutex_unlock(&sLock);
}
