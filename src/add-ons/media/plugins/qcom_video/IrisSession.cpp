/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "IrisSession.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


static const bigtime_t kAnswerTimeout = 1000000;
static const size_t kMessageSize = 4096;

static uint32 sNextSession = 0x10;


bool
iris_debug_enabled()
{
	static int enabled = -1;
	if (enabled < 0)
		enabled = getenv("IRIS_DEBUG") != NULL ? 1 : 0;
	return enabled != 0;
}


IrisSession::IrisSession()
	:
	fSession(0),
	fOpen(false),
	fStarted(false),
	fFailed(false)
{
}


IrisSession::~IrisSession()
{
}


status_t
IrisSession::_OpenSession(uint32 type, uint32 codec)
{
	status_t status = fDevice.Open();
	if (status != B_OK) {
		return _Fail(status, "cannot open the video codec: %s",
			strerror(status));
	}

	// unique among the teams that use the codec: the driver refuses a
	// session another open has
	for (int32 attempt = 0; attempt < 16; attempt++) {
		fSession = ((uint32)getpid() << 12)
			| (atomic_add((int32*)&sNextSession, 1) & 0xfff);
		uint32 init[5] = { 20, HFI_CMD_SYS_SESSION_INIT, fSession, type,
			codec };
		status = fDevice.Send(init);
		if (status != B_BUSY)
			break;
	}
	if (status != B_OK)
		_Fail(status, "cannot send to the firmware: %s", strerror(status));
	if (status == B_OK)
		status = _WaitFor(HFI_MSG_SYS_SESSION_INIT);
	if (status != B_OK) {
		_Fail(status, "the firmware did not open a session: %s",
			strerror(status));
		fDevice.Close();
		return status;
	}
	fOpen = true;
	return B_OK;
}


void
IrisSession::_EndSession()
{
	if (!fOpen)
		return;
	if (fStarted && !fFailed) {
		_Simple(HFI_CMD_SESSION_STOP, HFI_MSG_SESSION_STOP);
		if (!fFailed) {
			_Simple(HFI_CMD_SESSION_RELEASE_RESOURCES,
				HFI_MSG_SESSION_RELEASE_RESOURCES);
		}
	}
	_Simple(HFI_CMD_SYS_SESSION_END, HFI_MSG_SYS_SESSION_END);
	fOpen = false;
	fStarted = false;
}


status_t
IrisSession::_Fail(status_t status, const char* format, ...)
{
	char message[256];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	if (fError.IsEmpty())
		fError = message;
	IRIS_TRACE("%s\n", message);
	return status;
}


status_t
IrisSession::_Send(const uint32* packet)
{
	if (iris_debug_enabled() && packet[1] != HFI_CMD_SESSION_EMPTY_BUFFER
		&& packet[1] != HFI_CMD_SESSION_FILL_BUFFER) {
		IRIS_TRACE("send %#" B_PRIx32 " (%" B_PRIu32 " bytes)", packet[1],
			packet[0]);
		for (uint32 i = 3; i < packet[0] / 4 && i < 16; i++)
			fprintf(stderr, " %#" B_PRIx32, packet[i]);
		fprintf(stderr, "\n");
	}
	static const bool sTraceAll = getenv("IRIS_TRACE_ALL") != NULL;
	if (sTraceAll) {
		// (lab: every packet, to compare runs)
		fprintf(stderr, "iris> %" B_PRId64 ":", system_time());
		for (uint32 i = 0; i < packet[0] / 4 && i < 24; i++)
			fprintf(stderr, " %" B_PRIx32, i == 2 ? 0 : packet[i]);
		fprintf(stderr, "\n");
	}
	status_t status = fDevice.Send(packet);
	if (status != B_OK)
		return _Fail(status, "cannot send to the firmware: %s",
			strerror(status));
	return B_OK;
}


status_t
IrisSession::_SetProperty(uint32 property, const uint32* payload,
	uint32 words)
{
	uint32 packet[32] = { 20 + words * 4, HFI_CMD_SESSION_SET_PROPERTY,
		fSession, 1, property };
	if (words > 0)
		memcpy(packet + 5, payload, words * 4);
	return _Send(packet);
}


status_t
IrisSession::_SetProperty(uint32 property, uint32 a)
{
	return _SetProperty(property, &a, 1);
}


status_t
IrisSession::_SetProperty(uint32 property, uint32 a, uint32 b)
{
	uint32 payload[2] = { a, b };
	return _SetProperty(property, payload, 2);
}


status_t
IrisSession::_SetProperty(uint32 property, uint32 a, uint32 b, uint32 c)
{
	uint32 payload[3] = { a, b, c };
	return _SetProperty(property, payload, 3);
}


