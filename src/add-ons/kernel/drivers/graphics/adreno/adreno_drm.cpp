/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Adreno driver's userland interface (adreno_haiku.h): Linux's msm
	DRM ioctls on /dev/graphics/adreno/0, as Mesa's Turnip uses them without
	VM_BIND. Buffers are page-locked kernel areas, mapped into the GPU's
	address space where userland puts them (MSM_INFO_SET_IOVA) and cloned
	into teams on request. Every submission goes to the one ring and ends
	with a fence, a number the CP writes when it gets there; sync objects
	hold such numbers, so a fence is signaled once the CP's completed fence
	has reached it. */


#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <Drivers.h>
#include <KernelExport.h>
#include <condition_variable.h>
#include <kernel.h>
#include <lock.h>
#include <team.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <libfdt.h>

#include "adreno_private.h"

#include <graphics/adreno/adreno_haiku.h>
#include <graphics/adreno/drm.h>
#include <graphics/adreno/msm_drm.h>


extern void* gFDT;

int32 api_version = B_CUR_DRIVER_API_VERSION;


#define DRIVER_NAME				"msm"
#define DRIVER_MAJOR			1
#define DRIVER_MINOR			12

#define CP_INDIRECT_BUFFER		0x3f
#define CP_EVENT_WRITE			0x46
#define EVENT_PC_CCU_INVALIDATE_DEPTH 24
#define EVENT_PC_CCU_INVALIDATE_COLOR 25

#define MAX_HANDLES				65536
#define MAX_SUBMIT_BOS			16384
#define MAX_SUBMIT_CMDS			1024
#define MAX_SYNCOBJ_HANDLES		4096
#define MAX_TIMELINE_POINTS		256

// buffers without an address of userland's get one up here
#define KERNEL_VA_START			(ADRENO_VA_USER_START + ADRENO_VA_USER_SIZE / 2)

/*	Every open shares the one GPU address space (no per-process page tables
	yet), so each team gets a window of its own below KERNEL_VA_START for the
	addresses it chooses (MSM_PARAM_VA_START/VA_SIZE, which Turnip allocates
	from): two processes would otherwise put buffers at the same address,
	and one's mapping would replace the other's. All opens of a team share
	its window (Turnip asks for it on one and places buffers on another). */
#define FILE_VA_WINDOW			(8ull * 1024 * 1024 * 1024)
#define FILE_VA_WINDOWS			((KERNEL_VA_START - ADRENO_VA_USER_START) \
									/ FILE_VA_WINDOW)


static const char* sDeviceNames[] = { "graphics/adreno/0", NULL };


//	#pragma mark - buffers


struct Buffer {
	Buffer*		next;		// in sBuffers
	Buffer*		previous;
	int32		references;
	area_id		area;
	uint8*		address;
	size_t		size;
	uint32		flags;		// MSM_BO_*
	uint64		iova;		// 0 if not in the GPU's address space
	uint32		lastFence;	// of the last submission that used it
	char		name[32];
};


static mutex sVaLock = MUTEX_INITIALIZER("adreno va");
static uint64 sKernelVaNext = KERNEL_VA_START;
static struct {
	team_id		team;
	int32		opens;			// 0: free
} sVaWindows[FILE_VA_WINDOWS];


// every buffer, for the fault dump
static mutex sBuffersLock = MUTEX_INITIALIZER("adreno buffers");
static Buffer* sBuffers;


static Buffer*
buffer_create(uint64 size, uint32 flags)
{
	if (size == 0 || size > 4ull * 1024 * 1024 * 1024)
		return NULL;
	Buffer* buffer = (Buffer*)calloc(1, sizeof(Buffer));
	if (buffer == NULL)
		return NULL;
	buffer->size = (size + B_PAGE_SIZE - 1) & ~(uint64)(B_PAGE_SIZE - 1);
	buffer->flags = flags;
	buffer->references = 1;
	buffer->area = create_area("adreno buffer", (void**)&buffer->address,
		B_ANY_KERNEL_ADDRESS, buffer->size, B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA);
	if (buffer->area < 0) {
		free(buffer);
		return NULL;
	}
	memset(buffer->address, 0, buffer->size);
	MutexLocker locker(sBuffersLock);
	buffer->next = sBuffers;
	if (sBuffers != NULL)
		sBuffers->previous = buffer;
	sBuffers = buffer;
	return buffer;
}


/*!	The buffers at \a iova and their words there, for the fault dump. */
void
adreno_dump_buffer(uint64 iova, int32 count)
{
	MutexLocker locker(sBuffersLock);
	int32 found = 0;
	for (Buffer* buffer = sBuffers; buffer != NULL; buffer = buffer->next) {
		if (buffer->iova == 0 || iova < buffer->iova
			|| iova >= buffer->iova + buffer->size) {
			continue;
		}
		found++;
		uint64 offset = iova - buffer->iova;
		dprintf("adreno: fault: buffer %#" B_PRIx64 "+%#" B_PRIxSIZE " flags %#"
			B_PRIx32 " \"%s\", offset %#" B_PRIx64 ", last fence %" B_PRIu32
			"\n", buffer->iova, buffer->size, buffer->flags, buffer->name, offset,
			buffer->lastFence);
		uint32* words = (uint32*)(buffer->address + offset);
		for (int32 i = 0; i < count && offset + i * 4 + 16 <= buffer->size;
				i += 4) {
			dprintf("adreno: buffer %#" B_PRIx64 ": %08" B_PRIx32 " %08" B_PRIx32
				" %08" B_PRIx32 " %08" B_PRIx32 "\n", iova + i * 4, words[i],
				words[i + 1], words[i + 2], words[i + 3]);
		}
		phys_addr_t physical = 0;
		physical_entry entry;
		if (get_memory_map(words, 4, &entry, 1) == B_OK)
			physical = entry.address;
		dprintf("adreno: fault: the CPU's page is at %#" B_PRIxPHYSADDR "\n",
			physical);
	}
	if (found == 0)
		dprintf("adreno: fault: no buffer at %#" B_PRIx64 "\n", iova);
}


static void
buffer_unmap(Buffer* buffer)
{
	if (buffer->iova == 0)
		return;
	// the GPU may still read it
	adreno_wait_fence(buffer->lastFence, B_INFINITE_TIMEOUT);
	adreno_gpu_unmap(buffer->iova, buffer->size);
	buffer->iova = 0;
}


