/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef USB_HID_GAMEPAD_PROTOCOLS_H
#define USB_HID_GAMEPAD_PROTOCOLS_H


/*!	The wire formats of game pads that are not (quite) HID: the Xbox 360
	controller, the Xbox One / Series controllers (GIP, "Gaming Input
	Protocol") and the rumble output reports of the Sony pads.

	Nothing here talks to the USB stack: these functions only translate bytes,
	so a test program can feed them captured packets (see
	src/tests/add-ons/kernel/drivers/input/gamepad_protocols_test).

	Both Xbox pads are presented to the HID parser through a synthesized
	report descriptor in the same layout, so applications see the same
	buttons and axes in the same order on either pad:

		buttons	 1-4  D-pad up, down, left, right
				 5    Start / Menu
				 6    Back / View
				 7, 8 left and right stick press
				 9, 10 left and right shoulder (LB, RB)
				11    Guide (Xbox button)
				12-15 A, B, X, Y
				16    Share (Xbox Series pads only)
		axes	 0, 1 left and right trigger (released = minimum)
				 2, 3 left stick X, Y
				 4, 5 right stick X, Y

	The stick Y axes grow downwards, like those of every HID game pad (the
	pads themselves send them growing upwards).
*/


#include <SupportDefs.h>


class HIDWriter;


// #pragma mark - Xbox 360


#define XBOX360_REPORT_SIZE			20

status_t	xbox360_build_descriptor(HIDWriter& writer);

/*!	Copies an input packet of the pad into \a report (XBOX360_REPORT_SIZE
	bytes), with the stick Y axes flipped. Returns false for the other
	messages the pad sends (LED and rumble state), which carry no input.
*/
bool		xbox360_translate(const uint8* packet, size_t length,
				uint8* report);

/*!	Fills \a packet (8 bytes) with a rumble output message. */
size_t		xbox360_build_rumble(uint16 strong, uint16 weak, uint8* packet);


// #pragma mark - Xbox One / Series (GIP)


#define GIP_REPORT_SIZE				14
#define GIP_MAX_PACKET				64

enum {
	GIP_CMD_ACK				= 0x01,
	GIP_CMD_ANNOUNCE		= 0x02,
	GIP_CMD_STATUS			= 0x03,
	GIP_CMD_IDENTIFY		= 0x04,
	GIP_CMD_POWER			= 0x05,
	GIP_CMD_AUTHENTICATE	= 0x06,
	GIP_CMD_VIRTUAL_KEY		= 0x07,
	GIP_CMD_RUMBLE			= 0x09,
	GIP_CMD_LED				= 0x0a,
	GIP_CMD_FIRMWARE		= 0x0c,
	GIP_CMD_INPUT			= 0x20
};

enum {
	GIP_OPT_ACK				= 0x10,
	GIP_OPT_INTERNAL		= 0x20
};

struct gip_translation {
	bool		deliver;
		// the report changed: hand it to the HID parser
	bool		announced;
		// the pad (re)started and announced itself: initialize it again
	size_t		ack_length;
	uint8		ack[13];
		// to be sent to the pad when ack_length is not 0
};

status_t	gip_build_descriptor(HIDWriter& writer);

/*!	Translates one GIP packet. \a report (GIP_REPORT_SIZE bytes) holds the
	last state and is updated in place: the Guide button and the rest of the
	pad come in separate packets.
*/
void		gip_translate(uint16 vendor, uint16 product, const uint8* packet,
				size_t length, uint8* report, gip_translation& result);

/*!	Returns the \a index th initialization packet for this pad, or 0 when
	there are no more. \a packet must hold GIP_MAX_PACKET bytes; its sequence
	number byte is left at 0 for the caller to fill in.
*/
size_t		gip_init_packet(uint16 vendor, uint16 product, uint32 index,
				uint8* packet);

/*!	Fills \a packet (13 bytes) with a rumble command. The magnitudes are
	0-65535; the pad's own range is 0-100.
*/
size_t		gip_build_rumble(uint8 sequence, uint16 strong, uint16 weak,
				uint16 leftTrigger, uint16 rightTrigger, uint8* packet);


// #pragma mark - Sony


#define DUALSHOCK4_OUTPUT_SIZE		32
#define DUALSENSE_OUTPUT_SIZE		48

size_t		dualshock4_build_rumble(uint16 strong, uint16 weak,
				uint8* report);
size_t		dualsense_build_rumble(uint16 strong, uint16 weak,
				uint8* report);


#endif // USB_HID_GAMEPAD_PROTOCOLS_H
