/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _HID_HOST_H_
#define _HID_HOST_H_


#include <Locker.h>
#include <Message.h>
#include <OS.h>
#include <String.h>

#include <bluetooth/bluetooth.h>

#include <map>


/*!	The host side of the Bluetooth HID Profile 1.1, for Classic (BR/EDR)
	keyboards and mice.

	A HID device has two L2CAP channels: control (PSM 0x11) and interrupt
	(PSM 0x13), which carries its input reports. When the user connects a new
	device, its SDP record gives the report descriptor (kept in
	~/config/settings/bluetooth/hid_devices) and we open both channels. A
	bonded device that wakes up pages us and opens them itself, so we listen
	on both PSMs, and keep page scan on while there are bonded devices.

	Input reports go to the bluetooth_hid driver, whose keyboard and mouse
	devices the input server reads like USB ones; output reports (the
	keyboard LEDs) come back from it.

	Thread safety: the public methods are called from the HCI event thread,
	the application thread and our own threads, under fLock.
*/
class HIDHost {
public:
								HIDHost();
								~HIDHost();

			status_t			Start();
			void				Stop();

			bool				IsHIDDevice(const bdaddr_t& address);
			bool				HasDevices();

			// The SDP records of a device that was just paired; false if it
			// has no HID service we can use.
			bool				AddDevice(const bdaddr_t& address,
									const BMessage& services,
									const char* name);
			// A keyboard whose SDP records could not be read (its Class of
			// Device says keyboard): driven in the boot protocol.
			bool				AddBootKeyboard(const bdaddr_t& address,
									const char* name);
			void				RemoveDevice(const bdaddr_t& address);

			// Opens the channels to a device the user connected.
			void				Connect(const bdaddr_t& address);

			void				LinkEncrypted(const bdaddr_t& address,
									bool encrypted, bool deviceInitiated);
			void				LinkDown(const bdaddr_t& address);

private:
			class Session;
			friend class Session;

			struct Pending {
				int			control;
				int			interrupt;
			};
			struct ListenJob;
			struct ConnectJob;

	static	uint64				_Key(const bdaddr_t& address);

	static	status_t			_ListenThread(void* data);
			void				_Listen(uint16 psm, int index);
	static	status_t			_ConnectThread(void* data);
	static	status_t			_ReconnectCheckThread(void* data);
			void				_ConnectChannels(const bdaddr_t& address);
			int					_OpenChannel(const bdaddr_t& address,
									uint16 psm);

			void				_ChannelOpened(uint16 psm, int socket,
									const bdaddr_t& address);
			void				_StartSession(const bdaddr_t& address,
									int control, int interrupt);
			void				_SessionEnded(Session* session);

			bool				_IsEncrypted(const bdaddr_t& address);
			bool				_DeviceInfo(const bdaddr_t& address,
									BMessage& device);
			int32				_FindStored(const bdaddr_t& address,
									BMessage* _device);
			bool				_RemoveStored(const bdaddr_t& address);

			status_t			_Load();
			status_t			_Save();

private:
			BLocker				fLock;
			BMessage			fDevices;
				// "device" messages: "address", "name", "descriptor",
				// "vendor", "product", "reconnect"
			std::map<uint64, Pending>	fPending;
			std::map<uint64, Session*>	fSessions;
			std::map<uint64, bool>		fEncrypted;

			int					fListenSockets[2];
			thread_id			fListenThreads[2];
			bool				fQuitting;
};


#endif	// _HID_HOST_H_
