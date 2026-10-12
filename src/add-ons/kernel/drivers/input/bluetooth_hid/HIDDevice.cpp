/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "HIDDevice.h"

#include <new>
#include <stdlib.h>
#include <string.h>

#include "bluetooth_hid.h"
#include <util/AutoLock.h>

#include "Driver.h"
#include "HIDReport.h"
#include "ProtocolHandler.h"


// Reports of one ID that wait for their handler. A key press and release
// that arrive together need two; a mouse that nobody reads fills them and
// then loses its oldest movements, which is harmless.
static const uint32 kQueueSlots = 64;
// How long the Bluetooth server waits for room when a device that is open
// has a full queue: it then reads its channel more slowly, rather than
// losing a key.
static const bigtime_t kQueueFullWait = 500000;
// Keyboard LED changes waiting for the Bluetooth server to send them.
static const uint32 kOutputSlots = 8;


struct HIDDevice::ReportQueue {
	HIDReport*	report;
	uint8*		delivered;
		// the copy the protocol handler reads; it is only reused once the
		// handler waits again, that is, after it finished with it
	size_t		deliveredLength;
	uint8*		slots;
		// kQueueSlots of a size_t length and fMaxReportSize bytes
	uint32		head;
	uint32		count;
};


HIDDevice::HIDDevice(const uint8* descriptor, size_t descriptorLength,
	const char* name)
	:
	fStatus(B_NO_INIT),
	fOpenCount(0),
	fRemoved(false),
	fCookies(0),
	fParser(this),
	fProtocolHandlerCount(0),
	fProtocolHandlerList(NULL),
	fQueues(NULL),
	fQueueCount(0),
	fMaxReportSize(0),
	fOutput(NULL),
	fOutputHead(0),
	fOutputCount(0),
	fDropped(0)
{
	strlcpy(fName, name, sizeof(fName));
	mutex_init(&fLock, "bluetooth hid device");
	fOutputCondition.Init(this, "bluetooth hid output");
	fQueueSpace.Init(this, "bluetooth hid queue space");

	status_t status = fParser.ParseReportDescriptor(descriptor,
		descriptorLength);
	if (status != B_OK) {
		ERROR("%s: the report descriptor does not parse: %s\n", fName,
			strerror(status));
		fStatus = status;
		return;
	}

	fMaxReportSize = fParser.MaxReportSize();
	if (fMaxReportSize == 0 || fMaxReportSize > BLUETOOTH_HID_MAX_REPORT) {
		ERROR("%s: unusable report size %" B_PRIuSIZE "\n", fName,
			fMaxReportSize);
		fStatus = B_BAD_DATA;
		return;
	}

	fQueueCount = fParser.CountReports(HID_REPORT_TYPE_INPUT);
	if (fQueueCount > 0) {
		fQueues = new(std::nothrow) ReportQueue[fQueueCount];
		if (fQueues == NULL) {
			fStatus = B_NO_MEMORY;
			return;
		}
		memset(fQueues, 0, sizeof(ReportQueue) * fQueueCount);
		for (uint32 i = 0; i < fQueueCount; i++) {
			ReportQueue& queue = fQueues[i];
			queue.report = fParser.ReportAt(HID_REPORT_TYPE_INPUT, i);
			queue.delivered = (uint8*)malloc(fMaxReportSize);
			queue.slots = (uint8*)malloc(kQueueSlots
				* (sizeof(size_t) + fMaxReportSize));
			if (queue.delivered == NULL || queue.slots == NULL) {
				fStatus = B_NO_MEMORY;
				return;
			}
		}
	}

	fOutput = (uint8*)malloc(kOutputSlots
		* (sizeof(bluetooth_hid_output) + fMaxReportSize));
	if (fOutput == NULL) {
		fStatus = B_NO_MEMORY;
		return;
	}

	ProtocolHandler::AddHandlers(*this, fProtocolHandlerList,
		fProtocolHandlerCount);
	if (fProtocolHandlerCount == 0) {
		ERROR("%s: no keyboard, mouse, tablet or joystick in its report "
			"descriptor\n", fName);
		fStatus = B_NOT_SUPPORTED;
		return;
	}

	TRACE_ALWAYS("%s: %" B_PRIu32 " input report%s, %" B_PRIu32 " handler%s, "
		"report IDs %s\n", fName, fQueueCount, fQueueCount == 1 ? "" : "s",
		fProtocolHandlerCount, fProtocolHandlerCount == 1 ? "" : "s",
		fParser.UsesReportIDs() ? "yes" : "no");
	fStatus = B_OK;
}


