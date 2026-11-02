/*
 * Copyright 2011 Michael Lotz <mmlr@mlotz.ch>
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */


#include "QuirkyDevices.h"

#include "GamepadProtocols.h"
#include "HIDDevice.h"
#include "HIDWriter.h"

#include <string.h>

#include <usb/USB_hid.h>


static status_t
sixaxis_init(usb_device device, const usb_configuration_info *config,
	size_t interfaceIndex)
{
	TRACE_ALWAYS("found SIXAXIS controller, putting it in operational mode\n");

	// an extra get_report is required for the SIXAXIS to become operational
	uint8 dummy[18];
	status_t result = gUSBModule->send_request(device, USB_REQTYPE_INTERFACE_IN
			| USB_REQTYPE_CLASS, B_USB_REQUEST_HID_GET_REPORT, 0x03f2 /* ? */,
		interfaceIndex, sizeof(dummy), dummy, NULL);
	if (result != B_OK) {
		TRACE_ALWAYS("failed to set operational mode: %s\n", strerror(result));
	}

	return result;
}


static status_t
sixaxis_build_descriptor(HIDWriter &writer)
{
	writer.BeginCollection(COLLECTION_APPLICATION,
		B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_JOYSTICK);

	main_item_data_converter converter;
	converter.flat_data = 0; // defaults
	converter.main_data.array_variable = 1;
	converter.main_data.no_preferred = 1;

	writer.SetReportID(1);

	// unknown / padding
	writer.DefineInputPadding(1, 8);

	// digital button state on/off
	writer.DefineInputData(19, 1, converter.main_data, 0, 1,
		B_HID_USAGE_PAGE_BUTTON, 1);

	// padding to 32 bit boundary
	writer.DefineInputPadding(13, 1);

	// left analog stick
	writer.DefineInputData(1, 8, converter.main_data, 0, 255,
		B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_X);
	writer.DefineInputData(1, 8, converter.main_data, 0, 255,
		B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_Y);

	// right analog stick
	writer.DefineInputData(1, 8, converter.main_data, 0, 255,
		B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_X);
	writer.DefineInputData(1, 8, converter.main_data, 0, 255,
		B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_Y);

	// unknown / padding
	writer.DefineInputPadding(4, 8);

	// pressure sensitive button states
	writer.DefineInputData(12, 8, converter.main_data, 0, 255,
		B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_VNO, B_HID_UID_GD_VNO);

	// unknown / padding / operation mode / battery status / connection ...
	writer.DefineInputPadding(15, 8);

	// accelerometer x, y and z
	writer.DefineInputData(3, 16, converter.main_data, 0, UINT16_MAX,
		B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_VX);

	// gyroscope
	writer.DefineInputData(1, 16, converter.main_data, 0, UINT16_MAX,
		B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_VBRZ);

	return writer.EndCollection();
}


