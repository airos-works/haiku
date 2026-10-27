/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	pvrtest: exercises the powervr driver's DRM interface the way Mesa's
	PowerVR Vulkan driver uses it, without Mesa: the generic DRM ioctls,
	the device queries, buffers and their CPU mappings, GPU VM contexts and
	mappings, and sync objects. Prints a line per check and a summary;
	exits 0 when every check passed.

		pvrtest				all checks
		pvrtest -v			also print the query results in full
*/


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <OS.h>

#include <pvr_haiku.h>

#include <uapi/drm/pvr_drm.h>


// drm.h's generic structures (as the driver's glue/pvr_haiku_drm.h)
struct drm_version {
	int		version_major;
	int		version_minor;
	int		version_patchlevel;
	size_t	name_len;
	char*	name;
	size_t	date_len;
	char*	date;
	size_t	desc_len;
	char*	desc;
};

struct drm_gem_close {
	__u32	handle;
	__u32	pad;
};

struct drm_get_cap {
	__u64	capability;
	__u64	value;
};

struct drm_syncobj_create {
	__u32	handle;
	__u32	flags;
};

struct drm_syncobj_destroy {
	__u32	handle;
	__u32	pad;
};

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

#define DRM_CAP_SYNCOBJ				0x13
#define DRM_CAP_SYNCOBJ_TIMELINE	0x14

#define DRM_SYNCOBJ_CREATE_SIGNALED				(1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL			(1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT	(1 << 1)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE	(1 << 2)
#define DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED	(1 << 0)

#define PVR_NR(name)	(DRM_COMMAND_BASE + DRM_PVR_##name)

enum {
	DRM_PVR_DEV_QUERY = 0x00,
	DRM_PVR_CREATE_BO,
	DRM_PVR_GET_BO_MMAP_OFFSET,
	DRM_PVR_CREATE_VM_CONTEXT,
	DRM_PVR_DESTROY_VM_CONTEXT,
	DRM_PVR_VM_MAP,
	DRM_PVR_VM_UNMAP,
	DRM_PVR_CREATE_CONTEXT,
	DRM_PVR_DESTROY_CONTEXT
};


static int sFD = -1;
static bool sVerbose = false;
static int sPassed = 0;
static int sFailed = 0;


/*!	One ioctl the way a libdrm stand-in would do it: 0 or the errno. */
template<typename Args>
static int
pvr_ioctl(uint32 nr, Args* args)
{
	if (ioctl(sFD, PVR_HAIKU_OP(nr), args, sizeof(*args)) == 0)
		return 0;
	return errno;
}


static void
check(bool ok, const char* what, const char* detail = NULL)
{
	if (ok)
		sPassed++;
	else
		sFailed++;
	printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what,
		detail != NULL ? ": " : "", detail != NULL ? detail : "");
}


static void
check_error(int error, int expected, const char* what)
{
	char detail[96];
	if (error == expected)
		snprintf(detail, sizeof(detail), "%s", strerror(error));
	else {
		snprintf(detail, sizeof(detail), "%s, expected %s", strerror(error),
			strerror(expected));
	}
	check(error == expected, what, detail);
}


static int64
monotonic_nsec(bigtime_t fromNow)
{
	// CLOCK_MONOTONIC, as libdrm users compute absolute timeouts
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64)now.tv_sec * 1000000000LL + now.tv_nsec + fromNow * 1000;
}


//	#pragma mark - generic DRM


static void
test_generic()
{
	char name[32] = {};
	char desc[128] = {};
	drm_version version = {};
	version.name = name;
	version.name_len = sizeof(name) - 1;
	version.desc = desc;
	version.desc_len = sizeof(desc) - 1;
	int error = pvr_ioctl(PVR_HAIKU_NR_VERSION, &version);
	char detail[200];
	snprintf(detail, sizeof(detail), "%s %d.%d.%d, \"%s\"", name,
		version.version_major, version.version_minor,
		version.version_patchlevel, desc);
	check(error == 0 && strcmp(name, "powervr") == 0, "VERSION",
		error == 0 ? detail : strerror(error));

	drm_get_cap cap = {};
	cap.capability = DRM_CAP_SYNCOBJ;
	error = pvr_ioctl(PVR_HAIKU_NR_GET_CAP, &cap);
	check(error == 0 && cap.value == 1, "GET_CAP SYNCOBJ");
	cap.capability = DRM_CAP_SYNCOBJ_TIMELINE;
	cap.value = 0;
	error = pvr_ioctl(PVR_HAIKU_NR_GET_CAP, &cap);
	check(error == 0 && cap.value == 1, "GET_CAP SYNCOBJ_TIMELINE");
	cap.capability = 0x7777;
	check_error(pvr_ioctl(PVR_HAIKU_NR_GET_CAP, &cap), EINVAL,
		"GET_CAP of an unknown capability");
}


