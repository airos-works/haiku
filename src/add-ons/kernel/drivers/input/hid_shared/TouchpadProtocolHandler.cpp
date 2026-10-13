/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


//!	Precision touchpads: contacts in, touchpad_movement out.


#include "Driver.h"
#include "TouchpadProtocolHandler.h"

#include "HIDCollection.h"
#include "HIDDevice.h"
#include "HIDReport.h"
#include "HIDReportItem.h"

#include <new>
#include <string.h>
#include <usb/USB_hid.h>

#include <kernel.h>


// Device configuration values (Windows Precision Touchpad).
static const uint32 kInputModeTouchpad = 3;

// The input server treats contacts below the pressure range as no contact;
// precision touchpads tell contact or not, so a contact gets this one.
static const uint8 kContactPressure = 50;
static const uint8 kContactWidth = 4;


TouchpadProtocolHandler::TouchpadProtocolHandler(HIDReport& report,
	HIDCollection& collection)
	:
	ProtocolHandler(report.Device(), "input/touchpad/" DEVICE_PATH_SUFFIX "/",
		0),
	fReport(report),
	fCollection(collection),
	fContactSlots(0),
	fContactCount(NULL),
	fButtonCount(0),
	fMinX(0),
	fMaxX(0),
	fMinY(0),
	fMaxY(0),
	fFrameContacts(0),
	fFrameExpected(0),
	fFrameButtons(0)
{
	memset(fContacts, 0, sizeof(fContacts));
	memset(fButtons, 0, sizeof(fButtons));

	for (uint32 i = 0; i < collection.CountChildren()
			&& fContactSlots < kMaxContacts; i++) {
		HIDCollection* child = collection.ChildAt(i);
		if (child == NULL || child->UsagePage() != B_HID_USAGE_PAGE_DIGITIZER
			|| child->UsageID() != B_HID_UID_DIG_FINGER) {
			continue;
		}

		Contact contact = {};
		for (uint32 j = 0; j < child->CountItems(); j++) {
			HIDReportItem* item = child->ItemAt(j);
			if (item == NULL || !item->HasData() || item->Report() != &report)
				continue;

			uint16 page = item->UsagePage();
			uint16 id = item->UsageID();
			if (page == B_HID_USAGE_PAGE_DIGITIZER
				&& id == B_HID_UID_DIG_TIP_SWITCH) {
				contact.tip = item;
			} else if (page == B_HID_USAGE_PAGE_DIGITIZER
				&& id == B_HID_UID_DIG_TOUCH_VALID) {
				contact.confidence = item;
			} else if (page == B_HID_USAGE_PAGE_GENERIC_DESKTOP
				&& id == B_HID_UID_GD_X) {
				contact.x = item;
			} else if (page == B_HID_USAGE_PAGE_GENERIC_DESKTOP
				&& id == B_HID_UID_GD_Y) {
				contact.y = item;
			}
		}

		if (contact.tip == NULL || contact.x == NULL || contact.y == NULL)
			continue;

		if (fContactSlots == 0) {
			fMinX = contact.x->Minimum();
			fMaxX = contact.x->Maximum();
			fMinY = contact.y->Minimum();
			fMaxY = contact.y->Maximum();
		}
		fContacts[fContactSlots++] = contact;
	}

	for (uint32 i = 0; i < collection.CountItems(); i++) {
		HIDReportItem* item = collection.ItemAt(i);
		if (item == NULL || !item->HasData() || item->Report() != &report)
			continue;

		if (item->UsagePage() == B_HID_USAGE_PAGE_DIGITIZER
			&& item->UsageID() == B_HID_UID_DIG_CONTACT_COUNT) {
			fContactCount = item;
		} else if (item->UsagePage() == B_HID_USAGE_PAGE_BUTTON
			&& item->UsageID() >= 1 && item->UsageID() <= kMaxButtons
			&& fButtonCount < kMaxButtons) {
			fButtons[fButtonCount++] = item;
		}
	}

	memset(&fSpecs, 0, sizeof(fSpecs));
	fSpecs.areaStartX = fMinX;
	fSpecs.areaEndX = fMaxX;
	fSpecs.areaStartY = fMinY;
	fSpecs.areaEndY = fMaxY;
	fSpecs.width = fMaxX - fMinX;
	fSpecs.edgeMotionWidth = (fMaxX - fMinX) / 50;
	fSpecs.minPressure = 0;
	fSpecs.realMaxPressure = kContactPressure;
	fSpecs.maxPressure = 255;

	TRACE_ALWAYS("touchpad with %" B_PRIu32 " contacts, %" B_PRIu32
		" button%s, %" B_PRIu32 "x%" B_PRIu32 "\n", fContactSlots, fButtonCount,
		fButtonCount == 1 ? "" : "s", fMaxX - fMinX, fMaxY - fMinY);
}


