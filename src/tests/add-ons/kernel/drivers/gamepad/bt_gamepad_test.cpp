/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Checks that a Bluetooth game pad reaches BJoystick without one: creates
	a device on the bluetooth_hid driver's control device (as the Bluetooth
	server does for a Classic HID device, and the LE input server add-on for
	a HID-over-GATT game pad), with a report descriptor laid out like the
	DualShock 4's Bluetooth report 1 (a Game Pad collection: X, Y, Z, Rz,
	hat, 14 buttons, a vendor counter, Rx, Ry triggers), writes input
	reports and reads them back through BJoystick.

	"bt_gamepad_test hold <seconds>" only creates the device and keeps it
	that long, for trying other programs (gamepad_test) on it.

	Needs the bluetooth_hid driver (/dev/input/bluetooth_hid/control) and
	permission to open it.
*/


#include <Joystick.h>
#include <String.h>

#include <bluetooth_hid.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>


static const uint8 kDescriptor[] = {
	0x05, 0x01, 0x09, 0x05, 0xa1, 0x01, 0x85, 0x01,
	// X, Y, Z, Rz: 8 bit
	0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
	0x15, 0x00, 0x26, 0xff, 0x00, 0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
	// hat switch, null state
	0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3b, 0x01,
	0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81, 0x42,
	// 14 buttons
	0x65, 0x00, 0x05, 0x09, 0x19, 0x01, 0x29, 0x0e, 0x15, 0x00, 0x25, 0x01,
	0x75, 0x01, 0x95, 0x0e, 0x81, 0x02,
	// vendor counter
	0x06, 0x00, 0xff, 0x09, 0x20, 0x75, 0x06, 0x95, 0x01, 0x15, 0x00,
	0x25, 0x7f, 0x81, 0x02,
	// Rx, Ry: triggers
	0x05, 0x01, 0x09, 0x33, 0x09, 0x34, 0x15, 0x00, 0x26, 0xff, 0x00,
	0x75, 0x08, 0x95, 0x02, 0x81, 0x02,
	0xc0
};


static int sFailures = 0;


