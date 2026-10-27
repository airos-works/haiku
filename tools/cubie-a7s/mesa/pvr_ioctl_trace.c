/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Host-side (Linux) LD_PRELOAD tracer for "build.sh shim": prints every DRM
// ioctl Mesa's PowerVR driver makes, in order, with the fields a kernel
// driver has to act on, then passes it on (to Mesa's pvr drm-shim, which
// answers like the Linux kernel driver for any BVNC). It shows what the
// air/OS powervr driver will receive: same numbers, same structures; only
// GET_BO_MMAP_OFFSET + mmap() become PVR_HAIKU_NR_MAP_BO there.
// PVR_TRACE_FAIL_SUBMIT=N makes the N-th SUBMIT_JOBS and every one after it
// fail with EIO without reaching the shim, as for a context the kernel
// ended (a GPU reset): what Mesa does then is in the log.
// CPU maps of buffer objects (mmap() of the DRM device, munmap() of such a
// map) are logged too, as "mmap" and "munmap" lines: on air/OS they are
// PVR_HAIKU_NR_MAP_BO and delete_area() of the clone.
// The shim gives every syncobj handle 1 and every VM context, context, free
// list and HWRT data set handle 0 (it ignores them afterwards), so the tracer
// numbers those itself, 1, 2, 3... per kind, to keep the trace readable.
//   LD_PRELOAD=libpvr_ioctl_trace.so:libpowervr_noop_drm_shim.so program


#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>

#include "drm-uapi/drm.h"
#include "drm-uapi/pvr_drm.h"


static int (*sNextIoctl)(int, unsigned long, ...);
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static unsigned sSequence;
static unsigned sSubmits;
static unsigned sFailSubmit;
static uint32_t sNextHandle[0x100];

// file descriptors DRM requests went to, and the CPU maps made through them
#define MAX_FDS		1024
static uint8_t sDrmFds[MAX_FDS];
static void** sMaps;
static size_t sMapCount, sMapCapacity;
static void* (*sNextMmap)(void*, size_t, int, int, int, off_t);
static void* (*sNextMmap64)(void*, size_t, int, int, int, off64_t);
static int (*sNextMunmap)(void*, size_t);


// the tracer's own handle for objects the shim does not number
static void
renumber(unsigned nr, void* arg)
{
	uint32_t* handle;
	switch (nr) {
		case 0x43:
			handle
				= &((struct drm_pvr_ioctl_create_vm_context_args*)arg)->handle;
			break;
		case 0x47:
			handle = &((struct drm_pvr_ioctl_create_context_args*)arg)->handle;
			break;
		case 0x49:
			handle
				= &((struct drm_pvr_ioctl_create_free_list_args*)arg)->handle;
			break;
		case 0x4b:
			handle = &((struct drm_pvr_ioctl_create_hwrt_dataset_args*)
				arg)->handle;
			break;
		case 0xbf:
			handle = &((struct drm_syncobj_create*)arg)->handle;
			break;
		default:
			return;
	}
	pthread_mutex_lock(&sLock);
	*handle = ++sNextHandle[nr];
	pthread_mutex_unlock(&sLock);
}


