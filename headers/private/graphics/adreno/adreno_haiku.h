/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _ADRENO_HAIKU_H
#define _ADRENO_HAIKU_H


/*	The Adreno driver's interface to userland: Linux's msm_drm.h and drm.h
	structures, unchanged, behind Haiku ioctl op codes ADRENO_HAIKU_OP(nr),
	nr being the Linux DRM ioctl number: the driver's own ioctls are
	0x40-0x4d (DRM_COMMAND_BASE + DRM_MSM_*), the generic DRM ioctls keep
	theirs. The length argument of ioctl() is the size of the structure the
	request names (IOCPARM_LEN() of the Linux request code); that many bytes
	are copied in, zero-extended to the driver's structure, and copied back
	out, as drm_ioctl() does. Haiku-only ops use nr 0xe0-0xef.

	This is the contract Mesa's Turnip ("msm" kernel interface) and
	freedreno are built against, as for the PowerVR driver (pvr_haiku.h):
		ioctl(fd, ADRENO_HAIKU_OP(request & 0xff), arg, IOCPARM_LEN(request))

	Differences from Linux, all where Haiku has no equivalent:
	- No mmap(): ADRENO_HAIKU_NR_MAP_BO clones a buffer into the caller.
	- MSM_INFO_GET_OFFSET returns 0; only IOVA, SET_IOVA, FLAGS and NAME of
	  DRM_MSM_GEM_INFO are answered.
	- One GPU address space for every open (no per-process page tables
	  yet): each process gets a window of its own, which VA_START/VA_SIZE
	  describe and SET_IOVA must stay in, so that two processes never put
	  buffers at the same address; buffers without one get an address from
	  the driver.
	- Sync objects are per open; there is no DRM prime/dma-buf. */


#include <SupportDefs.h>


#define ADRENO_HAIKU_DEVICE_PATH		"/dev/graphics/adreno/0"
#define ADRENO_HAIKU_DEVICE_ENV			"HAIKU_ADRENO_DEVICE"

// 'A' 'D' 'R' nn
#define ADRENO_HAIKU_OP_BASE			0x41445200u
#define ADRENO_HAIKU_OP(nr)				(ADRENO_HAIKU_OP_BASE | ((nr) & 0xffu))
#define ADRENO_HAIKU_OP_NR(op)			((op) & 0xffu)
#define ADRENO_HAIKU_IS_OP(op)			(((op) & 0xffffff00u) == ADRENO_HAIKU_OP_BASE)

// generic DRM ioctls the driver answers (drm.h numbers and structures)
#define ADRENO_HAIKU_NR_VERSION				0x00	// struct drm_version
#define ADRENO_HAIKU_NR_GEM_CLOSE			0x09	// struct drm_gem_close
#define ADRENO_HAIKU_NR_GET_CAP				0x0c	// struct drm_get_cap
#define ADRENO_HAIKU_NR_SYNCOBJ_CREATE		0xbf
#define ADRENO_HAIKU_NR_SYNCOBJ_DESTROY		0xc0
#define ADRENO_HAIKU_NR_SYNCOBJ_WAIT		0xc3
#define ADRENO_HAIKU_NR_SYNCOBJ_RESET		0xc4
#define ADRENO_HAIKU_NR_SYNCOBJ_SIGNAL		0xc5
#define ADRENO_HAIKU_NR_SYNCOBJ_TIMELINE_WAIT 0xca
#define ADRENO_HAIKU_NR_SYNCOBJ_QUERY		0xcb
#define ADRENO_HAIKU_NR_SYNCOBJ_TRANSFER	0xcc
#define ADRENO_HAIKU_NR_SYNCOBJ_TIMELINE_SIGNAL 0xcd

// the driver's own (msm_drm.h: DRM_COMMAND_BASE 0x40 + DRM_MSM_*)
#define ADRENO_HAIKU_NR_MSM_FIRST			0x40	// DRM_MSM_GET_PARAM
#define ADRENO_HAIKU_NR_MSM_LAST			0x4d	// DRM_MSM_VM_BIND

// Haiku-only
#define ADRENO_HAIKU_NR_MAP_BO		0xe0	// struct adreno_haiku_map_bo

#define ADRENO_HAIKU_ABI_VERSION	1


/*	ADRENO_HAIKU_OP(ADRENO_HAIKU_NR_MAP_BO) replaces GET_OFFSET + mmap():
	the buffer's kernel area is cloned into the calling team, cached
	(the SC8280XP's GPU is I/O coherent) unless the buffer was created with
	MSM_BO_WC or MSM_BO_UNCACHED. With address 0 the clone goes anywhere,
	otherwise exactly there. Unmap with delete_area(area). */
struct adreno_haiku_map_bo {
	uint32	handle;					// in: buffer handle
	int32	area;					// out: the clone's area
	uint64	address;				// in: placement or 0; out: address
	uint64	size;					// out
	uint32	flags;					// in: none yet
	uint32	reserved;
};


#endif	// _ADRENO_HAIKU_H
