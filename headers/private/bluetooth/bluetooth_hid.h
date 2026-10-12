/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BLUETOOTH_HID_H
#define _BLUETOOTH_HID_H


#include <Drivers.h>


/*!	Bluetooth HID devices (keyboards, mice) as kernel input devices.

	The Bluetooth server owns the L2CAP channels of a Classic HID device
	(Bluetooth HID Profile 1.1). For each connected device it opens the
	control device, describes the device with BLUETOOTH_HID_CREATE, and then
	writes every input report it receives on the interrupt channel: the bytes
	after the DATA|Input header, report ID first when the device uses report
	IDs. The shared HID protocol handlers publish the usual
	input/keyboard/bluetooth/N and input/mouse/bluetooth/N devices from the
	report descriptor, so the input server's keyboard and mouse add-ons drive
	them like USB ones.

	Reading the control device blocks until an output report (the keyboard
	LEDs) is to be sent: one bluetooth_hid_output header, then the report,
	report ID first when the device uses them. Closing the control device
	removes the HID device.
*/

#define BLUETOOTH_HID_CONTROL_NAME		"input/bluetooth_hid/control"
#define BLUETOOTH_HID_CONTROL_PATH		"/dev/" BLUETOOTH_HID_CONTROL_NAME

#define BLUETOOTH_HID_MAX_DESCRIPTOR	4096
#define BLUETOOTH_HID_MAX_REPORT		512

#define BLUETOOTH_HID_VERSION			1

enum {
	BLUETOOTH_HID_CREATE = B_DEVICE_OP_CODES_END + 0x4248
		// bluetooth_hid_create*
};

typedef struct bluetooth_hid_create {
	uint32		version;
		// BLUETOOTH_HID_VERSION
	uint8		address[6];
	uint16		vendor_id;
	uint16		product_id;
	uint32		descriptor_length;
	const void*	descriptor;
		// the HID report descriptor from the device's SDP record
	char		name[64];
} bluetooth_hid_create;

enum {
	BLUETOOTH_HID_REPORT_OUTPUT = 2,
	BLUETOOTH_HID_REPORT_FEATURE = 3
};

typedef struct bluetooth_hid_output {
	uint8		type;
		// BLUETOOTH_HID_REPORT_OUTPUT or _FEATURE, as in the HIDP header
	uint8		reserved;
	uint16		length;
		// of the report that follows
} bluetooth_hid_output;


#endif	// _BLUETOOTH_HID_H
