/*
 * Copyright 2011 Michael Lotz <mmlr@mlotz.ch>
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * The GIP packet formats and the per-model initialization packets follow the
 * Linux xpad driver (drivers/input/joystick/xpad.c), the Sony output reports
 * Linux's hid-playstation driver.
 */


#include "GamepadProtocols.h"

#include "HIDWriter.h"

#include <string.h>

#include <usb/USB_hid.h>


static inline uint16
read16(const uint8* data)
{
	return data[0] | (data[1] << 8);
}


static inline void
write16(uint8* data, uint16 value)
{
	data[0] = value & 0xff;
	data[1] = value >> 8;
}


static inline uint8
scale(uint16 magnitude, uint8 maximum)
{
	return (uint8)(((uint32)magnitude * maximum + 32767) / 65535);
}


/*!	The buttons, triggers and sticks the Xbox pads share, in the layout
	described in GamepadProtocols.h. \a triggerBits is 8 for the 360 pad and
	10 for GIP pads; only GIP pads can have a Share button.
*/
static void
build_xbox_controls(HIDWriter& writer, uint8 triggerBits, bool shareButton)
{
	main_item_data_converter converter;
	converter.flat_data = 0; // defaults
	converter.main_data.array_variable = 1;
	converter.main_data.no_preferred = 1;

	// d-pad, start, back, stick presses, shoulders, guide
	writer.DefineInputData(11, 1, converter.main_data, 0, 1,
		B_HID_USAGE_PAGE_BUTTON, 1);
	// the 360 pad leaves this bit unused, Xbox Series pads put Share there
	if (shareButton) {
		writer.DefineInputData(1, 1, converter.main_data, 0, 1,
			B_HID_USAGE_PAGE_BUTTON, 16);
	} else
		writer.DefineInputPadding(1, 1);
	// A, B, X, Y
	writer.DefineInputData(4, 1, converter.main_data, 0, 1,
		B_HID_USAGE_PAGE_BUTTON, 12);

	// triggers
	uint32 triggerMaximum = (1 << triggerBits) - 1;
	writer.DefineInputData(1, triggerBits, converter.main_data, 0,
		triggerMaximum, B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_Z);
	if (triggerBits % 8 != 0)
		writer.DefineInputPadding(1, 8 - triggerBits % 8);
	writer.DefineInputData(1, triggerBits, converter.main_data, 0,
		triggerMaximum, B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_RZ);
	if (triggerBits % 8 != 0)
		writer.DefineInputPadding(1, 8 - triggerBits % 8);

	// sticks
	writer.DefineInputData(2, 16, converter.main_data, (uint32)-32768, 32767,
		B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_X);
	writer.DefineInputData(2, 16, converter.main_data, (uint32)-32768, 32767,
		B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_RX);
}


// #pragma mark - Xbox 360


status_t
xbox360_build_descriptor(HIDWriter& writer)
{
	writer.BeginCollection(COLLECTION_APPLICATION,
		B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_JOYSTICK);

	// message type, length
	writer.DefineInputPadding(2, 8);

	build_xbox_controls(writer, 8, false);

	// unknown / padding
	writer.DefineInputPadding(6, 8);

	return writer.EndCollection();
}


bool
xbox360_translate(const uint8* packet, size_t length, uint8* report)
{
	// Message type 0 is the input report; 1 (LED state), 2 and 3 (rumble
	// state) are short status messages.
	if (length < XBOX360_REPORT_SIZE || packet[0] != 0x00
		|| packet[1] != XBOX360_REPORT_SIZE) {
		return false;
	}

	memcpy(report, packet, XBOX360_REPORT_SIZE);
	write16(report + 8, ~read16(packet + 8));
	write16(report + 12, ~read16(packet + 12));
	return true;
}


size_t
xbox360_build_rumble(uint16 strong, uint16 weak, uint8* packet)
{
	memset(packet, 0, 8);
	packet[1] = 0x08;
	packet[3] = strong >> 8;
		// left, heavy motor
	packet[4] = weak >> 8;
		// right, light motor
	return 8;
}


// #pragma mark - GIP


struct gip_init_packet_entry {
	uint16		vendor;
	uint16		product;
		// 0 matches any
	uint8		length;
	uint8		data[13];
};


