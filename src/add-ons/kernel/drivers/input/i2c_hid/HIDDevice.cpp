/*
 * Copyright 2020, Jérôme Duval, jerome.duval@gmail.com.
 * Copyright 2008-2011, Michael Lotz <mmlr@mlotz.ch>
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */


//!	Driver for I2C Human Interface Devices.


#include "Driver.h"
#include "GpioInterrupt.h"
#include "HIDDevice.h"
#include "HIDReport.h"
#include "HIDReportItem.h"
#include "HIDWriter.h"
#include "ProtocolHandler.h"

#include <usb/USB_hid.h>
#include <util/AutoLock.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <new>


// As specified in https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/touchscreen-required-hid-top-level-collections
#define HID_USAGE_MICROSOFT_THQA_CERTIFICATE 0xC5


// Input reports of one ID that wait for their handler: a touchpad that
// nobody reads loses its oldest ones, which is harmless.
static const uint32 kQueueSlots = 16;
// Without an interrupt line the input register is polled this often while
// the device is open.
static const bigtime_t kPollInterval = 10000;
// How long the reader waits for the interrupt before it looks again
// (for the device being closed or the driver going away).
static const bigtime_t kInterruptWait = 250000;
// When the line is asserted but the interrupt did not arrive: watch the
// pin instead of waiting for it.
static const bigtime_t kPinPollInterval = 4000;


struct HIDDevice::ReportQueue {
	HIDReport*	report;
	uint8*		delivered;
		// the copy the protocol handler reads; only reused once the handler
		// waits again, that is, after it finished with it
	uint8*		slots;
		// kQueueSlots of a size_t length and fMaxReportSize bytes
	uint32		head;
	uint32		count;
};