static status_t
buffer_map(Buffer* buffer, uint64 iova)
{
	if (iova < ADRENO_VA_USER_START
		|| iova + buffer->size > ADRENO_VA_USER_START + ADRENO_VA_USER_SIZE
		|| (iova & (B_PAGE_SIZE - 1)) != 0) {
		return B_BAD_VALUE;
	}
	// the SC8280XP's GPU is I/O coherent: everything is mapped cacheable
	status_t status = adreno_gpu_map(iova, buffer->address, buffer->size, true);
	if (status != B_OK)
		return status;
	buffer->iova = iova;
	return B_OK;
}


static void
buffer_release(Buffer* buffer)
{
	if (atomic_add(&buffer->references, -1) != 1)
		return;
	{
		MutexLocker locker(sBuffersLock);
		if (buffer->previous != NULL)
			buffer->previous->next = buffer->next;
		else
			sBuffers = buffer->next;
		if (buffer->next != NULL)
			buffer->next->previous = buffer->previous;
	}
	buffer_unmap(buffer);
	delete_area(buffer->area);
	free(buffer);
}


//	#pragma mark - sync objects


/*	A sync object holds points of a timeline, each with the fence that
	signals it (0: signaled already); a binary one holds point 0 only.
	Linux semantics: a timeline point is signaled when it and all earlier
	ones are; "completed" is the highest such point. */

struct SyncPoint {
	uint64	point;
	uint32	fence;
};

struct Syncobj {
	int32		references;
	SyncPoint	points[MAX_TIMELINE_POINTS];
	uint32		count;
	uint64		completed;	// all points up to here are signaled
	bool		hasFence;	// binary: a fence (or a signaled stub) is set
};


static mutex sSyncLock = MUTEX_INITIALIZER("adreno syncobjs");
static ConditionVariable sSyncChanged;


static inline bool
fence_signaled(uint32 fence)
{
	return fence == 0 || (int32)(adreno_completed_fence() - fence) >= 0;
}


/*!	Drops the signaled points at the start; under the sync lock. */
static void
syncobj_collect(Syncobj* syncobj)
{
	uint32 drop = 0;
	while (drop < syncobj->count && fence_signaled(syncobj->points[drop].fence)) {
		if (syncobj->points[drop].point > syncobj->completed)
			syncobj->completed = syncobj->points[drop].point;
		drop++;
	}
	// a binary object keeps its last fence, signaled or not
	if (drop == syncobj->count && drop > 0)
		drop--;
	if (drop > 0) {
		memmove(syncobj->points, syncobj->points + drop,
			(syncobj->count - drop) * sizeof(SyncPoint));
		syncobj->count -= drop;
	}
}


static uint64
syncobj_last_point(Syncobj* syncobj)
{
	return syncobj->count > 0 ? syncobj->points[syncobj->count - 1].point
		: syncobj->completed;
}


/*!	Whether \a point has a fence (\a submitted) and whether it is signaled.
	Point 0 is the current fence. */
static void
syncobj_state(Syncobj* syncobj, uint64 point, bool& submitted, bool& signaled,
	uint32& fence)
{
	syncobj_collect(syncobj);
	fence = 0;
	if (point == 0) {
		submitted = syncobj->hasFence;
		if (syncobj->count > 0)
			fence = syncobj->points[syncobj->count - 1].fence;
		signaled = submitted && fence_signaled(fence);
		return;
	}
	submitted = syncobj_last_point(syncobj) >= point;
	signaled = syncobj->completed >= point;
	if (submitted && !signaled) {
		for (uint32 i = 0; i < syncobj->count; i++) {
			if (!fence_signaled(syncobj->points[i].fence)) {
				fence = syncobj->points[i].fence;
				break;
			}
		}
	}
}


static void
syncobj_set_fence(Syncobj* syncobj, uint32 fence)
{
	uint64 point = syncobj_last_point(syncobj);
	syncobj->points[0] = {point, fence};
	syncobj->count = 1;
	syncobj->hasFence = true;
}


static status_t
syncobj_add_point(Syncobj* syncobj, uint64 point, uint32 fence)
{
	if (point == 0) {
		syncobj_set_fence(syncobj, fence);
		return B_OK;
	}
	syncobj_collect(syncobj);
	if (point <= syncobj_last_point(syncobj))
		return B_OK;
		// Linux warns and keeps the timeline as it is
	if (syncobj->count == MAX_TIMELINE_POINTS) {
		// wait for the oldest to go
		uint32 oldest = syncobj->points[0].fence;
		mutex_unlock(&sSyncLock);
		adreno_wait_fence(oldest, system_time() + 1000000);
		mutex_lock(&sSyncLock);
		syncobj_collect(syncobj);
		if (syncobj->count == MAX_TIMELINE_POINTS)
			return B_NO_MEMORY;
	}
	syncobj->points[syncobj->count++] = {point, fence};
	syncobj->hasFence = true;
	return B_OK;
}


//	#pragma mark - files


// a queue's GPU losses are counted from its creation: kQueueSlots of a
// file's queues have their own count, the rest share the file's
#define QUEUE_SLOTS				64

struct File {
	mutex		lock;
	Buffer**	buffers;
	uint32		bufferCount;
	Syncobj**	syncobjs;
	uint32		syncobjCount;
	uint32		nextQueue;
	uint32		lastFence;
	uint64		vaStart;	// its window of the address space
	uint64		vaSize;
	int32		vaWindow;
	// adreno_faults() when each queue was created: work of a queue from
	// before the GPU was last lost fails (the reset dropped what it had
	// queued, and what it sends next builds on that)
	uint32		openLosses;
	uint32		queueLosses[QUEUE_SLOTS];
};


static uint32
queue_losses(File* file, uint32 queue)
{
	return queue < QUEUE_SLOTS ? file->queueLosses[queue] : file->openLosses;
}


/*!	The CPU's lines of the buffers the CP fetches (the command streams,
	which Turnip makes GPU read-only and write-combined) cleaned to memory
	before the CP runs them: they are mapped cacheable on both sides, and
	the CP read a stream as zeros (an opcode error) while the CPU still held
	it. */
static void
clean_command_buffers(Buffer** buffers, uint32 count)
{
	for (uint32 i = 0; i < count; i++) {
		Buffer* buffer = buffers[i];
		if (buffer == NULL || (buffer->flags & MSM_BO_GPU_READONLY) == 0
			|| (buffer->flags & MSM_BO_CACHED_COHERENT) != 0) {
			continue;
		}
		addr_t end = (addr_t)buffer->address + buffer->size;
		for (addr_t line = (addr_t)buffer->address; line < end; line += 64)
			asm volatile("dc cvac, %0" : : "r" (line) : "memory");
	}
	asm volatile("dsb sy" : : : "memory");
}