/*!	Returns whether it added a handler for the collection, which then needs
	no other (as an absolute tablet, say).
*/
bool
TouchpadProtocolHandler::AddHandlers(HIDDevice& device,
	HIDCollection& collection, ProtocolHandler*& handlerList)
{
	if (collection.UsagePage() != B_HID_USAGE_PAGE_DIGITIZER
		|| collection.UsageID() != B_HID_UID_DIG_TOUCH_PAD) {
		return false;
	}

	HIDParser& parser = device.Parser();
	uint32 maxReportCount = parser.CountReports(HID_REPORT_TYPE_INPUT);
	if (maxReportCount == 0)
		return false;

	uint32 inputReportCount = 0;
	HIDReport* inputReports[maxReportCount];
	collection.BuildReportList(HID_REPORT_TYPE_INPUT, inputReports,
		inputReportCount);

	for (uint32 i = 0; i < inputReportCount; i++) {
		// the report with contacts in it
		HIDReport* report = inputReports[i];
		if (report->FindItem(B_HID_USAGE_PAGE_DIGITIZER,
				B_HID_UID_DIG_TIP_SWITCH) == NULL
			|| report->FindItem(B_HID_USAGE_PAGE_GENERIC_DESKTOP,
				B_HID_UID_GD_X) == NULL) {
			continue;
		}

		TouchpadProtocolHandler* handler
			= new(std::nothrow) TouchpadProtocolHandler(*report, collection);
		if (handler == NULL)
			return false;
		if (handler->fContactSlots == 0 || handler->fMaxX <= handler->fMinX
			|| handler->fMaxY <= handler->fMinY) {
			delete handler;
			continue;
		}

		handler->SetNextHandler(handlerList);
		handlerList = handler;
		return true;
	}

	return false;
}


status_t
TouchpadProtocolHandler::Open(uint32 flags, uint32* cookie)
{
	status_t status = ProtocolHandler::Open(flags, cookie);
	if (status != B_OK)
		return status;

	// Report contacts rather than act as a mouse, with the surface and the
	// buttons both reporting.
	status = _SetFeature(B_HID_UID_DIG_DEVICE_MODE, kInputModeTouchpad);
	if (status != B_OK) {
		TRACE_ALWAYS("touchpad: cannot switch to reporting contacts: %s\n",
			strerror(status));
	}
	_SetFeature(B_HID_UID_DIG_SURFACE_SWITCH, 1);

	fFrameExpected = 0;
	return B_OK;
}


status_t
TouchpadProtocolHandler::Control(uint32* cookie, uint32 op, void* buffer,
	size_t length)
{
	switch (op) {
		case B_GET_DEVICE_NAME:
		{
			const char name[] = DEVICE_NAME " Touchpad";
			return IOGetDeviceName(name, buffer, length);
		}

		case MS_IS_TOUCHPAD:
			if (buffer == NULL)
				return B_OK;
			if (length < sizeof(fSpecs) || !IS_USER_ADDRESS(buffer))
				return B_BAD_VALUE;
			return user_memcpy(buffer, &fSpecs, sizeof(fSpecs));

		case MS_READ_TOUCHPAD:
		{
			touchpad_read read;
			if (length < sizeof(touchpad_read))
				return B_BUFFER_OVERFLOW;
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&read.timeout,
					&((touchpad_read*)buffer)->timeout, sizeof(bigtime_t))
						!= B_OK) {
				return B_BAD_ADDRESS;
			}

			read.event = MS_READ_TOUCHPAD;
			while (true) {
				status_t result = _ReadReport(&read.u.touchpad, read.timeout,
					cookie);
				if (result == B_BUSY)
					continue;
				if (result != B_OK)
					return result;

				if (user_memcpy(buffer, &read, sizeof(read)) != B_OK)
					return B_BAD_ADDRESS;
				return B_OK;
			}
		}

		case MS_NUM_EVENTS:
			if (fReport.Device()->IsRemoved())
				return B_DEV_NOT_READY;
			return 0;

		case MS_SET_CLICKSPEED:
			return B_OK;
	}

	return B_ERROR;
}