static const char*
request_name(unsigned nr)
{
	switch (nr) {
		case 0x00: return "VERSION";
		case 0x09: return "GEM_CLOSE";
		case 0x0c: return "GET_CAP";
		case 0x2d: return "PRIME_HANDLE_TO_FD";
		case 0x2e: return "PRIME_FD_TO_HANDLE";
		case 0x40: return "PVR_DEV_QUERY";
		case 0x41: return "PVR_CREATE_BO";
		case 0x42: return "PVR_GET_BO_MMAP_OFFSET";
		case 0x43: return "PVR_CREATE_VM_CONTEXT";
		case 0x44: return "PVR_DESTROY_VM_CONTEXT";
		case 0x45: return "PVR_VM_MAP";
		case 0x46: return "PVR_VM_UNMAP";
		case 0x47: return "PVR_CREATE_CONTEXT";
		case 0x48: return "PVR_DESTROY_CONTEXT";
		case 0x49: return "PVR_CREATE_FREE_LIST";
		case 0x4a: return "PVR_DESTROY_FREE_LIST";
		case 0x4b: return "PVR_CREATE_HWRT_DATASET";
		case 0x4c: return "PVR_DESTROY_HWRT_DATASET";
		case 0x4d: return "PVR_SUBMIT_JOBS";
		case 0xbf: return "SYNCOBJ_CREATE";
		case 0xc0: return "SYNCOBJ_DESTROY";
		case 0xc1: return "SYNCOBJ_HANDLE_TO_FD";
		case 0xc2: return "SYNCOBJ_FD_TO_HANDLE";
		case 0xc3: return "SYNCOBJ_WAIT";
		case 0xc4: return "SYNCOBJ_RESET";
		case 0xc5: return "SYNCOBJ_SIGNAL";
		case 0xca: return "SYNCOBJ_TIMELINE_WAIT";
		case 0xcb: return "SYNCOBJ_QUERY";
		case 0xcc: return "SYNCOBJ_TRANSFER";
		case 0xcd: return "SYNCOBJ_TIMELINE_SIGNAL";
		default: return NULL;
	}
}


static const char*
dev_query_name(uint32_t type)
{
	static const char* names[] = { "GPU_INFO", "RUNTIME_INFO", "QUIRKS",
		"ENHANCEMENTS", "HEAP_INFO", "STATIC_DATA_AREAS" };
	return type < 6 ? names[type] : "?";
}


static const char*
context_type_name(uint32_t type)
{
	static const char* names[] = { "RENDER", "COMPUTE", "TRANSFER_FRAG" };
	return type < 3 ? names[type] : "?";
}


static const char*
job_type_name(uint32_t type)
{
	static const char* names[] = { "GEOMETRY", "FRAGMENT", "COMPUTE",
		"TRANSFER_FRAG" };
	return type < 4 ? names[type] : "?";
}


static int64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}


static void
print_timeout(FILE* out, int64_t timeout)
{
	if (timeout == INT64_MAX)
		fprintf(out, " timeout=forever");
	else if (timeout == 0)
		fprintf(out, " timeout=0 (abs)");
	else
		fprintf(out, " timeout=now%+" PRId64 "us (abs)",
			(timeout - now_ns()) / 1000);
}


static void
print_handles(FILE* out, const char* label, uint64_t handles, uint64_t points,
	uint32_t count)
{
	const uint32_t* h = (const uint32_t*)(uintptr_t)handles;
	const uint64_t* p = (const uint64_t*)(uintptr_t)points;
	fprintf(out, " %s=[", label);
	for (uint32_t i = 0; i < count && i < 8; i++) {
		fprintf(out, "%s%u", i > 0 ? " " : "", h[i]);
		if (p != NULL)
			fprintf(out, ":%llu", (unsigned long long)p[i]);
	}
	fprintf(out, "%s]", count > 8 ? " ..." : "");
}