template<typename T> static uint32
add_handle(T**& table, uint32& count, T* object)
{
	for (uint32 i = 0; i < count; i++) {
		if (table[i] == NULL) {
			table[i] = object;
			return i + 1;
		}
	}
	if (count == MAX_HANDLES)
		return 0;
	uint32 newCount = count == 0 ? 64 : min_c(count * 2, MAX_HANDLES);
	T** newTable = (T**)realloc(table, newCount * sizeof(T*));
	if (newTable == NULL)
		return 0;
	memset(newTable + count, 0, (newCount - count) * sizeof(T*));
	table = newTable;
	newTable[count] = object;
	uint32 handle = count + 1;
	count = newCount;
	return handle;
}


static Buffer*
lookup_buffer(File* file, uint32 handle)
{
	if (handle == 0 || handle > file->bufferCount)
		return NULL;
	return file->buffers[handle - 1];
}


static Syncobj*
lookup_syncobj(File* file, uint32 handle)
{
	if (handle == 0 || handle > file->syncobjCount)
		return NULL;
	return file->syncobjs[handle - 1];
}


static bigtime_t
deadline_from_timespec(const drm_msm_timespec& timeout)
{
	// absolute CLOCK_MONOTONIC, which is system_time() on Haiku
	int64 us = timeout.tv_sec * 1000000 + timeout.tv_nsec / 1000;
	return us <= 0 ? 0 : us;
}


static bigtime_t
deadline_from_nanoseconds(int64 nanoseconds)
{
	if (nanoseconds <= 0)
		return 0;
	if (nanoseconds >= INT64_MAX / 2)
		return B_INFINITE_TIMEOUT;
	return nanoseconds / 1000;
}


//	#pragma mark - msm ioctls


static status_t
get_param(File* file, drm_msm_param* param)
{
	if (param->pipe != MSM_PIPE_3D0)
		return EINVAL;
	switch (param->param) {
		case MSM_PARAM_GPU_ID:
			param->value = ADRENO_GPU_ID;
			return B_OK;
		case MSM_PARAM_GMEM_SIZE:
			param->value = ADRENO_GMEM_SIZE;
			return B_OK;
		case MSM_PARAM_GMEM_BASE:
			param->value = ADRENO_GMEM_BASE;
			return B_OK;
		case MSM_PARAM_CHIP_ID:
			param->value = ADRENO_CHIP_ID;
			return B_OK;
		case MSM_PARAM_MAX_FREQ:
			param->value = ADRENO_MAX_FREQUENCY;
			return B_OK;
		case MSM_PARAM_TIMESTAMP:
			param->value = adreno_timestamp();
			return B_OK;
		case MSM_PARAM_PRIORITIES:
			param->value = 1;
			return B_OK;
		case MSM_PARAM_PP_PGTABLE:
		case MSM_PARAM_SUSPENDS:
		case MSM_PARAM_RAYTRACING:
		case MSM_PARAM_HAS_PRR:
			param->value = 0;
			return B_OK;
		case MSM_PARAM_FAULTS:
			param->value = adreno_faults();
			return B_OK;
		case MSM_PARAM_VA_START:
			param->value = file->vaStart;
			return B_OK;
		case MSM_PARAM_VA_SIZE:
			param->value = file->vaSize;
			return B_OK;
		case MSM_PARAM_HIGHEST_BANK_BIT:
			param->value = ADRENO_HIGHEST_BANK_BIT;
			return B_OK;
		case MSM_PARAM_UBWC_SWIZZLE:
			param->value = ADRENO_UBWC_SWIZZLE;
			return B_OK;
		case MSM_PARAM_MACROTILE_MODE:
			param->value = ADRENO_MACROTILE_MODE;
			return B_OK;
		case MSM_PARAM_UCHE_TRAP_BASE:
			param->value = ADRENO_UCHE_TRAP_BASE;
			return B_OK;
	}
	return EINVAL;
}


static status_t
set_param(File* file, drm_msm_param* param)
{
	switch (param->param) {
		case MSM_PARAM_COMM:
		case MSM_PARAM_CMDLINE:
		case MSM_PARAM_SYSPROF:
			return B_OK;
		case MSM_PARAM_EN_VM_BIND:
			// no VM_BIND: Turnip uses SET_IOVA
			return EINVAL;
	}
	return EINVAL;
}


static status_t
gem_new(File* file, drm_msm_gem_new* args)
{
	if ((args->flags & ~(MSM_BO_SCANOUT | MSM_BO_GPU_READONLY
			| MSM_BO_CACHE_MASK | MSM_BO_NO_SHARE)) != 0) {
		return EINVAL;
	}
	Buffer* buffer = buffer_create(args->size, args->flags);
	if (buffer == NULL)
		return ENOMEM;
	MutexLocker locker(file->lock);
	args->handle = add_handle(file->buffers, file->bufferCount, buffer);
	if (args->handle == 0) {
		locker.Unlock();
		buffer_release(buffer);
		return ENOMEM;
	}
	return B_OK;
}


static status_t
gem_info(File* file, drm_msm_gem_info* args)
{
	MutexLocker locker(file->lock);
	Buffer* buffer = lookup_buffer(file, args->handle);
	if (buffer == NULL)
		return ENOENT;

	switch (args->info) {
		case MSM_INFO_GET_OFFSET:
			args->value = 0;
			return B_OK;
		case MSM_INFO_GET_IOVA:
		{
			if (buffer->iova == 0) {
				MutexLocker vaLocker(sVaLock);
				uint64 iova = sKernelVaNext;
				if (iova + buffer->size
						> ADRENO_VA_USER_START + ADRENO_VA_USER_SIZE) {
					return ENOMEM;
				}
				status_t status = buffer_map(buffer, iova);
				if (status != B_OK)
					return ENOMEM;
				sKernelVaNext += buffer->size;
			}
			args->value = buffer->iova;
			return B_OK;
		}
		case MSM_INFO_SET_IOVA:
			if (args->value == 0) {
				buffer_unmap(buffer);
				return B_OK;
			}
			if (buffer->iova == args->value)
				return B_OK;
			if (buffer->iova != 0)
				return EBUSY;
			// only in its own window
			if (args->value < file->vaStart
				|| args->value + buffer->size > file->vaStart + file->vaSize) {
				static int32 sReported;
				if (atomic_add(&sReported, 1) < 8) {
					ERROR("SET_IOVA %#" B_PRIx64 "+%#" B_PRIxSIZE " outside the "
						"open's window %#" B_PRIx64 "+%#" B_PRIx64 "\n",
						args->value, buffer->size, file->vaStart, file->vaSize);
				}
				return EINVAL;
			}
			return buffer_map(buffer, args->value) == B_OK ? B_OK : EINVAL;
		case MSM_INFO_GET_FLAGS:
			args->value = buffer->flags;
			return B_OK;
		case MSM_INFO_SET_NAME:
		{
			uint32 length = min_c(args->len, sizeof(buffer->name) - 1);
			char name[sizeof(buffer->name)] = {};
			if (length > 0 && user_memcpy(name, (void*)(addr_t)args->value,
					length) != B_OK) {
				return EFAULT;
			}
			memcpy(buffer->name, name, sizeof(name));
			return B_OK;
		}
		case MSM_INFO_GET_NAME:
			args->len = strlen(buffer->name);
			if (args->value != 0 && user_memcpy((void*)(addr_t)args->value,
					buffer->name, args->len) != B_OK) {
				return EFAULT;
			}
			return B_OK;
		case MSM_INFO_SET_METADATA:
		case MSM_INFO_GET_METADATA:
			return EOPNOTSUPP;
	}
	return EINVAL;
}