HIDDevice::HIDDevice(uint16 descriptorAddress, i2c_device_interface* i2c,
	i2c_device i2cCookie, GpioInterrupt* interrupt)
	:	fStatus(B_NO_INIT),
		fTransferBufferSize(0),
		fTransferBuffer(NULL),
		fInputBuffer(NULL),
		fOpenCount(0),
		fRemoved(false),
		fParser(this),
		fProtocolHandlerCount(0),
		fProtocolHandlerList(NULL),
		fDescriptorAddress(descriptorAddress),
		fI2C(i2c),
		fI2CCookie(i2cCookie),
		fInterrupt(interrupt),
		fPollPinState(false),
		fQueues(NULL),
		fQueueCount(0),
		fMaxReportSize(0),
		fReaderThread(-1),
		fStopReader(false)
{
	mutex_init(&fLock, "i2c hid device");
	fOpenCondition.Init(this, "i2c hid open");

	// fetch HID descriptor
	fStatus = _FetchBuffer((uint8*)&fDescriptorAddress,
		sizeof(fDescriptorAddress), &fDescriptor, sizeof(fDescriptor));
	if (fStatus != B_OK) {
		ERROR("failed to fetch HID descriptor\n");
		return;
	}
	if (fDescriptor.wHIDDescLength != sizeof(i2c_hid_descriptor)
		|| fDescriptor.wMaxInputLength < 3) {
		ERROR("bad HID descriptor: length %u, max input %u\n",
			fDescriptor.wHIDDescLength, fDescriptor.wMaxInputLength);
		fStatus = B_BAD_DATA;
		return;
	}

	// the reset needs the command register from the descriptor
	fStatus = _Reset();
	if (fStatus != B_OK) {
		ERROR("failed to reset the device\n");
		return;
	}

	// fetch HID Report descriptor

	HIDWriter descriptorWriter;

	uint16 descriptorLength = fDescriptor.wReportDescLength;
	fReportDescriptor = (uint8 *)malloc(descriptorLength);
	if (fReportDescriptor == NULL) {
		ERROR("failed to allocate buffer for report descriptor\n");
		fStatus = B_NO_MEMORY;
		return;
	}

	uint16 reportDescRegister = fDescriptor.wReportDescRegister;
	fStatus = _FetchBuffer((uint8*)&reportDescRegister,
		sizeof(reportDescRegister), fReportDescriptor,
		descriptorLength);
	if (fStatus != B_OK) {
		ERROR("failed tot get report descriptor\n");
		free(fReportDescriptor);
		return;
	}

#if 1
	// save report descriptor for troubleshooting
	char outputFile[128];
	sprintf(outputFile, "/tmp/i2c_hid_report_descriptor_%04x_%04x.bin",
		fDescriptor.wVendorID, fDescriptor.wProductID);
	int fd = open(outputFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		write(fd, fReportDescriptor, descriptorLength);
		close(fd);
	}
#endif

	status_t result = fParser.ParseReportDescriptor(fReportDescriptor,
		descriptorLength);
	free(fReportDescriptor);

	if (result != B_OK) {
		ERROR("parsing the report descriptor failed\n");
		fStatus = result;
		return;
	}

// enable for debugging hid reports
#if 0
	for (uint32 i = 0; i < fParser.CountReports(HID_REPORT_TYPE_ANY); i++)
		fParser.ReportAt(HID_REPORT_TYPE_ANY, i)->PrintToStream();
#endif

	if (fParser.MaxReportSize() == 0) {
		TRACE_ALWAYS("report claims a report size of 0\n");
		fStatus = B_BAD_DATA;
		return;
	}

	// Input reports come with their length in front; feature reports read
	// with GET_REPORT, too. We pad the allocations so that we can always
	// read 32 bits at a time (as done in HIDReportItem) without the need
	// for an additional boundary check.
	fMaxReportSize = max_c(fDescriptor.wMaxInputLength,
		fParser.MaxReportSize() + 3);
	fTransferBufferSize = fMaxReportSize;
	fTransferBuffer = (uint8 *)malloc(fTransferBufferSize + 4);
	fInputBuffer = (uint8 *)malloc(fDescriptor.wMaxInputLength + 4);
	if (fTransferBuffer == NULL || fInputBuffer == NULL) {
		TRACE_ALWAYS("failed to allocate transfer buffer\n");
		fStatus = B_NO_MEMORY;
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
			queue.delivered = (uint8*)malloc(fMaxReportSize + 4);
			queue.slots = (uint8*)malloc(kQueueSlots
				* (sizeof(size_t) + fMaxReportSize));
			if (queue.delivered == NULL || queue.slots == NULL) {
				fStatus = B_NO_MEMORY;
				return;
			}
		}
	}

	for (uint32 i = 0; i < fParser.CountReports(HID_REPORT_TYPE_FEATURE); i++) {
		HIDReport *report = fParser.ReportAt(HID_REPORT_TYPE_FEATURE, i);

		// we do nothing with this value other than debugging
		// perhaps we can get rid of this in a future patch
		HIDReportItem *latencyMode = report->FindItem(B_HID_USAGE_PAGE_DIGITIZER,
			B_HID_UID_DIG_LATENCY_MODE);

		if (latencyMode) {
			status_t result = _FetchReport(report);
			if (result != B_OK)
				continue;

			if (latencyMode->Extract() == B_OK) {
				uint32 value = latencyMode->Data();
				TRACE_ALWAYS("Current latency mode:%d\n", value);
			}
		}

		// Some trackpads expects this blob to be fetched before running
		// https://learn.microsoft.com/en-us/windows-hardware/design/component-guidelines/touchpad-windows-precision-touchpad-collection#device-certification-status-feature-report
		// https://patchwork.kernel.org/project/linux-input/patch/1457344958-9987-1-git-send-email-benjamin.tissoires@redhat.com/
		HIDReportItem *win8Blob = report->FindItem(B_HID_USAGE_PAGE_MICROSOFT,
			HID_USAGE_MICROSOFT_THQA_CERTIFICATE);

		if (win8Blob != NULL) {
			status_t result = _FetchReport(report);
			if (result != B_OK)
				continue;

			TRACE_ALWAYS("Fetched a Win8 trackpad blob\n");
		}
	}

	// A touchpad switches itself into reporting contacts when its handler is
	// opened; until then it stays in (or goes back to) mouse mode, which is
	// what it starts in after a reset.

	ProtocolHandler::AddHandlers(*this, fProtocolHandlerList,
		fProtocolHandlerCount);

	// Asleep until somebody opens it.
	_SetPower(I2C_HID_POWER_OFF);

	fReaderThread = spawn_kernel_thread(&_ReaderThread, "i2c hid reader",
		B_REAL_TIME_DISPLAY_PRIORITY, this);
	if (fReaderThread < 0) {
		fStatus = fReaderThread;
		return;
	}
	resume_thread(fReaderThread);

	TRACE_ALWAYS("%04x:%04x, %" B_PRIu32 " input report%s, %" B_PRIu32
		" handler%s, %s\n", fDescriptor.wVendorID, fDescriptor.wProductID,
		fQueueCount, fQueueCount == 1 ? "" : "s", fProtocolHandlerCount,
		fProtocolHandlerCount == 1 ? "" : "s",
		fInterrupt != NULL ? "interrupt driven" : "polled");
	fStatus = B_OK;
}


