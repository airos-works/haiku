/*
 * Copyright 2008-2011, Michael Lotz <mmlr@mlotz.ch>
 * Distributed under the terms of the MIT license.
 */
#ifndef USB_HID_DEVICE_H
#define USB_HID_DEVICE_H


#include "HIDParser.h"

#include <USB3.h>


class ProtocolHandler;


class HIDDevice {
public:
								HIDDevice(usb_device device,
									const usb_configuration_info *config,
									size_t interfaceIndex, int32 quirkyIndex);
								~HIDDevice();

			void				SetParentCookie(int32 cookie);
			int32				ParentCookie() const { return fParentCookie; }

			status_t			InitCheck() const { return fStatus; }

			bool				IsOpen() const { return fOpenCount > 0; }
			status_t			Open(ProtocolHandler *handler, uint32 flags);
			status_t			Close(ProtocolHandler *handler);
			void				ReleaseOpenReference();
			bool				HasOpenReferences() const
									{ return fOpenReferences > 0; }
			int32				OpenCount() const { return fOpenCount; }

			void				Removed();
			bool				IsRemoved() const { return fRemoved; }

			status_t			MaybeScheduleTransfer(HIDReport*);

			status_t			SendReport(HIDReport *report);

	// For quirky devices: who they are and a way to talk to them besides
	// HID reports.
			uint16				VendorID() const { return fVendorID; }
			uint16				ProductID() const { return fProductID; }
			usb_device			USBDevice() const { return fDevice; }
			size_t				InterfaceIndex() const
									{ return fInterfaceIndex; }
			uint8				NextOutputSequence()
									{ return (uint8)atomic_add(
										&fOutputSequence, 1); }

			status_t			QueueOutput(const void *data, size_t length);
									// on the interrupt out endpoint, without
									// waiting: may be called from transfer
									// callbacks
			status_t			SendOutputReport(const void *data,
									size_t length);
									// an output report, report ID first: on
									// the interrupt out endpoint when there
									// is one, else with SET_REPORT
			void				Restart();
									// run the quirk's start function again
									// (at most every two seconds)

	// Rumble (force feedback), for the joystick protocol handler
			uint32				RumbleFlags() const;
									// js_flag_force_feedback,
									// js_flag_trigger_rumble
			status_t			SetRumble(uint16 strong, uint16 weak,
									uint16 leftTrigger, uint16 rightTrigger);

			HIDParser &			Parser() { return fParser; }
			ProtocolHandler *	ProtocolHandlerAt(uint32 index) const;

			// only to be used for the kernel debugger information
			usb_pipe			InterruptPipe() const { return fInterruptPipe; }

private:
	static	void				_TransferCallback(void *cookie,
									status_t status, void *data,
									size_t actualLength);
	static	void				_UnstallCallback(void *cookie,
									status_t status, void *data,
									size_t actualLength);
	static	void				_OutputCallback(void *cookie,
									status_t status, void *data,
									size_t actualLength);
			void				_Start();

private:
			status_t			fStatus;
			usb_device			fDevice;
			usb_pipe			fInterruptPipe;
			usb_pipe			fOutputPipe;
			size_t				fInterfaceIndex;
			int32				fQuirkyIndex;
			uint16				fVendorID;
			uint16				fProductID;
			uint8				fEndpointAddress;

			int32				fTransferScheduled;
			size_t				fTransferBufferSize;
			uint8 *				fTransferBuffer;
			uint8 *				fReportBuffer;
				// what a quirk's translate function makes of the transfer
			int32				fOutputSequence;
			bigtime_t			fLastStart;

			int32				fParentCookie;
			int32				fOpenCount;
			int32				fOpenReferences;
			bool				fRemoved;

			HIDParser			fParser;

			uint32				fProtocolHandlerCount;
			ProtocolHandler *	fProtocolHandlerList;
};


#endif // USB_HID_DEVICE_H
