/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Host test of what i2c_hid does with a precision touchpad when the system
	resumes (haiku#187): the driver's HIDDevice and the shared HID parser and
	TouchpadProtocolHandler, against a fake touchpad on a fake I2C bus that
	behaves as the specification says: after a reset, and after losing its
	power, it is a mouse again until it is told to report contacts.

	And of what the touchpad handler makes of the contacts of a real one (the
	Framework Laptop 13's PixArt PIXA3854): palms, fingers on its edges,
	contact slots not in use.

	The reader thread is run by the test itself, for a few rounds.
*/


#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Driver.h"
#include "GpioInterrupt.h"
#include "HIDCollection.h"
// (the test hands the device its input reports, as its reader does)
#define private public
#include "HIDDevice.h"
#undef private
#include "HIDParser.h"
#include "HIDReport.h"
#include "I2CHIDProtocol.h"
#include "ProtocolHandler.h"
#include "TouchpadProtocolHandler.h"

#include <kdevice_manager.h>


// #pragma mark - the kernel


static char sLog[16384];
static bool sVerbose;

extern "C" int
test_dprintf(const char* format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	strncat(sLog, line, sizeof(sLog) - strlen(sLog) - 1);
	if (sVerbose)
		printf("    | %s", line);
	return 0;
}


const char*
test_strerror(status_t status)
{
	static char buffer[32];
	snprintf(buffer, sizeof(buffer), "error %d", (int)status);
	return status == B_OK ? "No error" : buffer;
}


void mutex_init(mutex* m, const char*) { m->locked = 0; }
void mutex_destroy(mutex*) {}

void
mutex_lock(mutex* m)
{
	if (m->locked != 0) {
		printf("FAILED: a lock is taken twice (deadlock)\n");
		exit(1);
	}
	m->locked = 1;
}

void mutex_unlock(mutex* m) { m->locked = 0; }


static thread_func sThreadFunction;
static void* sThreadData;

extern "C" thread_id
spawn_kernel_thread(thread_func function, const char*, int32, void* data)
{
	sThreadFunction = function;
	sThreadData = data;
	return 1;
}

extern "C" status_t resume_thread(thread_id) { return B_OK; }
extern "C" status_t wait_for_thread(thread_id, status_t*) { return B_OK; }

static bigtime_t sTime;
static void (*sSnoozeHook)();

extern "C" status_t
snooze(bigtime_t time)
{
	sTime += time;
	if (sSnoozeHook != NULL)
		sSnoozeHook();
	return B_OK;
}

extern "C" bigtime_t system_time(void) { return sTime; }

extern "C" int32
atomic_add(int32* value, int32 addValue)
{
	int32 old = *value;
	*value += addValue;
	return old;
}

extern "C" int32 atomic_get(int32* value) { return *value; }

static uint32 sResumesFinished;
extern "C" uint32 device_manager_resumes_finished(void)
{
	return sResumesFinished;
}

// the driver's debug dump of the report descriptor: not on the build host
extern "C" int test_open(const char*, int, ...) { return -1; }


// The device is polled here (no interrupt line), so nothing of these runs.
GpioInterrupt::~GpioInterrupt() {}
bool GpioInterrupt::Asserted() const { return false; }
status_t GpioInterrupt::Wait(bigtime_t) { return B_TIMED_OUT; }
void GpioInterrupt::Mask() {}
#ifdef TEST_NEW
void GpioInterrupt::Configure() {}
#else
void GpioInterrupt::Configure() {}
#endif
const char* GpioInterrupt::ControllerName() const { return "none"; }


// #pragma mark - ProtocolHandler, as much as the touchpad handler needs


ProtocolHandler::ProtocolHandler(HIDDevice* device, const char* basePath,
	size_t ringBufferSize)
	:
	fStatus(B_OK),
	fDevice(device),
	fBasePath(basePath),
	fPublishPath(NULL),
	fRingBuffer(NULL),
	fNextHandler(NULL)
{
}

ProtocolHandler::~ProtocolHandler() {}

void
ProtocolHandler::AddHandlers(HIDDevice& device, ProtocolHandler*& handlerList,
	uint32& handlerCount)
{
	HIDCollection* root = device.Parser().RootCollection();
	uint32 count = root->CountChildrenFlat(COLLECTION_APPLICATION);
	for (uint32 i = 0; i < count; i++) {
		HIDCollection* collection = root->ChildAtFlat(COLLECTION_APPLICATION,
			i);
		if (collection != NULL)
			TouchpadProtocolHandler::AddHandlers(device, *collection,
				handlerList);
	}
	handlerCount = 0;
	for (ProtocolHandler* handler = handlerList; handler != NULL;
			handler = handler->NextHandler()) {
		handlerCount++;
	}
}

status_t ProtocolHandler::Open(uint32 flags, uint32* cookie)
{
	return fDevice->Open(this, flags);
}

status_t ProtocolHandler::Close(uint32* cookie)
{
	*cookie |= PROTOCOL_HANDLER_COOKIE_FLAG_CLOSED;
	return fDevice->Close(this);
}

status_t ProtocolHandler::Read(uint32*, off_t, void*, size_t*)
{
	return B_ERROR;
}

status_t ProtocolHandler::Write(uint32*, off_t, const void*, size_t*)
{
	return B_ERROR;
}

status_t ProtocolHandler::Control(uint32*, uint32, void*, size_t)
{
	return B_ERROR;
}

#ifdef TEST_NEW
void ProtocolHandler::DeviceWasReset() {}
#endif

void ProtocolHandler::SetNextHandler(ProtocolHandler* next)
{
	fNextHandler = next;
}

status_t ProtocolHandler::IOGetDeviceName(const char*, void*, size_t)
{
	return B_OK;
}


// #pragma mark - the touchpad


// Microsoft's sample descriptor of a precision touchpad, with two fingers:
// the touch pad collection (report 3, device capabilities in feature report
// 8), the configuration collection (input mode in feature report 4, surface
// and button switch in 5) and the mouse it is until it is configured
// (report 1).
#define FINGER \
	0x05, 0x0d, 0x09, 0x22, 0xa1, 0x02, 0x15, 0x00, 0x25, 0x01, 0x09, 0x47, \
	0x09, 0x42, 0x95, 0x02, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x02, \
	0x25, 0x02, 0x09, 0x51, 0x81, 0x02, 0x75, 0x01, 0x95, 0x04, 0x81, 0x03, \
	0x05, 0x01, 0x15, 0x00, 0x26, 0xff, 0x0f, 0x75, 0x10, 0x55, 0x0e, 0x65, \
	0x13, 0x09, 0x30, 0x35, 0x00, 0x46, 0x90, 0x01, 0x95, 0x01, 0x81, 0x02, \
	0x46, 0x13, 0x01, 0x09, 0x31, 0x81, 0x02, 0xc0

static const uint8 kReportDescriptor[] = {
	0x05, 0x0d, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x03,
	FINGER,
	FINGER,
	0x55, 0x0c, 0x66, 0x01, 0x10, 0x47, 0xff, 0xff, 0x00, 0x00, 0x27, 0xff,
	0xff, 0x00, 0x00, 0x75, 0x10, 0x95, 0x01, 0x05, 0x0d, 0x09, 0x56, 0x81,
	0x02, 0x09, 0x54, 0x25, 0x7f, 0x95, 0x01, 0x75, 0x08, 0x81, 0x02, 0x05,
	0x09, 0x09, 0x01, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02, 0x95,
	0x07, 0x81, 0x03, 0x05, 0x0d, 0x85, 0x08, 0x09, 0x55, 0x09, 0x59, 0x75,
	0x04, 0x95, 0x02, 0x25, 0x0f, 0xb1, 0x02, 0xc0,

	0x05, 0x0d, 0x09, 0x0e, 0xa1, 0x01, 0x85, 0x04, 0x09, 0x22, 0xa1, 0x02,
	0x09, 0x52, 0x15, 0x00, 0x25, 0x0a, 0x75, 0x08, 0x95, 0x01, 0xb1, 0x02,
	0xc0, 0x09, 0x22, 0xa1, 0x00, 0x85, 0x05, 0x09, 0x57, 0x09, 0x58, 0x75,
	0x01, 0x95, 0x02, 0x25, 0x01, 0xb1, 0x02, 0x95, 0x06, 0xb1, 0x03, 0xc0,
	0xc0,

	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x01, 0x09, 0x01, 0xa1, 0x00,
	0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02,
	0x81, 0x02, 0x95, 0x06, 0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
	0x75, 0x10, 0x95, 0x02, 0x25, 0x0a, 0x81, 0x06, 0xc0, 0xc0
};

// The PixArt PIXA3854 (093a:0274) of the Framework Laptop 13, as it reports
// itself: a mouse (report 2), the touch pad with five contacts (report 1:
// confidence, tip, identifier, x 0-1337, y 0-876 each; then the contact
// count, three buttons and the scan time) and its configuration.
static const uint8 kPixArtDescriptor[] = {
	0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x85, 0x02, 0x05, 0x01, 0x09, 0x01,
	0xa1, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x02, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x02, 0x81, 0x02, 0x05, 0x0d, 0x09, 0x32, 0x75, 0x01,
	0x95, 0x01, 0x81, 0x02, 0x75, 0x05, 0x95, 0x01, 0x81, 0x03, 0x05, 0x01,
	0x75, 0x10, 0x95, 0x01, 0x35, 0x00, 0x45, 0x00, 0x16, 0x00, 0x80, 0x26,
	0xff, 0x7f, 0x09, 0x30, 0x81, 0x26, 0x16, 0x00, 0x80, 0x26, 0xff, 0x7f,
	0x09, 0x31, 0x81, 0x26, 0xc0, 0xc0, 0x05, 0x0d, 0x09, 0x05, 0xa1, 0x01,
	0x85, 0x01, 0x05, 0x0d, 0x09, 0x22, 0xa1, 0x02, 0x09, 0x47, 0x09, 0x42,
	0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02, 0x95, 0x06,
	0x81, 0x03, 0x09, 0x51, 0x25, 0x0f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
	0x05, 0x01, 0x09, 0x30, 0x75, 0x10, 0x55, 0x0e, 0x65, 0x11, 0x35, 0x00,
	0x46, 0x5a, 0x04, 0x27, 0x39, 0x05, 0x00, 0x00, 0x81, 0x02, 0x09, 0x31,
	0x46, 0xda, 0x02, 0x27, 0x6c, 0x03, 0x00, 0x00, 0x81, 0x02, 0xc0, 0x05,
	0x0d, 0x09, 0x22, 0xa1, 0x02, 0x09, 0x47, 0x09, 0x42, 0x15, 0x00, 0x25,
	0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02, 0x95, 0x06, 0x81, 0x03, 0x09,
	0x51, 0x25, 0x0f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0x05, 0x01, 0x09,
	0x30, 0x75, 0x10, 0x55, 0x0e, 0x65, 0x11, 0x35, 0x00, 0x46, 0x5a, 0x04,
	0x27, 0x39, 0x05, 0x00, 0x00, 0x81, 0x02, 0x09, 0x31, 0x46, 0xda, 0x02,
	0x27, 0x6c, 0x03, 0x00, 0x00, 0x81, 0x02, 0xc0, 0x05, 0x0d, 0x09, 0x22,
	0xa1, 0x02, 0x09, 0x47, 0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
	0x95, 0x02, 0x81, 0x02, 0x95, 0x06, 0x81, 0x03, 0x09, 0x51, 0x25, 0x0f,
	0x75, 0x08, 0x95, 0x01, 0x81, 0x02, 0x05, 0x01, 0x09, 0x30, 0x75, 0x10,
	0x55, 0x0e, 0x65, 0x11, 0x35, 0x00, 0x46, 0x5a, 0x04, 0x27, 0x39, 0x05,
	0x00, 0x00, 0x81, 0x02, 0x09, 0x31, 0x46, 0xda, 0x02, 0x27, 0x6c, 0x03,
	0x00, 0x00, 0x81, 0x02, 0xc0, 0x05, 0x0d, 0x09, 0x22, 0xa1, 0x02, 0x09,
	0x47, 0x09, 0x42, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81,
	0x02, 0x95, 0x06, 0x81, 0x03, 0x09, 0x51, 0x25, 0x0f, 0x75, 0x08, 0x95,
	0x01, 0x81, 0x02, 0x05, 0x01, 0x09, 0x30, 0x75, 0x10, 0x55, 0x0e, 0x65,
	0x11, 0x35, 0x00, 0x46, 0x5a, 0x04, 0x27, 0x39, 0x05, 0x00, 0x00, 0x81,
	0x02, 0x09, 0x31, 0x46, 0xda, 0x02, 0x27, 0x6c, 0x03, 0x00, 0x00, 0x81,
	0x02, 0xc0, 0x05, 0x0d, 0x09, 0x22, 0xa1, 0x02, 0x09, 0x47, 0x09, 0x42,
	0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02, 0x81, 0x02, 0x95, 0x06,
	0x81, 0x03, 0x09, 0x51, 0x25, 0x0f, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
	0x05, 0x01, 0x09, 0x30, 0x75, 0x10, 0x55, 0x0e, 0x65, 0x11, 0x35, 0x00,
	0x46, 0x5a, 0x04, 0x27, 0x39, 0x05, 0x00, 0x00, 0x81, 0x02, 0x09, 0x31,
	0x46, 0xda, 0x02, 0x27, 0x6c, 0x03, 0x00, 0x00, 0x81, 0x02, 0xc0, 0x05,
	0x0d, 0x09, 0x54, 0x15, 0x00, 0x25, 0x05, 0x75, 0x08, 0x95, 0x01, 0x81,
	0x02, 0x05, 0x09, 0x09, 0x01, 0x09, 0x02, 0x09, 0x03, 0x15, 0x00, 0x25,
	0x01, 0x75, 0x01, 0x95, 0x03, 0x81, 0x02, 0x95, 0x05, 0x81, 0x03, 0x05,
	0x0d, 0x09, 0x56, 0x55, 0x0c, 0x66, 0x01, 0x10, 0x35, 0x00, 0x47, 0xff,
	0xff, 0x00, 0x00, 0x15, 0x00, 0x27, 0xff, 0xff, 0x00, 0x00, 0x75, 0x10,
	0x95, 0x01, 0x81, 0x02, 0x05, 0x0d, 0x09, 0x55, 0x15, 0x00, 0x25, 0x05,
	0x75, 0x08, 0x95, 0x01, 0x85, 0x03, 0xb1, 0x02, 0x05, 0x0d, 0x09, 0x59,
	0x15, 0x00, 0x25, 0x0f, 0x75, 0x08, 0x95, 0x01, 0x85, 0x04, 0xb1, 0x02,
	0x06, 0x00, 0xff, 0x85, 0x05, 0x75, 0x08, 0x15, 0x00, 0x09, 0xc6, 0x25,
	0x08, 0x95, 0x01, 0xb1, 0x02, 0x09, 0xc7, 0x26, 0xff, 0x00, 0x95, 0x20,
	0xb1, 0x02, 0xc0, 0x05, 0x0d, 0x09, 0x0e, 0xa1, 0x01, 0x05, 0x0d, 0x09,
	0x22, 0xa1, 0x02, 0x09, 0x52, 0x15, 0x00, 0x25, 0x0a, 0x75, 0x08, 0x95,
	0x01, 0x85, 0x06, 0xb1, 0x02, 0xc0, 0x05, 0x0d, 0x09, 0x22, 0xa1, 0x00,
	0x09, 0x57, 0x09, 0x58, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x02,
	0x85, 0x07, 0xb1, 0x02, 0x95, 0x06, 0xb1, 0x03, 0xc0, 0x05, 0x0d, 0x09,
	0x60, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x85, 0x08, 0xb1,
	0x02, 0x95, 0x07, 0xb1, 0x03, 0xc0, 0x06, 0x00, 0xff, 0x09, 0x01, 0xa1,
	0x01, 0x85, 0x42, 0x09, 0x06, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08,
	0x95, 0x03, 0xb1, 0x02, 0x06, 0x00, 0xff, 0x09, 0x05, 0x15, 0x00, 0x26,
	0xff, 0x00, 0x75, 0x08, 0x96, 0x00, 0x01, 0x85, 0x41, 0xb1, 0x02, 0x85,
	0x43, 0x09, 0x06, 0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x03,
	0xb1, 0x02, 0xc0
};

static const uint16 kDescriptorRegister = 0x0001;
static const uint16 kReportDescriptorRegister = 0x0002;
static const uint16 kInputRegister = 0x0003;
static const uint16 kCommandRegister = 0x0005;
static const uint16 kDataRegister = 0x0006;

static const uint8 kInputModeMouse = 0;
static const uint8 kInputModeTouchpad = 3;
static const uint8 kInputModeReportID = 4;
static const uint8 kSwitchesReportID = 5;


struct FakeTouchpad {
	const uint8*	descriptor;
	size_t			descriptorLength;
		// NULL: Microsoft's sample
	uint8	inputMode;
	uint8	switches;
		// bit 0 surface, bit 1 buttons; both on after a reset
	bool	resetAnswerPending;
	int		resets;
	int		modeWrites;
	int		commands;

	void PowerOn()
	{
		inputMode = kInputModeMouse;
		switches = 0x3;
		resetAnswerPending = false;
	}

	void Reset()
	{
		resets++;
		PowerOn();
		resetAnswerPending = true;
	}

	status_t Command(i2c_op op, const uint8* cmd, size_t cmdLength,
		uint8* data, size_t dataLength)
	{
		commands++;
		bool read = op == I2C_OP_READ || op == I2C_OP_READ_STOP;

		if (cmdLength == 0 && read) {
			// the input register: the answer to a reset, or nothing
			memset(data, 0, dataLength);
			resetAnswerPending = false;
			return B_OK;
		}

		if (cmdLength == 2 && read) {
			uint16 reg = cmd[0] | (cmd[1] << 8);
			if (reg == kDescriptorRegister) {
				i2c_hid_descriptor descriptor = {};
				descriptor.wHIDDescLength = sizeof(descriptor);
				descriptor.bcdVersion = 0x0100;
				descriptor.wReportDescLength = this->descriptor != NULL
					? descriptorLength : sizeof(kReportDescriptor);
				descriptor.wReportDescRegister = kReportDescriptorRegister;
				descriptor.wInputRegister = kInputRegister;
				descriptor.wMaxInputLength = 40;
				descriptor.wCommandRegister = kCommandRegister;
				descriptor.wDataRegister = kDataRegister;
				descriptor.wVendorID = 0x093a;
				descriptor.wProductID = 0x0274;
				memcpy(data, &descriptor, min_c(dataLength,
					sizeof(descriptor)));
				return B_OK;
			}
			if (reg == kReportDescriptorRegister) {
				if (this->descriptor != NULL) {
					memcpy(data, this->descriptor, min_c(dataLength,
						descriptorLength));
				} else {
					memcpy(data, kReportDescriptor, min_c(dataLength,
						sizeof(kReportDescriptor)));
				}
				return B_OK;
			}
			return B_ERROR;
		}

		if (cmdLength < 4 || (cmd[0] | (cmd[1] << 8)) != kCommandRegister)
			return B_ERROR;

		uint8 opcode = cmd[3] & 0x0f;
		uint8 reportID = cmd[2] & 0x0f;
		switch (opcode) {
			case I2C_HID_CMD_RESET:
				Reset();
				return B_OK;

			case I2C_HID_CMD_SET_POWER:
				return B_OK;

			case I2C_HID_CMD_SET_REPORT:
				// the data: length, report ID, payload
				if (dataLength < 4 || data[2] != reportID)
					return B_ERROR;
				if (reportID == kInputModeReportID) {
					inputMode = data[3];
					modeWrites++;
				} else if (reportID == kSwitchesReportID)
					switches = data[3] & 0x3;
				return B_OK;

			case I2C_HID_CMD_GET_REPORT:
				memset(data, 0, dataLength);
				if (dataLength < 4)
					return B_ERROR;
				data[0] = 4;
				data[2] = reportID;
				if (reportID == kInputModeReportID)
					data[3] = inputMode;
				else if (reportID == kSwitchesReportID)
					data[3] = switches;
				return B_OK;
		}
		return B_ERROR;
	}
};


static status_t
fake_exec_command(i2c_device cookie, i2c_op op, const void* cmd,
	size_t cmdLength, void* data, size_t dataLength)
{
	return ((FakeTouchpad*)cookie)->Command(op, (const uint8*)cmd, cmdLength,
		(uint8*)data, dataLength);
}

static status_t fake_acquire_bus(i2c_device) { return B_OK; }
static void fake_release_bus(i2c_device) {}

static i2c_device_interface sI2C = {
	fake_exec_command,
	fake_acquire_bus,
	fake_release_bus
};


// #pragma mark - the test


static int sFailures;

static void
check(bool ok, const char* format, ...)
{
	char text[256];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	if (!ok)
		sFailures++;
	printf("%s: %s\n", ok ? "ok" : "FAILED", text);
}


static HIDDevice* sDevice;
static int sRounds;

static void
stop_reader_after_some_rounds()
{
	// the reader snoozes once per empty poll
	if (++sRounds == 5)
		sDevice->Removed();
}


static void
run_reader(HIDDevice* device)
{
	sDevice = device;
	sRounds = 0;
	sSnoozeHook = stop_reader_after_some_rounds;
	sThreadFunction(sThreadData);
	sSnoozeHook = NULL;
}


static const char*
mode_name(uint8 mode)
{
	return mode == kInputModeTouchpad ? "reporting contacts"
		: mode == kInputModeMouse ? "a mouse" : "?";
}


/*!	One sleep of the system with the touchpad open, as input_server has it.
	\a losesPower: a suspend to RAM; else a sleep in S0 (suspend to idle),
	in which the touchpad keeps what it was set up to do.
*/
static void
test_sleep(bool losesPower)
{
	const char* sleep = losesPower ? "suspend to RAM" : "suspend to idle";
	printf("--- %s\n", sleep);

	FakeTouchpad touchpad = {};
	touchpad.PowerOn();
	sLog[0] = '\0';
	sThreadFunction = NULL;

	HIDDevice* device = new HIDDevice(kDescriptorRegister, &sI2C, &touchpad,
		NULL);
	if (device->InitCheck() != B_OK || sThreadFunction == NULL) {
		check(false, "the device is set up (%s)\n%s",
			test_strerror(device->InitCheck()), sLog);
		return;
	}

	ProtocolHandler* handler = device->ProtocolHandlerAt(0);
	check(handler != NULL && device->ProtocolHandlerAt(1) == NULL,
		"one handler, for the touch pad collection");
	if (handler == NULL)
		return;

	check(touchpad.inputMode == kInputModeMouse,
		"not opened, the touchpad is %s", mode_name(touchpad.inputMode));

	uint32 cookie = 0;
	status_t status = handler->Open(0, &cookie);
	check(status == B_OK && touchpad.inputMode == kInputModeTouchpad
			&& touchpad.switches == 0x3,
		"opened, it is %s", mode_name(touchpad.inputMode));

	// The system sleeps and comes back; whoever has the touchpad open keeps
	// it open.
	int resetsBefore = touchpad.resets;
#ifdef TEST_NEW
	device->Suspending(losesPower);
#endif
	if (losesPower)
		touchpad.PowerOn();
	sResumesFinished++;

	run_reader(device);

	int resets = touchpad.resets - resetsBefore;
	if (losesPower) {
		check(resets == 1, "after the %s the device is reset (%d time%s)",
			sleep, resets, resets == 1 ? "" : "s");
	} else {
		check(resets == 0, "after the %s the device is not reset (%d time%s)",
			sleep, resets, resets == 1 ? "" : "s");
	}
	check(touchpad.inputMode == kInputModeTouchpad && touchpad.switches == 0x3,
		"after the %s the open touchpad is %s", sleep,
		mode_name(touchpad.inputMode));
	check(!touchpad.resetAnswerPending, "the answer to the reset was read");

	handler->Close(&cookie);
	delete device;
}


/*!	Nobody has the touchpad open: a resume must not switch it. */
static void
test_closed()
{
	printf("--- suspend to RAM, nobody has it open\n");
	FakeTouchpad touchpad = {};
	touchpad.PowerOn();
	sLog[0] = '\0';

	HIDDevice* device = new HIDDevice(kDescriptorRegister, &sI2C, &touchpad,
		NULL);
	ProtocolHandler* handler = device->InitCheck() == B_OK
		? device->ProtocolHandlerAt(0) : NULL;
	if (handler == NULL) {
		check(false, "the device is set up");
		return;
	}

	// opened and closed again, as by a program that only looked
	uint32 cookie = 0;
	handler->Open(0, &cookie);
	handler->Close(&cookie);
	int modeWrites = touchpad.modeWrites;

#ifdef TEST_NEW
	device->Suspending(true);
#endif
	touchpad.PowerOn();
	sResumesFinished++;

	// (the reader waits for somebody to open the device; the wait is the
	// test's, and ends it)
	sDevice = device;
	sRounds = 4;
	uint32 other = 0;
	device->Open(NULL, 0);
		// keeps the reader polling, as another handler's user would
	(void)other;
	sSnoozeHook = stop_reader_after_some_rounds;
	sThreadFunction(sThreadData);
	sSnoozeHook = NULL;

	check(touchpad.modeWrites == modeWrites
			&& touchpad.inputMode == kInputModeMouse,
		"closed, the touchpad stays %s", mode_name(touchpad.inputMode));
	delete device;
}


// #pragma mark - palms


struct TestContact {
	uint8	id;
	bool	confident;
	uint16	x;
	uint16	y;
	bool	lifted;
};

static const uint32 kPixArtSlots = 5;


/*!	One report of the PixArt touchpad with \a count contacts in it; the
	slots after those hold \a stale, as a touchpad may leave them.
*/
static void
send_contacts(HIDDevice* device, const TestContact* contacts, uint32 count,
	const TestContact* stale = NULL)
{
	uint8 data[1 + kPixArtSlots * 6 + 4] = {};
	data[0] = 1;
	for (uint32 i = 0; i < kPixArtSlots; i++) {
		const TestContact* contact = i < count ? &contacts[i] : stale;
		if (contact == NULL)
			continue;
		uint8* slot = data + 1 + i * 6;
		slot[0] = (contact->confident ? 1 : 0) | (contact->lifted ? 0 : 2);
		slot[1] = contact->id;
		slot[2] = contact->x & 0xff;
		slot[3] = contact->x >> 8;
		slot[4] = contact->y & 0xff;
		slot[5] = contact->y >> 8;
	}
	data[1 + kPixArtSlots * 6] = count;
	device->_InputReport(data, sizeof(data));
}


static touchpad_movement
read_movement(ProtocolHandler* handler, uint32* cookie)
{
	touchpad_read read = {};
	read.timeout = 1000;
	touchpad_movement movement = {};
	if (handler->Control(cookie, MS_READ_TOUCHPAD, &read, sizeof(read))
			!= B_OK) {
		check(false, "a movement is read");
		return movement;
	}
	return read.u.touchpad;
}


static uint32
finger_count(const touchpad_movement& movement)
{
	return __builtin_popcount(movement.fingers);
}


/*!	A frame: the contacts go in, the movement the input server gets comes
	out, 10 ms after the last one.
*/
static touchpad_movement
frame(HIDDevice* device, ProtocolHandler* handler, uint32* cookie,
	const TestContact* contacts, uint32 count,
	const TestContact* stale = NULL)
{
	sTime += 10000;
	send_contacts(device, contacts, count, stale);
	return read_movement(handler, cookie);
}


static void
test_palms()
{
	printf("--- palms on the PixArt touchpad\n");

	FakeTouchpad touchpad = {};
	touchpad.descriptor = kPixArtDescriptor;
	touchpad.descriptorLength = sizeof(kPixArtDescriptor);
	touchpad.PowerOn();
	sLog[0] = '\0';
	sThreadFunction = NULL;

	HIDDevice* device = new HIDDevice(kDescriptorRegister, &sI2C, &touchpad,
		NULL);
	ProtocolHandler* handler = device->InitCheck() == B_OK
		? device->ProtocolHandlerAt(0) : NULL;
	check(handler != NULL, "a touchpad handler (%s)\n%s",
		test_strerror(device->InitCheck()), sLog);
	if (handler == NULL) {
		delete device;
		return;
	}
	touchpad_specs specs = {};
	handler->Control(NULL, MS_IS_TOUCHPAD, &specs, sizeof(specs));
	check(specs.areaEndX == 1337 && specs.areaEndY == 876,
		"a surface of 1337 x 876 (%u x %u)", specs.areaEndX, specs.areaEndY);

	uint32 cookie = 0;
	handler->Open(0, &cookie);

	// a finger in the middle
	TestContact finger = { 3, true, 600, 400, false };
	touchpad_movement movement = frame(device, handler, &cookie, &finger, 1);
	check(finger_count(movement) == 1 && movement.xPosition == 600
			&& movement.yPosition == 876 - 400 && movement.fingerWidth == 4,
		"a finger: one at (%u, %u), width %u", movement.xPosition,
		movement.yPosition, movement.fingerWidth);
	finger.lifted = true;
	movement = frame(device, handler, &cookie, &finger, 1);
	check(finger_count(movement) == 0 && movement.xPosition == 0,
		"it lifts: no finger");
	movement = frame(device, handler, &cookie, NULL, 0);

	// a contact the touchpad trusts at first and then takes for a palm
	TestContact palm = { 4, true, 700, 300, false };
	movement = frame(device, handler, &cookie, &palm, 1);
	check(finger_count(movement) == 1, "a palm that touches: a finger first");
	palm.confident = false;
	movement = frame(device, handler, &cookie, &palm, 1);
	check(finger_count(movement) == 0 && movement.fingerWidth == 15,
		"then a palm: no finger, and the width says palm (%u)",
		movement.fingerWidth);
	palm.confident = true;
	palm.x += 30;
	movement = frame(device, handler, &cookie, &palm, 1);
	check(finger_count(movement) == 0 && movement.fingerWidth != 15,
		"trusted again: still a palm, said once (%u fingers, width %u)",
		finger_count(movement), movement.fingerWidth);
	palm.lifted = true;
	frame(device, handler, &cookie, &palm, 1);
	movement = frame(device, handler, &cookie, NULL, 0);
	palm.lifted = false;
	palm.confident = true;
	palm.id = 5;
	movement = frame(device, handler, &cookie, &palm, 1);
	check(finger_count(movement) == 1,
		"lifted, a new contact there is a finger again");
	palm.lifted = true;
	frame(device, handler, &cookie, &palm, 1);

	// a resting palm does not move the finger's point
	TestContact two[2] = {
		{ 1, false, 200, 800, false },
		{ 2, true, 650, 300, false }
	};
	movement = frame(device, handler, &cookie, two, 2);
	check(finger_count(movement) == 1 && movement.xPosition == 650,
		"a palm resting and a finger: the finger alone (%u fingers, x %u)",
		finger_count(movement), movement.xPosition);

	// slots after the contact count are not contacts, whatever is in them
	TestContact ghost = { 9, true, 100, 100, false };
	movement = frame(device, handler, &cookie, &two[1], 1, &ghost);
	check(finger_count(movement) == 1 && movement.xPosition == 650,
		"an old contact in an unused slot is none (%u fingers, x %u)",
		finger_count(movement), movement.xPosition);
	two[1].lifted = true;
	frame(device, handler, &cookie, &two[1], 1);
	movement = frame(device, handler, &cookie, NULL, 0);

	// a palm on a side edge, while typing
	TestContact edge = { 6, true, 40, 500, false };
	movement = frame(device, handler, &cookie, &edge, 1);
	check(finger_count(movement) == 0, "a contact on the left edge: none");
	sTime += 250000;
	edge.x = 400;
	movement = frame(device, handler, &cookie, &edge, 1);
	check(finger_count(movement) == 0,
		"it stayed there too long: a palm wherever it goes then");
	edge.lifted = true;
	frame(device, handler, &cookie, &edge, 1);

	// a finger starting a movement at the right edge
	edge = { 7, true, 1320, 500, false };
	movement = frame(device, handler, &cookie, &edge, 1);
	check(finger_count(movement) == 0, "a contact on the right edge: none");
	edge.x = 1150;
	movement = frame(device, handler, &cookie, &edge, 1);
	check(finger_count(movement) == 1 && movement.xPosition == 1150,
		"it soon leaves the edge: a finger (%u fingers)",
		finger_count(movement));
	edge.lifted = true;
	frame(device, handler, &cookie, &edge, 1);

	// after a close and an open, nothing is left of before
	TestContact resting = { 8, false, 600, 400, false };
	frame(device, handler, &cookie, &resting, 1);
	handler->Close(&cookie);
	cookie = 0;
	handler->Open(0, &cookie);
	resting.confident = true;
	movement = frame(device, handler, &cookie, &resting, 1);
	check(finger_count(movement) == 1,
		"reopened, contacts are new to it");

	handler->Close(&cookie);
	delete device;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	sVerbose = argc > 1 && strcmp(argv[1], "-v") == 0;

	test_sleep(true);
	test_sleep(false);
	test_closed();
	test_palms();

	printf("%d failed\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
