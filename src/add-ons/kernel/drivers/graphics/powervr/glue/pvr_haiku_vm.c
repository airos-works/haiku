/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	pvr_vm.h on Haiku: GPU virtual memory contexts on drm/imagination's own
	page tables (upstream pvr_mmu.c), with the mappings in a list sorted by
	address instead of drm_gpuvm. What Mesa does with it is simple: it owns
	the address space and maps and unmaps whole buffers at addresses it
	chose, so a mapping may not overlap another, and an unmap names one
	mapping exactly (drm_gpuva_find() in pvr_vm.c, VM_UNMAP in pvr_drm.h).

	The heap and static data area tables and their queries are pvr_vm.c's
	(DESIGN.md §3.1: Mesa must see the same answers as on Linux). */


#include "pvr_device.h"
#include "pvr_drv.h"
#include "pvr_fw.h"
#include "pvr_gem.h"
#include "pvr_haiku_device.h"
#include "pvr_mmu.h"
#include "pvr_rogue_fwif.h"
#include "pvr_rogue_heap_config.h"
#include "pvr_vm.h"


struct pvr_vm_mapping {
	struct list_head		link;
	u64						device_addr;
	u64						size;
	u64						offset;
	struct pvr_gem_object*	pvr_obj;
};

struct pvr_vm_context {
	struct pvr_device*		pvr_dev;
	struct pvr_mmu_context*	mmu_ctx;
	struct mutex			lock;
	struct pvr_fw_object*	fw_mem_ctx_obj;
	struct kref				ref_count;
	struct list_head		mappings;	/* sorted by device_addr */
};


struct pvr_vm_context*
pvr_vm_context_get(struct pvr_vm_context* vm_ctx)
{
	if (vm_ctx != NULL)
		kref_get(&vm_ctx->ref_count);
	return vm_ctx;
}


dma_addr_t
pvr_vm_get_page_table_root_addr(struct pvr_vm_context* vm_ctx)
{
	return pvr_mmu_get_root_table_dma_addr(vm_ctx->mmu_ctx);
}


struct dma_resv*
pvr_vm_get_dma_resv(struct pvr_vm_context* vm_ctx)
{
	// no reservation objects: the queues order the jobs themselves
	(void)vm_ctx;
	return NULL;
}


bool
pvr_device_addr_is_valid(u64 device_addr)
{
	return (device_addr & ~PVR_PAGE_TABLE_ADDR_MASK) == 0
		&& (device_addr & ~PVR_DEVICE_PAGE_MASK) == 0;
}


bool
pvr_device_addr_and_size_are_valid(struct pvr_vm_context* vm_ctx,
	u64 device_addr, u64 size)
{
	(void)vm_ctx;
	return pvr_device_addr_is_valid(device_addr) && size != 0
		&& (size & ~PVR_DEVICE_PAGE_MASK) == 0
		&& device_addr + size > device_addr
		&& device_addr + size <= PVR_PAGE_TABLE_ADDR_SPACE_SIZE;
}


/* #pragma mark - contexts */


static void
fw_mem_context_init(void* cpu_ptr, void* priv)
{
	struct rogue_fwif_fwmemcontext* fw_mem_ctx = cpu_ptr;
	struct pvr_vm_context* vm_ctx = priv;

	fw_mem_ctx->pc_dev_paddr = pvr_vm_get_page_table_root_addr(vm_ctx);
	fw_mem_ctx->page_cat_base_reg_set = ROGUE_FW_BIF_INVALID_PCSET;
}