static status_t
elecom_build_descriptor(HIDWriter &writer)
{
	uint8 patchedDescriptor[] = {
		0x05, 0x01, // Usage Page (Generic Desktop Ctrls)
		0x09, 0x02, // Usage (Mouse)
		0xA1, 0x01, // Collection (Application)
		0x09, 0x01, // Usage (Pointer)
		0xA1, 0x00, // Collection (Physical)
		0x85, 0x01, // Report ID (1)
		0x95, 0x06, // Report Count (6) (is 5 in the original descriptor)
		0x75, 0x01, // Report Size (1)
		0x05, 0x09, // Usage Page (Button)
		0x19, 0x01, // Usage Minimum (0x01)
		0x29, 0x06, // Usage Maximum (0x06) (is 5 in the original descriptor)
		0x15, 0x00, // Logical Minimum (0)
		0x25, 0x01, // Logical Maximum (1)
		0x81, 0x02, // Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
		0x95, 0x01, // Report Count (1)
		0x75, 0x02, // Report Size (2) (is 3 in the original descriptor)
		0x81, 0x01, // Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
		0x75, 0x10, // Report Size (16)
		0x95, 0x02, // Report Count (2)
		0x05, 0x01, // Usage Page (Generic Desktop Ctrls)
		0x09, 0x30, // Usage (X)
		0x09, 0x31, // Usage (Y)
		0x16, 0x00, 0x80,  // Logical Minimum (-32768)
		0x26, 0xFF, 0x7F,  // Logical Maximum (32767)
		0x81, 0x06,  // Input (Data,Var,Rel,No Wrap,Linear,Preferred State,No Null Position)
		0xC0,        // End Collection
		0xA1, 0x00,  // Collection (Physical)
		0x95, 0x01,  // Report Count (1)
		0x75, 0x08,  // Report Size (8)
		0x05, 0x01,  // Usage Page (Generic Desktop Ctrls)
		0x09, 0x38,  // Usage (Wheel)
		0x15, 0x81,  // Logical Minimum (-127)
		0x25, 0x7F,  // Logical Maximum (127)
		0x81, 0x06,  // Input (Data,Var,Rel,No Wrap,Linear,Preferred State,No Null Position)
		0xC0,        // End Collection
		0xA1, 0x00,  // Collection (Physical)
		0x95, 0x01,  // Report Count (1)
		0x75, 0x08,  // Report Size (8)
		0x05, 0x0C,  // Usage Page (Consumer)
		0x0A, 0x38, 0x02,  // Usage (AC Pan)
		0x15, 0x81,  // Logical Minimum (-127)
		0x25, 0x7F,  // Logical Maximum (127)
		0x81, 0x06,  // Input (Data,Var,Rel,No Wrap,Linear,Preferred State,No Null Position)
		0xC0,        // End Collection
		0xC0,        // End Collection
		0x06, 0x01, 0xFF,  // Usage Page (Vendor Defined 0xFF01)
		0x09, 0x00,  // Usage (0x00)
		0xA1, 0x01,  // Collection (Application)
		0x85, 0x02,  // Report ID (2)
		0x09, 0x00,  // Usage (0x00)
		0x15, 0x00,  // Logical Minimum (0)
		0x26, 0xFF, 0x00,  // Logical Maximum (255)
		0x75, 0x08,  // Report Size (8)
		0x95, 0x07,  // Report Count (7)
		0x81, 0x02,  // Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
		0xC0,        // End Collection
		0x05, 0x0C,  // Usage Page (Consumer)
		0x09, 0x01,  // Usage (Consumer Control)
		0xA1, 0x01,  // Collection (Application)
		0x85, 0x05,  // Report ID (5)
		0x19, 0x00,  // Usage Minimum (Unassigned)
		0x2A, 0x3C, 0x02,  // Usage Maximum (AC Format)
		0x15, 0x00,  // Logical Minimum (0)
		0x26, 0x3C, 0x02,  // Logical Maximum (572)
		0x95, 0x01,  // Report Count (1)
		0x75, 0x10,  // Report Size (16)
		0x81, 0x00,  // Input (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
		0xC0,        // End Collection
		0x05, 0x01,  // Usage Page (Generic Desktop Ctrls)
		0x09, 0x80,  // Usage (Sys Control)
		0xA1, 0x01,  // Collection (Application)
		0x85, 0x03,  // Report ID (3)
		0x19, 0x81,  // Usage Minimum (Sys Power Down)
		0x29, 0x83,  // Usage Maximum (Sys Wake Up)
		0x15, 0x00,  // Logical Minimum (0)
		0x25, 0x01,  // Logical Maximum (1)
		0x75, 0x01,  // Report Size (1)
		0x95, 0x03,  // Report Count (3)
		0x81, 0x02,  // Input (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position)
		0x95, 0x05,  // Report Count (5)
		0x81, 0x01,  // Input (Const,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
		0xC0,        // End Collection
		0x06, 0xBC, 0xFF,  // Usage Page (Vendor Defined 0xFFBC)
		0x09, 0x88,  // Usage (0x88)
		0xA1, 0x01,  // Collection (Application)
		0x85, 0x04,  // Report ID (4)
		0x95, 0x01,  // Report Count (1)
		0x75, 0x08,  // Report Size (8)
		0x15, 0x00,  // Logical Minimum (0)
		0x26, 0xFF, 0x00,  // Logical Maximum (255)
		0x19, 0x00,  // Usage Minimum (0x00)
		0x2A, 0xFF, 0x00,  // Usage Maximum (0xFF)
		0x81, 0x00,  // Input (Data,Array,Abs,No Wrap,Linear,Preferred State,No Null Position)
		0xC0,        // End Collection
		0x06, 0x02, 0xFF,  // Usage Page (Vendor Defined 0xFF02)
		0x09, 0x02,  // Usage (0x02)
		0xA1, 0x01,  // Collection (Application)
		0x85, 0x06,  // Report ID (6)
		0x09, 0x02,  // Usage (0x02)
		0x15, 0x00,  // Logical Minimum (0)
		0x26, 0xFF, 0x00,  // Logical Maximum (255)
		0x75, 0x08,  // Report Size (8)
		0x95, 0x07,  // Report Count (7)
		0xB1, 0x02,
			// Feature (Data,Var,Abs,No Wrap,Linear,Preferred State,No Null Position, Non-volatile)
		0xC0,        // End Collection
	};

	return writer.Write(&patchedDescriptor, sizeof(patchedDescriptor));
}


