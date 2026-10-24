/*
 * Copyright 2026, air/OS. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

/* air/OS (Haiku): the part of libdrm that Mesa's Turnip (freedreno, "msm"
 * kernel interface) uses, on top of the native "adreno" kernel driver
 * (/dev/graphics/adreno/0). Made from the PowerVR driver's stand-in.
 *
 * The kernel takes Linux's drm.h and msm_drm.h structures unchanged. Its
 * contract is headers/private/graphics/adreno/adreno_haiku.h in the air/OS
 * tree: a request goes out as op code ADRENO_HAIKU_OP(nr), nr being the
 * Linux DRM ioctl number (the request's low 8 bits), with the length of the
 * structure the request encodes (IOCPARM_LEN). So this needs no table.
 *
 * Semantics follow libdrm 2.4.123, with two deliberate differences:
 *  - every function returns 0 on success and -1 with errno set on failure
 *    (libdrm's waits return -errno, which is positive on Haiku);
 *  - a syncobj wait that runs out of time fails with errno ETIME, whatever
 *    timeout code the kernel used (B_TIMED_OUT, B_WOULD_BLOCK), and is not
 *    retried on EAGAIN (which is B_WOULD_BLOCK on Haiku).
 * Syncobj wait timeouts are absolute CLOCK_MONOTONIC nanoseconds, as on
 * Linux (on Haiku that is system_time() * 1000); INT64_MAX waits for good.
 *
 * There is no device list (drmGetDevices2() finds nothing; tu_knl.cc opens
 * the node itself), no display device, and no file descriptors for buffers
 * or sync objects yet: the PRIME and syncobj fd functions fail with
 * EOPNOTSUPP.
 */
#ifndef __HAIKU__
/* everywhere else (the host tools' build, for one): the real libdrm */
#include_next <xf86drm.h>
#else

#ifndef HAIKU_ADRENO_XF86DRM_H
#define HAIKU_ADRENO_XF86DRM_H

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "drm-uapi/drm.h"
#include "drm-uapi/msm_drm.h"

