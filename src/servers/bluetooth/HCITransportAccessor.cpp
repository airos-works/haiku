/*
 * Copyright 2007-2008 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include <Autolock.h>
#include <String.h>

#include <string.h>

#include <LELog.h>

#include "BluetoothServer.h"
#include "HCITransportAccessor.h"


HCITransportAccessor::HCITransportAccessor(BPath* path)
	:
	HCIDelegate(path),
	fQueueLock("HCI command queue"),
	fCredits(1),
	fStalls(0),
	fLastSent(0)
{
	status_t status;

	fDescriptor = open (path->Path(), O_RDWR);
	if (fDescriptor > 0) {
		// find out which ID was assigned
		status = ioctl(fDescriptor, GET_HCI_ID, &fIdentifier, 0);
		printf("%s: hid retrieved %" B_PRIx32 " status=%" B_PRId32 "\n",
			__FUNCTION__, fIdentifier, status);
	} else {
		printf("%s: Device driver %s could not be opened %" B_PRId32 "\n",
			__FUNCTION__, path->Path(), fIdentifier);
		fIdentifier = B_ERROR;
	}

}


HCITransportAccessor::~HCITransportAccessor()
{
	if (fDescriptor > 0) {
		close(fDescriptor);
		fDescriptor = -1;
		fIdentifier = B_ERROR;
	}
}


status_t
HCITransportAccessor::_Send(const uint8* command, size_t size)
{
	fLastSent = system_time();
	return ioctl(fDescriptor, ISSUE_BT_COMMAND, (void*)command, size);
}


void
HCITransportAccessor::_Drain()
{
	while (fCredits > 0 && !fQueue.empty()) {
		std::vector<uint8> command;
		command.swap(fQueue.front());
		fQueue.pop_front();
		fCredits--;
		status_t status = _Send(command.data(), command.size());
		if (status != B_OK) {
			Bluetooth::LELog(Bluetooth::LE_LOG_ERROR, "hci",
				"queued command %#06x failed to send: %s",
				command[0] | (command[1] << 8), strerror(status));
		}
	}
}


status_t
HCITransportAccessor::IssueCommand(raw_command rc, size_t size)
{
	if (Id() < 0 || fDescriptor < 0)
		return B_ERROR;
	// header and at most 255 bytes of parameters: the drivers refuse
	// anything else
	if (rc == NULL || size < 3 || size > 3 + 255)
		return B_BAD_VALUE;

	BAutolock lock(fQueueLock);
	if (fCredits > 0 && fQueue.empty()) {
		fCredits--;
		return _Send((const uint8*)rc, size);
	}
	if (fQueue.size() >= 64)
		return B_BUSY;
	const uint8* bytes = (const uint8*)rc;
	fQueue.push_back(std::vector<uint8>(bytes, bytes + size));
	Bluetooth::LELog(Bluetooth::LE_LOG_TRACE, "hci",
		"command %#06x queued behind %zu (no credits)",
		bytes[0] | (bytes[1] << 8), fQueue.size() - 1);
	return B_OK;
}


void
HCITransportAccessor::CommandCredits(uint8 credits)
{
	BAutolock lock(fQueueLock);
	fCredits = credits;
	fStalls = 0;
	_Drain();
}


void
HCITransportAccessor::Pulse()
{
	BAutolock lock(fQueueLock);
	// A lost Command Complete must not wedge every later command.
	if (!fQueue.empty() && fCredits == 0
		&& system_time() - fLastSent > 2000000) {
		if (++fStalls >= 3) {
			Bluetooth::LELog(Bluetooth::LE_LOG_ERROR, "hci",
				"controller has not answered %" B_PRId32 " commands in a row; "
				"its USB link has probably dropped (check syslog for \"usb "
				"error\"). Restarting the computer recovers it.", fStalls);
		}
		Bluetooth::LELog(Bluetooth::LE_LOG_ERROR, "hci",
			"no command credit for 2 s with %zu queued; sending anyway",
			fQueue.size());
		fCredits = 1;
		_Drain();
	}
}


status_t
HCITransportAccessor::Launch() {

	uint32 dummy;
	return ioctl(fDescriptor, BT_UP, &dummy, sizeof(uint32));

}
