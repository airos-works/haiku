/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <LEHIDMouseDecoder.h>

#include "HIDCollection.h"
#include "HIDParser.h"
#include "HIDReport.h"
#include "HIDReportItem.h"

#include <Errors.h>
#include <new>


namespace Bluetooth {

static const uint16 kGenericDesktop = 0x01;
static const uint16 kButton = 0x09;
static const uint16 kConsumer = 0x0c;
static const uint16 kXAxis = 0x30;
static const uint16 kYAxis = 0x31;
static const uint16 kWheel = 0x38;
static const uint16 kPan = 0x0238;
static const uint16 kMaximumButtons = 8;
static const uint16 kJoystick = 0x04;
static const uint16 kGamePad = 0x05;
static const uint16 kMultiAxisController = 0x08;


LEHIDMouseDecoder::LEHIDMouseDecoder()
	:
	fParser(NULL)
{
}


LEHIDMouseDecoder::~LEHIDMouseDecoder()
{
	delete fParser;
}


status_t
LEHIDMouseDecoder::Init(const uint8* reportMap, size_t length)
{
	delete fParser;
	fParser = NULL;
	if (reportMap == NULL || length == 0 || length > 4096)
		return B_BAD_VALUE;
	HIDParser* parser = new(std::nothrow) HIDParser(NULL);
	if (parser == NULL)
		return B_NO_MEMORY;
	status_t status = parser->ParseReportDescriptor(reportMap, length);
	if (status != B_OK) {
		delete parser;
		return status;
	}
	bool hasMouse = false;
	for (uint8 i = 0; i < parser->CountReports(HID_REPORT_TYPE_INPUT); i++) {
		HIDReport* report = parser->ReportAt(HID_REPORT_TYPE_INPUT, i);
		HIDReportItem* x = report->FindItem(kGenericDesktop, kXAxis);
		HIDReportItem* y = report->FindItem(kGenericDesktop, kYAxis);
		if (x != NULL && y != NULL && x->HasData() && y->HasData()
			&& x->Relative() && y->Relative()) {
			hasMouse = true;
			break;
		}
	}
	if (!hasMouse) {
		delete parser;
		return B_ENTRY_NOT_FOUND;
	}
	fParser = parser;
	return B_OK;
}


static int32
ReadAxis(HIDReportItem* item)
{
	if (item == NULL || !item->HasData() || item->Extract() != B_OK
		|| !item->Valid())
		return 0;
	return (int32)item->Data();
}


bool
LEHIDMouseDecoder::SupportsReport(uint8 reportID) const
{
	if (fParser == NULL)
		return false;
	HIDReport* report = fParser->FindReport(HID_REPORT_TYPE_INPUT, reportID);
	if (report == NULL)
		return false;
	HIDReportItem* x = report->FindItem(kGenericDesktop, kXAxis);
	HIDReportItem* y = report->FindItem(kGenericDesktop, kYAxis);
	return x != NULL && y != NULL && x->HasData() && y->HasData()
		&& x->Relative() && y->Relative();
}


status_t
LEHIDMouseDecoder::Decode(uint8 reportID, const uint8* value,
	size_t length, LEMouseReport& output)
{
	output = {};
	if (fParser == NULL)
		return B_NO_INIT;
	if (value == NULL)
		return B_BAD_VALUE;
	HIDReport* report = fParser->FindReport(HID_REPORT_TYPE_INPUT, reportID);
	if (report == NULL)
		return B_ENTRY_NOT_FOUND;
	if (length < report->ReportSize())
		return B_BAD_DATA;
	if (!SupportsReport(reportID))
		return B_ENTRY_NOT_FOUND;
	HIDReportItem* x = report->FindItem(kGenericDesktop, kXAxis);
	HIDReportItem* y = report->FindItem(kGenericDesktop, kYAxis);
	// HIDReportItem reads directly from this buffer; validate the complete
	// descriptor-defined length before exposing it to the parser.
	report->SetReport(B_OK, const_cast<uint8*>(value), length);
	output.x = ReadAxis(x);
	output.y = ReadAxis(y);
	output.wheelY = ReadAxis(report->FindItem(kGenericDesktop, kWheel));
	output.wheelX = ReadAxis(report->FindItem(kConsumer, kPan));
	for (uint32 i = 0; i < report->CountItems(); i++) {
		HIDReportItem* item = report->ItemAt(i);
		if (item == NULL || !item->HasData()
			|| item->UsagePage() != kButton || item->UsageID() == 0
			|| item->UsageID() > kMaximumButtons)
			continue;
		if (item->Extract() == B_OK && item->Valid()
			&& (item->Data() & 1) != 0)
			output.buttons |= 1u << (item->UsageID() - 1);
	}
	report->SetReport(B_NO_INIT, NULL, 0);
	return B_OK;
}



bool
LEHIDReportMapHasGamepad(const uint8* reportMap, size_t length)
{
	if (reportMap == NULL || length == 0 || length > 4096)
		return false;
	HIDParser parser(NULL);
	if (parser.ParseReportDescriptor(reportMap, length) != B_OK)
		return false;
	HIDCollection* root = parser.RootCollection();
	if (root == NULL)
		return false;
	uint32 count = root->CountChildrenFlat(COLLECTION_APPLICATION);
	for (uint32 i = 0; i < count; i++) {
		HIDCollection* collection
			= root->ChildAtFlat(COLLECTION_APPLICATION, i);
		if (collection != NULL && collection->UsagePage() == kGenericDesktop
			&& (collection->UsageID() == kJoystick
				|| collection->UsageID() == kGamePad
				|| collection->UsageID() == kMultiAxisController))
			return true;
	}
	return false;
}

} // namespace Bluetooth