// #pragma mark - Xbox 360


static status_t
xbox360_translate_report(HIDDevice &device, const uint8 *data, size_t length,
	uint8 *report, size_t *reportLength)
{
	if (!xbox360_translate(data, length, report))
		return B_BUSY;

	*reportLength = XBOX360_REPORT_SIZE;
	return B_OK;
}


static status_t
xbox360_rumble(HIDDevice &device, uint16 strong, uint16 weak, uint16, uint16)
{
	uint8 packet[8];
	size_t length = xbox360_build_rumble(strong, weak, packet);
	return device.QueueOutput(packet, length);
}


// #pragma mark - Xbox One / Series (GIP)


static status_t
gip_start(HIDDevice &device)
{
	uint8 packet[GIP_MAX_PACKET];
	for (uint32 i = 0;; i++) {
		size_t length = gip_init_packet(device.VendorID(), device.ProductID(),
			i, packet);
		if (length == 0)
			break;

		packet[2] = device.NextOutputSequence();
		status_t result = device.QueueOutput(packet, length);
		if (result != B_OK)
			return result;
	}

	return B_OK;
}


static status_t
gip_translate_report(HIDDevice &device, const uint8 *data, size_t length,
	uint8 *report, size_t *reportLength)
{
	gip_translation result;
	gip_translate(device.VendorID(), device.ProductID(), data, length, report,
		result);

	if (result.ack_length > 0)
		device.QueueOutput(result.ack, result.ack_length);
	if (result.announced) {
		TRACE_ALWAYS("Xbox controller %04x:%04x announced itself, "
			"initializing it\n", device.VendorID(), device.ProductID());
		device.Restart();
	}

	if (!result.deliver)
		return B_BUSY;

	*reportLength = GIP_REPORT_SIZE;
	return B_OK;
}


static status_t
gip_rumble(HIDDevice &device, uint16 strong, uint16 weak, uint16 leftTrigger,
	uint16 rightTrigger)
{
	uint8 packet[13];
	size_t length = gip_build_rumble(device.NextOutputSequence(), strong,
		weak, leftTrigger, rightTrigger, packet);
	return device.QueueOutput(packet, length);
}


// #pragma mark - Sony


