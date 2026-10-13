/*
 * Copyright 2020, Jérôme Duval, jerome.duval@gmail.com.
 * Copyright 2008-2011, Michael Lotz <mmlr@mlotz.ch>
 * Distributed under the terms of the MIT license.
 */
#ifndef I2C_HID_DEVICE_H
#define I2C_HID_DEVICE_H


#include <condition_variable.h>
#include <i2c.h>
#include <lock.h>

#include "HIDParser.h"
#include "I2CHIDProtocol.h"


class GpioInterrupt;
class ProtocolHandler;


class HIDDevice {
public:
								HIDDevice(uint16 descriptorAddress,
									i2c_device_interface* i2c,
									i2c_device i2cCookie,
									GpioInterrupt* interrupt);
								~HIDDevice();

			status_t			InitCheck() const { return fStatus; }

			bool				IsOpen() const { return fOpenCount > 0; }
			status_t			Open(ProtocolHandler *handler, uint32 flags);
			status_t			Close(ProtocolHandler *handler);
			int32				OpenCount() const { return fOpenCount; }

			void				Removed();
			bool				IsRemoved() const { return fRemoved; }

			status_t			MaybeScheduleTransfer(HIDReport *report);

			status_t			SendReport(HIDReport *report);

			HIDParser &			Parser() { return fParser; }
			ProtocolHandler *	ProtocolHandlerAt(uint32 index) const;

private:
			struct ReportQueue;

	static	status_t			_ReaderThread(void* data);
			void				_Reader();
			status_t			_ReadInput(size_t& length);
			void				_InputReport(const uint8* data,
									size_t length);
			ReportQueue*		_QueueFor(HIDReport* report);
			void				_Deliver(ReportQueue* queue);

			status_t			_Reset();
			status_t			_SetPower(uint8 power);
			status_t			_WriteReport(uint8 type, uint8 id, void *data,
									size_t reportSize);
			status_t			_FetchBuffer(uint8* cmd, size_t cmdLength,
									void* buffer, size_t bufferLength);
			status_t			_FetchReport(HIDReport* report);
			status_t			_ExecCommand(i2c_op op, uint8* cmd,
									size_t cmdLength, void* buffer,
									size_t bufferLength);

private:
			status_t			fStatus;

			size_t				fTransferBufferSize;
			uint8 *				fTransferBuffer;
			uint8 *				fInputBuffer;

			int32				fOpenCount;
			bool				fRemoved;

			HIDParser			fParser;

			uint32				fProtocolHandlerCount;
			ProtocolHandler *	fProtocolHandlerList;

			uint16				fDescriptorAddress;
			i2c_hid_descriptor	fDescriptor;

			uint8*				fReportDescriptor;

			i2c_device_interface*	fI2C;
			i2c_device				fI2CCookie;

			GpioInterrupt*		fInterrupt;
			bool				fPollPinState;
			mutex				fLock;
			ConditionVariable	fOpenCondition;
			ReportQueue*		fQueues;
			uint32				fQueueCount;
			size_t				fMaxReportSize;
			thread_id			fReaderThread;
	volatile bool				fStopReader;
};


#endif // I2C_HID_DEVICE_H