HIDDevice::~HIDDevice()
{
	ProtocolHandler* handler = fProtocolHandlerList;
	while (handler != NULL) {
		ProtocolHandler* next = handler->NextHandler();
		delete handler;
		handler = next;
	}

	if (fQueues != NULL) {
		for (uint32 i = 0; i < fQueueCount; i++) {
			free(fQueues[i].delivered);
			free(fQueues[i].slots);
		}
		delete[] fQueues;
	}
	free(fOutput);
	mutex_destroy(&fLock);
}


status_t
HIDDevice::Open(ProtocolHandler* handler, uint32 flags)
{
	if (fRemoved)
		return B_DEV_NOT_READY;

	atomic_add(&fOpenCount, 1);
	return B_OK;
}


status_t
HIDDevice::Close(ProtocolHandler* handler)
{
	atomic_add(&fOpenCount, -1);
	return B_OK;
}


void
HIDDevice::Removed()
{
	MutexLocker locker(fLock);
	fRemoved = true;
	fOutputCondition.NotifyAll(B_DEV_NOT_READY);
	fQueueSpace.NotifyAll(B_DEV_NOT_READY);
	locker.Unlock();

	// Wake the handlers waiting for reports; they see IsRemoved() and stop.
	// One still busy with a report keeps it (SetReport() would take it away)
	// and is turned away by MaybeScheduleTransfer() when it waits again.
	for (uint32 i = 0; i < fQueueCount; i++) {
		if (fQueues[i].report->HasWaiters())
			fQueues[i].report->SetReport(B_DEV_NOT_READY, NULL, 0);
	}
}


status_t
HIDDevice::MaybeScheduleTransfer(HIDReport* report)
{
	// The handler is about to wait for this report: give it the oldest one
	// that came in meanwhile, if any. It is registered as a waiter already,
	// so it gets what is delivered here.
	MutexLocker locker(fLock);
	if (fRemoved)
		return ENODEV;

	ReportQueue* queue = _QueueFor(report);
	if (queue != NULL && queue->count > 0)
		_Deliver(queue);
	return B_OK;
}


status_t
HIDDevice::SendReport(HIDReport* report)
{
	// Called by a protocol handler (the keyboard LEDs) with the report filled
	// in; queue it for the Bluetooth server, which sends it on the interrupt
	// channel. Never wait for the server here: the input server calls this.
	size_t length = report->ReportSize();
	bool withID = fParser.UsesReportIDs();
	if (length + (withID ? 1 : 0) > fMaxReportSize)
		return B_BAD_VALUE;

	MutexLocker locker(fLock);
	if (fRemoved)
		return ENODEV;
	if (fOutputCount == kOutputSlots) {
		// The server is not reading; drop the oldest, the newest LED state
		// is what matters.
		fOutputHead = (fOutputHead + 1) % kOutputSlots;
		fOutputCount--;
	}

	uint32 slot = (fOutputHead + fOutputCount) % kOutputSlots;
	uint8* entry = fOutput
		+ slot * (sizeof(bluetooth_hid_output) + fMaxReportSize);
	bluetooth_hid_output* header = (bluetooth_hid_output*)entry;
	header->type = report->Type() == HID_REPORT_TYPE_FEATURE
		? BLUETOOTH_HID_REPORT_FEATURE : BLUETOOTH_HID_REPORT_OUTPUT;
	header->reserved = 0;
	header->length = length + (withID ? 1 : 0);
	uint8* data = entry + sizeof(bluetooth_hid_output);
	if (withID)
		*data++ = report->ID();
	memcpy(data, report->CurrentReport(), length);
	fOutputCount++;

	fOutputCondition.NotifyAll();
	return B_OK;
}


