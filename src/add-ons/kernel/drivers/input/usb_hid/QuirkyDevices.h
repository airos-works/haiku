/*
 * Copyright 2011 Michael Lotz <mmlr@mlotz.ch>
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */
#ifndef USB_HID_QUIRKY_DEVICES
#define USB_HID_QUIRKY_DEVICES

#include "Driver.h"

class HIDDevice;
class HIDWriter;

typedef status_t (*quirky_init_function)(usb_device device,
	const usb_configuration_info *config, size_t interfaceIndex);
typedef status_t (*quirky_build_descriptor)(HIDWriter &writer);

// Called once the device is set up, and again when it is first opened.
typedef status_t (*quirky_start_function)(HIDDevice &device);

// Turns what the device sent into a report in the layout of the built
// descriptor. Returns B_OK when \a report holds one, else (B_BUSY) the
// transfer carried nothing for the HID parser.
typedef status_t (*quirky_translate_function)(HIDDevice &device,
	const uint8 *data, size_t length, uint8 *report, size_t *reportLength);

// Rumble, the magnitudes 0-65535.
typedef status_t (*quirky_rumble_function)(HIDDevice &device, uint16 strong,
	uint16 weak, uint16 leftTrigger, uint16 rightTrigger);

enum {
	QUIRK_FIRST_INTERFACE_ONLY	= 0x01,
		// only interface number 0 carries the input (GIP pads)
	QUIRK_TRIGGER_RUMBLE		= 0x02
		// the rumble function drives trigger motors too
};

struct usb_hid_quirky_device {
	uint16					vendor_id;
	uint16					product_id;

	uint8					device_class;
	uint8					device_subclass;
	uint8					device_protocol;

	quirky_init_function	init_function;
	quirky_build_descriptor	build_descriptor;

	// optional, all of them
	quirky_start_function	start_function;
	quirky_translate_function translate_function;
	quirky_rumble_function	rumble_function;
	uint32					flags;
};

extern usb_hid_quirky_device gQuirkyDevices[];
extern int32 gQuirkyDeviceCount;

#endif // USB_HID_QUIRKY_DEVICES
