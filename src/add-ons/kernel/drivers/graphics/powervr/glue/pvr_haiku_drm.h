/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef POWERVR_HAIKU_DRM_H
#define POWERVR_HAIKU_DRM_H


/*	The generic DRM ioctls the driver answers besides its own (pvr_drm.h):
	their structures as Linux's include/uapi/drm/drm.h has them (the layout
	is what libdrm and Mesa pass), and the sync objects behind them. */


#include <linux/types.h>


struct dma_fence;
struct pvr_haiku_file;


struct drm_version {
	int				version_major;
	int				version_minor;
	int				version_patchlevel;
	size_t			name_len;
	char __user*	name;
	size_t			date_len;
	char __user*	date;
	size_t			desc_len;
	char __user*	desc;
};

struct drm_gem_close {
	__u32	handle;
	__u32	pad;
};

#define DRM_CAP_PRIME				0x5
#define DRM_CAP_SYNCOBJ				0x13
#define DRM_CAP_SYNCOBJ_TIMELINE	0x14

struct drm_get_cap {
	__u64	capability;
	__u64	value;
};

#define DRM_SYNCOBJ_CREATE_SIGNALED	(1 << 0)

struct drm_syncobj_create {
	__u32	handle;
	__u32	flags;
};

struct drm_syncobj_destroy {
	__u32	handle;
	__u32	pad;
};

#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL			(1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT	(1 << 1)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE	(1 << 2)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE	(1 << 3)

struct drm_syncobj_wait {
	__u64	handles;
	__s64	timeout_nsec;
	__u32	count_handles;
	__u32	flags;
	__u32	first_signaled;
	__u32	pad;
	__u64	deadline_nsec;
};

struct drm_syncobj_timeline_wait {
	__u64	handles;
	__u64	points;
	__s64	timeout_nsec;
	__u32	count_handles;
	__u32	flags;
	__u32	first_signaled;
	__u32	pad;
	__u64	deadline_nsec;
};

struct drm_syncobj_array {
	__u64	handles;
	__u32	count_handles;
	__u32	pad;
};

#define DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED	(1 << 0)

struct drm_syncobj_timeline_array {
	__u64	handles;
	__u64	points;
	__u32	count_handles;
	__u32	flags;
};

struct drm_syncobj_transfer {
	__u32	src_handle;
	__u32	dst_handle;
	__u64	src_point;
	__u64	dst_point;
	__u32	flags;
	__u32	pad;
};

/* drm.h's request numbers for the above. */
#define DRM_HAIKU_VERSION				0x00
#define DRM_HAIKU_GEM_CLOSE				0x09
#define DRM_HAIKU_GET_CAP				0x0c
#define DRM_HAIKU_SYNCOBJ_CREATE		0xbf
#define DRM_HAIKU_SYNCOBJ_DESTROY		0xc0
#define DRM_HAIKU_SYNCOBJ_WAIT			0xc3
#define DRM_HAIKU_SYNCOBJ_RESET			0xc4
#define DRM_HAIKU_SYNCOBJ_SIGNAL		0xc5
#define DRM_HAIKU_SYNCOBJ_TIMELINE_WAIT	0xca
#define DRM_HAIKU_SYNCOBJ_QUERY			0xcb
#define DRM_HAIKU_SYNCOBJ_TRANSFER		0xcc
#define DRM_HAIKU_SYNCOBJ_TIMELINE_SIGNAL 0xcd


/* pvr_haiku_sync.c: the ioctls */
int		pvr_haiku_syncobj_create(struct pvr_haiku_file* file,
			struct drm_syncobj_create* args);
int		pvr_haiku_syncobj_destroy(struct pvr_haiku_file* file,
			struct drm_syncobj_destroy* args);
int		pvr_haiku_syncobj_wait(struct pvr_haiku_file* file,
			struct drm_syncobj_wait* args);
int		pvr_haiku_syncobj_timeline_wait(struct pvr_haiku_file* file,
			struct drm_syncobj_timeline_wait* args);
int		pvr_haiku_syncobj_reset(struct pvr_haiku_file* file,
			struct drm_syncobj_array* args);
int		pvr_haiku_syncobj_signal(struct pvr_haiku_file* file,
			struct drm_syncobj_array* args);
int		pvr_haiku_syncobj_timeline_signal(struct pvr_haiku_file* file,
			struct drm_syncobj_timeline_array* args);
int		pvr_haiku_syncobj_query(struct pvr_haiku_file* file,
			struct drm_syncobj_timeline_array* args);
int		pvr_haiku_syncobj_transfer(struct pvr_haiku_file* file,
			struct drm_syncobj_transfer* args);

/* pvr_haiku_sync.c: the open's table (drm_syncobj.h is in compat) */
void	pvr_haiku_syncobjs_init(struct pvr_haiku_file* file);
void	pvr_haiku_syncobjs_fini(struct pvr_haiku_file* file);


#endif	/* POWERVR_HAIKU_DRM_H */
