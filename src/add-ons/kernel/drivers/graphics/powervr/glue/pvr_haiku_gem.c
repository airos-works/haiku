/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	pvr_gem.h on Haiku: buffer objects are locked, Normal-NC kernel areas
	(lx_dma_buffer_alloc()), pinned and zeroed at creation as with GEM
	shmem in drm/imagination (pvr_gem.c: pvr_gem_object_create()). The
	kernel mapping lasts as long as the object, so vmap/vunmap only check.
	Handles are per open file (struct pvr_haiku_file), like GEM's. */


#include "pvr_device.h"
#include "pvr_gem.h"
#include "pvr_haiku_device.h"


/*!	pvr_gem.c's pvr_gem_object_flags_validate(): no undefined flags, and
	memory the PM or the firmware owns cannot be mapped into userland.
*/
static bool
flags_are_valid(u64 flags)
{
	const u64 forbidden = DRM_PVR_BO_PM_FW_PROTECT
		| DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS;
	return (flags & PVR_BO_UNDEFINED_MASK) == 0
		&& (flags & forbidden) != forbidden;
}


/*!	The rest of an object around its buffer. */
static void
init_object(struct pvr_device* pvr_dev, struct pvr_gem_object* object,
	u64 flags)
{
	static int64 sNextOffset = 1;

	object->base.dev = from_pvr_device(pvr_dev);
	object->base.size = object->buffer.size;
	object->base.resv = &object->base._resv;
	dma_resv_init(&object->base._resv);
	object->base.vma_node.offset
		= (u64)atomic_add64(&sNextOffset, 1) << PAGE_SHIFT;
	kref_init(&object->base.refcount);
	// The GPU is not cache coherent: never CPU cached (pvr_gem.c does the
	// same when the device is not DMA coherent).
	object->flags = flags & ~PVR_BO_CPU_CACHED;
}


struct pvr_gem_object*
pvr_gem_object_create(struct pvr_device* pvr_dev, size_t size, u64 flags)
{
	if (size == 0 || !flags_are_valid(flags))
		return ERR_PTR(-EINVAL);

	struct pvr_gem_object* object
		= (struct pvr_gem_object*)kzalloc(sizeof(*object), GFP_KERNEL);
	if (object == NULL)
		return ERR_PTR(-ENOMEM);

	int error = lx_dma_buffer_alloc(&object->buffer, size, "powervr buffer");
	if (error != 0) {
		kfree(object);
		return ERR_PTR(error);
	}

	init_object(pvr_dev, object, flags);
	return object;
}


/*!	An object over the calling team's memory at \a address (page aligned,
	within one area the team can read and write), for
	PVR_HAIKU_NR_IMPORT_HOST. The pages stay locked while the object
	lives, even if the team deletes its area. Userland cannot map it (it
	already has it), so only DRM_PVR_BO_BYPASS_DEVICE_CACHE is allowed.
*/
struct pvr_gem_object*
pvr_haiku_gem_object_import(struct pvr_device* pvr_dev, const void* address,
	size_t size, u64 flags)
{
	if ((flags & ~(u64)DRM_PVR_BO_BYPASS_DEVICE_CACHE) != 0)
		return ERR_PTR(-EINVAL);

	struct pvr_gem_object* object
		= (struct pvr_gem_object*)kzalloc(sizeof(*object), GFP_KERNEL);
	if (object == NULL)
		return ERR_PTR(-ENOMEM);

	int error = lx_dma_buffer_import(&object->buffer, address, size);
	if (error != 0) {
		kfree(object);
		return ERR_PTR(error);
	}

	init_object(pvr_dev, object, flags);
	return object;
}


static void
pvr_gem_object_release(struct kref* kref)
{
	struct pvr_gem_object* object = container_of(kref, struct pvr_gem_object,
		base.refcount);
	struct pvr_device* pvr_dev = to_pvr_device(object->base.dev);

	dma_resv_fini(&object->base._resv);

	if (to_haiku_device(pvr_dev)->keep_memory) {
		// The GPU may still read or write it: leave it to the next boot.
		dprintf("powervr: keeping a %zu KiB firmware buffer (area %" B_PRId32
			"), the GPU may still use it\n", object->buffer.size / 1024,
			object->buffer.area);
		kfree(object->sgt.sgl);
		kfree(object);
		return;
	}

	lx_dma_buffer_free(&object->buffer);
	kfree(object->sgt.sgl);
	kfree(object);
}