HIDDevice::~HIDDevice()
{
	if (fReaderThread >= 0) {
		fStopReader = true;
		fOpenCondition.NotifyAll();
		status_t result;
		wait_for_thread(fReaderThread, &result);
	}

	ProtocolHandler *handler = fProtocolHandlerList;
	while (handler != NULL) {
		ProtocolHandler *next = handler->NextHandler();
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

	delete fInterrupt;
	free(fTransferBuffer);
	free(fInputBuffer);
	mutex_destroy(&fLock);
}


status_t
HIDDevice::Open(ProtocolHandler *handler, uint32 flags)
{
	if (fRemoved)
		return B_DEV_NOT_READY;

	MutexLocker locker(fLock);
	if (fOpenCount++ == 0) {
		status_t status = _SetPower(I2C_HID_POWER_ON);
		if (status != B_OK) {
			fOpenCount--;
			return status;
		}
		fOpenCondition.NotifyAll();
	}

	return B_OK;
}


status_t
HIDDevice::Close(ProtocolHandler *handler)
{
	MutexLocker locker(fLock);
	if (--fOpenCount == 0)
		_SetPower(I2C_HID_POWER_OFF);

	return B_OK;
}


void
HIDDevice::Removed()
{
	MutexLocker locker(fLock);
	fRemoved = true;

	// Wake the handlers waiting for reports; they see IsRemoved() and stop.
	for (uint32 i = 0; i < fQueueCount; i++) {
		if (fQueues[i].report->HasWaiters())
			fQueues[i].report->SetReport(B_DEV_NOT_READY, NULL, 0);
	}
}


status_t
HIDDevice::MaybeScheduleTransfer(HIDReport *report)
{
	if (fRemoved)
		return ENODEV;

	if (report->Type() == HID_REPORT_TYPE_FEATURE)
		return _FetchReport(report);

	// The handler is about to wait for this report: give it the oldest one
	// that came in meanwhile, if any. It is registered as a waiter already,
	// so it gets what the reader delivers later, too.
	MutexLocker locker(fLock);
	ReportQueue* queue = _QueueFor(report);
	if (queue != NULL && queue->count > 0)
		_Deliver(queue);

	return B_OK;
}


status_t
HIDDevice::SendReport(HIDReport *report)
{
	uint8 reportType = 0;

	switch (report->Type()) {
		case HID_REPORT_TYPE_INPUT:
			reportType = 1;
			break;

		case HID_REPORT_TYPE_OUTPUT:
			reportType = 2;
			break;

		case HID_REPORT_TYPE_FEATURE:
			reportType = 3;
			break;
	}

	return _WriteReport(reportType, report->ID(), report->CurrentReport(), report->ReportSize());
}


ProtocolHandler *
HIDDevice::ProtocolHandlerAt(uint32 index) const
{
	ProtocolHandler *handler = fProtocolHandlerList;
	while (handler != NULL) {
		if (index == 0)
			return handler;

		handler = handler->NextHandler();
		index--;
	}

	return NULL;
}


/*static*/ status_t
HIDDevice::_ReaderThread(void* data)
{
	((HIDDevice*)data)->_Reader();
	return B_OK;
}


/*!	Reads the input reports as the device signals them and queues them for
	their handlers. The device keeps its interrupt line asserted until its
	report has been read, so each read happens while the line is asserted.
*/
void
HIDDevice::_Reader()
{
	uint32 missedInterrupts = 0;
	uint32 errors = 0;
	uint32 emptyReads = 0;

	while (!fStopReader && !fRemoved) {
		if (fOpenCount == 0) {
			ConditionVariableEntry entry;
			fOpenCondition.Add(&entry);
			if (fOpenCount == 0 && !fStopReader)
				entry.Wait(B_RELATIVE_TIMEOUT, 1000000);
			continue;
		}

		if (fInterrupt != NULL && !fInterrupt->Asserted()) {
			if (fPollPinState) {
				snooze(kPinPollInterval);
				continue;
			}

			status_t status = fInterrupt->Wait(kInterruptWait);
			if (status == B_TIMED_OUT && fInterrupt->Asserted()
				&& ++missedInterrupts == 3) {
				// The line is asserted, but its interrupt does not come
				// through: watch the pin instead.
				TRACE_ALWAYS("%04x:%04x: the interrupt of GPIO %s pin %"
					B_PRIu32 " is not delivered, watching the pin instead\n",
					fDescriptor.wVendorID, fDescriptor.wProductID,
					fInterrupt->ControllerName(), fInterrupt->Pin());
				fInterrupt->Mask();
				fPollPinState = true;
			}
			continue;
		}

		size_t length;
		status_t status = _ReadInput(length);
		if (status != B_OK) {
			if (errors++ < 5) {
				ERROR("%04x:%04x: reading input failed: %s\n",
					fDescriptor.wVendorID, fDescriptor.wProductID,
					strerror(status));
			}
			snooze(errors < 100 ? kPollInterval : 100 * kPollInterval);
			continue;
		}
		errors = 0;

		if (length == 0) {
			// Nothing to report (or the answer to a reset). A line that
			// stays asserted regardless must not keep us reading.
			if (fInterrupt == NULL || ++emptyReads > 2)
				snooze(kPollInterval);
			continue;
		}
		emptyReads = 0;

		_InputReport(fInputBuffer + 2, length);
	}
}


status_t
HIDDevice::_ReadInput(size_t& length)
{
	length = 0;

	// A plain read: the device answers with its pending input report.
	status_t status = _ExecCommand(I2C_OP_READ_STOP, NULL, 0, fInputBuffer,
		fDescriptor.wMaxInputLength);
	if (status != B_OK)
		return status;

	size_t reportLength = fInputBuffer[0] | (fInputBuffer[1] << 8);
	if (reportLength <= 2 || reportLength == 0xffff)
		return B_OK;

	if (reportLength > fDescriptor.wMaxInputLength)
		reportLength = fDescriptor.wMaxInputLength;

	length = reportLength - 2;
	return B_OK;
}


void
HIDDevice::_InputReport(const uint8* data, size_t length)
{
	uint8 id = fParser.UsesReportIDs() ? data[0] : 0;
	HIDReport* report = fParser.FindReport(HID_REPORT_TYPE_INPUT, id);

	MutexLocker locker(fLock);
	ReportQueue* queue = _QueueFor(report);
	if (queue == NULL || length > fMaxReportSize)
		return;

	if (queue->count == kQueueSlots) {
		// nobody reads this report: lose the oldest
		queue->head = (queue->head + 1) % kQueueSlots;
		queue->count--;
	}

	size_t slotSize = sizeof(size_t) + fMaxReportSize;
	uint8* slot = queue->slots
		+ ((queue->head + queue->count) % kQueueSlots) * slotSize;
	*(size_t*)slot = length;
	memcpy(slot + sizeof(size_t), data, length);
	queue->count++;

	if (queue->report->HasWaiters())
		_Deliver(queue);
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
	memset(queue->delivered + length, 0, fMaxReportSize + 4 - length);

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
	}
}