//	#pragma mark - DEV_QUERY


template<typename Result>
static int
dev_query(uint32 type, Result* result)
{
	drm_pvr_ioctl_dev_query_args args = {};
	args.type = type;
	args.size = sizeof(*result);
	args.pointer = (uint64)(addr_t)result;
	return pvr_ioctl(PVR_NR(DEV_QUERY), &args);
}


static drm_pvr_heap sHeaps[DRM_PVR_HEAP_COUNT];
static uint32 sHeapCount;


static void
test_dev_query()
{
	// the size probe Mesa starts with
	drm_pvr_ioctl_dev_query_args probe = {};
	probe.type = DRM_PVR_DEV_QUERY_GPU_INFO_GET;
	int error = pvr_ioctl(PVR_NR(DEV_QUERY), &probe);
	check(error == 0 && probe.size == sizeof(drm_pvr_dev_query_gpu_info),
		"DEV_QUERY size probe");

	drm_pvr_dev_query_gpu_info gpu = {};
	error = dev_query(DRM_PVR_DEV_QUERY_GPU_INFO_GET, &gpu);
	char detail[200];
	snprintf(detail, sizeof(detail), "BVNC %u.%u.%u.%u, %u phantom(s)",
		(unsigned)(gpu.gpu_id >> 48), (unsigned)((gpu.gpu_id >> 32) & 0xffff),
		(unsigned)((gpu.gpu_id >> 16) & 0xffff),
		(unsigned)(gpu.gpu_id & 0xffff), (unsigned)gpu.num_phantoms);
	check(error == 0 && gpu.gpu_id != 0, "DEV_QUERY GPU_INFO",
		error == 0 ? detail : strerror(error));

	drm_pvr_dev_query_runtime_info runtime = {};
	error = dev_query(DRM_PVR_DEV_QUERY_RUNTIME_INFO_GET, &runtime);
	snprintf(detail, sizeof(detail), "free list %llu-%llu pages, common store"
		" %u/%u, max coeffs %u, CDM local memory %u",
		(unsigned long long)runtime.free_list_min_pages,
		(unsigned long long)runtime.free_list_max_pages,
		(unsigned)runtime.common_store_alloc_region_size,
		(unsigned)runtime.common_store_partition_space_size,
		(unsigned)runtime.max_coeffs,
		(unsigned)runtime.cdm_max_local_mem_size_regs);
	check(error == 0 && runtime.free_list_max_pages != 0,
		"DEV_QUERY RUNTIME_INFO", error == 0 ? detail : strerror(error));

	uint32 quirks[64];
	drm_pvr_dev_query_quirks quirkQuery = {};
	quirkQuery.quirks = (uint64)(addr_t)quirks;
	quirkQuery.count = 64;
	error = dev_query(DRM_PVR_DEV_QUERY_QUIRKS_GET, &quirkQuery);
	int length = snprintf(detail, sizeof(detail), "%u (%u must-have):",
		(unsigned)quirkQuery.count, (unsigned)quirkQuery.musthave_count);
	for (uint32 i = 0; error == 0 && i < quirkQuery.count && i < 64
			&& length < (int)sizeof(detail) - 8; i++) {
		length += snprintf(detail + length, sizeof(detail) - length, " %u",
			(unsigned)quirks[i]);
	}
	check(error == 0, "DEV_QUERY QUIRKS", error == 0 ? detail
		: strerror(error));

	uint32 enhancements[64];
	drm_pvr_dev_query_enhancements enhancementQuery = {};
	enhancementQuery.enhancements = (uint64)(addr_t)enhancements;
	enhancementQuery.count = 64;
	error = dev_query(DRM_PVR_DEV_QUERY_ENHANCEMENTS_GET, &enhancementQuery);
	snprintf(detail, sizeof(detail), "%u", (unsigned)enhancementQuery.count);
	check(error == 0, "DEV_QUERY ENHANCEMENTS", error == 0 ? detail
		: strerror(error));

	drm_pvr_dev_query_heap_info heapInfo = {};
	heapInfo.heaps.stride = sizeof(drm_pvr_heap);
	heapInfo.heaps.count = DRM_PVR_HEAP_COUNT;
	heapInfo.heaps.array = (uint64)(addr_t)sHeaps;
	error = dev_query(DRM_PVR_DEV_QUERY_HEAP_INFO_GET, &heapInfo);
	sHeapCount = error == 0 ? heapInfo.heaps.count : 0;
	snprintf(detail, sizeof(detail), "%u heaps", (unsigned)sHeapCount);
	check(error == 0 && sHeapCount >= 3 && sHeaps[0].size != 0,
		"DEV_QUERY HEAP_INFO", error == 0 ? detail : strerror(error));
	for (uint32 i = 0; sVerbose && i < sHeapCount; i++) {
		printf("      heap %u: %#llx + %#llx, flags %#x, pages 2^%u\n",
			(unsigned)i, (unsigned long long)sHeaps[i].base,
			(unsigned long long)sHeaps[i].size, (unsigned)sHeaps[i].flags,
			(unsigned)sHeaps[i].page_size_log2);
	}

	drm_pvr_static_data_area areas[16];
	drm_pvr_dev_query_static_data_areas areaInfo = {};
	areaInfo.static_data_areas.stride = sizeof(drm_pvr_static_data_area);
	areaInfo.static_data_areas.count = 16;
	areaInfo.static_data_areas.array = (uint64)(addr_t)areas;
	error = dev_query(DRM_PVR_DEV_QUERY_STATIC_DATA_AREAS_GET, &areaInfo);
	snprintf(detail, sizeof(detail), "%u areas",
		(unsigned)areaInfo.static_data_areas.count);
	check(error == 0 && areaInfo.static_data_areas.count > 0,
		"DEV_QUERY STATIC_DATA_AREAS", error == 0 ? detail
			: strerror(error));
	for (uint32 i = 0; sVerbose && error == 0
			&& i < areaInfo.static_data_areas.count; i++) {
		printf("      area %u: usage %u, heap %u, offset %#llx, %u bytes\n",
			(unsigned)i, (unsigned)areas[i].area_usage,
			(unsigned)areas[i].location_heap_id,
			(unsigned long long)areas[i].offset, (unsigned)areas[i].size);
	}
}