/* from the air/OS tree, by path (the build adds its directory) */
#include <adreno_haiku.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The kernel's op numbers are Linux's; the length is the request's size. */
#ifndef __cplusplus
#define HAIKU_ADRENO_CHECK_REQUEST(request, nr, type)                         \
   _Static_assert(((request) & 0xff) == (nr), #request " number");            \
   _Static_assert(IOCPARM_LEN(request) == sizeof(type), #request " size")
HAIKU_ADRENO_CHECK_REQUEST(DRM_IOCTL_VERSION, ADRENO_HAIKU_NR_VERSION,
                           struct drm_version);
HAIKU_ADRENO_CHECK_REQUEST(DRM_IOCTL_GEM_CLOSE, ADRENO_HAIKU_NR_GEM_CLOSE,
                           struct drm_gem_close);
HAIKU_ADRENO_CHECK_REQUEST(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL,
                           ADRENO_HAIKU_NR_SYNCOBJ_TIMELINE_SIGNAL,
                           struct drm_syncobj_timeline_array);
HAIKU_ADRENO_CHECK_REQUEST(DRM_IOCTL_MSM_GET_PARAM, ADRENO_HAIKU_NR_MSM_FIRST,
                           struct drm_msm_param);
HAIKU_ADRENO_CHECK_REQUEST(DRM_IOCTL_MSM_GEM_SUBMIT,
                           ADRENO_HAIKU_NR_MSM_FIRST + DRM_MSM_GEM_SUBMIT,
                           struct drm_msm_gem_submit);
#undef HAIKU_ADRENO_CHECK_REQUEST
#endif

static inline int
drmIoctl(int fd, unsigned long request, void *arg)
{
   int ret;

   do {
      ret = ioctl(fd, ADRENO_HAIKU_OP(request & 0xff), arg,
                  (size_t)IOCPARM_LEN(request));
   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

   return ret;
}

/* A syncobj wait: restarted after a signal (the timeout is absolute), never
 * spun on EAGAIN, and any way of running out of time reads ETIME. */
static inline int
haiku_adreno_wait_ioctl(int fd, unsigned long request, void *arg)
{
   int ret;

   do {
      ret = ioctl(fd, ADRENO_HAIKU_OP(request & 0xff), arg,
                  (size_t)IOCPARM_LEN(request));
   } while (ret == -1 && errno == EINTR);

   if (ret == -1 && (errno == ETIMEDOUT || errno == EWOULDBLOCK))
      errno = ETIME;

   return ret == 0 ? 0 : -1;
}

static inline int
haiku_adreno_unsupported(void)
{
   errno = EOPNOTSUPP;
   return -1;
}

/* The driver's own ioctls (libdrm's drmCommand*): DRM_COMMAND_BASE + index,
 * the length given. */
static inline int
drmCommandWriteRead(int fd, unsigned long index, void *data,
                    unsigned long size)
{
   int ret;

   do {
      ret = ioctl(fd, ADRENO_HAIKU_OP(DRM_COMMAND_BASE + index), data,
                  (size_t)size);
   } while (ret == -1 && (errno == EINTR || errno == EAGAIN));

   return ret == 0 ? 0 : -errno;
}

static inline int
drmCommandWrite(int fd, unsigned long index, void *data, unsigned long size)
{
   return drmCommandWriteRead(fd, index, data, size);
}

static inline int
drmCommandRead(int fd, unsigned long index, void *data, unsigned long size)
{
   return drmCommandWriteRead(fd, index, data, size);
}

static inline void
drmFree(void *pt)
{
   free(pt);
}

/* -- version, capabilities, master ------------------------------------- */

typedef struct _drmVersion {
   int version_major;
   int version_minor;
   int version_patchlevel;
   int name_len;
   char *name;
   int date_len;
   char *date;
   int desc_len;
   char *desc;
} drmVersion, *drmVersionPtr;

static inline void
drmFreeVersion(drmVersionPtr version)
{
   if (version == NULL)
      return;

   free(version->name);
   free(version->date);
   free(version->desc);
   free(version);
}

/* Two calls, as libdrm: the lengths, then the strings. */
static inline drmVersionPtr
drmGetVersion(int fd)
{
   struct drm_version request;
   drmVersionPtr version;

   memset(&request, 0, sizeof(request));
   if (drmIoctl(fd, DRM_IOCTL_VERSION, &request) != 0)
      return NULL;

   version = (drmVersionPtr)calloc(1, sizeof(*version));
   if (version == NULL)
      return NULL;

   version->name = (char *)calloc(1, request.name_len + 1);
   version->date = (char *)calloc(1, request.date_len + 1);
   version->desc = (char *)calloc(1, request.desc_len + 1);
   if (version->name == NULL || version->date == NULL ||
       version->desc == NULL) {
      drmFreeVersion(version);
      return NULL;
   }

   request.name = version->name;
   request.date = version->date;
   request.desc = version->desc;
   if (drmIoctl(fd, DRM_IOCTL_VERSION, &request) != 0) {
      drmFreeVersion(version);
      return NULL;
   }

   version->version_major = request.version_major;
   version->version_minor = request.version_minor;
   version->version_patchlevel = request.version_patchlevel;
   version->name_len = (int)request.name_len;
   version->date_len = (int)request.date_len;
   version->desc_len = (int)request.desc_len;
   return version;
}

static inline int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
   struct drm_get_cap request;

   memset(&request, 0, sizeof(request));
   request.capability = capability;
   if (drmIoctl(fd, DRM_IOCTL_GET_CAP, &request) != 0)
      return -1;

   *value = request.value;
   return 0;
}

/* there is no display device and so no master */
static inline int
drmDropMaster(int fd)
{
   (void)fd;
   return 0;
}

/* -- devices --------------------------------------------------------------- */

#define DRM_NODE_PRIMARY 0
#define DRM_NODE_CONTROL 1
#define DRM_NODE_RENDER  2
#define DRM_NODE_MAX     3

#define DRM_BUS_PCI      0
#define DRM_BUS_USB      1
#define DRM_BUS_PLATFORM 2
#define DRM_BUS_HOST1X   3

typedef struct _drmPlatformBusInfo {
   char fullname[512];
} drmPlatformBusInfo, *drmPlatformBusInfoPtr;

typedef struct _drmPlatformDeviceInfo {
   char **compatible; /* NULL terminated list of compatible strings */
} drmPlatformDeviceInfo, *drmPlatformDeviceInfoPtr;

/* libdrm's layout; only platform devices exist here */
typedef struct _drmDevice {
   char **nodes; /* DRM_NODE_MAX sized array */
   int available_nodes; /* DRM_NODE_* bitmask */
   int bustype;
   union {
      void *pci;
      void *usb;
      drmPlatformBusInfoPtr platform;
      void *host1x;
   } businfo;
   union {
      void *pci;
      void *usb;
      drmPlatformDeviceInfoPtr platform;
      void *host1x;
   } deviceinfo;
} drmDevice, *drmDevicePtr;

/* No device list: the Adreno device is opened by path (tu_knl.cc). */
static inline int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
   (void)flags; (void)devices; (void)max_devices;
   return 0;
}

static inline int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
   (void)fd; (void)flags; (void)device;
   errno = ENODEV;
   return -1;
}

