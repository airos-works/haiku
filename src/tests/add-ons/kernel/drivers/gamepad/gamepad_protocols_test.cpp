/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Checks usb_hid's game pad translations without a game pad: the
	synthesized Xbox 360 and Xbox One (GIP) report descriptors are parsed
	with the shared HID parser, packets laid out as the pads send them (see
	Linux drivers/input/joystick/xpad.c) are translated, and the result is
	read back the way JoystickProtocolHandler reads it (axes scaled to 16
	bit signed, buttons by usage). Also checks the GIP initialization and
	rumble packets and the Sony rumble reports byte for byte.
*/


#include "GamepadProtocols.h"
#include "HIDCollection.h"
#include "HIDParser.h"
#include "HIDReport.h"
#include "HIDReportItem.h"
#include "HIDWriter.h"

#include <stdio.h>
#include <string.h>

#include <usb/USB_hid.h>


static bool sVerbose = false;
static int sFailures = 0;
static int sChecks = 0;


#define CHECK(condition, format...) \
	do { \
		sChecks++; \
		if (!(condition)) { \
			sFailures++; \
			printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #condition); \
			printf(format); \
			printf("\n"); \
		} \
	} while (false)


/*!	What a JoystickProtocolHandler makes of a report: the same item
	selection and value extraction.
*/
class JoystickView {
public:
	JoystickView(HIDParser& parser)
		:
		fReport(NULL),
		fAxisCount(0),
		fButtonCount(0),
		fMaxButton(0)
	{
		if (parser.CountReports(HID_REPORT_TYPE_INPUT) != 1)
			return;
		fReport = parser.ReportAt(HID_REPORT_TYPE_INPUT, 0);

		for (uint32 i = 0; i < fReport->CountItems(); i++) {
			HIDReportItem* item = fReport->ItemAt(i);
			if (!item->HasData())
				continue;
			if (item->UsagePage() == B_HID_USAGE_PAGE_BUTTON) {
				if (fButtonCount < 32)
					fButtons[fButtonCount++] = item;
				if (fMaxButton < item->UsageID())
					fMaxButton = item->UsageID();
			} else if (item->UsagePage() == B_HID_USAGE_PAGE_GENERIC_DESKTOP
				&& item->UsageID() >= B_HID_UID_GD_X
				&& item->UsageID() <= B_HID_UID_GD_WHEEL) {
				if (fAxisCount < 16) {
					fAxisUsages[fAxisCount] = item->UsageID();
					fAxes[fAxisCount++] = item;
				}
			}
		}
	}

	bool IsValid() const { return fReport != NULL; }
	HIDReport* Report() const { return fReport; }
	uint32 CountAxes() const { return fAxisCount; }
	uint32 MaxButton() const { return fMaxButton; }
	uint16 AxisUsage(uint32 index) const { return fAxisUsages[index]; }

	void Read(uint8* report, size_t length)
	{
		fReport->SetReport(B_OK, report, length);
		fButtonBits = 0;
		for (uint32 i = 0; i < fAxisCount; i++) {
			fAxisValues[i] = 0;
			if (fAxes[i]->Extract() == B_OK && fAxes[i]->Valid())
				fAxisValues[i] = (int16)fAxes[i]->ScaledData(16, true);
		}
		for (uint32 i = 0; i < fButtonCount; i++) {
			uint16 index = fButtons[i]->UsageID() - 1;
			if (fButtons[i]->Extract() == B_OK && fButtons[i]->Valid())
				fButtonBits |= (fButtons[i]->Data() & 1) << index;
		}
	}

	void Print(const char* label) const
	{
		if (!sVerbose)
			return;
		printf("  %-22s buttons %#06" B_PRIx32 " axes", label, fButtonBits);
		for (uint32 i = 0; i < fAxisCount; i++)
			printf(" %6d", fAxisValues[i]);
		printf("\n");
	}

	int16 Axis(uint32 index) const { return fAxisValues[index]; }
	uint32 Buttons() const { return fButtonBits; }

private:
	HIDReport*		fReport;
	HIDReportItem*	fAxes[16];
	uint16			fAxisUsages[16];
	int16			fAxisValues[16];
	uint32			fAxisCount;
	HIDReportItem*	fButtons[32];
	uint32			fButtonCount;
	uint32			fMaxButton;
	uint32			fButtonBits;
};