static status_t
gem_cpu_prep(File* file, drm_msm_gem_cpu_prep* args)
{
	uint32 fence;
	{
		MutexLocker locker(file->lock);
		Buffer* buffer = lookup_buffer(file, args->handle);
		if (buffer == NULL)
			return ENOENT;
		fence = buffer->lastFence;
	}
	if ((args->op & MSM_PREP_NOSYNC) != 0)
		return fence_signaled(fence) ? B_OK : EBUSY;
	status_t status = adreno_wait_fence(fence,
		deadline_from_timespec(args->timeout));
	if (status == B_TIMED_OUT)
		return ETIMEDOUT;
	if (status == B_DEV_NOT_READY)
		return EIO;
	return status == B_OK ? B_OK : EINTR;
}


static status_t
gem_submit(File* file, drm_msm_gem_submit* args)
{
	if ((args->flags & MSM_PIPE_ID_MASK) != MSM_PIPE_3D0
		|| (args->flags & (MSM_SUBMIT_FENCE_FD_IN | MSM_SUBMIT_FENCE_FD_OUT
			| MSM_SUBMIT_FENCE_SN_IN)) != 0
		|| args->nr_bos > MAX_SUBMIT_BOS || args->nr_cmds > MAX_SUBMIT_CMDS
		|| args->nr_in_syncobjs > MAX_SYNCOBJ_HANDLES
		|| args->nr_out_syncobjs > MAX_SYNCOBJ_HANDLES) {
		return EINVAL;
	}
	uint32 stride = args->syncobj_stride;
	if ((args->nr_in_syncobjs != 0 || args->nr_out_syncobjs != 0)
		&& stride < sizeof(drm_msm_syncobj)) {
		return EINVAL;
	}

	drm_msm_gem_submit_bo* bos = (drm_msm_gem_submit_bo*)malloc(
		max_c(1, args->nr_bos) * sizeof(drm_msm_gem_submit_bo));
	drm_msm_gem_submit_cmd* cmds = (drm_msm_gem_submit_cmd*)malloc(
		max_c(1, args->nr_cmds) * sizeof(drm_msm_gem_submit_cmd));
	drm_msm_syncobj* in = (drm_msm_syncobj*)calloc(
		max_c(1, args->nr_in_syncobjs), sizeof(drm_msm_syncobj));
	drm_msm_syncobj* out = (drm_msm_syncobj*)calloc(
		max_c(1, args->nr_out_syncobjs), sizeof(drm_msm_syncobj));
	Buffer** buffers = (Buffer**)calloc(max_c(1, args->nr_bos),
		sizeof(Buffer*));
	status_t status = B_OK;
	if (bos == NULL || cmds == NULL || in == NULL || out == NULL
		|| buffers == NULL) {
		status = ENOMEM;
	} else if (user_memcpy(bos, (void*)(addr_t)args->bos,
			args->nr_bos * sizeof(*bos)) != B_OK
		|| user_memcpy(cmds, (void*)(addr_t)args->cmds,
			args->nr_cmds * sizeof(*cmds)) != B_OK) {
		status = EFAULT;
	}
	for (uint32 i = 0; status == B_OK && i < args->nr_in_syncobjs; i++) {
		if (user_memcpy(&in[i], (uint8*)(addr_t)args->in_syncobjs + i * stride,
				sizeof(drm_msm_syncobj)) != B_OK) {
			status = EFAULT;
		}
	}
	for (uint32 i = 0; status == B_OK && i < args->nr_out_syncobjs; i++) {
		if (user_memcpy(&out[i],
				(uint8*)(addr_t)args->out_syncobjs + i * stride,
				sizeof(drm_msm_syncobj)) != B_OK) {
			status = EFAULT;
		}
	}

	MutexLocker locker(file->lock);
	for (uint32 i = 0; status == B_OK && i < args->nr_bos; i++) {
		buffers[i] = lookup_buffer(file, bos[i].handle);
		if (buffers[i] == NULL || buffers[i]->iova == 0) {
			static int32 sReported;
			if (atomic_add(&sReported, 1) < 8) {
				ERROR("submit: buffer %" B_PRIu32 " (handle %" B_PRIu32 ") %s\n",
					i, bos[i].handle, buffers[i] == NULL
						? "does not exist" : "has no address");
			}
			buffers[i] = NULL;
			status = EINVAL;
		}
		else {
			atomic_add(&buffers[i]->references, 1);
			bos[i].presumed = buffers[i]->iova;
		}
	}
	uint32 ibs = 0;
	for (uint32 i = 0; status == B_OK && i < args->nr_cmds; i++) {
		const drm_msm_gem_submit_cmd& cmd = cmds[i];
		if (cmd.nr_relocs != 0 || cmd.submit_idx >= args->nr_bos
			|| (cmd.size & 3) != 0 || cmd.size == 0
			|| (uint64)cmd.submit_offset + cmd.size
				> buffers[cmd.submit_idx]->size) {
			static int32 sReported;
			if (atomic_add(&sReported, 1) < 8) {
				ERROR("submit: command %" B_PRIu32 " refused: type %" B_PRIu32
					" buffer %" B_PRIu32 " offset %#" B_PRIx32 " size %#" B_PRIx32
					" relocs %" B_PRIu32 "\n", i, cmd.type, cmd.submit_idx,
					cmd.submit_offset, cmd.size, cmd.nr_relocs);
			}
			status = EINVAL;
		} else if (cmd.type == MSM_SUBMIT_CMD_BUF
			|| cmd.type == MSM_SUBMIT_CMD_CTX_RESTORE_BUF) {
			ibs++;
		}
	}

	// in-fences come from this ring, which runs them in order: only that
	// they were submitted matters
	MutexLocker syncLocker(sSyncLock);
	for (uint32 i = 0; status == B_OK && i < args->nr_in_syncobjs; i++) {
		Syncobj* syncobj = lookup_syncobj(file, in[i].handle);
		if (syncobj == NULL) {
			status = EINVAL;
			break;
		}
		bool submitted, signaled;
		uint32 fence;
		syncobj_state(syncobj, in[i].point, submitted, signaled, fence);
		if (!submitted)
			status = EINVAL;
	}
	for (uint32 i = 0; status == B_OK && i < args->nr_out_syncobjs; i++) {
		if (lookup_syncobj(file, out[i].handle) == NULL)
			status = EINVAL;
	}
	syncLocker.Unlock();

	if (status == B_OK && (adreno_gpu_lost()
			|| adreno_faults() != queue_losses(file, args->queueid))) {
		status = EIO;
	}
	if (status == B_OK) {
		clean_command_buffers(buffers, args->nr_bos);
		adreno_ring_lock();
		status = adreno_ring_reserve(4 + ibs * 4 + 8);
		if (status == B_OK) {
			adreno_ring_emit(adreno_pkt7(CP_EVENT_WRITE, 1));
			adreno_ring_emit(EVENT_PC_CCU_INVALIDATE_DEPTH);
			adreno_ring_emit(adreno_pkt7(CP_EVENT_WRITE, 1));
			adreno_ring_emit(EVENT_PC_CCU_INVALIDATE_COLOR);
			for (uint32 i = 0; i < args->nr_cmds; i++) {
				const drm_msm_gem_submit_cmd& cmd = cmds[i];
				if (cmd.type != MSM_SUBMIT_CMD_BUF
					&& cmd.type != MSM_SUBMIT_CMD_CTX_RESTORE_BUF) {
					continue;
				}
				uint64 iova = buffers[cmd.submit_idx]->iova + cmd.submit_offset;
				adreno_ring_emit(adreno_pkt7(CP_INDIRECT_BUFFER, 3));
				adreno_ring_emit((uint32)iova);
				adreno_ring_emit((uint32)(iova >> 32));
				adreno_ring_emit((cmd.size / 4) & 0xfffff);
			}
			args->fence = adreno_ring_flush_fence();
			file->lastFence = args->fence;
		} else
			status = EBUSY;
		adreno_ring_unlock();
	}

	if (status == B_OK) {
		for (uint32 i = 0; i < args->nr_bos; i++)
			buffers[i]->lastFence = args->fence;

		syncLocker.Lock();
		for (uint32 i = 0; i < args->nr_in_syncobjs; i++) {
			Syncobj* syncobj = lookup_syncobj(file, in[i].handle);
			if ((in[i].flags & MSM_SYNCOBJ_RESET) != 0
				&& syncobj != NULL) {
				syncobj->count = 0;
				syncobj->hasFence = false;
			}
		}
		for (uint32 i = 0; i < args->nr_out_syncobjs; i++) {
			Syncobj* syncobj = lookup_syncobj(file, out[i].handle);
			if (syncobj != NULL)
				syncobj_add_point(syncobj, out[i].point, args->fence);
		}
		sSyncChanged.NotifyAll();
		syncLocker.Unlock();

		// tell userland where its buffers are, as Linux does
		if (args->nr_bos > 0)
			user_memcpy((void*)(addr_t)args->bos, bos,
				args->nr_bos * sizeof(*bos));
	}
	locker.Unlock();

	for (uint32 i = 0; buffers != NULL && i < args->nr_bos; i++) {
		if (buffers[i] != NULL)
			buffer_release(buffers[i]);
	}
	free(buffers);
	free(out);
	free(in);
	free(cmds);
	free(bos);
	return status;
}


