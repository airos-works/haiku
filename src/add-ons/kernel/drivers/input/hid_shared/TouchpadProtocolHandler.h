/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef HID_TOUCHPAD_PROTOCOL_HANDLER_H
#define HID_TOUCHPAD_PROTOCOL_HANDLER_H


#include <keyboard_mouse_driver.h>
#include <lock.h>

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
	virtual						~TouchpadProtocolHandler();

	static	bool				AddHandlers(HIDDevice& device,
									HIDCollection& collection,
									ProtocolHandler*& handlerList);

	virtual	status_t			Open(uint32 flags, uint32* cookie);
	virtual	status_t			Close(uint32* cookie);
	virtual	void				DeviceWasReset();
	virtual	status_t			Control(uint32* cookie, uint32 op,
									void* buffer, size_t length);

private:
	static	const uint32		kMaxContacts = 10;
	static	const uint32		kMaxButtons = 3;

			struct Contact {
				HIDReportItem*	tip;
				HIDReportItem*	confidence;
				HIDReportItem*	id;
				HIDReportItem*	x;
				HIDReportItem*	y;
			};

			// a contact touching in a frame
			struct FrameContact {
				uint32			id;
				uint32			x;
				uint32			y;
				bool			confident;
			};

			// what a contact on the surface is taken for, from the time it
			// touches until it lifts
			enum {
				kFinger,
				kPalm,
				kEdge
					// began at a side edge: a palm unless it leaves the
					// edge soon
			};

			struct TrackedContact {
				uint32			id;
				uint8			kind;
				bool			seen;
				bigtime_t		began;
			};

			status_t			_Configure();
			status_t			_SetFeature(uint16 usageID, uint32 value);
			status_t			_ReadReport(touchpad_movement* movement,
									bigtime_t timeout, uint32* cookie);
			void				_TrackFrame(touchpad_movement* movement);
			bool				_InSideEdge(uint32 x) const;

			HIDReport&			fReport;
			HIDCollection&		fCollection;
			int32				fOpenCount;
			mutex				fConfigureLock;
				// an open and the transport's thread may both configure

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
			FrameContact		fFrame[kMaxContacts];
			uint32				fFrameContacts;
			uint32				fFrameExpected;
			uint32				fFrameButtons;

			// the contacts on the surface, from frame to frame
			TrackedContact		fTracked[kMaxContacts];
			uint32				fTrackedCount;
			uint32				fEdgeWidth;
};


#endif	// HID_TOUCHPAD_PROTOCOL_HANDLER_H
