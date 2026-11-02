/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Game pads through BJoystick, for checking real controllers:

	gamepad_test							list the joystick devices
	gamepad_test <device> [seconds]			print the state as it changes
	gamepad_test <device> rumble <strong> <weak> <ms> [<left> <right>]
											run the motors (0-65535)
*/


#include <Joystick.h>
#include <String.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static void
print_state(BJoystick& joystick, int32 axes, int32 hats, int32 buttons)
{
	int16 axisValues[axes > 0 ? axes : 1];
	uint8 hatValues[hats > 0 ? hats : 1];
	bool buttonValues[buttons > 0 ? buttons : 1];
	joystick.GetAxisValues(axisValues);
	joystick.GetHatValues(hatValues);
	joystick.GetButtonValues(buttonValues);

	printf("axes");
	for (int32 i = 0; i < axes; i++)
		printf(" %6d", axisValues[i]);
	if (hats > 0) {
		printf("  hats");
		for (int32 i = 0; i < hats; i++)
			printf(" %u", hatValues[i]);
	}
	printf("  buttons");
	for (int32 i = 0; i < buttons; i++) {
		if (buttonValues[i])
			printf(" %" B_PRId32, i + 1);
	}
	printf("\n");
}


int
main(int argc, char** argv)
{
	BJoystick joystick;
	if (argc < 2) {
		int32 count = joystick.CountDevices();
		printf("%" B_PRId32 " joystick device%s\n", count,
			count == 1 ? "" : "s");
		for (int32 i = 0; i < count; i++) {
			char name[B_OS_NAME_LENGTH];
			if (joystick.GetDeviceName(i, name, sizeof(name)) == B_OK)
				printf("  %s\n", name);
		}
		return 0;
	}

	if (joystick.Open(argv[1], true) < 0) {
		fprintf(stderr, "cannot open %s\n", argv[1]);
		return 1;
	}

	BString name;
	joystick.GetControllerName(&name);
	int32 axes = joystick.CountAxes();
	int32 hats = joystick.CountHats();
	int32 buttons = joystick.CountButtons();
	uint16 vendor = 0, product = 0;
	joystick.GetDeviceIDs(&vendor, &product);
	printf("%s: \"%s\" %04x:%04x, %" B_PRId32 " axes, %" B_PRId32 " hats, %"
		B_PRId32 " buttons, rumble: %s%s\n", argv[1], name.String(), vendor,
		product, axes, hats, buttons, joystick.HasRumble() ? "yes" : "no",
		joystick.HasTriggerRumble() ? " (with trigger motors)" : "");

	if (argc >= 6 && strcmp(argv[2], "rumble") == 0) {
		uint16 strong = (uint16)strtoul(argv[3], NULL, 0);
		uint16 weak = (uint16)strtoul(argv[4], NULL, 0);
		bigtime_t duration = strtoll(argv[5], NULL, 0) * 1000;
		uint16 left = argc >= 8 ? (uint16)strtoul(argv[6], NULL, 0) : 0;
		uint16 right = argc >= 8 ? (uint16)strtoul(argv[7], NULL, 0) : 0;
		status_t result = joystick.SetRumble(strong, weak, duration, left,
			right);
		printf("SetRumble(%u, %u, %" B_PRId64 " ms, %u, %u): %s\n", strong,
			weak, duration / 1000, left, right, strerror(result));
		// Keep the device open while the effect runs: closing it stops the
		// motors.
		if (result == B_OK)
			snooze(duration + 100000);
		return result == B_OK ? 0 : 1;
	}

	bigtime_t end = system_time()
		+ (argc >= 3 ? atoi(argv[2]) : 10) * 1000000LL;
	bigtime_t lastTimestamp = -1;
	while (system_time() < end) {
		if (joystick.Update() != B_OK) {
			fprintf(stderr, "update failed\n");
			return 1;
		}
		if (joystick.timestamp != lastTimestamp) {
			lastTimestamp = joystick.timestamp;
			print_state(joystick, axes, hats, buttons);
		}
		snooze(16000);
	}

	return 0;
}