static inline void
drmFreeDevice(drmDevicePtr *device)
{
   (void)device;
}

static inline void
drmFreeDevices(drmDevicePtr devices[], int count)
{
   (void)devices; (void)count;
}

/* -- PRIME (buffer file descriptors) --------------------------------------- */

static inline int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
#ifdef ADRENO_HAIKU_NR_PRIME_HANDLE_TO_FD
   struct drm_prime_handle request;

   memset(&request, 0, sizeof(request));
   request.handle = handle;
   request.flags = flags;
   request.fd = -1;
   if (drmIoctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &request) != 0)
      return -1;

   *prime_fd = request.fd;
   return 0;
#else
   (void)fd; (void)handle; (void)flags; (void)prime_fd;
   return haiku_adreno_unsupported();
#endif
}

static inline int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
#ifdef ADRENO_HAIKU_NR_PRIME_FD_TO_HANDLE
   struct drm_prime_handle request;

   memset(&request, 0, sizeof(request));
   request.fd = prime_fd;
   if (drmIoctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &request) != 0)
      return -1;

   *handle = request.handle;
   return 0;
#else
   (void)fd; (void)prime_fd; (void)handle;
   return haiku_adreno_unsupported();
#endif
}

/* -- sync objects ---------------------------------------------------------- */

static inline int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
   struct drm_syncobj_create request;

   memset(&request, 0, sizeof(request));
   request.flags = flags;
   if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_CREATE, &request) != 0)
      return -1;

   *handle = request.handle;
   return 0;
}

static inline int
drmSyncobjDestroy(int fd, uint32_t handle)
{
   struct drm_syncobj_destroy request;

   memset(&request, 0, sizeof(request));
   request.handle = handle;
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_DESTROY, &request) == 0 ? 0 : -1;
}

static inline int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
#ifdef ADRENO_HAIKU_NR_SYNCOBJ_HANDLE_TO_FD
   struct drm_syncobj_handle request;

   memset(&request, 0, sizeof(request));
   request.handle = handle;
   request.fd = -1;
   if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &request) != 0)
      return -1;

   *obj_fd = request.fd;
   return 0;
#else
   (void)fd; (void)handle; (void)obj_fd;
   return haiku_adreno_unsupported();
#endif
}

static inline int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
#ifdef ADRENO_HAIKU_NR_SYNCOBJ_FD_TO_HANDLE
   struct drm_syncobj_handle request;

   memset(&request, 0, sizeof(request));
   request.fd = obj_fd;
   if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &request) != 0)
      return -1;

   *handle = request.handle;
   return 0;
#else
   (void)fd; (void)obj_fd; (void)handle;
   return haiku_adreno_unsupported();
#endif
}