/*!	Sets a device configuration feature (input mode, surface and button
	switch) wherever the device keeps it; the switches share their report.
*/
status_t
TouchpadProtocolHandler::_SetFeature(uint16 usageID, uint32 value)
{
	HIDParser& parser = fReport.Device()->Parser();
	for (uint32 i = 0; i < parser.CountReports(HID_REPORT_TYPE_FEATURE); i++) {
		HIDReport* report = parser.ReportAt(HID_REPORT_TYPE_FEATURE, i);
		HIDReportItem* item = report->FindItem(B_HID_USAGE_PAGE_DIGITIZER,
			usageID);
		if (item == NULL)
			continue;

		item->SetData(value);
		if (usageID == B_HID_UID_DIG_SURFACE_SWITCH) {
			HIDReportItem* buttons = report->FindItem(
				B_HID_USAGE_PAGE_DIGITIZER, B_HID_UID_DIG_BUTTON_SWITCH);
			if (buttons != NULL)
				buttons->SetData(value);
		}
		return report->SendReport();
	}

	return B_ENTRY_NOT_FOUND;
}


status_t
TouchpadProtocolHandler::_ReadReport(touchpad_movement* movement,
	bigtime_t timeout, uint32* cookie)
{
	status_t result = fReport.WaitForReport(timeout);
	if (result != B_OK) {
		if (fReport.Device()->IsRemoved())
			return B_DEV_NOT_READY;
		if (result == B_INTERRUPTED || result == B_TIMED_OUT)
			return result;
		if ((*cookie & PROTOCOL_HANDLER_COOKIE_FLAG_CLOSED) != 0)
			return B_FILE_ERROR;

		// "busy" happens when other reports come in on the same input
		return B_BUSY;
	}

	Point points[kMaxContacts];
	uint32 touching = 0;
	for (uint32 i = 0; i < fContactSlots; i++) {
		Contact& contact = fContacts[i];
		if (contact.tip->Extract() != B_OK || !contact.tip->Valid()
			|| contact.tip->Data() == 0) {
			continue;
		}
		// a palm, or whatever the touchpad does not trust to be a finger
		if (contact.confidence != NULL && contact.confidence->Extract() == B_OK
			&& contact.confidence->Valid() && contact.confidence->Data() == 0) {
			continue;
		}
		if (contact.x->Extract() != B_OK || contact.y->Extract() != B_OK)
			continue;

		points[touching].x = contact.x->Data();
		points[touching].y = contact.y->Data();
		touching++;
	}

	uint32 reported = 0;
	if (fContactCount != NULL && fContactCount->Extract() == B_OK
		&& fContactCount->Valid()) {
		reported = fContactCount->Data();
	}

	uint32 buttons = 0;
	for (uint32 i = 0; i < fButtonCount; i++) {
		HIDReportItem* button = fButtons[i];
		if (button->Extract() == B_OK && button->Valid())
			buttons |= (button->Data() & 1) << (button->UsageID() - 1);
	}

	fReport.DoneProcessing();

	// A touchpad with fewer contact slots than fingers on it sends one
	// frame in several reports: the first one has the contact count, the
	// others none.
	if (fFrameExpected > 0 && reported == 0) {
		for (uint32 i = 0; i < touching && fFrameContacts < kMaxContacts; i++)
			fFrame[fFrameContacts++] = points[i];
		fFrameButtons |= buttons;
		if (fFrameExpected > fContactSlots) {
			fFrameExpected -= fContactSlots;
			return B_BUSY;
		}
		fFrameExpected = 0;
	} else {
		fFrameContacts = 0;
		for (uint32 i = 0; i < touching; i++)
			fFrame[fFrameContacts++] = points[i];
		fFrameButtons = buttons;
		if (reported > fContactSlots) {
			fFrameExpected = reported - fContactSlots;
			return B_BUSY;
		}
		fFrameExpected = 0;
	}

	memset(movement, 0, sizeof(*movement));
	movement->buttons = fFrameButtons;
	if (fFrameContacts == 0)
		return B_OK;

	uint64 sumX = 0;
	uint64 sumY = 0;
	for (uint32 i = 0; i < fFrameContacts; i++) {
		sumX += fFrame[i].x;
		sumY += fFrame[i].y;
	}
	uint32 x = sumX / fFrameContacts;
	uint32 y = sumY / fFrameContacts;
	x = max_c(fMinX, min_c(fMaxX, x));
	y = max_c(fMinY, min_c(fMaxY, y));

	// The input server's touchpads count up; precision touchpads count down
	// from the top. (0, 0) means no contact to it.
	movement->xPosition = max_c(x, 1);
	movement->yPosition = max_c(fMaxY - (y - fMinY), 1);
	movement->zPressure = kContactPressure;
	movement->fingerWidth = kContactWidth;
	uint32 fingers = min_c(fFrameContacts, 5);
	movement->fingers = (1 << fingers) - 1;
	return B_OK;
}