static status_t
wait_fence(File* file, drm_msm_wait_fence* args)
{
	if ((args->flags & ~MSM_WAIT_FENCE_BOOST) != 0)
		return EINVAL;
	if ((int32)(args->fence - file->lastFence) > 0
		&& (int32)(args->fence - adreno_completed_fence()) > 0) {
		// a fence that was never handed out
		return EINVAL;
	}
	status_t status = adreno_wait_fence(args->fence,
		deadline_from_timespec(args->timeout));
	if (status == B_TIMED_OUT)
		return ETIMEDOUT;
	if (status == B_DEV_NOT_READY)
		return EIO;
	return status == B_OK ? B_OK : EINTR;
}


//	#pragma mark - sync object ioctls


static status_t
syncobj_create(File* file, drm_syncobj_create* args)
{
	if ((args->flags & ~DRM_SYNCOBJ_CREATE_SIGNALED) != 0)
		return EINVAL;
	Syncobj* syncobj = (Syncobj*)calloc(1, sizeof(Syncobj));
	if (syncobj == NULL)
		return ENOMEM;
	syncobj->references = 1;
	if ((args->flags & DRM_SYNCOBJ_CREATE_SIGNALED) != 0)
		syncobj_set_fence(syncobj, 0);
	MutexLocker locker(file->lock);
	args->handle = add_handle(file->syncobjs, file->syncobjCount, syncobj);
	if (args->handle == 0) {
		free(syncobj);
		return ENOMEM;
	}
	return B_OK;
}


static status_t
syncobj_destroy(File* file, drm_syncobj_destroy* args)
{
	MutexLocker locker(file->lock);
	Syncobj* syncobj = lookup_syncobj(file, args->handle);
	if (syncobj == NULL)
		return EINVAL;
	file->syncobjs[args->handle - 1] = NULL;
	MutexLocker syncLocker(sSyncLock);
	free(syncobj);
	return B_OK;
}