void
pvr_gem_object_get(struct pvr_gem_object* pvr_obj)
{
	kref_get(&pvr_obj->base.refcount);
}


void
pvr_gem_object_put(struct pvr_gem_object* pvr_obj)
{
	if (pvr_obj != NULL)
		kref_put(&pvr_obj->base.refcount, pvr_gem_object_release);
}


void*
pvr_gem_object_vmap(struct pvr_gem_object* pvr_obj)
{
	if (pvr_obj->buffer.address == NULL)
		return ERR_PTR(-ENOMEM);
	return pvr_obj->buffer.address;
}


void
pvr_gem_object_vunmap(struct pvr_gem_object* pvr_obj)
{
	// the kernel mapping stays for the object's life
	(void)pvr_obj;
}


int
pvr_gem_get_dma_addr(struct pvr_gem_object* pvr_obj, u32 offset,
	dma_addr_t* dma_addr_out)
{
	return lx_dma_buffer_address(&pvr_obj->buffer, offset, dma_addr_out);
}


struct drm_gem_object*
pvr_gem_create_object(struct drm_device* drm_dev, size_t size)
{
	// only GEM shmem calls this, which Haiku does not have
	(void)drm_dev;
	(void)size;
	return ERR_PTR(-ENODEV);
}


/*!	The buffer's physical runs as a scatterlist, each entry at most 2 GiB
	(dma_length is 32 bits). Built on the first call, freed with the
	object.
*/
struct sg_table*
pvr_gem_object_get_pages_sgt(struct pvr_gem_object* pvr_obj)
{
	if (pvr_obj->sgt.sgl != NULL)
		return &pvr_obj->sgt;

	const struct lx_dma_buffer* buffer = &pvr_obj->buffer;
	const u64 maxLength = 1ULL << 31;
	u32 count = 0;
	for (u32 i = 0; i < buffer->run_count; i++)
		count += (u32)DIV_ROUND_UP(buffer->runs[i].size, maxLength);

	struct scatterlist* entries
		= (struct scatterlist*)kcalloc(count, sizeof(*entries), GFP_KERNEL);
	if (entries == NULL)
		return ERR_PTR(-ENOMEM);

	u32 entry = 0;
	for (u32 i = 0; i < buffer->run_count; i++) {
		for (u64 done = 0; done < buffer->runs[i].size; done += maxLength) {
			entries[entry].dma_address = buffer->runs[i].address + done;
			entries[entry].dma_length
				= (unsigned int)min(buffer->runs[i].size - done, maxLength);
			entry++;
		}
	}
	pvr_obj->sgt.nents = count;
	pvr_obj->sgt.orig_nents = count;
	pvr_obj->sgt.sgl = entries;
	return &pvr_obj->sgt;
}


/* #pragma mark - handles */


int
pvr_gem_object_into_handle(struct pvr_gem_object* pvr_obj,
	struct pvr_file* pvr_file, u32* handle)
{
	struct pvr_haiku_file* file = to_haiku_file(pvr_file);
	u32 newHandle;
	int error = xa_alloc(&file->bo_handles, &newHandle, pvr_obj,
		xa_limit_32b, GFP_KERNEL);
	if (error != 0)
		return error;

	// the handle owns the reference now (as drm_gem_handle_create() does)
	*handle = newHandle;
	return 0;
}


struct pvr_gem_object*
pvr_gem_object_from_handle(struct pvr_file* pvr_file, u32 handle)
{
	struct pvr_haiku_file* file = to_haiku_file(pvr_file);
	xa_lock(&file->bo_handles);
	struct pvr_gem_object* pvr_obj
		= (struct pvr_gem_object*)xa_load(&file->bo_handles, handle);
	if (pvr_obj != NULL)
		pvr_gem_object_get(pvr_obj);
	xa_unlock(&file->bo_handles);
	return pvr_obj;
}


int
pvr_gem_handle_delete(struct pvr_file* pvr_file, u32 handle)
{
	struct pvr_haiku_file* file = to_haiku_file(pvr_file);
	struct pvr_gem_object* pvr_obj
		= (struct pvr_gem_object*)xa_erase(&file->bo_handles, handle);
	if (pvr_obj == NULL)
		return -EINVAL;
	pvr_gem_object_put(pvr_obj);
	return 0;
}