static inline int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
#ifdef ADRENO_HAIKU_NR_SYNCOBJ_FD_TO_HANDLE
   struct drm_syncobj_handle request;

   memset(&request, 0, sizeof(request));
   request.fd = sync_file_fd;
   request.handle = handle;
   request.flags = DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE;
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &request) == 0 ? 0 : -1;
#else
   (void)fd; (void)handle; (void)sync_file_fd;
   return haiku_adreno_unsupported();
#endif
}

static inline int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
#ifdef ADRENO_HAIKU_NR_SYNCOBJ_HANDLE_TO_FD
   struct drm_syncobj_handle request;

   memset(&request, 0, sizeof(request));
   request.fd = -1;
   request.handle = handle;
   request.flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE;
   if (drmIoctl(fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &request) != 0)
      return -1;

   *sync_file_fd = request.fd;
   return 0;
#else
   (void)fd; (void)handle; (void)sync_file_fd;
   return haiku_adreno_unsupported();
#endif
}

/* flags: DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, _WAIT_FOR_SUBMIT */
static inline int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
               int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
   struct drm_syncobj_wait request;

   memset(&request, 0, sizeof(request));
   request.handles = (uintptr_t)handles;
   request.timeout_nsec = timeout_nsec;
   request.count_handles = num_handles;
   request.flags = flags;
   if (haiku_adreno_wait_ioctl(fd, DRM_IOCTL_SYNCOBJ_WAIT, &request) != 0)
      return -1;

   if (first_signaled != NULL)
      *first_signaled = request.first_signaled;
   return 0;
}

static inline int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
   struct drm_syncobj_array request;

   memset(&request, 0, sizeof(request));
   request.handles = (uintptr_t)handles;
   request.count_handles = handle_count;
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_RESET, &request) == 0 ? 0 : -1;
}

static inline int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
   struct drm_syncobj_array request;

   memset(&request, 0, sizeof(request));
   request.handles = (uintptr_t)handles;
   request.count_handles = handle_count;
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &request) == 0 ? 0 : -1;
}

static inline int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points,
                         uint32_t handle_count)
{
   struct drm_syncobj_timeline_array request;

   memset(&request, 0, sizeof(request));
   request.handles = (uintptr_t)handles;
   request.points = (uintptr_t)points;
   request.count_handles = handle_count;
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &request) == 0
      ? 0 : -1;
}

/* flags: also DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE (until the point has a
 * fence, not until it signals) */
static inline int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points,
                       unsigned num_handles, int64_t timeout_nsec,
                       unsigned flags, uint32_t *first_signaled)
{
   struct drm_syncobj_timeline_wait request;

   memset(&request, 0, sizeof(request));
   request.handles = (uintptr_t)handles;
   request.points = (uintptr_t)points;
   request.timeout_nsec = timeout_nsec;
   request.count_handles = num_handles;
   request.flags = flags;
   if (haiku_adreno_wait_ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT,
                            &request) != 0) {
      return -1;
   }

   if (first_signaled != NULL)
      *first_signaled = request.first_signaled;
   return 0;
}

/* flags: DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED */
static inline int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points,
                 uint32_t handle_count, uint32_t flags)
{
   struct drm_syncobj_timeline_array request;

   memset(&request, 0, sizeof(request));
   request.handles = (uintptr_t)handles;
   request.points = (uintptr_t)points;
   request.count_handles = handle_count;
   request.flags = flags;
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_QUERY, &request) == 0 ? 0 : -1;
}

static inline int
drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points,
                uint32_t handle_count)
{
   return drmSyncobjQuery2(fd, handles, points, handle_count, 0);
}

/* point 0 is the binary payload; the null job moves fences between
 * timeline points with this */
static inline int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
                   uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
   struct drm_syncobj_transfer request;

   memset(&request, 0, sizeof(request));
   request.src_handle = src_handle;
   request.dst_handle = dst_handle;
   request.src_point = src_point;
   request.dst_point = dst_point;
   request.flags = flags;
   return drmIoctl(fd, DRM_IOCTL_SYNCOBJ_TRANSFER, &request) == 0 ? 0 : -1;
}

#ifdef __cplusplus
}
#endif

#endif /* HAIKU_ADRENO_XF86DRM_H */

#endif /* __HAIKU__ */
