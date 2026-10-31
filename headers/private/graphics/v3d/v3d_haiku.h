/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _V3D_HAIKU_H
#define _V3D_HAIKU_H


/*	The interface of the V3D render device (/dev/graphics/v3d/0).

	The requests carry the structures of Linux' DRM V3D interface
	(v3d_drm.h, MIT licensed, part of Mesa's sources), so Mesa's v3d and v3dv
	drivers need no translation. What differs:

	- Buffers are mapped by the driver: V3D_HAIKU_MMAP_BO clones the buffer
	  into the calling team and returns the address in drm_v3d_mmap_bo's
	  "offset". Unmap with delete_area(area_for(address)).
	- There are no DRM sync objects or sync files. Jobs run strictly in the
	  order they were submitted and every job has a sequence number; a
	  "sync object" here just remembers the number of the last job it was
	  given as out_sync (also through the multi-sync extension), or that it
	  was signalled or reset by hand. in_sync handles are accepted and
	  ignored: the order already guarantees what they ask for.
	- No PRIME/dma-buf. Memory of the calling team can be imported as a
	  buffer object instead (V3D_HAIKU_IMPORT_BO): a frame buffer cloned from
	  the display driver, say, or a clone of another team's area. */


#include <Drivers.h>
#include <SupportDefs.h>


#define V3D_HAIKU_DEVICE_PATH	"/dev/graphics/v3d/0"

enum {
	V3D_HAIKU_FIRST_OP = B_DEVICE_OP_CODES_END + 0x7600,

	// struct drm_v3d_*
	V3D_HAIKU_GET_PARAM = V3D_HAIKU_FIRST_OP,
	V3D_HAIKU_CREATE_BO,
	V3D_HAIKU_MMAP_BO,
	V3D_HAIKU_GET_BO_OFFSET,
	V3D_HAIKU_WAIT_BO,
	V3D_HAIKU_SUBMIT_CL,
	V3D_HAIKU_SUBMIT_TFU,
	V3D_HAIKU_SUBMIT_CSD,

	// struct v3d_haiku_handle
	V3D_HAIKU_CLOSE_BO,
	V3D_HAIKU_SYNC_CREATE,
	V3D_HAIKU_SYNC_DESTROY,

	// struct v3d_haiku_sync
	V3D_HAIKU_SYNC_WAIT,
		// waits for the job a sync object stands for
	V3D_HAIKU_SYNC_GET,
		// returns that job's sequence number
	V3D_HAIKU_SEQNO_WAIT,
		// waits for a job by its sequence number
	V3D_HAIKU_SYNC_RESET,
		// back to "not signalled, no job"
	V3D_HAIKU_SYNC_SIGNAL,
		// signalled without a job
	V3D_HAIKU_SYNC_TRANSFER,
		// takes over the state of the sync object named in "seqno"
	V3D_HAIKU_CPU_PREPARE,
		// v3d_haiku_handle: wait for GPU ownership, then synchronize for CPU

	// Since V3D_HAIKU_PARAM_IMPORT_BO (1).
	V3D_HAIKU_IMPORT_BO,
		// v3d_haiku_import_bo
	V3D_HAIKU_CPU_PREPARE_RANGES,
		// v3d_haiku_cpu_ranges: CPU_PREPARE for some bytes of a buffer only,
		// or cache maintenance of an imported buffer
};

// Haiku-private capability and allocation flag. Cacheable buffers require
// CPU_PREPARE before EVERY CPU access interval, including an already mapped
// buffer. Submission transfers ownership back to the GPU. They must not be
// used for persistent/coherent mappings or accessed while a job uses them.
#define V3D_HAIKU_PARAM_CACHEABLE_BO	0x10000
#define V3D_HAIKU_BO_CACHEABLE		0x80000000u
// Capability version 2 permits a read-only CPU interval. A zero pad is
// read/write; CPU writes require that mode before the first store.
#define V3D_HAIKU_CPU_READ_ONLY		0x1