static status_t
dualshock4_rumble(HIDDevice &device, uint16 strong, uint16 weak, uint16,
	uint16)
{
	uint8 report[DUALSHOCK4_OUTPUT_SIZE];
	size_t length = dualshock4_build_rumble(strong, weak, report);
	return device.SendOutputReport(report, length);
}


static status_t
dualsense_rumble(HIDDevice &device, uint16 strong, uint16 weak, uint16,
	uint16)
{
	uint8 report[DUALSENSE_OUTPUT_SIZE];
	size_t length = dualsense_build_rumble(strong, weak, report);
	return device.SendOutputReport(report, length);
}


usb_hid_quirky_device gQuirkyDevices[] = {
	{
		// The Sony SIXAXIS controller (PS3) needs a GET_REPORT to become
		// operational which we do in sixaxis_init. Also the normal report
		// descriptor doesn't contain items for the motion sensing data
		// and pressure sensitive buttons, so we constrcut a new report
		// descriptor that includes those extra items.
		0x054c, 0x0268, USB_INTERFACE_CLASS_HID, 0, 0,
		sixaxis_init, sixaxis_build_descriptor,
		NULL, NULL, NULL, 0
	},

	{
		// Elecom trackball which has 6 buttons, and includes them in the report, but not in the
		// report descriptor. Construct a report descriptor including all 6 buttons.
		0x056e, 0x00fd, USB_INTERFACE_CLASS_HID, 0, 0,
		NULL, elecom_build_descriptor,
		NULL, NULL, NULL, 0
	},

	{
		// XBOX 360 controllers aren't really HID (marked vendor specific).
		// They therefore don't provide a HID/report descriptor either. The
		// input stream is HID-like enough though. We therefore claim support
		// and build a report descriptor of our own; the translation drops
		// the status messages and flips the Y axes to grow downwards.
		0, 0, 0xff /* vendor specific */, 0x5d /* XBOX controller */, 0x01,
		NULL, xbox360_build_descriptor,
		NULL, xbox360_translate_report, xbox360_rumble, 0
	},

	{
		// Xbox One and Xbox Series controllers, and the third-party ones
		// built like them (PowerA, PDP, Hori, Razer...), speak GIP on a
		// vendor specific interface: woken up with a power-on packet, they
		// send input packets that we translate into reports of a descriptor
		// of our own. Interface 0 carries the input; the others are audio
		// and security.
		0, 0, 0xff /* vendor specific */, 0x47 /* GIP */, 0xd0,
		NULL, gip_build_descriptor,
		gip_start, gip_translate_report, gip_rumble,
		QUIRK_FIRST_INTERFACE_ONLY | QUIRK_TRIGGER_RUMBLE
	},

	// Sony DualShock 4 and DualSense pads are HID devices; the quirk only
	// adds their rumble output reports.
	{
		0x054c, 0x05c4, USB_INTERFACE_CLASS_HID, 0, 0,	// DualShock 4
		NULL, NULL, NULL, NULL, dualshock4_rumble, 0
	},
	{
		0x054c, 0x09cc, USB_INTERFACE_CLASS_HID, 0, 0,	// DualShock 4 (2nd)
		NULL, NULL, NULL, NULL, dualshock4_rumble, 0
	},
	{
		0x054c, 0x0ba0, USB_INTERFACE_CLASS_HID, 0, 0,	// DS4 USB adapter
		NULL, NULL, NULL, NULL, dualshock4_rumble, 0
	},
	{
		0x054c, 0x0ce6, USB_INTERFACE_CLASS_HID, 0, 0,	// DualSense
		NULL, NULL, NULL, NULL, dualsense_rumble, 0
	},
	{
		0x054c, 0x0df2, USB_INTERFACE_CLASS_HID, 0, 0,	// DualSense Edge
		NULL, NULL, NULL, NULL, dualsense_rumble, 0
	}
};

int32 gQuirkyDeviceCount
	= sizeof(gQuirkyDevices) / sizeof(gQuirkyDevices[0]);
