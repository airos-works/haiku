/*
 * The parts of Linux's include/uapi/drm/drm.h that the Adreno driver's
 * interface uses (adreno_haiku.h): the structures of the generic DRM ioctls
 * it answers, unchanged.
 *
 * Copyright 1999 Precision Insight, Inc., Cedar Park, Texas.
 * Copyright 2000 VA Linux Systems, Inc., Sunnyvale, California.
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef _ADRENO_DRM_H_
#define _ADRENO_DRM_H_

#include <SupportDefs.h>

typedef int8 __s8;
typedef uint8 __u8;
typedef int16 __s16;
typedef uint16 __u16;
typedef int32 __s32;
typedef uint32 __u32;
typedef int64 __s64;
typedef uint64 __u64;
typedef unsigned long __kernel_size_t;
#define __user

#if defined(__cplusplus)
extern "C" {
#endif

#define DRM_COMMAND_BASE	0x40

/* Linux' request codes are only used for their number and size here
 * (adreno_haiku.h); msm_drm.h's DRM_IOCTL_MSM_* stay unused. */
#define DRM_IOW(nr, type)	(nr)
#define DRM_IOR(nr, type)	(nr)
#define DRM_IOWR(nr, type)	(nr)

struct drm_version {
	int version_major;	  /**< Major version */
	int version_minor;	  /**< Minor version */
	int version_patchlevel;	  /**< Patch level */
	__kernel_size_t name_len;	  /**< Length of name buffer */
	char __user *name;	  /**< Name of driver */
	__kernel_size_t date_len;	  /**< Length of date buffer */
	char __user *date;	  /**< User-space buffer to hold date */
	__kernel_size_t desc_len;	  /**< Length of desc buffer */
	char __user *desc;	  /**< User-space buffer to hold desc */
};

struct drm_gem_close {
	__u32 handle;
	__u32 pad;
};

#define DRM_CAP_SYNCOBJ		0x13
#define DRM_CAP_SYNCOBJ_TIMELINE	0x14

struct drm_get_cap {
	__u64 capability;
	__u64 value;
};

struct drm_syncobj_create {
	__u32 handle;
#define DRM_SYNCOBJ_CREATE_SIGNALED (1 << 0)
	__u32 flags;
};

struct drm_syncobj_destroy {
	__u32 handle;
	__u32 pad;
};

#define DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE (1 << 0)
#define DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_TIMELINE         (1 << 1)
#define DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE (1 << 0)
#define DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_TIMELINE         (1 << 1)
struct drm_syncobj_handle {
	__u32 handle;
	__u32 flags;

	__s32 fd;
	__u32 pad;

	__u64 point;
};

struct drm_syncobj_transfer {
	__u32 src_handle;
	__u32 dst_handle;
	__u64 src_point;
	__u64 dst_point;
	__u32 flags;
	__u32 pad;
};

#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL (1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT (1 << 1)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE (1 << 2) /* wait for time point to become available */
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE (1 << 3) /* set fence deadline to deadline_nsec */
struct drm_syncobj_wait {
	__u64 handles;
	/* absolute timeout */
	__s64 timeout_nsec;
	__u32 count_handles;
	__u32 flags;
	__u32 first_signaled; /* only valid when not waiting all */
	__u32 pad;
	/**
	 * @deadline_nsec - fence deadline hint
	 *
	 * Deadline hint, in absolute CLOCK_MONOTONIC, to set on backing
	 * fence(s) if the DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE flag is
	 * set.
	 */
	__u64 deadline_nsec;
};

struct drm_syncobj_timeline_wait {
	__u64 handles;
	/* wait on specific timeline point for every handles*/
	__u64 points;
	/* absolute timeout */
	__s64 timeout_nsec;
	__u32 count_handles;
	__u32 flags;
	__u32 first_signaled; /* only valid when not waiting all */
	__u32 pad;
	/**
	 * @deadline_nsec - fence deadline hint
	 *
	 * Deadline hint, in absolute CLOCK_MONOTONIC, to set on backing
	 * fence(s) if the DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE flag is
	 * set.
	 */
	__u64 deadline_nsec;
};

/**
 * struct drm_syncobj_eventfd
 * @handle: syncobj handle.
 * @flags: Zero to wait for the point to be signalled, or
 *         &DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE to wait for a fence to be
 *         available for the point.
 * @point: syncobj timeline point (set to zero for binary syncobjs).
 * @fd: Existing eventfd to sent events to.
 * @pad: Must be zero.
 *
 * Register an eventfd to be signalled by a syncobj. The eventfd counter will
 * be incremented by one.
 */
struct drm_syncobj_eventfd {
	__u32 handle;
	__u32 flags;
	__u64 point;
	__s32 fd;
	__u32 pad;
};


struct drm_syncobj_array {
	__u64 handles;
	__u32 count_handles;
	__u32 pad;
};

#define DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED (1 << 0) /* last available point on timeline syncobj */
struct drm_syncobj_timeline_array {
	__u64 handles;
	__u64 points;
	__u32 count_handles;
	__u32 flags;
};

#if defined(__cplusplus)
}
#endif

#endif