//	#pragma mark - buffers and VM


static uint32
create_bo(uint64 size, uint64 flags, int* _error)
{
	drm_pvr_ioctl_create_bo_args args = {};
	args.size = size;
	args.flags = flags;
	*_error = pvr_ioctl(PVR_NR(CREATE_BO), &args);
	return *_error == 0 ? args.handle : 0;
}


static void
close_bo(uint32 handle)
{
	drm_gem_close args = {};
	args.handle = handle;
	check(pvr_ioctl(PVR_HAIKU_NR_GEM_CLOSE, &args) == 0, "GEM_CLOSE");
}


static void
test_buffers()
{
	const uint64 size = 64 * 1024;
	int error;
	uint32 handle = create_bo(size, DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS,
		&error);
	check(error == 0 && handle != 0, "CREATE_BO 64 KiB, CPU access",
		error != 0 ? strerror(error) : NULL);
	if (handle == 0)
		return;

	pvr_haiku_map_bo map = {};
	map.handle = handle;
	error = pvr_ioctl(PVR_HAIKU_NR_MAP_BO, &map);
	check(error == 0 && map.area >= 0 && map.size == size, "MAP_BO",
		error != 0 ? strerror(error) : NULL);
	if (error == 0) {
		uint32* words = (uint32*)(addr_t)map.address;
		bool zero = true;
		for (uint32 i = 0; i < size / 4; i++)
			zero &= words[i] == 0;
		check(zero, "new buffer reads as zeroes");
		for (uint32 i = 0; i < size / 4; i++)
			words[i] = 0x5a000000 | i;

		// a second mapping sees the same memory
		pvr_haiku_map_bo again = {};
		again.handle = handle;
		error = pvr_ioctl(PVR_HAIKU_NR_MAP_BO, &again);
		bool same = error == 0;
		const uint32* other = (const uint32*)(addr_t)again.address;
		for (uint32 i = 0; same && i < size / 4; i++)
			same = other[i] == (0x5a000000 | i);
		check(same, "second MAP_BO sees the first one's writes");
		if (error == 0)
			delete_area(again.area);
		delete_area(map.area);
	}

	// without CPU access it cannot be mapped
	uint32 hidden = create_bo(size, 0, &error);
	check(error == 0, "CREATE_BO without CPU access");
	pvr_haiku_map_bo denied = {};
	denied.handle = hidden;
	check_error(pvr_ioctl(PVR_HAIKU_NR_MAP_BO, &denied), EACCES,
		"MAP_BO of a buffer without CPU access");

	drm_pvr_ioctl_create_bo_args bad = {};
	bad.size = size;
	bad.flags = DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS
		| DRM_PVR_BO_PM_FW_PROTECT;
	check_error(pvr_ioctl(PVR_NR(CREATE_BO), &bad), EINVAL,
		"CREATE_BO with CPU access and PM_FW_PROTECT");

	// GPU VM: map the buffer at the start of the general heap and back
	drm_pvr_ioctl_create_vm_context_args vm = {};
	error = pvr_ioctl(PVR_NR(CREATE_VM_CONTEXT), &vm);
	check(error == 0 && vm.handle != 0, "CREATE_VM_CONTEXT",
		error != 0 ? strerror(error) : NULL);
	if (error == 0 && sHeapCount > 0) {
		uint64 address = sHeaps[DRM_PVR_HEAP_GENERAL].base;
		drm_pvr_ioctl_vm_map_args vmMap = {};
		vmMap.vm_context_handle = vm.handle;
		vmMap.device_addr = address;
		vmMap.handle = handle;
		vmMap.size = size;
		error = pvr_ioctl(PVR_NR(VM_MAP), &vmMap);
		char detail[64];
		snprintf(detail, sizeof(detail), "at %#llx",
			(unsigned long long)address);
		check(error == 0, "VM_MAP", error == 0 ? detail : strerror(error));

		vmMap.handle = hidden;
		check_error(pvr_ioctl(PVR_NR(VM_MAP), &vmMap), EINVAL,
			"VM_MAP over a mapped range");

		drm_pvr_ioctl_vm_unmap_args vmUnmap = {};
		vmUnmap.vm_context_handle = vm.handle;
		vmUnmap.device_addr = address;
		vmUnmap.size = size;
		error = pvr_ioctl(PVR_NR(VM_UNMAP), &vmUnmap);
		check(error == 0, "VM_UNMAP", error != 0 ? strerror(error) : NULL);

		// left mapped: destroying the context has to clean it up
		vmMap.handle = hidden;
		check(pvr_ioctl(PVR_NR(VM_MAP), &vmMap) == 0,
			"VM_MAP again, left for DESTROY_VM_CONTEXT");

		// a transfer context needs no static state: the firmware context,
		// its CCCB and the job queue with its scheduler
		drm_pvr_ioctl_create_context_args context = {};
		context.type = DRM_PVR_CTX_TYPE_TRANSFER_FRAG;
		context.priority = DRM_PVR_CTX_PRIORITY_NORMAL;
		context.vm_context_handle = vm.handle;
		error = pvr_ioctl(PVR_NR(CREATE_CONTEXT), &context);
		check(error == 0 && context.handle != 0,
			"CREATE_CONTEXT transfer", error != 0 ? strerror(error) : NULL);
		if (error == 0) {
			drm_pvr_ioctl_destroy_context_args destroy = {};
			destroy.handle = context.handle;
			check(pvr_ioctl(PVR_NR(DESTROY_CONTEXT), &destroy) == 0,
				"DESTROY_CONTEXT");
		}
	}
	if (vm.handle != 0) {
		drm_pvr_ioctl_destroy_vm_context_args destroy = {};
		destroy.handle = vm.handle;
		check(pvr_ioctl(PVR_NR(DESTROY_VM_CONTEXT), &destroy) == 0,
			"DESTROY_VM_CONTEXT");
	}

	close_bo(hidden);
	close_bo(handle);
	drm_gem_close stale = {};
	stale.handle = handle;
	check_error(pvr_ioctl(PVR_HAIKU_NR_GEM_CLOSE, &stale), EINVAL,
		"GEM_CLOSE of a closed handle");
}


