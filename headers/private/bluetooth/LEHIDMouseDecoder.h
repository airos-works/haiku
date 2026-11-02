/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LE_HID_MOUSE_DECODER_H_
#define _LE_HID_MOUSE_DECODER_H_

#include <SupportDefs.h>
#include <stddef.h>

class HIDParser;

namespace Bluetooth {

struct LEMouseReport {
	int32 x;
	int32 y;
	int32 wheelX;
	int32 wheelY;
	uint32 buttons;
};


// Decodes the raw value of a HID-over-GATT Input Report characteristic.
// The Report Reference descriptor supplies reportID; the characteristic value
// itself does not carry that byte. Report map items are parsed with Haiku's
// existing HID parser, including signed relative axes and button usages.
class LEHIDMouseDecoder {
public:
	LEHIDMouseDecoder();
	~LEHIDMouseDecoder();

	status_t Init(const uint8* reportMap, size_t length);
	bool SupportsReport(uint8 reportID) const;
	status_t Decode(uint8 reportID, const uint8* value, size_t length,
		LEMouseReport& output);

private:
	HIDParser* fParser;
};


// Whether a report map has a joystick or game pad (a Generic Desktop Joystick,
// Game Pad or Multi-axis Controller application collection). Such devices go
// to the bluetooth_hid driver, which publishes them under /dev/joystick.
bool LEHIDReportMapHasGamepad(const uint8* reportMap, size_t length);

} // namespace Bluetooth

#endif