// Buttons as the joystick device numbers them (bit = button number - 1).
enum {
	kUp = 1 << 0, kDown = 1 << 1, kLeft = 1 << 2, kRight = 1 << 3,
	kStart = 1 << 4, kBack = 1 << 5, kLeftStick = 1 << 6,
	kRightStick = 1 << 7, kLB = 1 << 8, kRB = 1 << 9, kGuide = 1 << 10,
	kA = 1 << 11, kB = 1 << 12, kX = 1 << 13, kY = 1 << 14,
	kShare = 1 << 15
};


static void
print_bytes(const char* label, const uint8* data, size_t length)
{
	printf("  %s:", label);
	for (size_t i = 0; i < length; i++)
		printf(" %02x", data[i]);
	printf("\n");
}


static void
check_bytes(const char* label, const uint8* data, size_t length,
	const uint8* expected, size_t expectedLength)
{
	bool same = length == expectedLength
		&& memcmp(data, expected, length) == 0;
	CHECK(same, "%s differs", label);
	if (!same) {
		print_bytes("got     ", data, length);
		print_bytes("expected", expected, expectedLength);
	}
}


static bool
parse(HIDWriter& writer, HIDParser& parser, const char* name)
{
	status_t result = parser.ParseReportDescriptor(writer.Buffer(),
		writer.BufferLength());
	CHECK(result == B_OK, "parsing the %s descriptor: %s", name,
		strerror(result));
	if (result != B_OK)
		return false;

	HIDCollection* root = parser.RootCollection();
	CHECK(root != NULL
		&& root->CountChildrenFlat(COLLECTION_APPLICATION) == 1,
		"%s: one application collection", name);
	HIDCollection* application = root != NULL
		? root->ChildAtFlat(COLLECTION_APPLICATION, 0) : NULL;
	CHECK(application != NULL
		&& application->UsagePage() == B_HID_USAGE_PAGE_GENERIC_DESKTOP
		&& application->UsageID() == B_HID_UID_GD_JOYSTICK,
		"%s: a joystick collection (the joystick handler's condition)", name);
	CHECK(!parser.UsesReportIDs(), "%s: no report IDs", name);
	return true;
}