status_t
HIDDevice::_Reset()
{
	CALLED();
	status_t status = _SetPower(I2C_HID_POWER_ON);
	if (status != B_OK)
		return status;

	uint8 cmd[] = {
		(uint8)(fDescriptor.wCommandRegister & 0xff),
		(uint8)(fDescriptor.wCommandRegister >> 8),
		0,
		I2C_HID_CMD_RESET,
	};

	status = _ExecCommand(I2C_OP_WRITE_STOP, cmd, sizeof(cmd), NULL, 0);
	if (status != B_OK) {
		_SetPower(I2C_HID_POWER_OFF);
		return status;
	}

	// The device answers the reset with an empty input report, signalled by
	// its interrupt; read it so that the line is released. Without the line
	// give it the time the specification allows.
	uint8 response[2];
	for (int32 i = 0; i < 50; i++) {
		snooze(10000);
		if (fInterrupt != NULL && !fInterrupt->Asserted())
			continue;
		if (_ExecCommand(I2C_OP_READ_STOP, NULL, 0, response,
				sizeof(response)) == B_OK
			&& response[0] == 0 && response[1] == 0) {
			break;
		}
		if (fInterrupt == NULL && i >= 10)
			break;
	}

	return B_OK;
}


status_t
HIDDevice::_SetPower(uint8 power)
{
	CALLED();
	uint8 cmd[] = {
		(uint8)(fDescriptor.wCommandRegister & 0xff),
		(uint8)(fDescriptor.wCommandRegister >> 8),
		power,
		I2C_HID_CMD_SET_POWER
	};

	// Some devices miss the first command while they are asleep.
	status_t status = B_ERROR;
	for (int32 attempt = 0; attempt < 3 && status != B_OK; attempt++) {
		status = _ExecCommand(I2C_OP_WRITE_STOP, cmd, sizeof(cmd), NULL, 0);
		if (status != B_OK)
			snooze(1000);
	}

	if (status == B_OK && power == I2C_HID_POWER_ON)
		snooze(20000);
	return status;
}