status_t
IrisSession::_Simple(uint32 command, uint32 answer)
{
	uint32 packet[3] = { 12, command, fSession };
	status_t status = _Send(packet);
	if (status == B_OK)
		status = _WaitFor(answer);
	return status;
}


status_t
IrisSession::_Flush(uint32 type)
{
	uint32 packet[4] = { 16, HFI_CMD_SESSION_FLUSH, fSession, type };
	status_t status = _Send(packet);
	if (status == B_OK)
		status = _WaitFor(HFI_MSG_SESSION_FLUSH);
	return status;
}


status_t
IrisSession::_WaitFor(uint32 type)
{
	uint32 message[kMessageSize / 4];
	bigtime_t deadline = system_time() + kAnswerTimeout;
	while (true) {
		bigtime_t left = deadline - system_time();
		if (left <= 0)
			break;
		size_t size = 0;
		status_t status = fDevice.Receive(message, sizeof(message), left,
			&size);
		if (status == B_TIMED_OUT || status == B_WOULD_BLOCK)
			break;
		if (status != B_OK)
			return _Fail(status, "cannot receive: %s", strerror(status));
		status = _Handle(message, size);
		if (message[1] == type) {
			if (size >= 16 && message[3] != 0) {
				return _Fail(B_ERROR, "the firmware answered %#" B_PRIx32
					" with error %#" B_PRIx32, type, message[3]);
			}
			return status;
		}
		if (status != B_OK)
			return status;
	}
	return _Fail(B_TIMED_OUT, "no answer %#" B_PRIx32 " from the firmware",
		type);
}


status_t
IrisSession::_Pump(bigtime_t timeout)
{
	uint32 message[kMessageSize / 4];
	size_t size = 0;
	status_t status = fDevice.Receive(message, sizeof(message), timeout,
		&size);
	if (status == B_WOULD_BLOCK)
		return B_TIMED_OUT;
	if (status != B_OK)
		return status;
	static const bool sTraceAll = getenv("IRIS_TRACE_ALL") != NULL;
	if (sTraceAll) {
		fprintf(stderr, "iris< %" B_PRId64 ":", system_time());
		for (uint32 i = 0; i < size / 4 && i < 24; i++)
			fprintf(stderr, " %" B_PRIx32, i == 2 ? 0 : message[i]);
		fprintf(stderr, "\n");
	}
	return _Handle(message, size);
}


status_t
IrisSession::_HandleErrorEvent(const uint32* packet, size_t size)
{
	if (size < 24)
		return B_OK;
	uint32 event = packet[3];
	uint32 data1 = packet[4];
	uint32 data2 = packet[5];
	if (event == HFI_EVENT_SYS_ERROR) {
		fFailed = true;
		return _Fail(B_DEV_NOT_READY, "the codec's firmware failed");
	}
	if (event != HFI_EVENT_SESSION_ERROR)
		return B_OK;

	switch (data1) {
		case HFI_ERR_SESSION_UNSUPPORTED_SETTING:
		case HFI_ERR_SESSION_UNSUPPORT_BUFFERTYPE:
		case HFI_ERR_SESSION_INVALID_SCALE_FACTOR:
		case HFI_ERR_SESSION_UPSCALE_NOT_SUPPORTED:
			IRIS_TRACE("session error %#" B_PRIx32 " for command %#" B_PRIx32
				"\n", data1, data2);
			return B_OK;
	}
	fFailed = true;
	return _Fail(B_ERROR, "session error %#" B_PRIx32 " (command %#" B_PRIx32
		")", data1, data2);
}


status_t
IrisSession::_SetInternal(uint32 type, IrisDevice::Buffer& buffer,
	size_t size)
{
	// (what a codec does not use)
	if (size == 0)
		return B_OK;

	status_t status = fDevice.Allocate(size, buffer);
	if (status != B_OK) {
		return _Fail(status, "no memory for a %zu byte codec buffer: %s",
			size, strerror(status));
	}
	uint32 packet[9] = { 36, HFI_CMD_SESSION_SET_BUFFERS, fSession, type,
		(uint32)buffer.size, 0, (uint32)buffer.size, 1,
		buffer.deviceAddress };
	return _Send(packet);
}


status_t
IrisSession::_ReleaseInternal(uint32 type, IrisDevice::Buffer& buffer)
{
	if (buffer.deviceAddress == 0)
		return B_OK;
	uint32 packet[9] = { 36, HFI_CMD_SESSION_RELEASE_BUFFERS, fSession, type,
		(uint32)buffer.size, 0, 1, 1, buffer.deviceAddress };
	status_t status = _Send(packet);
	if (status == B_OK)
		status = _WaitFor(HFI_MSG_SESSION_RELEASE_BUFFERS);
	if (status == B_OK)
		fDevice.Free(buffer);
	return status;
}