// before the call: what the driver is asked
static void
print_in(FILE* out, unsigned nr, void* arg)
{
	switch (nr) {
		case 0x09:
			fprintf(out, " handle=%u", ((struct drm_gem_close*)arg)->handle);
			break;
		case 0x0c:
			fprintf(out, " cap=0x%llx",
				((struct drm_get_cap*)arg)->capability);
			break;
		case 0x40: {
			struct drm_pvr_ioctl_dev_query_args* a = arg;
			fprintf(out, " %s size=%u%s", dev_query_name(a->type), a->size,
				a->pointer == 0 ? " (size probe)" : "");
			break;
		}
		case 0x41: {
			struct drm_pvr_ioctl_create_bo_args* a = arg;
			fprintf(out, " size=0x%llx" " flags=%s%s%s", a->size,
				a->flags & DRM_PVR_BO_BYPASS_DEVICE_CACHE
					? "BYPASS_CACHE|" : "",
				a->flags & DRM_PVR_BO_PM_FW_PROTECT ? "PM_FW_PROTECT|" : "",
				a->flags & DRM_PVR_BO_ALLOW_CPU_USERSPACE_ACCESS
					? "CPU_ACCESS" : "");
			break;
		}
		case 0x42:
			fprintf(out, " handle=%u",
				((struct drm_pvr_ioctl_get_bo_mmap_offset_args*)arg)->handle);
			break;
		case 0x44: case 0x48: case 0x4a: case 0x4c:
			fprintf(out, " handle=%u", *(uint32_t*)arg);
			break;
		case 0x45: {
			struct drm_pvr_ioctl_vm_map_args* a = arg;
			fprintf(out, " vm=%u addr=0x%llx" " bo=%u offset=0x%llx"
				" size=0x%llx" " flags=%u", a->vm_context_handle,
				a->device_addr, a->handle, a->offset, a->size, a->flags);
			break;
		}
		case 0x46: {
			struct drm_pvr_ioctl_vm_unmap_args* a = arg;
			fprintf(out, " vm=%u addr=0x%llx" " size=0x%llx",
				a->vm_context_handle, a->device_addr, a->size);
			break;
		}
		case 0x47: {
			struct drm_pvr_ioctl_create_context_args* a = arg;
			fprintf(out, " %s priority=%d flags=%u static_state=%u bytes"
				" callstack=0x%llx" " vm=%u", context_type_name(a->type),
				a->priority, a->flags, a->static_context_state_len,
				a->callstack_addr, a->vm_context_handle);
			break;
		}
		case 0x49: {
			struct drm_pvr_ioctl_create_free_list_args* a = arg;
			fprintf(out, " addr=0x%llx" " pages=%u/%u/+%u threshold=%u%%"
				" vm=%u", a->free_list_gpu_addr, a->initial_num_pages,
				a->max_num_pages, a->grow_num_pages, a->grow_threshold,
				a->vm_context_handle);
			break;
		}
		case 0x4b: {
			struct drm_pvr_ioctl_create_hwrt_dataset_args* a = arg;
			fprintf(out, " %ux%u samples=%u layers=%u free_lists=%u,%u"
				" merge=%u,%u/%u,%u/%u,%u rgn_header_size=%u tpc=%u/%u",
				a->width, a->height, a->samples, a->layers,
				a->free_list_handles[0], a->free_list_handles[1],
				a->isp_merge_lower_x, a->isp_merge_lower_y,
				a->isp_merge_upper_x, a->isp_merge_upper_y,
				a->isp_merge_scale_x, a->isp_merge_scale_y,
				a->region_header_size, a->geom_data_args.tpc_size,
				a->geom_data_args.tpc_stride);
			break;
		}
		case 0x4d: {
			struct drm_pvr_ioctl_submit_jobs_args* a = arg;
			const struct drm_pvr_job* jobs
				= (const struct drm_pvr_job*)(uintptr_t)a->jobs.array;
			fprintf(out, " %u job(s)", a->jobs.count);
			for (uint32_t i = 0; i < a->jobs.count; i++) {
				const struct drm_pvr_job* job = (const void*)((const char*)jobs
					+ i * a->jobs.stride);
				fprintf(out, " [%s ctx=%u flags=0x%x stream=%u bytes",
					job_type_name(job->type), job->context_handle, job->flags,
					job->cmd_stream_len);
				if (job->hwrt.set_handle != 0) {
					fprintf(out, " hwrt=%u/%u", job->hwrt.set_handle,
						job->hwrt.data_index);
				}
				const struct drm_pvr_sync_op* ops
					= (const void*)(uintptr_t)job->sync_ops.array;
				for (uint32_t j = 0; j < job->sync_ops.count; j++) {
					const struct drm_pvr_sync_op* op = (const void*)
						((const char*)ops + j * job->sync_ops.stride);
					fprintf(out, " %s:%u%s",
						op->flags & DRM_PVR_SYNC_OP_FLAG_SIGNAL
							? "signal" : "wait",
						op->handle, (op->flags & 0xf)
							== DRM_PVR_SYNC_OP_FLAG_HANDLE_TYPE_TIMELINE_SYNCOBJ
							? "(timeline)" : "");
				}
				fprintf(out, "]");
			}
			break;
		}
		case 0xbf:
			fprintf(out, " flags=%s", ((struct drm_syncobj_create*)arg)->flags
				& DRM_SYNCOBJ_CREATE_SIGNALED ? "SIGNALED" : "0");
			break;
		case 0xc0:
			fprintf(out, " handle=%u",
				((struct drm_syncobj_destroy*)arg)->handle);
			break;
		case 0xc3: {
			struct drm_syncobj_wait* a = arg;
			print_handles(out, "handles", a->handles, 0, a->count_handles);
			fprintf(out, " flags=%s%s", a->flags & 1 ? "WAIT_ALL|" : "",
				a->flags & 2 ? "WAIT_FOR_SUBMIT" : "");
			print_timeout(out, a->timeout_nsec);
			break;
		}
		case 0xca: {
			struct drm_syncobj_timeline_wait* a = arg;
			print_handles(out, "handle:point", a->handles, a->points,
				a->count_handles);
			fprintf(out, " flags=%s%s%s", a->flags & 1 ? "WAIT_ALL|" : "",
				a->flags & 2 ? "WAIT_FOR_SUBMIT|" : "",
				a->flags & 4 ? "WAIT_AVAILABLE" : "");
			print_timeout(out, a->timeout_nsec);
			break;
		}
		case 0xc4: case 0xc5: {
			struct drm_syncobj_array* a = arg;
			print_handles(out, "handles", a->handles, 0, a->count_handles);
			break;
		}
		case 0xcd: {
			struct drm_syncobj_timeline_array* a = arg;
			print_handles(out, "handle:point", a->handles, a->points,
				a->count_handles);
			break;
		}
		case 0xcb: {
			struct drm_syncobj_timeline_array* a = arg;
			print_handles(out, "handles", a->handles, 0, a->count_handles);
			fprintf(out, " flags=0x%x", a->flags);
			break;
		}
		case 0xcc: {
			struct drm_syncobj_transfer* a = arg;
			fprintf(out, " %u:%llu" " -> %u:%llu" " flags=0x%x",
				a->src_handle, a->src_point, a->dst_handle, a->dst_point,
				a->flags);
			break;
		}
	}
}