/*!	Waits until all (\a waitAll) or any of the sync objects' points are
	signaled (or have a fence, \a available). Under the file lock.
*/
static status_t
syncobj_wait_array(File* file, const uint32* handles, const uint64* points,
	uint32 count, uint32 flags, bigtime_t deadline, uint32* _first)
{
	bool waitAll = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;
	bool forSubmit = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT) != 0;
	bool available = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE) != 0;

	while (true) {
		MutexLocker syncLocker(sSyncLock);
		uint32 done = 0, first = count;
		uint32 waitFence = 0;
		for (uint32 i = 0; i < count; i++) {
			Syncobj* syncobj = lookup_syncobj(file, handles[i]);
			if (syncobj == NULL)
				return EINVAL;
			bool submitted, signaled;
			uint32 fence;
			syncobj_state(syncobj, points != NULL ? points[i] : 0, submitted,
				signaled, fence);
			if (!submitted && !forSubmit)
				return EINVAL;
			bool ready = available ? submitted : signaled;
			if (ready) {
				done++;
				if (first == count)
					first = i;
			} else if (submitted && waitFence == 0)
				waitFence = fence;
		}
		if ((waitAll && done == count) || (!waitAll && done > 0)) {
			if (_first != NULL)
				*_first = first;
			return B_OK;
		}
		bigtime_t now = system_time();
		if (deadline != B_INFINITE_TIMEOUT && now >= deadline)
			return ETIME;
		// its fences never come
		if (adreno_gpu_lost())
			return EIO;

		// wait for a fence or a change of the sync objects
		bigtime_t until = now + 2000;
		if (deadline != B_INFINITE_TIMEOUT && deadline < until)
			until = deadline;
		if (waitFence != 0) {
			syncLocker.Unlock();
			mutex_unlock(&file->lock);
			adreno_wait_fence(waitFence, until);
			mutex_lock(&file->lock);
		} else {
			ConditionVariableEntry entry;
			sSyncChanged.Add(&entry);
			syncLocker.Unlock();
			mutex_unlock(&file->lock);
			status_t status = entry.Wait(B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT,
				until);
			mutex_lock(&file->lock);
			if (status == B_INTERRUPTED)
				return EINTR;
		}
	}
}


template<typename T> static status_t
copy_array(T*& array, uint64 userAddress, uint32 count)
{
	array = (T*)malloc(max_c(1, count) * sizeof(T));
	if (array == NULL)
		return ENOMEM;
	if (count > 0 && user_memcpy(array, (void*)(addr_t)userAddress,
			count * sizeof(T)) != B_OK) {
		free(array);
		array = NULL;
		return EFAULT;
	}
	return B_OK;
}


static status_t
syncobj_wait(File* file, drm_syncobj_wait* args, bool timeline,
	drm_syncobj_timeline_wait* timelineArgs)
{
	uint64 handlesAddress = timeline ? timelineArgs->handles : args->handles;
	uint32 count = timeline ? timelineArgs->count_handles : args->count_handles;
	uint32 flags = timeline ? timelineArgs->flags : args->flags;
	int64 timeout = timeline ? timelineArgs->timeout_nsec : args->timeout_nsec;
	if (count == 0 || count > MAX_SYNCOBJ_HANDLES)
		return EINVAL;

	uint32* handles;
	uint64* points = NULL;
	status_t status = copy_array(handles, handlesAddress, count);
	if (status != B_OK)
		return status;
	if (timeline) {
		status = copy_array(points, timelineArgs->points, count);
		if (status != B_OK) {
			free(handles);
			return status;
		}
	}
	uint32 first = 0;
	MutexLocker locker(file->lock);
	status = syncobj_wait_array(file, handles, points, count, flags,
		deadline_from_nanoseconds(timeout), &first);
	locker.Unlock();
	if (timeline)
		timelineArgs->first_signaled = first;
	else
		args->first_signaled = first;
	free(points);
	free(handles);
	return status;
}


static status_t
syncobj_array_op(File* file, uint64 handlesAddress, uint64 pointsAddress,
	uint32 count, uint32 op)
{
	if (count == 0 || count > MAX_SYNCOBJ_HANDLES)
		return EINVAL;
	uint32* handles;
	uint64* points = NULL;
	status_t status = copy_array(handles, handlesAddress, count);
	if (status != B_OK)
		return status;
	if (pointsAddress != 0) {
		status = copy_array(points, pointsAddress, count);
		if (status != B_OK) {
			free(handles);
			return status;
		}
	}

	MutexLocker locker(file->lock);
	MutexLocker syncLocker(sSyncLock);
	for (uint32 i = 0; status == B_OK && i < count; i++) {
		Syncobj* syncobj = lookup_syncobj(file, handles[i]);
		if (syncobj == NULL) {
			status = EINVAL;
			break;
		}
		switch (op) {
			case ADRENO_HAIKU_NR_SYNCOBJ_RESET:
				syncobj->count = 0;
				syncobj->hasFence = false;
				break;
			case ADRENO_HAIKU_NR_SYNCOBJ_SIGNAL:
				syncobj_set_fence(syncobj, 0);
				break;
			case ADRENO_HAIKU_NR_SYNCOBJ_TIMELINE_SIGNAL:
				status = syncobj_add_point(syncobj, points[i], 0);
				break;
			case ADRENO_HAIKU_NR_SYNCOBJ_QUERY:
			{
				syncobj_collect(syncobj);
				points[i] = syncobj->completed;
				break;
			}
		}
	}
	sSyncChanged.NotifyAll();
	syncLocker.Unlock();
	locker.Unlock();
	if (status == B_OK && op == ADRENO_HAIKU_NR_SYNCOBJ_QUERY
		&& user_memcpy((void*)(addr_t)pointsAddress, points,
			count * sizeof(uint64)) != B_OK) {
		status = EFAULT;
	}
	free(points);
	free(handles);
	return status;
}


static status_t
syncobj_transfer(File* file, drm_syncobj_transfer* args)
{
	MutexLocker locker(file->lock);
	MutexLocker syncLocker(sSyncLock);
	Syncobj* source = lookup_syncobj(file, args->src_handle);
	Syncobj* destination = lookup_syncobj(file, args->dst_handle);
	if (source == NULL || destination == NULL)
		return EINVAL;
	bool submitted, signaled;
	uint32 fence;
	syncobj_state(source, args->src_point, submitted, signaled, fence);
	if (!submitted)
		return EINVAL;
	status_t status = syncobj_add_point(destination, args->dst_point,
		signaled ? 0 : fence);
	sSyncChanged.NotifyAll();
	return status;
}


//	#pragma mark - device hooks


static status_t
adreno_open(const char* name, uint32 flags, void** _cookie)
{
	// The GPU comes up at the first request, not here: app_server opens
	// every graphics device at boot to ask for an accelerant, before the
	// firmware may be in place. A lost GPU takes no new users: their
	// OpenGL falls back to software.
	if (adreno_gpu_lost())
		return B_DEV_NOT_READY;
	File* file = (File*)calloc(1, sizeof(File));
	if (file == NULL)
		return B_NO_MEMORY;
	{
		MutexLocker vaLocker(sVaLock);
		team_id team = team_get_current_team_id();
		file->vaWindow = -1;
		for (uint32 i = 0; i < FILE_VA_WINDOWS; i++) {
			if (sVaWindows[i].opens > 0 && sVaWindows[i].team == team) {
				file->vaWindow = i;
				break;
			}
		}
		for (uint32 i = 0; file->vaWindow < 0 && i < FILE_VA_WINDOWS; i++) {
			if (sVaWindows[i].opens == 0) {
				sVaWindows[i].team = team;
				file->vaWindow = i;
			}
		}
		if (file->vaWindow >= 0)
			sVaWindows[file->vaWindow].opens++;
	}
	if (file->vaWindow < 0) {
		ERROR("no GPU address window left for another team\n");
		free(file);
		return B_BUSY;
	}
	file->vaStart = ADRENO_VA_USER_START + file->vaWindow * FILE_VA_WINDOW;
	file->vaSize = FILE_VA_WINDOW;
	mutex_init(&file->lock, "adreno file");
	file->nextQueue = 1;
	file->openLosses = adreno_faults();
	for (uint32 i = 0; i < QUEUE_SLOTS; i++)
		file->queueLosses[i] = file->openLosses;
	*_cookie = file;
	return B_OK;
}


