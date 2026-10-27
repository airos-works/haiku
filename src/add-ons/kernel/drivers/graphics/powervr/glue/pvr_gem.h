/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef PVR_GEM_H
#define PVR_GEM_H


/*	Haiku's replacement for drm/imagination's pvr_gem.h: buffer objects on
	locked kernel areas (struct lx_dma_buffer) instead of GEM shmem. Same
	names and semantics for what the reused files call; the flag values are
	upstream's. Firmware objects only for now: handles, user mappings and
	PRIME come with the ioctl interface (M3).

	Every object is mapped Normal-NC for the CPU (the GPU is not cache
	coherent, so PVR_BO_CPU_CACHED is never set) and pinned for its life. */


#include "pvr_rogue_heap_config.h"
#include "pvr_rogue_meta.h"

#include <uapi/drm/pvr_drm.h>

#include <drm/drm_gem.h>
#include <linux/types.h>


struct pvr_device;
struct pvr_file;


/* Kernel-only flags, as in upstream pvr_gem.h. */
#define PVR_BO_CPU_CACHED			BIT_ULL(63)
#define PVR_BO_FW_NO_CLEAR_ON_RESET	BIT_ULL(62)
#define PVR_BO_KERNEL_FLAGS_MASK \
	(PVR_BO_CPU_CACHED | PVR_BO_FW_NO_CLEAR_ON_RESET)
#define PVR_BO_UNDEFINED_MASK \
	(~(PVR_BO_KERNEL_FLAGS_MASK | DRM_PVR_BO_FLAGS_MASK))

#define PVR_BO_FW_FLAGS_DEVICE_CACHED	(ULL(0))
#define PVR_BO_FW_FLAGS_DEVICE_UNCACHED	DRM_PVR_BO_BYPASS_DEVICE_CACHE


struct pvr_gem_object {
	struct drm_gem_object	base;
	u64						flags;
	struct lx_dma_buffer	buffer;
	struct sg_table			sgt;		/* the buffer's runs, built once */
};

#define gem_from_pvr_gem(pvr_obj)	(&(pvr_obj)->base)
#define gem_to_pvr_gem(gem_obj) \
	container_of_const(gem_obj, struct pvr_gem_object, base)


struct pvr_gem_object*	pvr_gem_object_create(struct pvr_device* pvr_dev,
							size_t size, u64 flags);
struct drm_gem_object*	pvr_gem_create_object(struct drm_device* drm_dev,
							size_t size);

/* Handles: the handle owns the reference the caller had. */
int		pvr_gem_object_into_handle(struct pvr_gem_object* pvr_obj,
			struct pvr_file* pvr_file, u32* handle);
struct pvr_gem_object*	pvr_gem_object_from_handle(struct pvr_file* pvr_file,
							u32 handle);
int		pvr_gem_handle_delete(struct pvr_file* pvr_file, u32 handle);

struct sg_table*	pvr_gem_object_get_pages_sgt(
						struct pvr_gem_object* pvr_obj);

void*	pvr_gem_object_vmap(struct pvr_gem_object* pvr_obj);
void	pvr_gem_object_vunmap(struct pvr_gem_object* pvr_obj);

int		pvr_gem_get_dma_addr(struct pvr_gem_object* pvr_obj, u32 offset,
			dma_addr_t* dma_addr_out);

void	pvr_gem_object_get(struct pvr_gem_object* pvr_obj);
void	pvr_gem_object_put(struct pvr_gem_object* pvr_obj);

static __always_inline size_t
pvr_gem_object_size(struct pvr_gem_object* pvr_obj)
{
	return gem_from_pvr_gem(pvr_obj)->size;
}


/* The GEM core functions the reused files call. */
static __always_inline void
drm_gem_object_put(struct drm_gem_object* gem_obj)
{
	pvr_gem_object_put(gem_to_pvr_gem(gem_obj));
}

static __always_inline struct sg_table*
drm_gem_shmem_get_pages_sgt(struct drm_gem_object* gem_obj)
{
	return pvr_gem_object_get_pages_sgt(gem_to_pvr_gem(gem_obj));
}

/*	No mmap() of the device on Haiku (MAP_BO clones the buffer's area):
	the "offset" GET_BO_MMAP_OFFSET reports only names the buffer. */
static __always_inline int
drm_gem_create_mmap_offset(struct drm_gem_object* gem_obj)
{
	(void)gem_obj;
	return 0;
}

static __always_inline u64
drm_vma_node_offset_addr(struct drm_vma_offset_node* node)
{
	return node->offset;
}


#endif	/* PVR_GEM_H */
