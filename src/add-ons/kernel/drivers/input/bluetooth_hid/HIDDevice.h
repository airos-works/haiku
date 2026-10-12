/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BLUETOOTH_HID_DEVICE_H_
#define _BLUETOOTH_HID_DEVICE_H_


#include <condition_variable.h>
#include <lock.h>

#include "HIDParser.h"


class HIDReport;
class ProtocolHandler;


/*!	A HID device whose reports the Bluetooth server hands in.

	USB and I2C devices keep a report until the host asks for it, and the
	protocol handlers only ask while they wait for one. A Bluetooth device
	sends each report once, whenever it likes: two arriving together (after
	the radio was busy for a moment) must not lose the second, or a released
	key keeps repeating. So every input report is queued by its report ID and
	handed over only once a handler waits for that report.
*/
class HIDDevice {
public:
								HIDDevice(const uint8* descriptor,
									size_t descriptorLength,
									const char* name);
								~HIDDevice();

			status_t			InitCheck() const { return fStatus; }
			const char*			Name() const { return fName; }

			bool				IsOpen() const { return fOpenCount > 0; }
			status_t			Open(ProtocolHandler* handler, uint32 flags);
			status_t			Close(ProtocolHandler* handler);
			int32				OpenCount() const { return fOpenCount; }

			void				Removed();
			bool				IsRemoved() const { return fRemoved; }

	// The driver's open handles, counted under its lock: the last one closed
	// after the device went away deletes it.
			void				AcquireCookie() { fCookies++; }
			bool				ReleaseCookie() { return --fCookies == 0; }
			int32				CountCookies() const { return fCookies; }

			status_t			MaybeScheduleTransfer(HIDReport* report);
			status_t			SendReport(HIDReport* report);

			HIDParser&			Parser() { return fParser; }
			ProtocolHandler*	ProtocolHandlerAt(uint32 index) const;

	// From the Bluetooth server
			status_t			InputReport(const uint8* data, size_t length);
			status_t			ReadOutputReport(uint8* buffer,
									size_t* _length);

private:
			struct ReportQueue;

			ReportQueue*		_QueueFor(HIDReport* report);
			void				_Deliver(ReportQueue* queue);

private:
			status_t			fStatus;
			char				fName[64];

			int32				fOpenCount;
			bool				fRemoved;
			int32				fCookies;

			HIDParser			fParser;
			uint32				fProtocolHandlerCount;
			ProtocolHandler*	fProtocolHandlerList;

			mutex				fLock;
			ReportQueue*		fQueues;
			uint32				fQueueCount;
			size_t				fMaxReportSize;
				// including the report ID

			uint8*				fOutput;
				// ring of kOutputSlots reports, each prefixed by its
				// bluetooth_hid_output header
			uint32				fOutputHead;
			uint32				fOutputCount;
			ConditionVariable	fOutputCondition;
			ConditionVariable	fQueueSpace;

			uint32				fDropped;
};


#endif	// _BLUETOOTH_HID_DEVICE_H_