static status_t
adreno_close(void* cookie)
{
	return B_OK;
}


static status_t
adreno_free(void* cookie)
{
	File* file = (File*)cookie;
	mutex_lock(&file->lock);
	for (uint32 i = 0; i < file->bufferCount; i++) {
		if (file->buffers[i] != NULL)
			buffer_release(file->buffers[i]);
	}
	free(file->buffers);
	mutex_lock(&sSyncLock);
	for (uint32 i = 0; i < file->syncobjCount; i++)
		free(file->syncobjs[i]);
	mutex_unlock(&sSyncLock);
	free(file->syncobjs);
	mutex_destroy(&file->lock);
	{
		MutexLocker vaLocker(sVaLock);
		sVaWindows[file->vaWindow].opens--;
	}
	free(file);
	return B_OK;
}


static status_t
adreno_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_SUPPORTED;
}


static status_t
adreno_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_SUPPORTED;
}


static status_t
map_bo(File* file, adreno_haiku_map_bo* args)
{
	if (args->flags != 0 || args->reserved != 0)
		return EINVAL;
	MutexLocker locker(file->lock);
	Buffer* buffer = lookup_buffer(file, args->handle);
	if (buffer == NULL)
		return ENOENT;
	void* address = (void*)(addr_t)args->address;
	bool exact = address != NULL;
	if (exact && !IS_USER_ADDRESS(address))
		return EFAULT;
	area_id area = vm_clone_area(team_get_current_team_id(), "adreno buffer",
		&address, exact ? B_EXACT_ADDRESS : B_RANDOMIZED_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, REGION_NO_PRIVATE_MAP, buffer->area, true);
	if (area < 0)
		return area;
	args->area = area;
	args->address = (addr_t)address;
	args->size = buffer->size;
	return B_OK;
}


static status_t
dispatch(File* file, uint32 nr, void* data)
{
	switch (nr) {
		case ADRENO_HAIKU_NR_VERSION:
		{
			drm_version* version = (drm_version*)data;
			version->version_major = DRIVER_MAJOR;
			version->version_minor = DRIVER_MINOR;
			version->version_patchlevel = 0;
			const char* strings[3] = { DRIVER_NAME, "20261024",
				"Qualcomm Adreno (air/OS)" };
			uint64 addresses[3] = { (addr_t)version->name,
				(addr_t)version->date, (addr_t)version->desc };
			__kernel_size_t* lengths[3] = { &version->name_len,
				&version->date_len, &version->desc_len };
			for (int i = 0; i < 3; i++) {
				size_t length = strlen(strings[i]);
				if (addresses[i] != 0 && *lengths[i] > 0
					&& user_memcpy((void*)(addr_t)addresses[i], strings[i],
						min_c(length, *lengths[i])) != B_OK) {
					return EFAULT;
				}
				*lengths[i] = length;
			}
			return B_OK;
		}
		case ADRENO_HAIKU_NR_GEM_CLOSE:
		{
			drm_gem_close* args = (drm_gem_close*)data;
			MutexLocker locker(file->lock);
			Buffer* buffer = lookup_buffer(file, args->handle);
			if (buffer == NULL)
				return EINVAL;
			file->buffers[args->handle - 1] = NULL;
			locker.Unlock();
			buffer_release(buffer);
			return B_OK;
		}
		case ADRENO_HAIKU_NR_GET_CAP:
		{
			drm_get_cap* cap = (drm_get_cap*)data;
			if (cap->capability == DRM_CAP_SYNCOBJ
				|| cap->capability == DRM_CAP_SYNCOBJ_TIMELINE) {
				cap->value = 1;
				return B_OK;
			}
			return EINVAL;
		}
		case ADRENO_HAIKU_NR_SYNCOBJ_CREATE:
			return syncobj_create(file, (drm_syncobj_create*)data);
		case ADRENO_HAIKU_NR_SYNCOBJ_DESTROY:
			return syncobj_destroy(file, (drm_syncobj_destroy*)data);
		case ADRENO_HAIKU_NR_SYNCOBJ_WAIT:
			return syncobj_wait(file, (drm_syncobj_wait*)data, false, NULL);
		case ADRENO_HAIKU_NR_SYNCOBJ_TIMELINE_WAIT:
			return syncobj_wait(file, NULL, true,
				(drm_syncobj_timeline_wait*)data);
		case ADRENO_HAIKU_NR_SYNCOBJ_RESET:
		case ADRENO_HAIKU_NR_SYNCOBJ_SIGNAL:
		{
			drm_syncobj_array* args = (drm_syncobj_array*)data;
			return syncobj_array_op(file, args->handles, 0, args->count_handles,
				nr);
		}
		case ADRENO_HAIKU_NR_SYNCOBJ_TIMELINE_SIGNAL:
		case ADRENO_HAIKU_NR_SYNCOBJ_QUERY:
		{
			drm_syncobj_timeline_array* args
				= (drm_syncobj_timeline_array*)data;
			if (args->points == 0)
				return EINVAL;
			return syncobj_array_op(file, args->handles, args->points,
				args->count_handles, nr);
		}
		case ADRENO_HAIKU_NR_SYNCOBJ_TRANSFER:
			return syncobj_transfer(file, (drm_syncobj_transfer*)data);

		case DRM_COMMAND_BASE + DRM_MSM_GET_PARAM:
			return get_param(file, (drm_msm_param*)data);
		case DRM_COMMAND_BASE + DRM_MSM_SET_PARAM:
			return set_param(file, (drm_msm_param*)data);
		case DRM_COMMAND_BASE + DRM_MSM_GEM_NEW:
			return gem_new(file, (drm_msm_gem_new*)data);
		case DRM_COMMAND_BASE + DRM_MSM_GEM_INFO:
			return gem_info(file, (drm_msm_gem_info*)data);
		case DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_PREP:
			return gem_cpu_prep(file, (drm_msm_gem_cpu_prep*)data);
		case DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_FINI:
			return B_OK;
		case DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT:
			return gem_submit(file, (drm_msm_gem_submit*)data);
		case DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE:
			return wait_fence(file, (drm_msm_wait_fence*)data);
		case DRM_COMMAND_BASE + DRM_MSM_GEM_MADVISE:
		{
			drm_msm_gem_madvise* args = (drm_msm_gem_madvise*)data;
			MutexLocker locker(file->lock);
			if (lookup_buffer(file, args->handle) == NULL)
				return ENOENT;
			// nothing is ever purged
			args->retained = 1;
			return B_OK;
		}
		case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_NEW:
		{
			drm_msm_submitqueue* args = (drm_msm_submitqueue*)data;
			if (args->prio != 0)
				return EINVAL;
			MutexLocker locker(file->lock);
			args->id = file->nextQueue++;
			if (args->id < QUEUE_SLOTS)
				file->queueLosses[args->id] = adreno_faults();
			return B_OK;
		}
		case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_CLOSE:
			return B_OK;
		case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_QUERY:
		{
			drm_msm_submitqueue_query* args = (drm_msm_submitqueue_query*)data;
			if (args->param != MSM_SUBMITQUEUE_PARAM_FAULTS)
				return EINVAL;
			uint32 faults;
			{
				MutexLocker locker(file->lock);
				faults = adreno_faults() - queue_losses(file, args->id);
			}
			if (args->data != 0 && args->len >= sizeof(faults)
				&& user_memcpy((void*)(addr_t)args->data, &faults,
					sizeof(faults)) != B_OK) {
				return EFAULT;
			}
			args->len = sizeof(faults);
			return B_OK;
		}

		case ADRENO_HAIKU_NR_MAP_BO:
			return map_bo(file, (adreno_haiku_map_bo*)data);
	}
	return B_DEV_INVALID_IOCTL;
}