struct pvr_vm_context*
pvr_vm_create_context(struct pvr_device* pvr_dev, bool is_userspace_context)
{
	u64 device_addr_bits = 0;
	if (PVR_FEATURE_VALUE(pvr_dev, virtual_address_space_bits,
			&device_addr_bits) != 0
		|| device_addr_bits != PVR_PAGE_TABLE_ADDR_BITS) {
		drm_err(from_pvr_device(pvr_dev), "unsupported GPU address space"
			" (%llu bits)\n", device_addr_bits);
		return ERR_PTR(-EINVAL);
	}

	struct pvr_vm_context* vm_ctx = kzalloc_obj(*vm_ctx);
	if (vm_ctx == NULL)
		return ERR_PTR(-ENOMEM);
	vm_ctx->pvr_dev = pvr_dev;
	INIT_LIST_HEAD(&vm_ctx->mappings);

	vm_ctx->mmu_ctx = pvr_mmu_context_create(pvr_dev);
	int err = PTR_ERR_OR_ZERO(vm_ctx->mmu_ctx);
	if (err != 0)
		goto err_free;

	if (is_userspace_context) {
		err = pvr_fw_object_create(pvr_dev,
			sizeof(struct rogue_fwif_fwmemcontext),
			PVR_BO_FW_FLAGS_DEVICE_UNCACHED, fw_mem_context_init, vm_ctx,
			&vm_ctx->fw_mem_ctx_obj);
		if (err != 0)
			goto err_page_table_destroy;
	}

	mutex_init(&vm_ctx->lock);
	kref_init(&vm_ctx->ref_count);
	return vm_ctx;

err_page_table_destroy:
	pvr_mmu_context_destroy(vm_ctx->mmu_ctx);
err_free:
	kfree(vm_ctx);
	return ERR_PTR(err);
}


static void
pvr_vm_context_release(struct kref* ref_count)
{
	struct pvr_vm_context* vm_ctx
		= container_of(ref_count, struct pvr_vm_context, ref_count);

	if (vm_ctx->fw_mem_ctx_obj != NULL)
		pvr_fw_object_destroy(vm_ctx->fw_mem_ctx_obj);

	pvr_vm_unmap_all(vm_ctx);
	pvr_mmu_context_destroy(vm_ctx->mmu_ctx);
	mutex_destroy(&vm_ctx->lock);
	kfree(vm_ctx);
}


bool
pvr_vm_context_put(struct pvr_vm_context* vm_ctx)
{
	if (vm_ctx != NULL)
		return kref_put(&vm_ctx->ref_count, pvr_vm_context_release);
	return true;
}


struct pvr_vm_context*
pvr_vm_context_lookup(struct pvr_file* pvr_file, u32 handle)
{
	xa_lock(&pvr_file->vm_ctx_handles);
	struct pvr_vm_context* vm_ctx = pvr_vm_context_get(
		(struct pvr_vm_context*)xa_load(&pvr_file->vm_ctx_handles, handle));
	xa_unlock(&pvr_file->vm_ctx_handles);
	return vm_ctx;
}


void
pvr_destroy_vm_contexts_for_file(struct pvr_file* pvr_file)
{
	struct pvr_vm_context* vm_ctx;
	unsigned long handle;

	xa_for_each(&pvr_file->vm_ctx_handles, handle, vm_ctx) {
		pvr_vm_context_put((struct pvr_vm_context*)xa_erase(
			&pvr_file->vm_ctx_handles, handle));
	}
}


struct pvr_fw_object*
pvr_vm_get_fw_mem_context(struct pvr_vm_context* vm_ctx)
{
	return vm_ctx->fw_mem_ctx_obj;
}


/* #pragma mark - mappings */


