/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab test of the qcom_iris driver's userland interface (qcom_iris.h):
	the firmware's version, a buffer allocated and touched, and an H.264
	decode session opened and closed through HFI (generation 1). */


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include <qcom_iris.h>


#define HFI_CMD_SYS_SESSION_INIT	0x10007
#define HFI_CMD_SYS_SESSION_END		0x10008
#define HFI_MSG_SYS_SESSION_INIT	0x20006
#define HFI_MSG_SYS_SESSION_END		0x20007
#define HFI_SESSION_TYPE_DEC		2
#define HFI_VIDEO_CODEC_H264		0x00000002

static int sDevice;


static status_t
send(const uint32* packet)
{
	qcom_iris_packet request = { packet, packet[0] };
	return ioctl(sDevice, QCOM_IRIS_SEND, &request, sizeof(request)) == 0
		? B_OK : errno;
}


/*!	The next message of type \a type, others printed and skipped. */
static status_t
receive(uint32 type, uint32* packet, size_t size, bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	while (system_time() < deadline) {
		qcom_iris_receive request = {};
		request.buffer = packet;
		request.size = size;
		request.timeout = deadline - system_time();
		if (ioctl(sDevice, QCOM_IRIS_RECEIVE, &request, sizeof(request)) != 0)
			return errno;
		if (packet[1] == type)
			return B_OK;
		printf("  message %#x (%u bytes)\n", packet[1], request.received);
	}
	return B_TIMED_OUT;
}


int
main()
{
	sDevice = open(QCOM_IRIS_DEVICE, O_RDWR);
	if (sDevice < 0) {
		printf("open: %s\n", strerror(errno));
		return 1;
	}

	qcom_iris_info info = {};
	if (ioctl(sDevice, QCOM_IRIS_GET_INFO, &info, sizeof(info)) == 0) {
		printf("interface %u, firmware \"%s\", %u system properties\n",
			info.version, info.firmware, info.sys_init_properties);
	}

	qcom_iris_buffer buffer = {};
	buffer.size = 1024 * 1024;
	if (ioctl(sDevice, QCOM_IRIS_ALLOC, &buffer, sizeof(buffer)) != 0) {
		printf("alloc: %s\n", strerror(errno));
		return 1;
	}
	memset(buffer.address, 0x5a, buffer.size);
	printf("buffer at %p, device %#x, area %d\n", buffer.address,
		buffer.device_address, (int)buffer.area);

	uint32 session = 0x1234;
	uint32 init[5] = { 20, HFI_CMD_SYS_SESSION_INIT, session,
		HFI_SESSION_TYPE_DEC, HFI_VIDEO_CODEC_H264 };
	status_t status = send(init);
	uint32 packet[1024];
	if (status == B_OK)
		status = receive(HFI_MSG_SYS_SESSION_INIT, packet, sizeof(packet),
			1000000);
	printf("session init: %s, session %#x, error %#x\n", strerror(status),
		packet[2], packet[3]);

	uint32 end[3] = { 12, HFI_CMD_SYS_SESSION_END, session };
	status = send(end);
	if (status == B_OK)
		status = receive(HFI_MSG_SYS_SESSION_END, packet, sizeof(packet),
			1000000);
	printf("session end: %s, error %#x\n", strerror(status), packet[3]);

	ioctl(sDevice, QCOM_IRIS_FREE, &buffer.device_address,
		sizeof(buffer.device_address));
	close(sDevice);
	return status == B_OK ? 0 : 1;
}