static int
import_host(const void* address, uint64 size, uint64 flags, uint32* _handle)
{
	pvr_haiku_import_host args = {};
	args.address = (addr_t)address;
	args.size = size;
	args.flags = flags;
	int error = pvr_ioctl(PVR_HAIKU_NR_IMPORT_HOST, &args);
	*_handle = error == 0 ? args.handle : 0;
	return error;
}


/*!	IMPORT_HOST: the team's own memory as a buffer object. Whether the GPU
	really reads and writes it is up to the Vulkan tests.
*/
static void
test_import()
{
	const uint64 size = 64 * 1024;
	void* address = NULL;
	area_id area = create_area("pvrtest host memory", &address,
		B_ANY_ADDRESS, size, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (area < 0) {
		check(false, "create_area", strerror(area));
		return;
	}
	uint32* words = (uint32*)address;
	for (uint32 i = 0; i < size / 4; i++)
		words[i] = 0xa5000000 | i;

	uint32 handle;
	int error = import_host(address, size, 0, &handle);
	check(error == 0 && handle != 0, "IMPORT_HOST 64 KiB",
		error != 0 ? strerror(error) : NULL);
	if (handle == 0) {
		delete_area(area);
		return;
	}

	pvr_haiku_map_bo map = {};
	map.handle = handle;
	check_error(pvr_ioctl(PVR_HAIKU_NR_MAP_BO, &map), EACCES,
		"MAP_BO of an imported buffer");

	uint32 part;
	error = import_host((uint8*)address + 16 * 1024, 32 * 1024,
		DRM_PVR_BO_BYPASS_DEVICE_CACHE, &part);
	check(error == 0, "IMPORT_HOST part of an area, bypassing the SLC",
		error != 0 ? strerror(error) : NULL);

	uint32 bad;
	check_error(import_host((uint8*)address + 100, B_PAGE_SIZE, 0, &bad),
		EINVAL, "IMPORT_HOST unaligned");
	check_error(import_host(address, size + B_PAGE_SIZE, 0, &bad), EINVAL,
		"IMPORT_HOST past the area's end");
	check_error(import_host(address, size,
		DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS, &bad), EINVAL,
		"IMPORT_HOST with CPU access");
	check_error(import_host((void*)(addr_t)0xffff000000000000ull,
		B_PAGE_SIZE, 0, &bad), EINVAL, "IMPORT_HOST of a kernel address");

	void* readOnlyAddress = NULL;
	area_id readOnly = create_area("pvrtest read-only", &readOnlyAddress,
		B_ANY_ADDRESS, B_PAGE_SIZE, B_NO_LOCK, B_READ_AREA);
	if (readOnly >= 0) {
		check_error(import_host(readOnlyAddress, B_PAGE_SIZE, 0, &bad),
			EACCES, "IMPORT_HOST of a read-only area");
		delete_area(readOnly);
	}

	// wired areas, and an untouched clone of one (as the display's frame
	// buffer is handed out): not mapped in until touched
	static const struct {
		uint32		lock;
		const char*	name;
	} kWired[] = {
		{ B_CONTIGUOUS, "B_CONTIGUOUS" },
		{ B_FULL_LOCK, "B_FULL_LOCK" },
	};
	for (size_t i = 0; i < sizeof(kWired) / sizeof(kWired[0]); i++) {
		void* wiredAddress = NULL;
		area_id wired = create_area("pvrtest wired", &wiredAddress,
			B_ANY_ADDRESS, size, kWired[i].lock, B_READ_AREA | B_WRITE_AREA);
		if (wired < 0)
			continue;
		void* cloneAddress = NULL;
		area_id clone = clone_area("pvrtest wired clone", &cloneAddress,
			B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, wired);
		char what[64];
		uint32 wiredHandle;
		snprintf(what, sizeof(what), "IMPORT_HOST %s", kWired[i].name);
		error = import_host(wiredAddress, size, 0, &wiredHandle);
		check(error == 0, what, error != 0 ? strerror(error) : NULL);
		if (error == 0)
			close_bo(wiredHandle);
		if (clone >= 0) {
			snprintf(what, sizeof(what), "IMPORT_HOST untouched clone of %s",
				kWired[i].name);
			error = import_host(cloneAddress, size, 0, &wiredHandle);
			check(error == 0, what, error != 0 ? strerror(error) : NULL);
			if (error == 0)
				close_bo(wiredHandle);
			delete_area(clone);
		}
		delete_area(wired);
	}

	// in a GPU VM like any buffer
	drm_pvr_ioctl_create_vm_context_args vm = {};
	error = pvr_ioctl(PVR_NR(CREATE_VM_CONTEXT), &vm);
	if (error == 0 && sHeapCount > 0) {
		drm_pvr_ioctl_vm_map_args vmMap = {};
		vmMap.vm_context_handle = vm.handle;
		vmMap.device_addr = sHeaps[DRM_PVR_HEAP_GENERAL].base;
		vmMap.handle = handle;
		vmMap.size = size;
		error = pvr_ioctl(PVR_NR(VM_MAP), &vmMap);
		check(error == 0, "VM_MAP an imported buffer",
			error != 0 ? strerror(error) : NULL);
		// left mapped for DESTROY_VM_CONTEXT
	}
	if (vm.handle != 0) {
		drm_pvr_ioctl_destroy_vm_context_args destroy = {};
		destroy.handle = vm.handle;
		check(pvr_ioctl(PVR_NR(DESTROY_VM_CONTEXT), &destroy) == 0,
			"DESTROY_VM_CONTEXT with an imported buffer");
	}

	// the object keeps the memory after the team lets go of it
	delete_area(area);
	if (part != 0)
		close_bo(part);
	close_bo(handle);
}


//	#pragma mark - sync objects


static uint32
syncobj_create(uint32 flags)
{
	drm_syncobj_create args = {};
	args.flags = flags;
	if (pvr_ioctl(PVR_HAIKU_NR_SYNCOBJ_CREATE, &args) != 0)
		return 0;
	return args.handle;
}


static int
syncobj_wait(uint32 handle, uint32 flags, bigtime_t timeout)
{
	drm_syncobj_wait args = {};
	args.handles = (uint64)(addr_t)&handle;
	args.count_handles = 1;
	args.flags = flags;
	args.timeout_nsec = timeout < 0 ? INT64_MAX : monotonic_nsec(timeout);
	return pvr_ioctl(PVR_HAIKU_NR_SYNCOBJ_WAIT, &args);
}


static int
timeline_wait(uint32 handle, uint64 point, uint32 flags, bigtime_t timeout)
{
	drm_syncobj_timeline_wait args = {};
	args.handles = (uint64)(addr_t)&handle;
	args.points = (uint64)(addr_t)&point;
	args.count_handles = 1;
	args.flags = flags;
	args.timeout_nsec = monotonic_nsec(timeout);
	return pvr_ioctl(PVR_HAIKU_NR_SYNCOBJ_TIMELINE_WAIT, &args);
}


static int
syncobj_array(uint32 nr, uint32 handle)
{
	drm_syncobj_array args = {};
	args.handles = (uint64)(addr_t)&handle;
	args.count_handles = 1;
	return pvr_ioctl(nr, &args);
}


static int
timeline_array(uint32 nr, uint32 handle, uint64* point, uint32 flags)
{
	drm_syncobj_timeline_array args = {};
	args.handles = (uint64)(addr_t)&handle;
	args.points = (uint64)(addr_t)point;
	args.count_handles = 1;
	args.flags = flags;
	return pvr_ioctl(nr, &args);
}


static void
test_syncobjs()
{
	const uint32 forSubmit = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;

	uint32 binary = syncobj_create(0);
	check(binary != 0, "SYNCOBJ_CREATE");
	check_error(syncobj_wait(binary, 0, 0), EINVAL,
		"WAIT without a fence");
	bigtime_t start = system_time();
	int error = syncobj_wait(binary, forSubmit, 20000);
	bigtime_t waited = system_time() - start;
	char detail[96];
	snprintf(detail, sizeof(detail), "%s after %lld us", strerror(error),
		(long long)waited);
	check(error == ETIME && waited >= 15000 && waited < 500000,
		"WAIT_FOR_SUBMIT times out after 20 ms", detail);

	check(syncobj_array(PVR_HAIKU_NR_SYNCOBJ_SIGNAL, binary) == 0,
		"SYNCOBJ_SIGNAL");
	check(syncobj_wait(binary, 0, 0) == 0, "WAIT on a signaled object");
	check(syncobj_array(PVR_HAIKU_NR_SYNCOBJ_RESET, binary) == 0,
		"SYNCOBJ_RESET");
	check_error(syncobj_wait(binary, 0, 0), EINVAL, "WAIT after RESET");

	uint32 signaled = syncobj_create(DRM_SYNCOBJ_CREATE_SIGNALED);
	check(signaled != 0 && syncobj_wait(signaled, 0, 0) == 0,
		"SYNCOBJ_CREATE signaled");

	// wait for any / all of two
	uint32 pair[2] = { binary, signaled };
	drm_syncobj_wait any = {};
	any.handles = (uint64)(addr_t)pair;
	any.count_handles = 2;
	any.flags = forSubmit;
	any.timeout_nsec = monotonic_nsec(0);
	error = pvr_ioctl(PVR_HAIKU_NR_SYNCOBJ_WAIT, &any);
	check(error == 0 && any.first_signaled == 1, "WAIT for any of two");
	drm_syncobj_wait all = any;
	all.flags = forSubmit | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	check_error(pvr_ioctl(PVR_HAIKU_NR_SYNCOBJ_WAIT, &all), ETIME,
		"WAIT for all of two, one unsignaled");

	// timelines
	uint32 timeline = syncobj_create(0);
	uint64 point = 3;
	check(timeline_array(PVR_HAIKU_NR_SYNCOBJ_TIMELINE_SIGNAL, timeline,
		&point, 0) == 0, "TIMELINE_SIGNAL point 3");
	point = 0;
	error = timeline_array(PVR_HAIKU_NR_SYNCOBJ_QUERY, timeline, &point, 0);
	check(error == 0 && point == 3, "QUERY says 3");
	check(timeline_wait(timeline, 2, 0, 0) == 0, "TIMELINE_WAIT point 2");
	check(timeline_wait(timeline, 3, 0, 0) == 0, "TIMELINE_WAIT point 3");
	check_error(timeline_wait(timeline, 5, 0, 0), EINVAL,
		"TIMELINE_WAIT point 5, not submitted");
	check_error(timeline_wait(timeline, 5, forSubmit, 10000), ETIME,
		"TIMELINE_WAIT point 5 for submit");
	check(timeline_wait(timeline, 3, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE,
		0) == 0, "TIMELINE_WAIT point 3 available");

	point = 7;
	check(timeline_array(PVR_HAIKU_NR_SYNCOBJ_TIMELINE_SIGNAL, timeline,
		&point, 0) == 0, "TIMELINE_SIGNAL point 7");
	point = 0;
	timeline_array(PVR_HAIKU_NR_SYNCOBJ_QUERY, timeline, &point,
		DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED);
	check(point == 7, "QUERY last submitted says 7");
	check(timeline_wait(timeline, 5, 0, 0) == 0,
		"TIMELINE_WAIT point 5 after 7 signaled");

	// a waiter woken by a signal from another thread
	struct WakeTest {
		static status_t Signal(void* data)
		{
			snooze(30000);
			uint32 handle = *(uint32*)data;
			uint64 point = 9;
			timeline_array(PVR_HAIKU_NR_SYNCOBJ_TIMELINE_SIGNAL, handle,
				&point, 0);
			return B_OK;
		}
	};
	thread_id thread = spawn_thread(&WakeTest::Signal, "signaler",
		B_NORMAL_PRIORITY, &timeline);
	resume_thread(thread);
	start = system_time();
	error = timeline_wait(timeline, 9, forSubmit, 2000000);
	waited = system_time() - start;
	snprintf(detail, sizeof(detail), "%s after %lld us", strerror(error),
		(long long)waited);
	check(error == 0 && waited < 1000000,
		"TIMELINE_WAIT point 9 woken by another thread's signal", detail);
	status_t result;
	wait_for_thread(thread, &result);

	// transfer a timeline point into a binary object and back
	uint32 target = syncobj_create(0);
	drm_syncobj_transfer transfer = {};
	transfer.src_handle = timeline;
	transfer.src_point = 9;
	transfer.dst_handle = target;
	check(pvr_ioctl(PVR_HAIKU_NR_SYNCOBJ_TRANSFER, &transfer) == 0
		&& syncobj_wait(target, 0, 0) == 0, "TRANSFER timeline -> binary");
	transfer.src_handle = target;
	transfer.src_point = 0;
	transfer.dst_handle = timeline;
	transfer.dst_point = 12;
	check(pvr_ioctl(PVR_HAIKU_NR_SYNCOBJ_TRANSFER, &transfer) == 0
		&& timeline_wait(timeline, 12, 0, 0) == 0,
		"TRANSFER binary -> timeline point 12");

	uint32 handles[] = { binary, signaled, timeline, target };
	for (uint32 handle : handles) {
		drm_syncobj_destroy destroy = {};
		destroy.handle = handle;
		check(pvr_ioctl(PVR_HAIKU_NR_SYNCOBJ_DESTROY, &destroy) == 0,
			"SYNCOBJ_DESTROY");
	}
	check_error(syncobj_wait(binary, 0, 0), ENOENT,
		"WAIT on a destroyed object");
}


//	#pragma mark - buffer costs


/*!	--bench: the average cost of each step of a buffer's life, as Mesa
	goes through it (CREATE_BO, VM_MAP, MAP_BO, unmap, VM_UNMAP,
	GEM_CLOSE), for a few sizes.
*/
static void
bench_buffers()
{
	drm_pvr_ioctl_create_vm_context_args vm = {};
	if (pvr_ioctl(PVR_NR(CREATE_VM_CONTEXT), &vm) != 0 || sHeapCount == 0) {
		printf("bench: no VM context\n");
		return;
	}
	static const uint64 kSizes[] = { 4096, 65536, 1024 * 1024 };
	const int kRounds = 100;
	for (size_t s = 0; s < sizeof(kSizes) / sizeof(kSizes[0]); s++) {
		bigtime_t times[6] = {};
		for (int round = 0; round < kRounds; round++) {
			bigtime_t start = system_time();
			int error;
			uint32 handle = create_bo(kSizes[s],
				DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS, &error);
			bigtime_t t1 = system_time();
			if (handle == 0) {
				printf("bench: CREATE_BO: %s\n", strerror(error));
				return;
			}
			drm_pvr_ioctl_vm_map_args vmMap = {};
			vmMap.vm_context_handle = vm.handle;
			vmMap.device_addr = sHeaps[DRM_PVR_HEAP_GENERAL].base;
			vmMap.handle = handle;
			vmMap.size = kSizes[s];
			pvr_ioctl(PVR_NR(VM_MAP), &vmMap);
			bigtime_t t2 = system_time();
			pvr_haiku_map_bo map = {};
			map.handle = handle;
			pvr_ioctl(PVR_HAIKU_NR_MAP_BO, &map);
			bigtime_t t3 = system_time();
			if (map.area >= 0)
				delete_area(map.area);
			bigtime_t t4 = system_time();
			drm_pvr_ioctl_vm_unmap_args vmUnmap = {};
			vmUnmap.vm_context_handle = vm.handle;
			vmUnmap.device_addr = vmMap.device_addr;
			vmUnmap.size = kSizes[s];
			pvr_ioctl(PVR_NR(VM_UNMAP), &vmUnmap);
			bigtime_t t5 = system_time();
			drm_gem_close close = {};
			close.handle = handle;
			pvr_ioctl(PVR_HAIKU_NR_GEM_CLOSE, &close);
			bigtime_t t6 = system_time();
			times[0] += t1 - start;
			times[1] += t2 - t1;
			times[2] += t3 - t2;
			times[3] += t4 - t3;
			times[4] += t5 - t4;
			times[5] += t6 - t5;
		}
		printf("bench %4llu KiB: CREATE_BO %lld us, VM_MAP %lld, MAP_BO %lld,"
			" unmap %lld, VM_UNMAP %lld, GEM_CLOSE %lld; total %lld us\n",
			(unsigned long long)kSizes[s] / 1024,
			(long long)times[0] / kRounds, (long long)times[1] / kRounds,
			(long long)times[2] / kRounds, (long long)times[3] / kRounds,
			(long long)times[4] / kRounds, (long long)times[5] / kRounds,
			(long long)(times[0] + times[1] + times[2] + times[3] + times[4]
				+ times[5]) / kRounds);
	}
	drm_pvr_ioctl_destroy_vm_context_args destroy = {};
	destroy.handle = vm.handle;
	pvr_ioctl(PVR_NR(DESTROY_VM_CONTEXT), &destroy);
}


int
main(int argc, char** argv)
{
	bool bench = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0)
			sVerbose = true;
		else if (strcmp(argv[i], "--bench") == 0)
			bench = true;
		else {
			fprintf(stderr, "usage: %s [-v] [--bench]\n", argv[0]);
			return 2;
		}
	}

	const char* path = getenv(PVR_HAIKU_DEVICE_ENV);
	if (path == NULL)
		path = PVR_HAIKU_DEVICE_PATH;
	sFD = open(path, O_RDWR);
	if (sFD < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return 1;
	}

	if (bench) {
		test_dev_query();
		bench_buffers();
		close(sFD);
		return 0;
	}

	test_generic();
	test_dev_query();
	test_buffers();
	test_import();
	test_syncobjs();

	close(sFD);
	printf("pvrtest: %d passed, %d failed\n", sPassed, sFailed);
	return sFailed == 0 ? 0 : 1;
}
