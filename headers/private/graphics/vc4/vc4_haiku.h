/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _VC4_HAIKU_H
#define _VC4_HAIKU_H


/*	The interface of the VideoCore IV 3D device (/dev/graphics/vc4/0), the
	3D core of the BCM2835/6/7 (Raspberry Pi 1 to 3).

	The requests carry the structures of Linux' DRM VC4 interface
	(vc4_drm.h, MIT licensed, part of Mesa's sources), so Mesa's vc4 driver
	needs no translation; request VC4_HAIKU_FIRST_OP + n is DRM_VC4_* n.
	What differs:

	- Buffers are mapped by the driver: VC4_HAIKU_MMAP_BO maps the buffer
	  into the calling team and returns the address in drm_vc4_mmap_bo's
	  "offset". The mapping belongs to the driver: the team cannot delete,
	  resize or protect it, and it goes away when the handle is closed
	  (VC4_HAIKU_CLOSE_BO) or the device is. Shader buffers map read-only.
	- There are no DRM sync objects or sync files: a job is waited for by
	  its sequence number (DRM_VC4_WAIT_SEQNO), as Mesa does without
	  DRM_CAP_SYNCOBJ. in_sync/out_sync must be 0.
	- No flink names, PRIME, performance monitors; GEM_MADVISE is accepted
	  but never purges (DRM_VC4_PARAM_SUPPORTS_MADVISE is 0).
	- Errors are Haiku's: B_BAD_VALUE where Linux says EINVAL, B_TIMED_OUT
	  for ETIME, B_INTERRUPTED for EINTR (repeat the request; a wait has
	  written the time that is left back into the request). */


#include <Drivers.h>
#include <SupportDefs.h>


#define VC4_HAIKU_DEVICE_PATH	"/dev/graphics/vc4/0"

enum {
	VC4_HAIKU_FIRST_OP = B_DEVICE_OP_CODES_END + 0x7700,

	// struct drm_vc4_*, numbered as DRM_VC4_*
	VC4_HAIKU_SUBMIT_CL = VC4_HAIKU_FIRST_OP,
	VC4_HAIKU_WAIT_SEQNO,
	VC4_HAIKU_WAIT_BO,
	VC4_HAIKU_CREATE_BO,
	VC4_HAIKU_MMAP_BO,
	VC4_HAIKU_CREATE_SHADER_BO,
	VC4_HAIKU_GET_HANG_STATE,
	VC4_HAIKU_GET_PARAM,
	VC4_HAIKU_SET_TILING,
	VC4_HAIKU_GET_TILING,
	VC4_HAIKU_LABEL_BO,
	VC4_HAIKU_GEM_MADVISE,

	// struct vc4_haiku_handle: DRM_IOCTL_GEM_CLOSE
	VC4_HAIKU_CLOSE_BO = VC4_HAIKU_FIRST_OP + 0x20,
	// struct vc4_haiku_info
	VC4_HAIKU_GET_INFO,
};

struct vc4_haiku_handle {
	uint32	handle;
	uint32	pad;
};

/*	VC4_HAIKU_GET_INFO: what the driver has to work with, for tests and
	diagnostics. */
struct vc4_haiku_info {
	uint32	ident[3];
		// V3D_IDENT0..2
	uint32	clock_rate;
		// Hz, as the firmware reports it
	uint64	pool_size;
		// the contiguous memory reserved for buffers, bytes
	uint64	pool_free;
	uint64	contiguous_bytes;
		// buffers outside the pool, in contiguous areas of their own
	uint32	bin_size;
		// the binner's memory, bytes
	uint32	bin_slots_used;
		// bit mask of its 512 KB slots
	uint64	submitted;
		// sequence number of the last job submitted
	uint64	completed;
		// ... and completed
	uint32	resets;
		// times the core was reset after a job hung
	uint32	interrupt;
	uint64	interrupts;
		// handled so far
};

// DRM_FORMAT_MOD_BROADCOM_VC4_T_TILED (drm_fourcc.h), for SET/GET_TILING
#define VC4_HAIKU_MOD_LINEAR		0ull
#define VC4_HAIKU_MOD_T_TILED		((0x07ull << 56) | 1)


#endif	/* _VC4_HAIKU_H */
