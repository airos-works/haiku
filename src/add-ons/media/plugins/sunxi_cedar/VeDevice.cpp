/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "VeDevice.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>


VeDevice::VeDevice()
	:
	fDevice(-1),
	fEngineTime(0),
	fSlices(0),
	fInPicture(false)
{
	memset(&fInfo, 0, sizeof(fInfo));
}


VeDevice::~VeDevice()
{
	Close();
}


status_t
VeDevice::Open(BString* _error)
{
	if (fDevice >= 0)
		return B_OK;
	fDevice = open(SUNXI_VE_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (fDevice < 0) {
		status_t status = errno;
		if (_error != NULL) {
			_error->SetToFormat("%s: %s", SUNXI_VE_DEVICE_PATH,
				strerror(status));
		}
		return status;
	}
	if (ioctl(fDevice, SUNXI_VE_GET_INFO, &fInfo, sizeof(fInfo)) != 0)
		memset(&fInfo, 0, sizeof(fInfo));
	fOps.reserve(1024);
	return B_OK;
}


void
VeDevice::Close()
{
	// the driver frees the buffers with the file descriptor
	if (fDevice >= 0)
		close(fDevice);
	fDevice = -1;
}


status_t
VeDevice::Allocate(VeBuffer& buffer, size_t size)
{
	sunxi_ve_allocate request;
	memset(&request, 0, sizeof(request));
	request.size = size;
	if (ioctl(fDevice, SUNXI_VE_ALLOCATE, &request, sizeof(request)) != 0)
		return errno;

	void* address;
	area_id clone = clone_area("sunxi_cedar buffer", &address,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, request.area);
	if (clone < 0) {
		ioctl(fDevice, SUNXI_VE_FREE, &request.buffer,
			sizeof(request.buffer));
		return clone;
	}
	buffer.id = request.buffer;
	buffer.clone = clone;
	buffer.address = (uint8*)address;
	buffer.size = request.size;
	buffer.bus = request.address;
	return B_OK;
}


void
VeDevice::Free(VeBuffer& buffer)
{
	if (buffer.clone >= 0)
		delete_area(buffer.clone);
	if (fDevice >= 0 && buffer.id != 0xffffffff)
		ioctl(fDevice, SUNXI_VE_FREE, &buffer.id, sizeof(buffer.id));
	buffer = VeBuffer();
}


void
VeDevice::SyncForDevice(const VeBuffer& buffer, size_t offset, size_t size)
{
	sunxi_ve_sync request = { buffer.id, (uint32)offset, (uint32)size,
		SUNXI_VE_SYNC_FOR_DEVICE };
	ioctl(fDevice, SUNXI_VE_SYNC, &request, sizeof(request));
}


void
VeDevice::SyncForCpu(const VeBuffer& buffer, size_t offset, size_t size)
{
	sunxi_ve_sync request = { buffer.id, (uint32)offset, (uint32)size,
		SUNXI_VE_SYNC_FOR_CPU };
	ioctl(fDevice, SUNXI_VE_SYNC, &request, sizeof(request));
}


void
VeDevice::Write(uint32 offset, uint32 value)
{
	sunxi_ve_op op = { SUNXI_VE_OP_WRITE, (uint16)offset, value };
	fOps.push_back(op);
}


void
VeDevice::PollClear(uint32 offset, uint32 mask)
{
	sunxi_ve_op op = { SUNXI_VE_OP_POLL_CLEAR, (uint16)offset, mask };
	fOps.push_back(op);
}


void
VeDevice::WriteBack(uint32 offset)
{
	sunxi_ve_op op = { SUNXI_VE_OP_WRITE_BACK, (uint16)offset, 0 };
	fOps.push_back(op);
}


void
VeDevice::EndPicture()
{
	if (fDevice >= 0)
		ioctl(fDevice, SUNXI_VE_END_PICTURE, NULL, 0);
}


status_t
VeDevice::Run(uint32 triggerRegister, uint32 triggerValue,
	uint32 statusRegister, uint32& status)
{
	sunxi_ve_run request;
	memset(&request, 0, sizeof(request));
	request.ops = fOps.data();
	request.count = fOps.size();
	request.trigger_register = triggerRegister;
	request.trigger_value = triggerValue;
	request.status_register = statusRegister;
	request.flags = fInPicture ? SUNXI_VE_RUN_PICTURE : 0;
	status_t result = B_OK;
	if (ioctl(fDevice, SUNXI_VE_RUN, &request, sizeof(request)) != 0)
		result = errno;
	status = request.status;
	fEngineTime += request.engine_time;
	fSlices++;
	fOps.clear();
	return result;
}
