/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef HID_TOUCHPAD_PROTOCOL_HANDLER_H
#define HID_TOUCHPAD_PROTOCOL_HANDLER_H


#include <keyboard_mouse_driver.h>

#include "ProtocolHandler.h"


class HIDCollection;
class HIDReport;
class HIDReportItem;


/*!	A touchpad that reports its contacts the way Windows Precision Touchpads
	do (digitizer touch pad collection with fingers, contact count, buttons):
	switched into reporting them when opened, and turned into the
	touchpad_movement events the input server makes pointer movements,
	scrolling and taps of.
*/
class TouchpadProtocolHandler : public ProtocolHandler {
public:
								TouchpadProtocolHandler(HIDReport& report,
									HIDCollection& collection);

	static	bool				AddHandlers(HIDDevice& device,
									HIDCollection& collection,
									ProtocolHandler*& handlerList);

	virtual	status_t			Open(uint32 flags, uint32* cookie);
	virtual	status_t			Control(uint32* cookie, uint32 op,
									void* buffer, size_t length);

private:
	static	const uint32		kMaxContacts = 10;
	static	const uint32		kMaxButtons = 3;

			struct Contact {
				HIDReportItem*	tip;
				HIDReportItem*	confidence;
				HIDReportItem*	x;
				HIDReportItem*	y;
			};

			struct Point {
				uint32			x;
				uint32			y;
			};

			status_t			_SetFeature(uint16 usageID, uint32 value);
			status_t			_ReadReport(touchpad_movement* movement,
									bigtime_t timeout, uint32* cookie);

			HIDReport&			fReport;
			HIDCollection&		fCollection;

			Contact				fContacts[kMaxContacts];
			uint32				fContactSlots;
			HIDReportItem*		fContactCount;
			HIDReportItem*		fButtons[kMaxButtons];
			uint32				fButtonCount;

			uint32				fMinX;
			uint32				fMaxX;
			uint32				fMinY;
			uint32				fMaxY;
			touchpad_specs		fSpecs;

			// a frame whose contacts come in more than one report
			// ("hybrid" reporting)
			Point				fFrame[kMaxContacts];
			uint32				fFrameContacts;
			uint32				fFrameExpected;
			uint32				fFrameButtons;
};


#endif	// HID_TOUCHPAD_PROTOCOL_HANDLER_H