// Capability: V3D_HAIKU_IMPORT_BO and V3D_HAIKU_CPU_PREPARE_RANGES (1).
#define V3D_HAIKU_PARAM_IMPORT_BO	0x10001

/*	V3D_HAIKU_IMPORT_BO: pages of the calling team as a buffer object. The
	driver holds them (wired, through a mapping of its own) until the handle
	is closed and the last job using it is done, so the caller may unmap its
	own mapping at any time; the GPU then still writes the pages, which no
	one else gets until then. The buffer cannot be mapped with MMAP_BO: the
	caller has it mapped already.

	Memory the CPU caches (an ordinary area, app_server's copy of the screen)
	gets no cache maintenance of its own: the caller asks for it, for the
	bytes concerned, with CPU_PREPARE_RANGES -- before a job writes them, and
	again once it is done, before the CPU reads what it wrote.
	Write-combining or uncached memory (a frame buffer) needs none. */
struct v3d_haiku_import_bo {
	uint64	address;
		// in: page aligned, in the calling team
	uint64	size;
		// in: rounded up to whole pages
	uint32	flags;
		// out: V3D_HAIKU_IMPORT_CACHED
	uint32	handle;
		// out
	uint32	offset;
		// out: the GPU's address of "address"
	uint32	pad;
};

// v3d_haiku_import_bo.flags: the memory is cached by the CPU
#define V3D_HAIKU_IMPORT_CACHED		0x1

// Bytes of a buffer: "rows" rows of "bytes" bytes each, "stride" bytes from
// one row's start to the next; 0 rows count as 1.
struct v3d_haiku_range {
	uint64	offset;
	uint32	bytes;
	uint32	rows;
	uint32	stride;
	uint32	pad;
};

#define V3D_HAIKU_MAX_RANGES		1024

/*	V3D_HAIKU_CPU_PREPARE_RANGES: CPU_PREPARE for the ranges only. Waits for
	the buffer's last job and hands the ranges' cache lines to the CPU,
	cleaned and invalidated, so a second call in the same interval loses
	nothing; without V3D_HAIKU_CPU_READ_ONLY the next submission cleans what
	was written. On an uncached buffer it only waits.

	On an imported cached buffer the lines are cleaned and invalidated in
	any case: before a job writes the bytes (what the CPU wrote there must
	not be written back over the job's results later), and after it, before
	the CPU reads them. V3D_HAIKU_RANGES_NO_WAIT, for imported buffers only,
	skips the wait: before a job, or when the caller has waited for the job
	it is about (later ones may be using the buffer already). */
struct v3d_haiku_cpu_ranges {
	uint32	handle;
	uint32	flags;
		// V3D_HAIKU_CPU_READ_ONLY, V3D_HAIKU_RANGES_NO_WAIT
	uint64	ranges;
		// user address of "count" v3d_haiku_range
	uint32	count;
	uint32	pad;
};

#define V3D_HAIKU_RANGES_NO_WAIT	0x2

// v3d_haiku_handle.pad of V3D_HAIKU_SYNC_CREATE: the sync object starts
// without a job and not signalled (DRM's default; Vulkan relies on it).
#define V3D_HAIKU_SYNC_UNSIGNALED		0x1
// v3d_haiku_sync.pad of V3D_HAIKU_SYNC_WAIT: a sync object without a job is
// waited for as well, instead of being an error.
#define V3D_HAIKU_SYNC_WAIT_FOR_SUBMIT	0x1

// As the handle of V3D_HAIKU_SEQNO_WAIT: "seqno" is a fence token, the low
// 31 bits of the sequence number of a job submitted earlier.
#define V3D_HAIKU_TOKEN	0xffffffffu

struct v3d_haiku_handle {
	uint32	handle;
	uint32	pad;
};

struct v3d_haiku_sync {
	uint32	handle;
	uint32	pad;
	uint64	seqno;
	int64	timeout_ns;
		// relative; negative waits for good, 0 only asks
};


#endif	/* _V3D_HAIKU_H */