/*!	Maps \a size bytes of \a pvr_obj from \a offset at \a device_addr, as
	pvr_vm_bind_op_map_init() and pvr_vm_gpuva_map() do. The MMU caches are
	flushed before the next job (pvr_mmu_flush_exec()), as on Linux.
*/
int
pvr_vm_map(struct pvr_vm_context* vm_ctx, struct pvr_gem_object* pvr_obj,
	u64 pvr_obj_offset, u64 device_addr, u64 size)
{
	const bool is_user = vm_ctx->fw_mem_ctx_obj != NULL;
	const u64 pvr_obj_size = pvr_gem_object_size(pvr_obj);
	u64 offset_plus_size;

	if (check_add_overflow(pvr_obj_offset, size, &offset_plus_size))
		return -EINVAL;
	if (is_user && !pvr_find_heap_containing(vm_ctx->pvr_dev, device_addr,
			size)) {
		return -EINVAL;
	}
	if (!pvr_device_addr_and_size_are_valid(vm_ctx, device_addr, size)
		|| (pvr_obj_offset & ~PAGE_MASK) != 0 || (size & ~PAGE_MASK) != 0
		|| pvr_obj_offset >= pvr_obj_size || offset_plus_size > pvr_obj_size) {
		return -EINVAL;
	}

	struct pvr_vm_mapping* mapping = kzalloc_obj(*mapping);
	if (mapping == NULL)
		return -ENOMEM;
	mapping->device_addr = device_addr;
	mapping->size = size;
	mapping->offset = pvr_obj_offset;

	struct sg_table* sgt = pvr_gem_object_get_pages_sgt(pvr_obj);
	int err = PTR_ERR_OR_ZERO(sgt);
	if (err != 0) {
		kfree(mapping);
		return err;
	}

	mutex_lock(&vm_ctx->lock);

	// the place in the sorted list, refusing overlaps
	struct list_head* next = &vm_ctx->mappings;
	struct pvr_vm_mapping* other;
	list_for_each_entry(other, &vm_ctx->mappings, link) {
		if (other->device_addr >= device_addr + size) {
			next = &other->link;
			break;
		}
		if (other->device_addr + other->size > device_addr) {
			mutex_unlock(&vm_ctx->lock);
			kfree(mapping);
			drm_err(from_pvr_device(vm_ctx->pvr_dev), "VM_MAP at %#llx +"
				" %#llx overlaps the mapping at %#llx + %#llx\n",
				device_addr, size, other->device_addr, other->size);
			return -EINVAL;
		}
	}

	struct pvr_mmu_op_context* op_ctx = pvr_mmu_op_context_create(
		vm_ctx->mmu_ctx, sgt, device_addr, pvr_obj_offset, size);
	err = PTR_ERR_OR_ZERO(op_ctx);
	if (err == 0) {
		err = pvr_mmu_map(op_ctx, size, pvr_obj->flags, device_addr);
		pvr_mmu_op_context_destroy(op_ctx);
	}
	if (err != 0) {
		mutex_unlock(&vm_ctx->lock);
		kfree(mapping);
		return err;
	}

	pvr_gem_object_get(pvr_obj);
	mapping->pvr_obj = pvr_obj;
	list_add_tail(&mapping->link, next);
	mutex_unlock(&vm_ctx->lock);
	return 0;
}


/*!	Unmaps one mapping; the MMU caches are flushed before this returns
	(pvr_mmu_op_context_destroy() does that for unmaps). The lock is held.
*/
static int
unmap_locked(struct pvr_vm_context* vm_ctx, struct pvr_vm_mapping* mapping)
{
	struct pvr_mmu_op_context* op_ctx = pvr_mmu_op_context_create(
		vm_ctx->mmu_ctx, NULL, mapping->device_addr, 0, 0);
	int err = PTR_ERR_OR_ZERO(op_ctx);
	if (err != 0)
		return err;
	err = pvr_mmu_unmap(op_ctx, mapping->device_addr, mapping->size);
	pvr_mmu_op_context_destroy(op_ctx);
	if (err != 0)
		return err;

	list_del(&mapping->link);
	pvr_gem_object_put(mapping->pvr_obj);
	kfree(mapping);
	return 0;
}


static struct pvr_vm_mapping*
find_mapping_locked(struct pvr_vm_context* vm_ctx, u64 device_addr, u64 size)
{
	struct pvr_vm_mapping* mapping;
	list_for_each_entry(mapping, &vm_ctx->mappings, link) {
		if (mapping->device_addr == device_addr && mapping->size == size)
			return mapping;
		if (mapping->device_addr > device_addr)
			break;
	}
	return NULL;
}


int
pvr_vm_unmap_obj(struct pvr_vm_context* vm_ctx,
	struct pvr_gem_object* pvr_obj, u64 device_addr, u64 size)
{
	mutex_lock(&vm_ctx->lock);
	struct pvr_vm_mapping* mapping = find_mapping_locked(vm_ctx, device_addr,
		size);
	int err = mapping != NULL && mapping->pvr_obj == pvr_obj
		? unmap_locked(vm_ctx, mapping) : -ENOENT;
	mutex_unlock(&vm_ctx->lock);
	return err;
}


int
pvr_vm_unmap(struct pvr_vm_context* vm_ctx, u64 device_addr, u64 size)
{
	mutex_lock(&vm_ctx->lock);
	struct pvr_vm_mapping* mapping = find_mapping_locked(vm_ctx, device_addr,
		size);
	int err = mapping != NULL ? unmap_locked(vm_ctx, mapping) : -ENOENT;
	mutex_unlock(&vm_ctx->lock);
	return err;
}