#define CHECK(condition, format...) \
	do { \
		if (!(condition)) { \
			sFailures++; \
			printf("FAIL line %d: %s: ", __LINE__, #condition); \
			printf(format); \
			printf("\n"); \
		} \
	} while (false)


static void
print_state(const char* label, const int16* axes, int32 axisCount,
	const uint8* hats, int32 hatCount, const bool* buttons, int32 buttonCount)
{
	printf("%-10s axes", label);
	for (int32 i = 0; i < axisCount; i++)
		printf(" %6d", axes[i]);
	printf("  hats");
	for (int32 i = 0; i < hatCount; i++)
		printf(" %u", hats[i]);
	printf("  buttons");
	for (int32 i = 0; i < buttonCount; i++) {
		if (buttons[i])
			printf(" %" B_PRId32, i + 1);
	}
	printf("\n");
}


int
main(int argc, char** argv)
{
	int control = open(BLUETOOTH_HID_CONTROL_PATH, O_RDWR);
	if (control < 0) {
		printf("cannot open %s: %s\n", BLUETOOTH_HID_CONTROL_PATH,
			strerror(errno));
		return 2;
	}

	bluetooth_hid_create create;
	memset(&create, 0, sizeof(create));
	create.version = BLUETOOTH_HID_VERSION;
	static const uint8 kAddress[6] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
	memcpy(create.address, kAddress, sizeof(kAddress));
	create.vendor_id = 0x054c;
	create.product_id = 0x05c4;
	create.descriptor_length = sizeof(kDescriptor);
	create.descriptor = kDescriptor;
	strlcpy(create.name, "bt_gamepad_test", sizeof(create.name));
	if (ioctl(control, BLUETOOTH_HID_CREATE, &create, sizeof(create)) != 0) {
		printf("BLUETOOTH_HID_CREATE: %s\n", strerror(errno));
		return 2;
	}

	if (argc > 2 && strcmp(argv[1], "hold") == 0) {
		printf("holding the device for %s s\n", argv[2]);
		snooze(atoi(argv[2]) * 1000000LL);
		close(control);
		return 0;
	}

	// Find the joystick it published.
	snooze(500000);
	BJoystick joystick;
	joystick.RescanDevices();
	BString path;
	for (int32 i = 0; i < joystick.CountDevices(); i++) {
		char name[B_OS_NAME_LENGTH];
		if (joystick.GetDeviceName(i, name, sizeof(name)) == B_OK) {
			printf("joystick device: %s\n", name);
			if (strstr(name, "bluetooth") != NULL)
				path = name;
		}
	}
	CHECK(path.Length() > 0, "no joystick/bluetooth device published");
	if (path.Length() == 0)
		return 1;

	if (joystick.Open(path.String(), true) < 0) {
		printf("cannot open %s\n", path.String());
		return 1;
	}

	int32 axisCount = joystick.CountAxes();
	int32 hatCount = joystick.CountHats();
	int32 buttonCount = joystick.CountButtons();
	printf("%s: %" B_PRId32 " axes, %" B_PRId32 " hats, %" B_PRId32
		" buttons\n", path.String(), axisCount, hatCount, buttonCount);
	CHECK(axisCount == 6 && hatCount == 1 && buttonCount == 14,
		"expected 6 axes, 1 hat, 14 buttons");
	if (axisCount != 6 || hatCount != 1 || buttonCount != 14)
		return 1;

	int16 axes[6];
	uint8 hats[1];
	bool buttons[14];

	struct {
		const char*	label;
		uint8		report[10];
		int16		axes[6];
		uint8		hat;
		uint16		buttons;
			// bit n = button n + 1
	} kSteps[] = {
		{ "centred", { 0x01, 0x80, 0x80, 0x80, 0x80, 0x08, 0x00, 0x00, 0x00,
			0x00 }, { 0, 0, 0, 0, -32640, -32640 }, 0, 0 },
		{ "pressed", { 0x01, 0xff, 0x00, 0x80, 0x80, 0x22, 0x01, 0x02, 0xff,
			0x00 }, { 32639, -32640, 0, 0, 32639, -32640 }, 3,
			(1 << 1) | (1 << 4) | (1 << 13) },
		{ "released", { 0x01, 0x80, 0x80, 0x80, 0x80, 0x08, 0x00, 0x00, 0x00,
			0x00 }, { 0, 0, 0, 0, -32640, -32640 }, 0, 0 },
	};

	for (size_t step = 0; step < B_COUNT_OF(kSteps); step++) {
		if (write(control, kSteps[step].report, sizeof(kSteps[step].report))
				< 0) {
			printf("writing a report: %s\n", strerror(errno));
			return 1;
		}
		snooze(100000);
		if (joystick.Update() != B_OK) {
			printf("BJoystick::Update failed\n");
			return 1;
		}
		joystick.GetAxisValues(axes);
		joystick.GetHatValues(hats);
		joystick.GetButtonValues(buttons);
		print_state(kSteps[step].label, axes, axisCount, hats, hatCount,
			buttons, buttonCount);

		for (int32 i = 0; i < 6; i++) {
			CHECK(axes[i] == kSteps[step].axes[i], "%s: axis %" B_PRId32
				" %d, expected %d", kSteps[step].label, i, axes[i],
				kSteps[step].axes[i]);
		}
		CHECK(hats[0] == kSteps[step].hat, "%s: hat %u, expected %u",
			kSteps[step].label, hats[0], kSteps[step].hat);
		for (int32 i = 0; i < 14; i++) {
			bool expected = (kSteps[step].buttons & (1 << i)) != 0;
			CHECK(buttons[i] == expected, "%s: button %" B_PRId32,
				kSteps[step].label, i + 1);
		}
	}

	joystick.Close();
	close(control);
	printf("%s\n", sFailures == 0 ? "PASS" : "FAILED");
	return sFailures == 0 ? 0 : 1;
}