// after the call: what the driver answered
static void
print_out(FILE* out, unsigned nr, void* arg)
{
	switch (nr) {
		case 0x00: {
			struct drm_version* v = arg;
			if (v->name != NULL)
				fprintf(out, " -> \"%.*s\"", (int)v->name_len, v->name);
			break;
		}
		case 0x0c:
			fprintf(out, " -> %llu", ((struct drm_get_cap*)arg)->value);
			break;
		case 0x41:
			fprintf(out, " -> handle %u",
				((struct drm_pvr_ioctl_create_bo_args*)arg)->handle);
			break;
		case 0x43:
			fprintf(out, " -> handle %u",
				((struct drm_pvr_ioctl_create_vm_context_args*)arg)->handle);
			break;
		case 0x47:
			fprintf(out, " -> handle %u",
				((struct drm_pvr_ioctl_create_context_args*)arg)->handle);
			break;
		case 0x49:
			fprintf(out, " -> handle %u",
				((struct drm_pvr_ioctl_create_free_list_args*)arg)->handle);
			break;
		case 0x4b:
			fprintf(out, " -> handle %u",
				((struct drm_pvr_ioctl_create_hwrt_dataset_args*)arg)->handle);
			break;
		case 0xbf:
			fprintf(out, " -> handle %u",
				((struct drm_syncobj_create*)arg)->handle);
			break;
		case 0xcb: {
			struct drm_syncobj_timeline_array* a = arg;
			print_handles(out, "-> points", a->handles, a->points,
				a->count_handles);
			break;
		}
	}
}