status_t
HIDDevice::_WriteReport(uint8 type, uint8 id, void *data, size_t reportSize)
{
	uint8 reportId = id > 15 ? 15 : id;
	size_t cmdLength = 6;
	uint8 cmd[] = {
		(uint8)(fDescriptor.wCommandRegister & 0xff),
		(uint8)(fDescriptor.wCommandRegister >> 8),
		(uint8)(reportId | (type << 4)),
		I2C_HID_CMD_SET_REPORT,
		0, 0, 0,
	};

	int dataOffset = 4;
	if (reportId == 15) {
		cmd[dataOffset++] = id;
		cmdLength++;
	}

	cmd[dataOffset++] = fDescriptor.wDataRegister & 0xff;
	cmd[dataOffset++] = fDescriptor.wDataRegister >> 8;

	// length, report ID (only if it has one), data
	size_t idLength = id != 0 ? 1 : 0;
	size_t bufferLength = 2 + idLength + reportSize;
	if (bufferLength > fTransferBufferSize)
		return B_BAD_VALUE;

	MutexLocker locker(fLock);
	fTransferBuffer[0] = bufferLength & 0x00ff;
	fTransferBuffer[1] = (bufferLength & 0xff00) >> 8;
	if (id != 0)
		fTransferBuffer[2] = id;

	memcpy(fTransferBuffer + 2 + idLength, data, reportSize);

	return _ExecCommand(I2C_OP_WRITE_STOP, cmd, cmdLength,
		fTransferBuffer, bufferLength);
}


/*!	Reads a feature report with GET_REPORT and hands it to its report, whose
	caller waits for it (or reads it right away).
*/
status_t
HIDDevice::_FetchReport(HIDReport* report)
{
	uint8 type = 3;
	uint8 id = report->ID();
	uint8 reportId = id > 15 ? 15 : id;
	size_t cmdLength = 6;
	uint8 cmd[] = {
		(uint8)(fDescriptor.wCommandRegister & 0xff),
		(uint8)(fDescriptor.wCommandRegister >> 8),
		(uint8)(reportId | (type << 4)),
		I2C_HID_CMD_GET_REPORT,
		0, 0, 0,
	};

	int dataOffset = 4;
	if (reportId == 15) {
		cmd[dataOffset++] = id;
		cmdLength++;
	}
	cmd[dataOffset++] = fDescriptor.wDataRegister & 0xff;
	cmd[dataOffset++] = fDescriptor.wDataRegister >> 8;

	size_t idLength = id != 0 ? 1 : 0;
	size_t bufferLength = report->ReportSize() + idLength + 2;
	if (bufferLength > fTransferBufferSize)
		return B_BAD_VALUE;

	MutexLocker locker(fLock);
	memset(fTransferBuffer, 0, fTransferBufferSize + 4);
	status_t status = _FetchBuffer(cmd, cmdLength, fTransferBuffer,
		bufferLength);
	if (status != B_OK)
		return status;

	uint16 actualLength = fTransferBuffer[0] | (fTransferBuffer[1] << 8);
	TRACE("_FetchReport %" B_PRIuSIZE " %" B_PRIu16 "\n", bufferLength,
		actualLength);
	if (actualLength < 2 + idLength || actualLength == 0xffff)
		return B_BAD_DATA;

	report->SetReport(B_OK, fTransferBuffer + 2 + idLength,
		min_c(actualLength, bufferLength) - 2 - idLength);
	return B_OK;
}


status_t
HIDDevice::_FetchBuffer(uint8* cmd, size_t cmdLength, void* buffer,
	size_t bufferLength)
{
	return _ExecCommand(I2C_OP_READ_STOP, cmd, cmdLength,
		buffer, bufferLength);
}


status_t
HIDDevice::_ExecCommand(i2c_op op, uint8* cmd, size_t cmdLength, void* buffer,
	size_t bufferLength)
{
	status_t status = fI2C->acquire_bus(fI2CCookie);
	if (status != B_OK)
		return status;
	status = fI2C->exec_command(fI2CCookie, op, cmd, cmdLength, buffer, bufferLength);
	fI2C->release_bus(fI2CCookie);
	return status;
}
