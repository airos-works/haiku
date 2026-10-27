/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	The DRM file and ioctl layer (Linux's drm_file.c and drm_ioctl.c) for
	the device's opens: each open is a struct pvr_haiku_file, and its
	ioctls go to drm/imagination's handlers (pvr_drv.c) or to the few
	generic DRM ioctls Mesa needs, implemented here and in
	pvr_haiku_sync.c. pvr_haiku.h describes the op codes.

	Sizes follow drm_ioctl(): the caller's structure size is the ioctl's
	length; that much is copied in if the request writes, zero-extended to
	the driver's size, and copied back out if it reads, even on failure. */


#include "pvr_device.h"
#include "pvr_drv.h"
#include "pvr_gem.h"
#include "pvr_haiku_device.h"
#include "pvr_haiku_drm.h"

#include <pvr_haiku.h>


#define TRACE(x...)	dprintf("powervr: " x)

/* the largest structure an ioctl may pass */
#define MAX_IOCTL_SIZE	4096


/* #pragma mark - generic DRM ioctls */


/*	drm_ioctl.c's drm_copy_field(): as much of \a value as fits, the full
	length back.
*/
static int
copy_field(char __user* buffer, size_t* _length, const char* value)
{
	size_t length = strlen(value);
	size_t copy = min(*_length, length);
	*_length = length;
	if (copy == 0 || buffer == NULL)
		return 0;
	return copy_to_user(buffer, value, copy) != 0 ? -EFAULT : 0;
}


static int
version_ioctl(struct pvr_haiku_file* file, void* data)
{
	const struct drm_driver* driver = pvr_haiku_drm_driver();
	struct drm_version* args = (struct drm_version*)data;
	(void)file;

	args->version_major = driver->major;
	args->version_minor = driver->minor;
	args->version_patchlevel = driver->patchlevel;
	int error = copy_field(args->name, &args->name_len, driver->name);
	if (error == 0)
		error = copy_field(args->date, &args->date_len, "0");
	if (error == 0)
		error = copy_field(args->desc, &args->desc_len, driver->desc);
	return error;
}


static int
gem_close_ioctl(struct pvr_haiku_file* file, void* data)
{
	struct drm_gem_close* args = (struct drm_gem_close*)data;
	return pvr_gem_handle_delete(to_pvr_file(&file->drm_file), args->handle);
}


static int
get_cap_ioctl(struct pvr_haiku_file* file, void* data)
{
	struct drm_get_cap* args = (struct drm_get_cap*)data;
	(void)file;

	switch (args->capability) {
		case DRM_CAP_SYNCOBJ:
		case DRM_CAP_SYNCOBJ_TIMELINE:
			args->value = 1;
			return 0;
		case DRM_CAP_PRIME:
			// no dma-buf file descriptors on Haiku
			args->value = 0;
			return 0;
		default:
			return -EINVAL;
	}
}


/*!	PVR_HAIKU_NR_MAP_BO: the buffer's area cloned into the caller, in place
	of GET_BO_MMAP_OFFSET and mmap().
*/
static int
map_bo_ioctl(struct pvr_haiku_file* file, void* data)
{
	struct pvr_haiku_map_bo* args = (struct pvr_haiku_map_bo*)data;
	if ((args->flags & ~PVR_HAIKU_MAP_BO_CACHED) != 0 || args->reserved != 0)
		return -EINVAL;
	struct pvr_gem_object* pvr_obj = pvr_gem_object_from_handle(
		to_pvr_file(&file->drm_file), args->handle);
	if (pvr_obj == NULL)
		return -ENOENT;

	int error = 0;
	if ((pvr_obj->flags & DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS) == 0)
		error = -EACCES;
	else {
		void* address = (void*)(uintptr_t)args->address;
		area_id area = lx_area_clone_to_user(pvr_obj->buffer.area, &address,
			args->address != 0, (args->flags & PVR_HAIKU_MAP_BO_CACHED) != 0);
		if (area < 0) {
			TRACE("MAP_BO: handle %u (%zu KiB) not mapped: %s\n",
				args->handle, pvr_gem_object_size(pvr_obj) / 1024,
				strerror(area));
			error = area == B_NO_MEMORY ? -ENOMEM : -EINVAL;
		} else {
			args->area = area;
			args->address = (u64)(uintptr_t)address;
			args->size = pvr_gem_object_size(pvr_obj);
		}
	}
	pvr_gem_object_put(pvr_obj);
	return error;
}


/*!	PVR_HAIKU_NR_IMPORT_HOST: a buffer object over the caller's memory. */
static int
import_host_ioctl(struct pvr_haiku_file* file, void* data)
{
	struct pvr_haiku_import_host* args = (struct pvr_haiku_import_host*)data;
	if (args->reserved != 0 || args->size > SIZE_MAX)
		return -EINVAL;

	struct pvr_file* pvr_file = to_pvr_file(&file->drm_file);
	struct pvr_gem_object* pvr_obj = pvr_haiku_gem_object_import(
		pvr_file->pvr_dev, (const void*)(uintptr_t)args->address,
		(size_t)args->size, args->flags);
	if (IS_ERR(pvr_obj)) {
		TRACE("IMPORT_HOST: %#" B_PRIx64 ", %" B_PRIu64 " KiB not imported:"
			" %d\n", args->address, args->size / 1024, (int)PTR_ERR(pvr_obj));
		return PTR_ERR(pvr_obj);
	}

	// as pvr_ioctl_create_bo(): the handle holds the only reference
	int error = pvr_gem_object_into_handle(pvr_obj, pvr_file, &args->handle);
	if (error != 0)
		pvr_gem_object_put(pvr_obj);
	return error;
}


#define SYNCOBJ_FUNCTION(function_, type_) \
	static int \
	syncobj_##function_(struct pvr_haiku_file* file, void* data) \
	{ \
		return pvr_haiku_syncobj_##function_(file, (type_*)data); \
	}

SYNCOBJ_FUNCTION(create, struct drm_syncobj_create)
SYNCOBJ_FUNCTION(destroy, struct drm_syncobj_destroy)
SYNCOBJ_FUNCTION(wait, struct drm_syncobj_wait)
SYNCOBJ_FUNCTION(reset, struct drm_syncobj_array)
SYNCOBJ_FUNCTION(signal, struct drm_syncobj_array)
SYNCOBJ_FUNCTION(timeline_wait, struct drm_syncobj_timeline_wait)
SYNCOBJ_FUNCTION(query, struct drm_syncobj_timeline_array)
SYNCOBJ_FUNCTION(transfer, struct drm_syncobj_transfer)
SYNCOBJ_FUNCTION(timeline_signal, struct drm_syncobj_timeline_array)

#define GENERIC_IOCTL(nr_, direction_, type_, function_) \
	{ _IOC((direction_), DRM_IOCTL_BASE, (nr_), sizeof(type_)), \
		(function_), #nr_ }
#define SYNCOBJ_IOCTL(name_, type_, function_) \
	GENERIC_IOCTL(DRM_HAIKU_SYNCOBJ_##name_, _IOC_READ | _IOC_WRITE, type_, \
		syncobj_##function_)

typedef int (*generic_ioctl_function)(struct pvr_haiku_file* file,
	void* data);

static const struct generic_ioctl {
	u32						cmd;
	generic_ioctl_function	function;
	const char*				name;
} kGenericIoctls[] = {
	GENERIC_IOCTL(DRM_HAIKU_VERSION, _IOC_READ | _IOC_WRITE,
		struct drm_version, version_ioctl),
	GENERIC_IOCTL(DRM_HAIKU_GEM_CLOSE, _IOC_WRITE, struct drm_gem_close,
		gem_close_ioctl),
	GENERIC_IOCTL(DRM_HAIKU_GET_CAP, _IOC_READ | _IOC_WRITE,
		struct drm_get_cap, get_cap_ioctl),
	SYNCOBJ_IOCTL(CREATE, struct drm_syncobj_create, create),
	SYNCOBJ_IOCTL(DESTROY, struct drm_syncobj_destroy, destroy),
	SYNCOBJ_IOCTL(WAIT, struct drm_syncobj_wait, wait),
	SYNCOBJ_IOCTL(RESET, struct drm_syncobj_array, reset),
	SYNCOBJ_IOCTL(SIGNAL, struct drm_syncobj_array, signal),
	SYNCOBJ_IOCTL(TIMELINE_WAIT, struct drm_syncobj_timeline_wait,
		timeline_wait),
	SYNCOBJ_IOCTL(QUERY, struct drm_syncobj_timeline_array, query),
	SYNCOBJ_IOCTL(TRANSFER, struct drm_syncobj_transfer, transfer),
	SYNCOBJ_IOCTL(TIMELINE_SIGNAL, struct drm_syncobj_timeline_array,
		timeline_signal),
	GENERIC_IOCTL(PVR_HAIKU_NR_MAP_BO, _IOC_READ | _IOC_WRITE,
		struct pvr_haiku_map_bo, map_bo_ioctl),
	GENERIC_IOCTL(PVR_HAIKU_NR_IMPORT_HOST, _IOC_READ | _IOC_WRITE,
		struct pvr_haiku_import_host, import_host_ioctl),
};


/* #pragma mark - statistics */


/*	Per ioctl number: calls, time spent in the handler (waits included),
	the longest call. Logged with the driver's dump, to tell what a frame
	costs in the kernel; lockless and approximate under concurrency. */
static struct {
	int64		count;
	int64		time;
	int64		max_time;
	const char*	name;
} sIoctlStats[256];


static void
count_ioctl(u32 nr, const char* name, bigtime_t elapsed)
{
	nr &= 0xff;
	sIoctlStats[nr].name = name;
	atomic_add64(&sIoctlStats[nr].count, 1);
	atomic_add64(&sIoctlStats[nr].time, elapsed);
	if (elapsed > atomic_get64(&sIoctlStats[nr].max_time))
		atomic_set64(&sIoctlStats[nr].max_time, elapsed);
}


void
pvr_haiku_ioctl_stats_dump(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(sIoctlStats); i++) {
		int64 count = atomic_get64(&sIoctlStats[i].count);
		if (count == 0)
			continue;
		int64 time = atomic_get64(&sIoctlStats[i].time);
		TRACE("dump: ioctl %s: %" B_PRId64 " calls, %" B_PRId64 " us, %"
			B_PRId64 " us each, longest %" B_PRId64 " us\n",
			sIoctlStats[i].name, count, time, time / count,
			atomic_get64(&sIoctlStats[i].max_time));
	}
}


/* #pragma mark - files */


status_t
pvr_haiku_file_open(struct pvr_device* pvr_dev, struct pvr_haiku_file** _file)
{
	const struct drm_driver* driver = pvr_haiku_drm_driver();
	struct pvr_haiku_file* file
		= (struct pvr_haiku_file*)kzalloc(sizeof(*file), GFP_KERNEL);
	if (file == NULL)
		return B_NO_MEMORY;

	file->pvr_dev = pvr_dev;
	xa_init_flags(&file->bo_handles, XA_FLAGS_ALLOC1);
	pvr_haiku_syncobjs_init(file);

	int error = driver->open(from_pvr_device(pvr_dev), &file->drm_file);
	if (error != 0) {
		pvr_haiku_syncobjs_fini(file);
		xa_destroy(&file->bo_handles);
		kfree(file);
		return lx_status(error);
	}
	*_file = file;
	return B_OK;
}


/*!	drm_file_free(): sync objects and buffer handles first, then the
	driver's postclose (contexts, free lists, HWRT data sets, VM contexts).
*/
void
pvr_haiku_file_close(struct pvr_haiku_file* file)
{
	const struct drm_driver* driver = pvr_haiku_drm_driver();

	pvr_haiku_syncobjs_fini(file);

	unsigned long handle;
	struct pvr_gem_object* pvr_obj;
	xa_for_each(&file->bo_handles, handle, pvr_obj) {
		xa_erase(&file->bo_handles, handle);
		pvr_gem_object_put(pvr_obj);
	}
	xa_destroy(&file->bo_handles);

	driver->postclose(from_pvr_device(file->pvr_dev), &file->drm_file);
	kfree(file);
}


status_t
pvr_haiku_file_ioctl(struct pvr_haiku_file* file, uint32 nr,
	void* buffer, size_t length)
{
	const struct drm_driver* driver = pvr_haiku_drm_driver();
	const struct drm_ioctl_desc* desc = NULL;
	const struct generic_ioctl* generic = NULL;
	u32 cmd;
	const char* name;

	if (nr >= DRM_COMMAND_BASE
		&& nr < DRM_COMMAND_BASE + (u32)driver->num_ioctls) {
		desc = &driver->ioctls[nr - DRM_COMMAND_BASE];
		if (desc->func == NULL)
			return B_DEV_INVALID_IOCTL;
		// the GPU's own ioctls need the firmware
		if (!READ_ONCE(file->pvr_dev->fw_dev.initialised))
			return B_NO_INIT;
		cmd = desc->cmd;
		name = desc->name;
	} else {
		for (size_t i = 0; i < ARRAY_SIZE(kGenericIoctls); i++) {
			if (_IOC_NR(kGenericIoctls[i].cmd) == nr) {
				generic = &kGenericIoctls[i];
				break;
			}
		}
		if (generic == NULL)
			return B_DEV_INVALID_IOCTL;
		cmd = generic->cmd;
		name = generic->name;
	}

	if (length == 0 || length > MAX_IOCTL_SIZE)
		return B_BAD_VALUE;
	if (!lx_access_ok(buffer, length))
		return B_BAD_ADDRESS;

	const size_t driverSize = _IOC_SIZE(cmd);
	const size_t inSize = (_IOC_DIR(cmd) & _IOC_WRITE) != 0 ? length : 0;
	const size_t outSize = (_IOC_DIR(cmd) & _IOC_READ) != 0 ? length : 0;
	const size_t kernelSize = max(length, driverSize);

	u64 stackData[32];
	void* data = stackData;
	if (kernelSize > sizeof(stackData)) {
		data = kmalloc(kernelSize, GFP_KERNEL);
		if (data == NULL)
			return B_NO_MEMORY;
	}

	int error = 0;
	if (copy_from_user(data, buffer, inSize) != 0)
		error = -EFAULT;
	else {
		memset((u8*)data + inSize, 0, kernelSize - inSize);
		bigtime_t start = system_time();
		if (desc != NULL) {
			error = desc->func(from_pvr_device(file->pvr_dev), data,
				&file->drm_file);
		} else
			error = generic->function(file, data);
		count_ioctl(nr, name, system_time() - start);
		if (error == 0 && desc != NULL
			&& to_haiku_device(file->pvr_dev)->verbose
			&& _IOC_NR(desc->cmd)
				== _IOC_NR(DRM_IOCTL_PVR_CREATE_HWRT_DATASET)) {
			// the render target's size decides how many tiles each
			// fragment job walks: worth seeing when tracing
			const struct drm_pvr_ioctl_create_hwrt_dataset_args* args
				= (const struct drm_pvr_ioctl_create_hwrt_dataset_args*)data;
			TRACE("HWRT dataset %u: %ux%u, %u samples, %u layers, merge"
				" %u,%u-%u,%u scale %u,%u, region headers %u\n",
				args->handle, args->width, args->height, args->samples,
				args->layers, args->isp_merge_lower_x,
				args->isp_merge_lower_y, args->isp_merge_upper_x,
				args->isp_merge_upper_y, args->isp_merge_scale_x,
				args->isp_merge_scale_y, args->region_header_size);
		}
		if (copy_to_user(buffer, data, outSize) != 0)
			error = -EFAULT;
	}

	if (data != stackData)
		kfree(data);

	// waits that end early are normal; anything else is worth a line
	if (error != 0 && error != -ETIME && error != -EINTR
		&& error != -EAGAIN) {
		TRACE("ioctl %s: %d\n", name, error);
	}
	return lx_status(error);
}