static size_t
argument_size(uint32 nr)
{
	switch (nr) {
		case ADRENO_HAIKU_NR_VERSION:	return sizeof(drm_version);
		case ADRENO_HAIKU_NR_GEM_CLOSE:	return sizeof(drm_gem_close);
		case ADRENO_HAIKU_NR_GET_CAP:	return sizeof(drm_get_cap);
		case ADRENO_HAIKU_NR_SYNCOBJ_CREATE: return sizeof(drm_syncobj_create);
		case ADRENO_HAIKU_NR_SYNCOBJ_DESTROY:
			return sizeof(drm_syncobj_destroy);
		case ADRENO_HAIKU_NR_SYNCOBJ_WAIT:	return sizeof(drm_syncobj_wait);
		case ADRENO_HAIKU_NR_SYNCOBJ_TIMELINE_WAIT:
			return sizeof(drm_syncobj_timeline_wait);
		case ADRENO_HAIKU_NR_SYNCOBJ_RESET:
		case ADRENO_HAIKU_NR_SYNCOBJ_SIGNAL:
			return sizeof(drm_syncobj_array);
		case ADRENO_HAIKU_NR_SYNCOBJ_TIMELINE_SIGNAL:
		case ADRENO_HAIKU_NR_SYNCOBJ_QUERY:
			return sizeof(drm_syncobj_timeline_array);
		case ADRENO_HAIKU_NR_SYNCOBJ_TRANSFER:
			return sizeof(drm_syncobj_transfer);
		case DRM_COMMAND_BASE + DRM_MSM_GET_PARAM:
		case DRM_COMMAND_BASE + DRM_MSM_SET_PARAM:
			return sizeof(drm_msm_param);
		case DRM_COMMAND_BASE + DRM_MSM_GEM_NEW:
			return sizeof(drm_msm_gem_new);
		case DRM_COMMAND_BASE + DRM_MSM_GEM_INFO:
			return sizeof(drm_msm_gem_info);
		case DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_PREP:
			return sizeof(drm_msm_gem_cpu_prep);
		case DRM_COMMAND_BASE + DRM_MSM_GEM_CPU_FINI:
			return sizeof(drm_msm_gem_cpu_fini);
		case DRM_COMMAND_BASE + DRM_MSM_GEM_SUBMIT:
			return sizeof(drm_msm_gem_submit);
		case DRM_COMMAND_BASE + DRM_MSM_WAIT_FENCE:
			return sizeof(drm_msm_wait_fence);
		case DRM_COMMAND_BASE + DRM_MSM_GEM_MADVISE:
			return sizeof(drm_msm_gem_madvise);
		case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_NEW:
			return sizeof(drm_msm_submitqueue);
		case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_CLOSE:
			return sizeof(uint32);
		case DRM_COMMAND_BASE + DRM_MSM_SUBMITQUEUE_QUERY:
			return sizeof(drm_msm_submitqueue_query);
		case ADRENO_HAIKU_NR_MAP_BO:
			return sizeof(adreno_haiku_map_bo);
	}
	return 0;
}


/*!	As drm_ioctl(): \a length bytes copied in, zero-extended to the
	driver's structure, then back out.
*/
static status_t
adreno_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	File* file = (File*)cookie;
	if (!ADRENO_HAIKU_IS_OP(op))
		return B_DEV_INVALID_IOCTL;
	uint32 nr = ADRENO_HAIKU_OP_NR(op);
	size_t size = argument_size(nr);
	if (size == 0)
		return B_DEV_INVALID_IOCTL;

	uint8 data[256];
	if (size > sizeof(data))
		return B_BAD_VALUE;
	memset(data, 0, size);
	size_t copy = min_c(length, size);
	if (copy > 0 && (buffer == NULL || !IS_USER_ADDRESS(buffer)
			|| user_memcpy(data, buffer, copy) != B_OK)) {
		return B_BAD_ADDRESS;
	}

	status_t status = adreno_gpu_start();
	if (status == B_OK)
		status = dispatch(file, nr, data);
	if (copy > 0 && user_memcpy(buffer, data, copy) != B_OK && status == B_OK)
		status = B_BAD_ADDRESS;
	return status;
}


static device_hooks sDeviceHooks = {
	adreno_open,
	adreno_close,
	adreno_free,
	adreno_control,
	adreno_read,
	adreno_write,
};


//	#pragma mark - driver


status_t
init_hardware()
{
	if (gFDT == NULL
		|| fdt_node_offset_by_compatible(gFDT, -1, "qcom,adreno-690.0") < 0) {
		return B_DEVICE_NOT_FOUND;
	}
	return B_OK;
}


status_t
init_driver()
{
	sSyncChanged.Init(&sSyncChanged, "adreno syncobjs");
	return B_OK;
}


void
uninit_driver()
{
}


const char**
publish_devices()
{
	return sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	return &sDeviceHooks;
}
