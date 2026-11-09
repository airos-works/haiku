/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Device.h"

#include <errno.h>
#include <fcntl.h>
#include <new>
#include <pthread.h>
#include <string.h>
#include <unistd.h>


static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
static Device* sDevice;


Device::Device(int fd)
	:
	fFD(fd),
	fReferences(1)
{
}


Device::~Device()
{
	close(fFD);
}


/*static*/ status_t
Device::Probe(rk3588_npu_info* info)
{
	// The device the team has open already tells as much.
	pthread_mutex_lock(&sLock);
	int fd = sDevice != NULL ? dup(sDevice->fFD) : -1;
	pthread_mutex_unlock(&sLock);

	if (fd < 0)
		fd = open(RK3588_NPU_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return errno;

	memset(info, 0, sizeof(*info));
	status_t status = B_OK;
	if (ioctl(fd, RK3588_NPU_GET_INFO, info, sizeof(*info)) != 0)
		status = errno;
	else if (info->interface_version != RK3588_NPU_INTERFACE_VERSION)
		status = B_MISMATCHED_VALUES;

	close(fd);
	return status;
}


/*static*/ status_t
Device::Acquire(Device** _device)
{
	pthread_mutex_lock(&sLock);

	if (sDevice != NULL) {
		sDevice->fReferences++;
		*_device = sDevice;
		pthread_mutex_unlock(&sLock);
		return B_OK;
	}

	status_t status = B_OK;
	int fd = open(RK3588_NPU_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		status = errno;

	if (status == B_OK) {
		sDevice = new(std::nothrow) Device(fd);
		if (sDevice == NULL) {
			close(fd);
			status = B_NO_MEMORY;
		}
	}

	*_device = sDevice;
	pthread_mutex_unlock(&sLock);
	return status;
}


void
Device::Release()
{
	pthread_mutex_lock(&sLock);
	bool last = --fReferences == 0;
	if (last)
		sDevice = NULL;
	pthread_mutex_unlock(&sLock);

	if (last)
		delete this;
}


status_t
Device::CreateBuffer(size_t size, DeviceBuffer& buffer)
{
	rk3588_npu_create_buffer args;
	memset(&args, 0, sizeof(args));
	args.size = size;
	if (ioctl(fFD, RK3588_NPU_CREATE_BUFFER, &args, sizeof(args)) != 0)
		return errno;

	buffer.handle = args.handle;
	buffer.device_address = (uint32)args.device_address;
	buffer.address = (uint8*)args.address;
	buffer.size = args.size;
	return B_OK;
}


void
Device::DeleteBuffer(DeviceBuffer& buffer)
{
	if (buffer.address == NULL)
		return;

	// the driver takes the area out of the team with it
	rk3588_npu_delete_buffer args;
	args.handle = buffer.handle;
	ioctl(fFD, RK3588_NPU_DELETE_BUFFER, &args, sizeof(args));

	buffer = DeviceBuffer();
}


status_t
Device::Sync(const DeviceBuffer& buffer, uint32 direction, size_t offset,
	size_t size)
{
	rk3588_npu_sync_buffer args;
	args.handle = buffer.handle;
	args.direction = direction;
	args.offset = offset;
	args.size = size;
	if (ioctl(fFD, RK3588_NPU_SYNC_BUFFER, &args, sizeof(args)) != 0)
		return errno;
	return B_OK;
}


status_t
Device::Submit(const rk3588_npu_task* tasks, uint32 count,
	bigtime_t* _duration)
{
	rk3588_npu_submit args;
	memset(&args, 0, sizeof(args));
	args.tasks = tasks;
	args.task_count = count;

	status_t status = B_OK;
	if (ioctl(fFD, RK3588_NPU_SUBMIT, &args, sizeof(args)) != 0)
		status = errno;

	if (_duration != NULL)
		*_duration = args.duration;
	return status;
}


/*static*/ bool
Device::IsGone(status_t error)
{
	// What the file system says of a node whose driver went away; a job
	// that failed or ran out of time says nothing of the kind.
	return error == B_FILE_ERROR || error == B_DEV_NOT_READY
		|| error == B_DEVICE_NOT_FOUND || error == B_ENTRY_NOT_FOUND
		|| error == B_DEV_INVALID_IOCTL || error == B_NO_INIT;
}
