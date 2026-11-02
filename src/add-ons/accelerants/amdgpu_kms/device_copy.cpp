/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Copies by the GPU's SDMA engine for app_server's flush.

	B_CREATE_FRAME_BUFFER_SOURCE, B_COPY_TO_FRAME_BUFFER: app_server keeps
	the screen's contents in buffers of system memory the GPU reads (GTT,
	cached for the CPU), and the engine copies the rectangles that changed
	into the frame buffer, so the CPU does not write each frame through the
	PCI aperture or into an APU's carve-out (summit#119).

	B_CREATE_FRAME_BUFFER_SHADOW, B_COPY_FRAME_BUFFER_SOURCE: app_server's
	back buffer is itself such a source, and the flush's shadows of it are
	buffers app_server cannot reach, which the engine fills from the back
	buffer: the CPU no longer copies everything it draws a second time. On
	a discrete card they are in video memory, from where the engine copies
	to the screen fastest (or in system memory when there is no room). An
	APU's video memory is a carve-out of the same RAM, which the engine
	reads and writes no faster than the rest (18 GB/s either way on the
	Framework 13's 840M), so there they stay in system memory: the carve-out
	keeps its room, and the CPU still reaches them for small copies.

	Updates of a few small rectangles (typing, a cursor's trail) are copied
	by the CPU here still, where it reaches both buffers: for them the GPU's
	round trip takes longer than the copy.

	Everything goes through amdgpu's own ioctls on the accelerant's file: a
	context, buffers and the GPU addresses (its VM is this file's alone) and
	command submissions on the SDMA ring, which run in the order they are
	made. A caller waits for its own copies without the lock, so that the
	flush's copy to the screen at a blank does not wait behind another
	thread's sleep. The copies are SDMA LINEAR_SUB_WINDOW packets, the same
	from CIK to RDNA 3.5; SI's DMA engine has other packets and gets no
	copies. */


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
static const uint32 kCommandBuffers = 4;
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
	void*	address;	// NULL when the CPU does not reach it
	uint32	domain;
};

struct Source {
	uint32		id;			// 0 when the slot is free
	GpuBuffer	buffer;
	uint32		width;
	uint32		height;
	uint32		bytesPerRow;
	bool		shadow;		// app_server does not see its bits
};

// a command submission on a context, to wait for
struct Fence {
	uint32	context;
	uint64	handle;		// 0: none
};

struct CommandBuffer {
	GpuBuffer	buffer;
	Fence		last;		// its last submission, which may still read it
};

// one side of a copy
struct Surface {
	uint32	handle;
	uint64	va;
	void*	address;
	uint32	bytesPerRow;
	uint32	width;
	uint32	height;
};


static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static bool sInitialized;
static bool sAvailable;
static uint32 sContext;
static uint64 sNextVa;
static uint64 sVaEnd;
static CommandBuffer sCommands[kCommandBuffers];
static uint32 sNextCommands;
static Fence sLastFence;
static Source sSources[kMaxSources];
static uint32 sNextSourceId = 1;
static uint32 sShadowDomain;
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


/*!	System memory (GTT) the GPU reads and writes and the CPU caches, mapped
	for both; or video memory, mapped for the GPU only. */
static status_t
create_gpu_buffer(uint64 size, uint32 domain, GpuBuffer& buffer)
{
	memset(&buffer, 0, sizeof(buffer));
	size = (size + B_PAGE_SIZE - 1) & ~(uint64)(B_PAGE_SIZE - 1);
	bool cpuAccess = domain != AMDGPU_GEM_DOMAIN_VRAM;

	drm_amdgpu_gem_create create = {};
	create.in.bo_size = size;
	create.in.alignment = B_PAGE_SIZE;
	create.in.domains = domain;
	create.in.domain_flags = cpuAccess ? AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED
		: AMDGPU_GEM_CREATE_NO_CPU_ACCESS;
	status_t status = ioctl_amdgpu(DRM_IOCTL_AMDGPU_GEM_CREATE, &create);
	if (status != B_OK)
		return status;
	buffer.handle = create.out.handle;
	buffer.size = size;
	buffer.domain = domain;

	if (cpuAccess) {
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


/*!	Returns once the submission is done, with or without the lock; an error
	when it failed (ECANCELED: a GPU reset ended it) or took too long. */
static status_t
wait_for(const Fence& fence)
{
	if (fence.handle == 0)
		return B_OK;
	// the timeout is absolute, in the kernel's monotonic clock: system_time()
	drm_amdgpu_wait_cs wait = {};
	wait.in.handle = fence.handle;
	wait.in.timeout = (uint64)(system_time() + kCopyTimeout) * 1000;
	wait.in.ip_type = AMDGPU_HW_IP_DMA;
	wait.in.ctx_id = fence.context;
	status_t status = ioctl_amdgpu(DRM_IOCTL_AMDGPU_WAIT_CS, &wait);
	if (status == B_OK && wait.out.status != 0)
		status = B_TIMED_OUT;
	return status;
}


/*!	Waits until no copy runs any more, which may read or write a buffer that
	is about to go; the lock must be held. Submissions run in order, so the
	last one is enough. One on a context that is gone ended with it. */
static void
wait_idle(void)
{
	if (sLastFence.handle == 0 || sLastFence.context != sContext)
		return;
	status_t status = wait_for(sLastFence);
	if (status != B_OK)
		TRACE("waiting for the copies to end: %s\n", strerror(status));
	sLastFence.handle = 0;
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
	sShadowDomain = (device.ids_flags & AMDGPU_IDS_FLAGS_FUSION) != 0
		? AMDGPU_GEM_DOMAIN_GTT : AMDGPU_GEM_DOMAIN_VRAM;

	status = create_context();
	for (uint32 i = 0; status == B_OK && i < kCommandBuffers; i++) {
		status = create_gpu_buffer(kCommandBufferSize, AMDGPU_GEM_DOMAIN_GTT,
			sCommands[i].buffer);
	}
	if (status != B_OK) {
		TRACE("no copies by the GPU: %s\n", strerror(status));
		for (uint32 i = 0; i < kCommandBuffers; i++)
			destroy_gpu_buffer(sCommands[i].buffer);
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


static Surface
source_surface(const Source& source)
{
	Surface surface;
	surface.handle = source.buffer.handle;
	surface.va = source.buffer.va;
	surface.address = source.buffer.address;
	surface.bytesPerRow = source.bytesPerRow;
	surface.width = source.width;
	surface.height = source.height;
	return surface;
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


//! the lock must be held
static status_t
target_surface(Surface& surface)
{
	status_t status = map_target();
	if (status != B_OK)
		return status;
	const Kms::Buffer& target = gInfo->frameBuffer;
	surface.handle = target.handle;
	surface.va = sTargetVa;
	surface.address = target.address;
	surface.bytesPerRow = target.pitch;
	surface.width = target.width;
	surface.height = target.height;
	return B_OK;
}


static status_t
submit(CommandBuffer& commands, uint32 dwords, const Surface& from,
	const Surface& to, Fence& _fence)
{
	drm_amdgpu_bo_list_entry buffers[3] = {};
	buffers[0].bo_handle = commands.buffer.handle;
	buffers[1].bo_handle = from.handle;
	buffers[2].bo_handle = to.handle;
	drm_amdgpu_bo_list_in list = {};
	list.bo_number = 3;
	list.bo_info_size = sizeof(drm_amdgpu_bo_list_entry);
	list.bo_info_ptr = (uint64)(addr_t)buffers;

	drm_amdgpu_cs_chunk_ib ib = {};
	ib.va_start = commands.buffer.va;
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
	if (status == ECANCELED) {
		// a GPU reset ended the context: once more with a new one
		free_context();
		status = create_context();
		if (status == B_OK) {
			cs.in.ctx_id = sContext;
			status = ioctl_amdgpu(DRM_IOCTL_AMDGPU_CS, &cs);
		}
	}
	if (status != B_OK)
		return status;

	_fence.context = sContext;
	_fence.handle = cs.out.handle;
	commands.last = _fence;
	sLastFence = _fence;
	return B_OK;
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


/*!	Copies the rectangles (inclusive) from one surface to the same place in
	the other, clipped to both, and returns when they are there. The lock is
	held on entry, and is not on return. */
static status_t
copy_rects(Surface from, Surface to, const fill_rect_params* list,
	uint32 count)
{
	uint32 width = min_c(from.width, to.width);
	uint32 height = min_c(from.height, to.height);
	if (width > kMaxSubWindow || height > kMaxSubWindow) {
		pthread_mutex_unlock(&sLock);
		return B_NOT_SUPPORTED;
	}

	uint64 pixels = 0;
	uint32 left, top, right, bottom;
	for (uint32 i = 0; i < count; i++) {
		if (clip(list[i], width, height, left, top, right, bottom))
			pixels += (uint64)(right - left) * (bottom - top);
	}
	if (pixels < kCpuCopyPixels && from.address != NULL
		&& to.address != NULL) {
		// the GPU's copies into these buffers are done: each one is
		// waited for before its caller goes on
		for (; count > 0; list++, count--) {
			if (!clip(*list, width, height, left, top, right, bottom))
				continue;
			size_t rowBytes = (right - left) * 4;
			for (uint32 y = top; y < bottom; y++) {
				memcpy((uint8*)to.address + y * to.bytesPerRow + left * 4,
					(uint8*)from.address + y * from.bytesPerRow + left * 4,
					rowBytes);
			}
		}
		pthread_mutex_unlock(&sLock);
		return B_OK;
	}

	// 4 byte elements; pitches and slices in elements, minus one
	uint32 fromPitch = from.bytesPerRow / 4;
	uint32 toPitch = to.bytesPerRow / 4;
	Fence fence = {};
	status_t status = B_OK;
	while (count > 0) {
		CommandBuffer& commandBuffer = sCommands[sNextCommands];
		sNextCommands = (sNextCommands + 1) % kCommandBuffers;
		if (commandBuffer.last.handle != 0
			&& commandBuffer.last.context == sContext) {
			// submitted a while ago, normally done: the engine may read it
			// still
			status = wait_for(commandBuffer.last);
			if (status != B_OK)
				break;
		}
		commandBuffer.last.handle = 0;

		uint32* commands = (uint32*)commandBuffer.buffer.address;
		uint32 dwords = 0;
		uint32 copies = 0;
		for (; count > 0 && copies < kCopiesPerSubmit; list++, count--) {
			if (!clip(*list, width, height, left, top, right, bottom))
				continue;
			commands[dwords++] = SDMA_PACKET(kSdmaOpcodeCopy,
				kSdmaCopyLinearSubWindow, 0) | (2u << 29);
			commands[dwords++] = (uint32)from.va;
			commands[dwords++] = (uint32)(from.va >> 32);
			commands[dwords++] = left | top << 16;
			commands[dwords++] = (fromPitch - 1) << 13;
			commands[dwords++] = fromPitch * from.height - 1;
			commands[dwords++] = (uint32)to.va;
			commands[dwords++] = (uint32)(to.va >> 32);
			commands[dwords++] = left | top << 16;
			commands[dwords++] = (toPitch - 1) << 13;
			commands[dwords++] = toPitch * to.height - 1;
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

		status = submit(commandBuffer, dwords, from, to, fence);
		if (status != B_OK)
			break;
	}
	pthread_mutex_unlock(&sLock);

	// the copies run in order: the last one done, all are; the buffers
	// stay while it runs (deleting one waits for the copies)
	if (status == B_OK)
		status = wait_for(fence);
	if (status != B_OK && status != ECANCELED) {
		// another thread may have replaced the context after a GPU reset
		// meanwhile, which ended this one's copies with it
		pthread_mutex_lock(&sLock);
		if (fence.handle != 0 && fence.context != sContext)
			status = ECANCELED;
		pthread_mutex_unlock(&sLock);
	}
	return status;
}


static status_t
create_source(uint16 width, uint16 height, color_space space, bool shadow,
	frame_buffer_source* source)
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
	uint64 size = (uint64)bytesPerRow * height;
	status_t status;
	if (shadow) {
		// where there is no room in video memory, system memory is
		// still better than the CPU's copy
		status = create_gpu_buffer(size, sShadowDomain, entry->buffer);
		if (status != B_OK && sShadowDomain != AMDGPU_GEM_DOMAIN_GTT) {
			TRACE("no room for a %ux%u shadow in video memory (%s), it is "
				"in system memory\n", width, height, strerror(status));
			status = create_gpu_buffer(size, AMDGPU_GEM_DOMAIN_GTT,
				entry->buffer);
		}
	} else
		status = create_gpu_buffer(size, AMDGPU_GEM_DOMAIN_GTT, entry->buffer);
	if (status != B_OK)
		return status;
	entry->id = sNextSourceId++;
	if (sNextSourceId == 0)
		sNextSourceId = 1;
	entry->width = width;
	entry->height = height;
	entry->bytesPerRow = bytesPerRow;
	entry->shadow = shadow;

	source->id = entry->id;
	source->bits = shadow ? NULL : entry->buffer.address;
	source->bytes_per_row = bytesPerRow;
	source->width = width;
	source->height = height;
	source->space = space;
	return B_OK;
}


extern "C" status_t
amdgpu_create_frame_buffer_source(uint16 width, uint16 height,
	color_space space, frame_buffer_source* source)
{
	return create_source(width, height, space, false, source);
}


/*!	A shadow of app_server's back buffer, which only the GPU fills and reads
	(and the CPU here, for small copies, when it is in system memory). */
extern "C" status_t
amdgpu_create_frame_buffer_shadow(uint16 width, uint16 height,
	color_space space, frame_buffer_source* shadow)
{
	return create_source(width, height, space, true, shadow);
}


extern "C" void
amdgpu_delete_frame_buffer_source(uint32 id)
{
	Locker _;
	Source* source = find_source(id);
	if (source == NULL)
		return;
	// a copy from or into it may run still
	wait_idle();
	destroy_gpu_buffer(source->buffer);
	source->id = 0;
}


/*!	Copies the rectangles (inclusive) to the same place in the frame buffer,
	clipped to both, and returns when they are there. */
static status_t
try_copy_to_frame_buffer(uint32 id, const fill_rect_params* list,
	uint32 count)
{
	pthread_mutex_lock(&sLock);
	Source* source = sAvailable ? find_source(id) : NULL;
	Surface target;
	status_t status = sAvailable ? (source != NULL
		? target_surface(target) : B_BAD_VALUE) : B_NOT_SUPPORTED;
	if (status != B_OK) {
		pthread_mutex_unlock(&sLock);
		return status;
	}
	return copy_rects(source_surface(*source), target, list, count);
}


static status_t
try_copy_between_sources(uint32 fromId, uint32 toId,
	const fill_rect_params* list, uint32 count)
{
	pthread_mutex_lock(&sLock);
	Source* from = sAvailable ? find_source(fromId) : NULL;
	Source* to = sAvailable ? find_source(toId) : NULL;
	status_t status = B_OK;
	if (!sAvailable)
		status = B_NOT_SUPPORTED;
	else if (from == NULL || to == NULL || from == to
		|| from->width != to->width || from->height != to->height)
		status = B_BAD_VALUE;
	if (status != B_OK) {
		pthread_mutex_unlock(&sLock);
		return status;
	}
	return copy_rects(source_surface(*from), source_surface(*to), list,
		count);
}


/*!	Copies the rectangles (inclusive) to the same place in the frame buffer,
	clipped to both, and returns when they are there. A GPU reset that ends
	the copy midway (ECANCELED) gets it made once more, on a new context. */
extern "C" status_t
amdgpu_copy_to_frame_buffer(uint32 id, const fill_rect_params* list,
	uint32 count)
{
	status_t status = try_copy_to_frame_buffer(id, list, count);
	if (status == ECANCELED)
		status = try_copy_to_frame_buffer(id, list, count);
	if (status != B_OK)
		TRACE("copy to the frame buffer failed: %s\n", strerror(status));
	return status;
}


/*!	Copies the rectangles (inclusive) from one source or shadow to the same
	place in another of the same size, and returns when they are there. */
extern "C" status_t
amdgpu_copy_frame_buffer_source(uint32 from, uint32 to,
	const fill_rect_params* list, uint32 count)
{
	status_t status = try_copy_between_sources(from, to, list, count);
	if (status == ECANCELED)
		status = try_copy_between_sources(from, to, list, count);
	if (status != B_OK)
		TRACE("copy between sources failed: %s\n", strerror(status));
	return status;
}


/*!	For tests: where shadows go (AMDGPU_GEM_DOMAIN_VRAM or _GTT; returns
	where they went so far), and where the one with \a id went. */
uint32
device_copy_set_shadow_domain(uint32 domain)
{
	Locker _;
	uint32 previous = sShadowDomain;
	sShadowDomain = domain;
	return previous;
}


uint32
device_copy_source_domain(uint32 id)
{
	Locker _;
	Source* source = find_source(id);
	return source != NULL ? source->buffer.domain : 0;
}


//! app_server is going away (the primary accelerant only)
void
uninit_device_copies(void)
{
	Locker _;
	wait_idle();
	for (uint32 i = 0; i < kMaxSources; i++) {
		if (sSources[i].id != 0)
			destroy_gpu_buffer(sSources[i].buffer);
		sSources[i].id = 0;
	}
	if (sAvailable) {
		for (uint32 i = 0; i < kCommandBuffers; i++) {
			destroy_gpu_buffer(sCommands[i].buffer);
			sCommands[i].last.handle = 0;
		}
		free_context();
	}
	sLastFence.handle = 0;
	sAvailable = false;
	sInitialized = false;
	sTargetMapped = false;
}


/*!	Held while the frame buffer is replaced (mode.cpp): no copy into the old
	one runs any more, nor starts. */
void
lock_device_copies(void)
{
	pthread_mutex_lock(&sLock);
	wait_idle();
}


void
unlock_device_copies(void)
{
	pthread_mutex_unlock(&sLock);
}