// Sent in this order, the sequence number (byte 2) filled in by the sender.
static const gip_init_packet_entry kGIPInitPackets[] = {
	// Hori pads (Titanfall 2 PDP 0e6f:0165, Hori 0f0d:0067) need the identify
	// command acknowledged to finish initializing / make the sticks work.
	{ 0x0e6f, 0x0165, 13, { GIP_CMD_ACK, GIP_OPT_INTERNAL, 0x00, 0x09, 0x00,
		GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0x3a, 0x00, 0x00, 0x00, 0x80,
		0x00 } },
	{ 0x0f0d, 0x0067, 13, { GIP_CMD_ACK, GIP_OPT_INTERNAL, 0x00, 0x09, 0x00,
		GIP_CMD_IDENTIFY, GIP_OPT_INTERNAL, 0x3a, 0x00, 0x00, 0x00, 0x80,
		0x00 } },
	// Power on: every pad with 2015 or later firmware needs it.
	{ 0, 0, 5, { GIP_CMD_POWER, GIP_OPT_INTERNAL, 0x00, 0x01, 0x00 } },
	// Xbox One S and Elite Series 2 pads last used over Bluetooth.
	{ 0x045e, 0x02ea, 5, { GIP_CMD_POWER, GIP_OPT_INTERNAL, 0x00, 0x0f,
		0x06 } },
	{ 0x045e, 0x0b00, 5, { GIP_CMD_POWER, GIP_OPT_INTERNAL, 0x00, 0x0f,
		0x06 } },
	// Elite Series 2: also report the paddles.
	{ 0x045e, 0x0b00, 6, { 0x4d, 0x10, 0x00, 0x02, 0x07, 0x00 } },
	// LED on, as Windows does; most PDP pads only start reporting after it
	// and the authentication-done message.
	{ 0, 0, 7, { GIP_CMD_LED, GIP_OPT_INTERNAL, 0x00, 0x03, 0x00, 0x01,
		0x14 } },
	{ 0, 0, 6, { GIP_CMD_AUTHENTICATE, GIP_OPT_INTERNAL, 0x00, 0x02, 0x01,
		0x00 } },
	// Some PowerA pads only report after a rumble command; stop it right
	// away, before the motors spin up.
	{ 0x24c6, 0x541a, 13, { GIP_CMD_RUMBLE, 0x00, 0x00, 0x09, 0x00, 0x0f,
		0x00, 0x00, 0x1d, 0x1d, 0xff, 0x00, 0x00 } },
	{ 0x24c6, 0x542a, 13, { GIP_CMD_RUMBLE, 0x00, 0x00, 0x09, 0x00, 0x0f,
		0x00, 0x00, 0x1d, 0x1d, 0xff, 0x00, 0x00 } },
	{ 0x24c6, 0x543a, 13, { GIP_CMD_RUMBLE, 0x00, 0x00, 0x09, 0x00, 0x0f,
		0x00, 0x00, 0x1d, 0x1d, 0xff, 0x00, 0x00 } },
	{ 0x24c6, 0x541a, 13, { GIP_CMD_RUMBLE, 0x00, 0x00, 0x09, 0x00, 0x0f,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ 0x24c6, 0x542a, 13, { GIP_CMD_RUMBLE, 0x00, 0x00, 0x09, 0x00, 0x0f,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
	{ 0x24c6, 0x543a, 13, { GIP_CMD_RUMBLE, 0x00, 0x00, 0x09, 0x00, 0x0f,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } },
};


// Where the Share button is: the byte this far from the end of an input
// packet (bit 0).
struct gip_share_button {
	uint16		vendor;
	uint16		product;
	uint8		offset_from_end;
};

static const gip_share_button kGIPShareButtons[] = {
	{ 0x045e, 0x0b12, 18 },	// Xbox Series X|S
};


status_t
gip_build_descriptor(HIDWriter& writer)
{
	writer.BeginCollection(COLLECTION_APPLICATION,
		B_HID_USAGE_PAGE_GENERIC_DESKTOP, B_HID_UID_GD_JOYSTICK);
	build_xbox_controls(writer, 10, true);
	return writer.EndCollection();
}


void
gip_translate(uint16 vendor, uint16 product, const uint8* packet,
	size_t length, uint8* report, gip_translation& result)
{
	result.deliver = false;
	result.announced = false;
	result.ack_length = 0;

	// command, options, sequence number, payload length
	if (length < 4)
		return;

	switch (packet[0]) {
		case GIP_CMD_ANNOUNCE:
			result.announced = true;
			return;

		case GIP_CMD_VIRTUAL_KEY:
		{
			// The Guide button. Pads that ask for it (the One S) keep sending
			// it until it is acknowledged, and report no other press of it.
			if (packet[1] == (GIP_OPT_ACK | GIP_OPT_INTERNAL)) {
				static const uint8 kAck[13] = { GIP_CMD_ACK, GIP_OPT_INTERNAL,
					0x00, 0x09, 0x00, GIP_CMD_VIRTUAL_KEY, GIP_OPT_INTERNAL,
					0x02, 0x00, 0x00, 0x00, 0x00, 0x00 };
				memcpy(result.ack, kAck, sizeof(kAck));
				result.ack[2] = packet[2];
				result.ack_length = sizeof(kAck);
			}
			if (length < 5)
				return;

			uint16 buttons = read16(report);
			if ((packet[4] & 0x03) != 0)
				buttons |= 1 << 10;
			else
				buttons &= ~(1 << 10);
			write16(report, buttons);
			result.deliver = true;
			return;
		}

		case GIP_CMD_INPUT:
		{
			if (length < 18)
				return;

			const uint8 face = packet[4];
			const uint8 pad = packet[5];
			uint16 buttons = read16(report) & (1 << 10);
				// keep the Guide button
			buttons |= pad & 0x0f;						// d-pad
			buttons |= ((face >> 2) & 0x03) << 4;		// menu, view
			buttons |= ((pad >> 6) & 0x03) << 6;		// stick presses
			buttons |= ((pad >> 4) & 0x03) << 8;		// shoulders
			buttons |= ((face >> 4) & 0x0f) << 12;		// A, B, X, Y

			for (size_t i = 0; i < B_COUNT_OF(kGIPShareButtons); i++) {
				const gip_share_button& share = kGIPShareButtons[i];
				if (share.vendor != vendor || share.product != product)
					continue;
				if (length >= 18 + (size_t)share.offset_from_end
					&& (packet[length - share.offset_from_end] & 0x01) != 0) {
					buttons |= 1 << 11;
				}
			}
			write16(report, buttons);

			// triggers, 10 bits
			write16(report + 2, read16(packet + 6) & 0x3ff);
			write16(report + 4, read16(packet + 8) & 0x3ff);

			// sticks, Y flipped to grow downwards
			write16(report + 6, read16(packet + 10));
			write16(report + 8, ~read16(packet + 12));
			write16(report + 10, read16(packet + 14));
			write16(report + 12, ~read16(packet + 16));
			result.deliver = true;
			return;
		}

		default:
			// status (battery), firmware, identification...
			return;
	}
}


size_t
gip_init_packet(uint16 vendor, uint16 product, uint32 index, uint8* packet)
{
	for (size_t i = 0; i < B_COUNT_OF(kGIPInitPackets); i++) {
		const gip_init_packet_entry& entry = kGIPInitPackets[i];
		if ((entry.vendor != 0 && entry.vendor != vendor)
			|| (entry.product != 0 && entry.product != product)) {
			continue;
		}

		if (index-- > 0)
			continue;

		memcpy(packet, entry.data, entry.length);
		return entry.length;
	}

	return 0;
}


size_t
gip_build_rumble(uint8 sequence, uint16 strong, uint16 weak,
	uint16 leftTrigger, uint16 rightTrigger, uint8* packet)
{
	packet[0] = GIP_CMD_RUMBLE;
	packet[1] = 0x00;
	packet[2] = sequence;
	packet[3] = 0x09;
	packet[4] = 0x00;
	packet[5] = 0x0f;
		// all motors: right, left, right trigger, left trigger
	packet[6] = scale(leftTrigger, 100);
	packet[7] = scale(rightTrigger, 100);
	packet[8] = scale(strong, 100);
		// left, heavy motor
	packet[9] = scale(weak, 100);
		// right, light motor
	packet[10] = 0xff;
		// on period (x 10 ms)
	packet[11] = 0x00;
		// off period
	packet[12] = 0xff;
		// repeat count: together "until told otherwise"
	return 13;
}


// #pragma mark - Sony


size_t
dualshock4_build_rumble(uint16 strong, uint16 weak, uint8* report)
{
	memset(report, 0, DUALSHOCK4_OUTPUT_SIZE);
	report[0] = 0x05;
		// report ID
	report[1] = 0x01;
		// valid: motors only, the light bar stays as it is
	report[4] = weak >> 8;
		// right, light motor
	report[5] = strong >> 8;
		// left, heavy motor
	return DUALSHOCK4_OUTPUT_SIZE;
}


size_t
dualsense_build_rumble(uint16 strong, uint16 weak, uint8* report)
{
	memset(report, 0, DUALSENSE_OUTPUT_SIZE);
	report[0] = 0x02;
		// report ID
	report[1] = 0x03;
		// valid: compatible vibration, haptics select (classic rumble)
	report[3] = weak >> 8;
		// right, light motor
	report[4] = strong >> 8;
		// left, heavy motor
	return DUALSENSE_OUTPUT_SIZE;
}