int
ioctl(int fd, unsigned long request, ...)
{
	va_list args;
	va_start(args, request);
	void* arg = va_arg(args, void*);
	va_end(args);

	if (sNextIoctl == NULL)
		sNextIoctl
			= (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");

	const char* name = _IOC_TYPE(request) == DRM_IOCTL_BASE
		? request_name(_IOC_NR(request)) : NULL;
	if (name == NULL)
		return sNextIoctl(fd, request, arg);

	unsigned nr = _IOC_NR(request);
	if (fd >= 0 && fd < MAX_FDS)
		sDrmFds[fd] = 1;
	char* line = NULL;
	size_t length = 0;
	FILE* out = open_memstream(&line, &length);
	if (out == NULL)
		return sNextIoctl(fd, request, arg);

	pthread_mutex_lock(&sLock);
	unsigned sequence = ++sSequence;
	pthread_mutex_unlock(&sLock);
	fprintf(out, "ioctl %4u %-24s", sequence, name);
	print_in(out, nr, arg);

	int result;
	int error;
	pthread_mutex_lock(&sLock);
	if (nr == 0x4d) {
		const char* fail = getenv("PVR_TRACE_FAIL_SUBMIT");
		sFailSubmit = fail != NULL ? (unsigned)strtoul(fail, NULL, 0) : 0;
		sSubmits++;
	}
	int inject = nr == 0x4d && sFailSubmit != 0 && sSubmits >= sFailSubmit;
	pthread_mutex_unlock(&sLock);
	if (inject) {
		fprintf(out, " [injected EIO]");
		result = -1;
		error = EIO;
	} else {
		result = sNextIoctl(fd, request, arg);
		error = errno;
	}

	if (result == 0) {
		renumber(nr, arg);
		print_out(out, nr, arg);
	}
	else {
		/* drm-shim returns -errno itself for requests it does not know */
		fprintf(out, " = %d (%s)", result,
			strerror(result < -1 ? -result : error));
	}
	fclose(out);

	pthread_mutex_lock(&sLock);
	fprintf(stderr, "%s\n", line);
	pthread_mutex_unlock(&sLock);
	free(line);
	errno = error;
	return result;
}


static void
log_map(const char* what, const char* name, void* address, size_t length,
	int result)
{
	pthread_mutex_lock(&sLock);
	unsigned sequence = ++sSequence;
	fprintf(stderr, "%s %4u %-24saddr=%p size=0x%zx", what, sequence, name,
		address, length);
	if (result == 0)
		fprintf(stderr, ", %zu mapped\n", sMapCount);
	else
		fprintf(stderr, " = -1\n");
	pthread_mutex_unlock(&sLock);
}


static void*
traced_map(void* result, size_t length, int fd)
{
	if (fd < 0 || fd >= MAX_FDS || !sDrmFds[fd])
		return result;
	if (result != MAP_FAILED) {
		pthread_mutex_lock(&sLock);
		if (sMapCount == sMapCapacity) {
			size_t capacity = sMapCapacity != 0 ? sMapCapacity * 2 : 256;
			void** maps = realloc(sMaps, capacity * sizeof(void*));
			if (maps != NULL) {
				sMaps = maps;
				sMapCapacity = capacity;
			}
		}
		if (sMapCount < sMapCapacity)
			sMaps[sMapCount++] = result;
		pthread_mutex_unlock(&sLock);
	}
	log_map("mmap", "CPU_MAP", result, length,
		result == MAP_FAILED ? -1 : 0);
	return result;
}


void*
mmap(void* address, size_t length, int protection, int flags, int fd,
	off_t offset)
{
	if (sNextMmap == NULL)
		sNextMmap = (void* (*)(void*, size_t, int, int, int, off_t))
			dlsym(RTLD_NEXT, "mmap");
	return traced_map(sNextMmap(address, length, protection, flags, fd,
		offset), length, fd);
}


void*
mmap64(void* address, size_t length, int protection, int flags, int fd,
	off64_t offset)
{
	if (sNextMmap64 == NULL)
		sNextMmap64 = (void* (*)(void*, size_t, int, int, int, off64_t))
			dlsym(RTLD_NEXT, "mmap64");
	return traced_map(sNextMmap64(address, length, protection, flags, fd,
		offset), length, fd);
}


int
munmap(void* address, size_t length)
{
	if (sNextMunmap == NULL)
		sNextMunmap = (int (*)(void*, size_t))dlsym(RTLD_NEXT, "munmap");
	int mapped = 0;
	pthread_mutex_lock(&sLock);
	for (size_t i = 0; i < sMapCount; i++) {
		if (sMaps[i] == address) {
			sMaps[i] = sMaps[--sMapCount];
			mapped = 1;
			break;
		}
	}
	pthread_mutex_unlock(&sLock);
	int result = sNextMunmap(address, length);
	if (mapped)
		log_map("munmap", "CPU_UNMAP", address, length, result);
	return result;
}
