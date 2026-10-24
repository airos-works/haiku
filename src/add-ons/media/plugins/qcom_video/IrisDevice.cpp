/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "IrisDevice.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <qcom_iris.h>


IrisDevice::IrisDevice()
	:
	fDevice(-1)
{
	fFirmware[0] = '\0';
}


IrisDevice::~IrisDevice()
{
	Close();
}


status_t
IrisDevice::Open()
{
	if (fDevice >= 0)
		return B_OK;
	fDevice = open(QCOM_IRIS_DEVICE, O_RDWR | O_CLOEXEC);
	if (fDevice < 0)
		return errno;
	qcom_iris_info info = {};
	if (ioctl(fDevice, QCOM_IRIS_GET_INFO, &info, sizeof(info)) != 0
		|| info.version != QCOM_IRIS_VERSION) {
		Close();
		return B_NOT_SUPPORTED;
	}
	strlcpy(fFirmware, info.firmware, sizeof(fFirmware));
	return B_OK;
}


void
IrisDevice::Close()
{
	// the driver frees the buffers and sessions of the open with it
	if (fDevice >= 0)
		close(fDevice);
	fDevice = -1;
}


status_t
IrisDevice::Send(const void* packet)
{
	qcom_iris_packet request = { packet, *(const uint32*)packet };
	return ioctl(fDevice, QCOM_IRIS_SEND, &request, sizeof(request)) == 0
		? B_OK : errno;
}


status_t
IrisDevice::Receive(void* packet, size_t size, bigtime_t timeout,
	size_t* _received)
{
	qcom_iris_receive request = {};
	request.buffer = packet;
	request.size = size;
	request.timeout = timeout;
	status_t status = ioctl(fDevice, QCOM_IRIS_RECEIVE, &request,
		sizeof(request)) == 0 ? B_OK : errno;
	if (_received != NULL)
		*_received = request.received;
	return status;
}


status_t
IrisDevice::Allocate(size_t size, Buffer& buffer)
{
	qcom_iris_buffer request = {};
	request.size = size;
	if (ioctl(fDevice, QCOM_IRIS_ALLOC, &request, sizeof(request)) != 0)
		return errno;
	buffer.address = request.address;
	buffer.deviceAddress = request.device_address;
	buffer.size = (request.size + B_PAGE_SIZE - 1) & ~(size_t)(B_PAGE_SIZE - 1);
	buffer.area = request.area;
	return B_OK;
}


void
IrisDevice::Free(Buffer& buffer)
{
	if (buffer.deviceAddress == 0)
		return;
	ioctl(fDevice, QCOM_IRIS_FREE, &buffer.deviceAddress,
		sizeof(buffer.deviceAddress));
	// the driver lets go of the codec's mapping; the clone in this team is
	// ours to delete
	delete_area(buffer.area);
	buffer = Buffer();
}