void
pvr_vm_unmap_all(struct pvr_vm_context* vm_ctx)
{
	mutex_lock(&vm_ctx->lock);
	while (!list_empty(&vm_ctx->mappings)) {
		struct pvr_vm_mapping* mapping = list_first_entry(&vm_ctx->mappings,
			struct pvr_vm_mapping, link);
		if (WARN_ON(unmap_locked(vm_ctx, mapping) != 0)) {
			// keep going: the buffer stays referenced
			list_del(&mapping->link);
			kfree(mapping);
		}
	}
	mutex_unlock(&vm_ctx->lock);
}


struct pvr_gem_object*
pvr_vm_find_gem_object(struct pvr_vm_context* vm_ctx, u64 device_addr,
	u64* mapped_offset_out, u64* mapped_size_out)
{
	struct pvr_gem_object* pvr_obj = NULL;
	struct pvr_vm_mapping* mapping;

	mutex_lock(&vm_ctx->lock);
	list_for_each_entry(mapping, &vm_ctx->mappings, link) {
		if (device_addr < mapping->device_addr)
			break;
		if (device_addr - mapping->device_addr < mapping->size) {
			pvr_obj = mapping->pvr_obj;
			pvr_gem_object_get(pvr_obj);
			if (mapped_offset_out != NULL)
				*mapped_offset_out = mapping->offset;
			if (mapped_size_out != NULL)
				*mapped_size_out = mapping->size;
			break;
		}
	}
	mutex_unlock(&vm_ctx->lock);
	return pvr_obj;
}


/* #pragma mark - heaps (pvr_vm.c) */


/* Static data areas are determined by firmware. */
static const struct drm_pvr_static_data_area static_data_areas[] = {
	{
		.area_usage = DRM_PVR_STATIC_DATA_AREA_FENCE,
		.location_heap_id = DRM_PVR_HEAP_GENERAL,
		.offset = 0,
		.size = 128,
	},
	{
		.area_usage = DRM_PVR_STATIC_DATA_AREA_YUV_CSC,
		.location_heap_id = DRM_PVR_HEAP_GENERAL,
		.offset = 128,
		.size = 1024,
	},
	{
		.area_usage = DRM_PVR_STATIC_DATA_AREA_VDM_SYNC,
		.location_heap_id = DRM_PVR_HEAP_PDS_CODE_DATA,
		.offset = 0,
		.size = 128,
	},
	{
		.area_usage = DRM_PVR_STATIC_DATA_AREA_EOT,
		.location_heap_id = DRM_PVR_HEAP_PDS_CODE_DATA,
		.offset = 128,
		.size = 128,
	},
	{
		.area_usage = DRM_PVR_STATIC_DATA_AREA_VDM_SYNC,
		.location_heap_id = DRM_PVR_HEAP_USC_CODE,
		.offset = 0,
		.size = 128,
	},
};