ProtocolHandler*
HIDDevice::ProtocolHandlerAt(uint32 index) const
{
	ProtocolHandler* handler = fProtocolHandlerList;
	while (handler != NULL) {
		if (index == 0)
			return handler;

		handler = handler->NextHandler();
		index--;
	}

	return NULL;
}


status_t
HIDDevice::InputReport(const uint8* data, size_t length)
{
	if (length == 0 || length > fMaxReportSize)
		return B_BAD_VALUE;

	uint8 id = fParser.UsesReportIDs() ? data[0] : 0;
	HIDReport* report = fParser.FindReport(HID_REPORT_TYPE_INPUT, id);

	MutexLocker locker(fLock);
	if (fRemoved)
		return ENODEV;

	ReportQueue* queue = _QueueFor(report);
	if (queue == NULL) {
		// Not described by its report descriptor; nobody could read it.
		if (fDropped++ < 5)
			TRACE_ALWAYS("%s: input report %u is not in the descriptor\n",
				fName, id);
		return B_OK;
	}

	size_t slotSize = sizeof(size_t) + fMaxReportSize;
	bigtime_t deadline = system_time() + kQueueFullWait;
	while (queue->count == kQueueSlots && IsOpen() && !fRemoved) {
		ConditionVariableEntry entry;
		fQueueSpace.Add(&entry);
		locker.Unlock();
		status_t status = entry.Wait(B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT,
			deadline);
		locker.Lock();
		if (status != B_OK)
			break;
	}
	if (fRemoved)
		return ENODEV;
	if (queue->count == kQueueSlots) {
		// nobody reads this report: lose the oldest
		queue->head = (queue->head + 1) % kQueueSlots;
		queue->count--;
	}
	uint8* slot = queue->slots
		+ ((queue->head + queue->count) % kQueueSlots) * slotSize;
	*(size_t*)slot = length;
	memcpy(slot + sizeof(size_t), data, length);
	queue->count++;

	if (queue->report->HasWaiters())
		_Deliver(queue);
	return B_OK;
}


status_t
HIDDevice::ReadOutputReport(uint8* buffer, size_t* _length)
{
	MutexLocker locker(fLock);
	while (fOutputCount == 0) {
		if (fRemoved)
			return B_DEV_NOT_READY;

		ConditionVariableEntry entry;
		fOutputCondition.Add(&entry);
		locker.Unlock();
		status_t status = entry.Wait(B_CAN_INTERRUPT);
		locker.Lock();
		if (status != B_OK && fOutputCount == 0)
			return status;
	}

	uint8* entry = fOutput
		+ fOutputHead * (sizeof(bluetooth_hid_output) + fMaxReportSize);
	size_t length = sizeof(bluetooth_hid_output)
		+ ((bluetooth_hid_output*)entry)->length;
	if (*_length < length)
		return B_BUFFER_OVERFLOW;

	memcpy(buffer, entry, length);
	fOutputHead = (fOutputHead + 1) % kOutputSlots;
	fOutputCount--;
	*_length = length;
	return B_OK;
}


HIDDevice::ReportQueue*
HIDDevice::_QueueFor(HIDReport* report)
{
	if (report == NULL)
		return NULL;

	for (uint32 i = 0; i < fQueueCount; i++) {
		if (fQueues[i].report == report)
			return &fQueues[i];
	}
	return NULL;
}


void
HIDDevice::_Deliver(ReportQueue* queue)
{
	// fLock is held: the queue cannot change underneath.
	size_t slotSize = sizeof(size_t) + fMaxReportSize;
	uint8* slot = queue->slots + queue->head * slotSize;
	size_t length = *(size_t*)slot;
	memcpy(queue->delivered, slot + sizeof(size_t), length);

	// The report as the handler parses it: without the report ID.
	uint8* report = queue->delivered;
	if (fParser.UsesReportIDs()) {
		report++;
		length--;
	}

	// A handler whose wait just timed out may have left in between; then
	// nobody got it, and it stays first in line.
	if (queue->report->SetReport(B_OK, report, length) > 0) {
		queue->head = (queue->head + 1) % kQueueSlots;
		queue->count--;
		fQueueSpace.NotifyAll();
	}
}
