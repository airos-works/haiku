/*
	Driver for USB Ethernet Control Model devices
	Copyright (C) 2008 Michael Lotz <mmlr@mlotz.ch>
	Distributed under the terms of the MIT license.
*/
#ifndef _USB_ECM_DEVICE_H_
#define _USB_ECM_DEVICE_H_

#include "Driver.h"

class ECMDevice {
public:
							ECMDevice(usb_device device);
							~ECMDevice();

		status_t			InitCheck() { return fStatus; };

		status_t			Open();
		bool				IsOpen() { return fOpen; };

		status_t			Close();
		status_t			Free();

		status_t			Read(uint8 *buffer, size_t *numBytes);
		status_t			Write(const uint8 *buffer, size_t *numBytes);
		status_t			Control(uint32 op, void *buffer, size_t length);

		void				Removed();
		bool				IsRemoved() { return fRemoved; };

		status_t			CompareAndReattach(usb_device device);

private:
static	void				_ReadCallback(void *cookie, int32 status,
								void *data, size_t actualLength);
static	void				_WriteCallback(void *cookie, int32 status,
								void *data, size_t actualLength);

		status_t			_StartRing();
		void				_StopRing();
static	void				_NotifyCallback(void *cookie, int32 status,
								void *data, size_t actualLength);

		status_t			_SetupDevice();
		status_t			_ReadMACAddress(usb_device device, uint8 *buffer);

		// state tracking
		status_t			fStatus;
		bool				fOpen;
		bool				fRemoved;
		int32				fInsideNotify;
		usb_device			fDevice;
		uint16				fVendorID;
		uint16				fProductID;

		// interface and device infos
		uint8				fControlInterfaceIndex;
		uint8				fDataInterfaceIndex;
		uint8				fMACAddressIndex;
		uint16				fMaxSegmentSize;

		// pipes for notifications and data io
		usb_pipe			fNotifyEndpoint;
		usb_pipe			fReadEndpoint;
		usb_pipe			fWriteEndpoint;

		// Each frame is a bulk transfer of its own, so frames are only
		// received at the rate of the bus if several reads stay queued; with
		// one read at a time a gigabit adapter managed 81 Mbit/s. Received
		// frames complete in the order their reads were queued, and are handed
		// out in that order; a frame is sent without waiting for the one
		// before it. As many reads are queued as the host controller takes,
		// up to kRingSlots (the xhci driver takes 15 per endpoint).
		enum {
			kRingSlots = 12,
			kBufferSize = 2048
		};
		struct ring_slot {
			ECMDevice*		device;
			uint8*			buffer;
			size_t			length;
			status_t		status;
		};
		ring_slot			fReadSlots[kRingSlots];
		uint8*				fWriteBuffers[kRingSlots];
		uint32				fRingSize;
		uint32				fReadHead;
		int32				fWriteNext;
		sem_id				fNotifyReadSem;
			// counts completed reads
		sem_id				fNotifyWriteSem;
			// counts free write buffers
		uint16				fWriteMaxPacketSize;
		bool				fRingStarted;

		uint8 *				fNotifyBuffer;
		uint32				fNotifyBufferLength;

		// connection data
		sem_id				fLinkStateChangeSem;
		uint8				fMACAddress[6];
		bool				fHasConnection;
		uint32				fDownstreamSpeed;
		uint32				fUpstreamSpeed;
};

#endif //_USB_ECM_DEVICE_H_