static void
test_gip()
{
	printf("GIP (Xbox One / Series)\n");

	HIDWriter writer;
	CHECK(gip_build_descriptor(writer) == B_OK, "building the descriptor");
	HIDParser parser(NULL);
	if (!parse(writer, parser, "GIP"))
		return;

	CHECK(parser.MaxReportSize(HID_REPORT_TYPE_INPUT) == GIP_REPORT_SIZE,
		"report size %zu, expected %d",
		parser.MaxReportSize(HID_REPORT_TYPE_INPUT), GIP_REPORT_SIZE);

	JoystickView view(parser);
	CHECK(view.IsValid(), "one input report");
	if (!view.IsValid())
		return;

	static const uint16 kAxes[] = { B_HID_UID_GD_Z, B_HID_UID_GD_RZ,
		B_HID_UID_GD_X, B_HID_UID_GD_Y, B_HID_UID_GD_RX, B_HID_UID_GD_RY };
	CHECK(view.CountAxes() == 6, "%" B_PRIu32 " axes", view.CountAxes());
	for (uint32 i = 0; i < 6 && i < view.CountAxes(); i++) {
		CHECK(view.AxisUsage(i) == kAxes[i], "axis %" B_PRIu32 " usage %#x",
			i, view.AxisUsage(i));
	}
	CHECK(view.MaxButton() == 16, "%" B_PRIu32 " buttons", view.MaxButton());

	uint8 report[GIP_REPORT_SIZE + 3] = {};
	gip_translation result;

	// At rest. Sticks at 0 read as 0 for X and -1 for the flipped Y.
	const uint8 kRest[] = { 0x20, 0x00, 0x01, 0x0e,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	gip_translate(0x045e, 0x02ea, kRest, sizeof(kRest), report, result);
	CHECK(result.deliver && !result.announced && result.ack_length == 0,
		"rest packet delivered");
	view.Read(report, GIP_REPORT_SIZE);
	view.Print("GIP at rest");
	CHECK(view.Buttons() == 0, "rest buttons %#" B_PRIx32, view.Buttons());
	// (HIDReportItem::ScaledData() repeats the bits of a 10 bit value to
	// fill 16: 0 -> 0x8020, 1023 -> 0x7fdf.)
	CHECK(view.Axis(0) == -32736 && view.Axis(1) == -32736,
		"released triggers at the minimum: %d %d", view.Axis(0),
		view.Axis(1));
	CHECK(view.Axis(2) == 0 && view.Axis(3) == -1 && view.Axis(4) == 0
		&& view.Axis(5) == -1, "centred sticks: %d %d %d %d", view.Axis(2),
		view.Axis(3), view.Axis(4), view.Axis(5));

	// A + Menu (byte 4: bit 4, bit 2), up + LB + right stick press
	// (byte 5: bit 0, 4, 7), left trigger full (1023), right trigger half
	// (512), left stick right and up, right stick left and down.
	const uint8 kPressed[] = { 0x20, 0x00, 0x02, 0x0e,
		0x14, 0x91, 0xff, 0x03, 0x00, 0x02,
		0xff, 0x7f, 0xff, 0x7f, 0x00, 0x80, 0x00, 0x80 };
	gip_translate(0x045e, 0x02ea, kPressed, sizeof(kPressed), report,
		result);
	CHECK(result.deliver, "input packet delivered");
	view.Read(report, GIP_REPORT_SIZE);
	view.Print("GIP A Menu Up LB RS");
	CHECK(view.Buttons() == (kA | kStart | kUp | kLB | kRightStick),
		"pressed buttons %#" B_PRIx32 ", expected %#x", view.Buttons(),
		kA | kStart | kUp | kLB | kRightStick);
	CHECK(view.Axis(0) == 32735, "left trigger full: %d", view.Axis(0));
	CHECK(view.Axis(1) == 0, "right trigger half: %d", view.Axis(1));
	CHECK(view.Axis(2) == 32767, "left stick right: %d", view.Axis(2));
	CHECK(view.Axis(3) == -32768, "left stick up reads negative: %d",
		view.Axis(3));
	CHECK(view.Axis(4) == -32768, "right stick left: %d", view.Axis(4));
	CHECK(view.Axis(5) == 32767, "right stick down reads positive: %d",
		view.Axis(5));

	// Every other button: B X Y (byte 4 bits 5-7), View (bit 3); down,
	// left, right, RB, left stick press (byte 5 bits 1, 2, 3, 5, 6).
	const uint8 kOthers[] = { 0x20, 0x00, 0x03, 0x0e,
		0xe8, 0x6e, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	gip_translate(0x045e, 0x02ea, kOthers, sizeof(kOthers), report, result);
	view.Read(report, GIP_REPORT_SIZE);
	uint32 expected = kB | kX | kY | kBack | kDown | kLeft | kRight | kRB
		| kLeftStick;
	CHECK(view.Buttons() == expected, "other buttons %#" B_PRIx32
		", expected %#" B_PRIx32, view.Buttons(), expected);

	// The Guide button comes in its own packet; the One S asks for an ack.
	const uint8 kGuideDown[] = { 0x07, 0x30, 0x05, 0x02, 0x01, 0x5b };
	gip_translate(0x045e, 0x02ea, kGuideDown, sizeof(kGuideDown), report,
		result);
	CHECK(result.deliver, "guide packet delivered");
	const uint8 kAck[] = { 0x01, 0x20, 0x05, 0x09, 0x00, 0x07, 0x20, 0x02,
		0x00, 0x00, 0x00, 0x00, 0x00 };
	check_bytes("guide ack", result.ack, result.ack_length, kAck,
		sizeof(kAck));
	view.Read(report, GIP_REPORT_SIZE);
	view.Print("GIP + Guide");
	CHECK(view.Buttons() == (expected | kGuide),
		"guide joins the held buttons: %#" B_PRIx32, view.Buttons());

	// It stays down across input packets until its own release.
	gip_translate(0x045e, 0x02ea, kRest, sizeof(kRest), report, result);
	view.Read(report, GIP_REPORT_SIZE);
	CHECK(view.Buttons() == kGuide, "guide held: %#" B_PRIx32,
		view.Buttons());
	const uint8 kGuideUp[] = { 0x07, 0x20, 0x06, 0x02, 0x00, 0x5b };
	gip_translate(0x045e, 0x02ea, kGuideUp, sizeof(kGuideUp), report,
		result);
	CHECK(result.deliver && result.ack_length == 0,
		"guide release, no ack asked for");
	view.Read(report, GIP_REPORT_SIZE);
	CHECK(view.Buttons() == 0, "guide released: %#" B_PRIx32,
		view.Buttons());

	// Packets without input.
	const uint8 kStatus[] = { 0x03, 0x20, 0x07, 0x04, 0x83, 0x00, 0x00,
		0x00 };
	gip_translate(0x045e, 0x02ea, kStatus, sizeof(kStatus), report, result);
	CHECK(!result.deliver && !result.announced, "status packet ignored");
	const uint8 kAnnounce[] = { 0x02, 0x20, 0x01, 0x1c, 0x7e, 0xed, 0x8b,
		0x11, 0x0f, 0xa8, 0x00, 0x00, 0x5e, 0x04, 0xea, 0x02, 0x05, 0x00,
		0x11, 0x00, 0x82, 0x0c, 0x00, 0x00, 0x04, 0x00, 0x01, 0x00, 0x01,
		0x00, 0x01, 0x00 };
	gip_translate(0x045e, 0x02ea, kAnnounce, sizeof(kAnnounce), report,
		result);
	CHECK(!result.deliver && result.announced, "announce noticed");
	const uint8 kShort[] = { 0x20, 0x00, 0x08, 0x0e, 0x10 };
	gip_translate(0x045e, 0x02ea, kShort, sizeof(kShort), report, result);
	CHECK(!result.deliver, "truncated input packet ignored");

	// Xbox Series: Share at 18 bytes from the end of a longer packet; not
	// read on other pads.
	uint8 series[48] = { 0x20, 0x00, 0x09, 0x2c, 0x10 };
	series[sizeof(series) - 18] = 0x01;
	gip_translate(0x045e, 0x0b12, series, sizeof(series), report, result);
	view.Read(report, GIP_REPORT_SIZE);
	CHECK(view.Buttons() == (kA | kShare), "Series A + Share: %#" B_PRIx32,
		view.Buttons());
	gip_translate(0x045e, 0x02ea, series, sizeof(series), report, result);
	view.Read(report, GIP_REPORT_SIZE);
	CHECK(view.Buttons() == kA, "no Share on a One S: %#" B_PRIx32,
		view.Buttons());

	// Initialization packets.
	struct {
		uint16 vendor, product;
		uint8 commands[8];
	} kInit[] = {
		// plain pad: power on, LED, authentication done
		{ 0x045e, 0x02dd, { 0x05, 0x0a, 0x06 } },
		// One S: plus the "was in Bluetooth mode" packet
		{ 0x045e, 0x02ea, { 0x05, 0x05, 0x0a, 0x06 } },
		// Elite 2: plus the paddle report request
		{ 0x045e, 0x0b00, { 0x05, 0x05, 0x4d, 0x0a, 0x06 } },
		// Hori: identify ack first
		{ 0x0f0d, 0x0067, { 0x01, 0x05, 0x0a, 0x06 } },
		// PowerA: rumble on and off at the end
		{ 0x24c6, 0x543a, { 0x05, 0x0a, 0x06, 0x09, 0x09 } },
	};
	for (size_t i = 0; i < B_COUNT_OF(kInit); i++) {
		uint8 packet[GIP_MAX_PACKET];
		uint32 count = 0;
		bool same = true;
		while (true) {
			size_t length = gip_init_packet(kInit[i].vendor,
				kInit[i].product, count, packet);
			if (length == 0)
				break;
			if (count >= 8 || packet[0] != kInit[i].commands[count])
				same = false;
			count++;
		}
		same = same && (count == 8 || kInit[i].commands[count] == 0);
		CHECK(same, "init packets of %04x:%04x", kInit[i].vendor,
			kInit[i].product);
	}
	uint8 packet[GIP_MAX_PACKET];
	size_t length = gip_init_packet(0x045e, 0x02dd, 0, packet);
	const uint8 kPowerOn[] = { 0x05, 0x20, 0x00, 0x01, 0x00 };
	check_bytes("power on", packet, length, kPowerOn, sizeof(kPowerOn));

	// Rumble: magnitudes to 0-100.
	length = gip_build_rumble(0x07, 65535, 32768, 0, 65535, packet);
	const uint8 kRumble[] = { 0x09, 0x00, 0x07, 0x09, 0x00, 0x0f, 0x00,
		0x64, 0x64, 0x32, 0xff, 0x00, 0xff };
	check_bytes("rumble", packet, length, kRumble, sizeof(kRumble));
}


static void
test_xbox360()
{
	printf("Xbox 360\n");

	HIDWriter writer;
	CHECK(xbox360_build_descriptor(writer) == B_OK, "building descriptor");
	HIDParser parser(NULL);
	if (!parse(writer, parser, "Xbox 360"))
		return;

	CHECK(parser.MaxReportSize(HID_REPORT_TYPE_INPUT) == XBOX360_REPORT_SIZE,
		"report size %zu", parser.MaxReportSize(HID_REPORT_TYPE_INPUT));
	JoystickView view(parser);
	CHECK(view.IsValid() && view.CountAxes() == 6,
		"one input report with 6 axes");
	if (!view.IsValid())
		return;
	CHECK(view.MaxButton() == 15, "%" B_PRIu32 " buttons", view.MaxButton());

	// Up, Start, LB, guide, A, Y; triggers 255 / 0; left stick up-right,
	// right stick down.
	const uint8 kPacket[] = { 0x00, 0x14, 0x11, 0x95, 0xff, 0x00,
		0xff, 0x7f, 0xff, 0x7f, 0x00, 0x00, 0x00, 0x80,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	uint8 report[XBOX360_REPORT_SIZE + 3] = {};
	CHECK(xbox360_translate(kPacket, sizeof(kPacket), report),
		"input message translated");
	view.Read(report, XBOX360_REPORT_SIZE);
	view.Print("360 input");
	uint32 expected = kUp | kStart | kLB | kGuide | kA | kY;
	CHECK(view.Buttons() == expected, "buttons %#" B_PRIx32 ", expected %#"
		B_PRIx32, view.Buttons(), expected);
	CHECK(view.Axis(0) == 32639 && view.Axis(1) == -32640,
		"triggers %d %d", view.Axis(0), view.Axis(1));
	CHECK(view.Axis(2) == 32767 && view.Axis(3) == -32768,
		"left stick right and up (negative): %d %d", view.Axis(2),
		view.Axis(3));
	CHECK(view.Axis(4) == 0 && view.Axis(5) == 32767,
		"right stick down (positive): %d %d", view.Axis(4), view.Axis(5));

	// LED and rumble state messages carry no input.
	const uint8 kLED[] = { 0x01, 0x03, 0x0e };
	CHECK(!xbox360_translate(kLED, sizeof(kLED), report), "LED message");
	const uint8 kRumbleState[] = { 0x03, 0x03, 0x00 };
	CHECK(!xbox360_translate(kRumbleState, sizeof(kRumbleState), report),
		"rumble state message");

	uint8 packet[8];
	size_t length = xbox360_build_rumble(0xffff, 0x8000, packet);
	const uint8 kRumble[] = { 0x00, 0x08, 0x00, 0xff, 0x80, 0x00, 0x00,
		0x00 };
	check_bytes("rumble", packet, length, kRumble, sizeof(kRumble));
}


static void
test_sony()
{
	printf("Sony rumble reports\n");

	uint8 report[DUALSENSE_OUTPUT_SIZE];
	size_t length = dualshock4_build_rumble(0xff00, 0x4000, report);
	CHECK(length == 32 && report[0] == 0x05 && report[1] == 0x01
		&& report[2] == 0 && report[3] == 0 && report[4] == 0x40
		&& report[5] == 0xff && report[6] == 0 && report[7] == 0
		&& report[8] == 0, "DualShock 4 report");

	length = dualsense_build_rumble(0xff00, 0x4000, report);
	CHECK(length == 48 && report[0] == 0x02 && report[1] == 0x03
		&& report[2] == 0 && report[3] == 0x40 && report[4] == 0xff
		&& report[5] == 0, "DualSense report");
}


int
main(int argc, char** argv)
{
	sVerbose = argc > 1 && strcmp(argv[1], "-v") == 0;

	test_gip();
	test_xbox360();
	test_sony();

	printf("%d checks, %d failed\n", sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}