static const struct drm_pvr_heap pvr_heaps[] = {
	[DRM_PVR_HEAP_GENERAL] = {
		.base = ROGUE_GENERAL_HEAP_BASE,
		.size = ROGUE_GENERAL_HEAP_SIZE,
		.flags = 0,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
	[DRM_PVR_HEAP_PDS_CODE_DATA] = {
		.base = ROGUE_PDSCODEDATA_HEAP_BASE,
		.size = ROGUE_PDSCODEDATA_HEAP_SIZE,
		.flags = 0,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
	[DRM_PVR_HEAP_USC_CODE] = {
		.base = ROGUE_USCCODE_HEAP_BASE,
		.size = ROGUE_USCCODE_HEAP_SIZE,
		.flags = 0,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
	[DRM_PVR_HEAP_RGNHDR] = {
		.base = ROGUE_RGNHDR_HEAP_BASE,
		.size = ROGUE_RGNHDR_HEAP_SIZE,
		.flags = 0,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
	[DRM_PVR_HEAP_VIS_TEST] = {
		.base = ROGUE_VISTEST_HEAP_BASE,
		.size = ROGUE_VISTEST_HEAP_SIZE,
		.flags = 0,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
	[DRM_PVR_HEAP_TRANSFER_FRAG] = {
		.base = ROGUE_TRANSFER_FRAG_HEAP_BASE,
		.size = ROGUE_TRANSFER_FRAG_HEAP_SIZE,
		.flags = 0,
		.page_size_log2 = PVR_DEVICE_PAGE_SHIFT,
	},
};


int
pvr_static_data_areas_get(const struct pvr_device* pvr_dev,
	struct drm_pvr_ioctl_dev_query_args* args)
{
	struct drm_pvr_dev_query_static_data_areas query = {0};
	int err;
	(void)pvr_dev;

	if (!args->pointer) {
		args->size = sizeof(struct drm_pvr_dev_query_static_data_areas);
		return 0;
	}

	err = PVR_UOBJ_GET(query, args->size, args->pointer);
	if (err < 0)
		return err;

	if (!query.static_data_areas.array) {
		query.static_data_areas.count = ARRAY_SIZE(static_data_areas);
		query.static_data_areas.stride
			= sizeof(struct drm_pvr_static_data_area);
		goto copy_out;
	}

	if (query.static_data_areas.count > ARRAY_SIZE(static_data_areas))
		query.static_data_areas.count = ARRAY_SIZE(static_data_areas);

	err = PVR_UOBJ_SET_ARRAY(&query.static_data_areas, static_data_areas);
	if (err < 0)
		return err;

copy_out:
	err = PVR_UOBJ_SET(args->pointer, args->size, query);
	if (err < 0)
		return err;

	if (args->size > sizeof(query))
		args->size = sizeof(query);
	return 0;
}


int
pvr_heap_info_get(const struct pvr_device* pvr_dev,
	struct drm_pvr_ioctl_dev_query_args* args)
{
	struct drm_pvr_dev_query_heap_info query = {0};
	u64 dest;
	int err;

	if (!args->pointer) {
		args->size = sizeof(struct drm_pvr_dev_query_heap_info);
		return 0;
	}

	err = PVR_UOBJ_GET(query, args->size, args->pointer);
	if (err < 0)
		return err;

	if (!query.heaps.array) {
		query.heaps.count = ARRAY_SIZE(pvr_heaps);
		query.heaps.stride = sizeof(struct drm_pvr_heap);
		goto copy_out;
	}

	if (query.heaps.count > ARRAY_SIZE(pvr_heaps))
		query.heaps.count = ARRAY_SIZE(pvr_heaps);

	/* Region header heap is only present if BRN63142 is present. */
	dest = query.heaps.array;
	for (size_t i = 0; i < query.heaps.count; i++) {
		struct drm_pvr_heap heap = pvr_heaps[i];

		if (i == DRM_PVR_HEAP_RGNHDR && !PVR_HAS_QUIRK(pvr_dev, 63142))
			heap.size = 0;

		err = PVR_UOBJ_SET(dest, query.heaps.stride, heap);
		if (err < 0)
			return err;

		dest += query.heaps.stride;
	}

copy_out:
	err = PVR_UOBJ_SET(args->pointer, args->size, query);
	if (err < 0)
		return err;

	if (args->size > sizeof(query))
		args->size = sizeof(query);
	return 0;
}


static __always_inline bool
pvr_heap_contains_range(const struct drm_pvr_heap* pvr_heap, u64 start,
	u64 end)
{
	return pvr_heap->base <= start && end < pvr_heap->base + pvr_heap->size;
}


const struct drm_pvr_heap*
pvr_find_heap_containing(struct pvr_device* pvr_dev, u64 start, u64 size)
{
	u64 end;

	if (check_add_overflow(start, size - 1, &end))
		return NULL;

	for (u32 heap_id = 0; heap_id < ARRAY_SIZE(pvr_heaps); heap_id++) {
		/* Filter heaps that present only with an associated quirk */
		if (heap_id == DRM_PVR_HEAP_RGNHDR
			&& !PVR_HAS_QUIRK(pvr_dev, 63142)) {
			continue;
		}

		if (pvr_heap_contains_range(&pvr_heaps[heap_id], start, end))
			return &pvr_heaps[heap_id];
	}

	return NULL;
}
