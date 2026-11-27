/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Pointer steps through input_server (#230).

	pointer_steps_test pairs [count [step]]
		Needs the PointerStepsDevice add-on in
		~/config/non-packaged/add-ons/input_server/devices. Puts the pointer
		at (200, 300), has the add-on queue two moves of `step` back to back,
		and checks that the pointer went 2 * step. input_server used to start
		every relative step of one round from the same position, so two
		moves it handled together went only `step`.
	pointer_steps_test set <x> <y>
	pointer_steps_test get
		For a test driven from outside (QEMU's mouse): put the pointer
		somewhere, and print where app_server has it.

	Exit status 0 when every pair moved the whole way.
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <InterfaceDefs.h>
#include <OS.h>
#include <WindowScreen.h>


static const char* kPortName = "pointer steps test";


static bool
wait_for_position(BPoint expected, BPoint& where, bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	while (true) {
		uint32 buttons;
		if (get_mouse(&where, &buttons) == B_OK && where == expected)
			return true;
		if (system_time() > deadline)
			return false;
		snooze(2000);
	}
}


static int
pairs(int32 count, int32 step)
{
	port_id port = find_port(kPortName);
	if (port < 0) {
		fprintf(stderr, "the PointerStepsDevice add-on is not running\n");
		return 2;
	}

	const BPoint start(200, 300);
	int32 whole = 0;
	int32 lost = 0;
	int32 other = 0;
	for (int32 i = 0; i < count; i++) {
		BPoint where;
		set_mouse_position((int32)start.x, (int32)start.y);
		if (!wait_for_position(start, where, 2000000)) {
			fprintf(stderr, "the pointer did not get to the start: %g, %g\n",
				where.x, where.y);
			return 2;
		}

		if (write_port(port, 1, &step, sizeof(step)) != B_OK) {
			fprintf(stderr, "the add-on went away\n");
			return 2;
		}

		// Both moves are queued at once; wait until the pointer has
		// settled.
		BPoint expected(start.x + 2 * step, start.y);
		if (wait_for_position(expected, where, 200000))
			whole++;
		else if (where == BPoint(start.x + step, start.y))
			lost++;
		else {
			other++;
			printf("pair %" B_PRId32 ": the pointer is at %g, %g\n", i,
				where.x, where.y);
		}
	}

	printf("%" B_PRId32 " pairs of %" B_PRId32 " px: %" B_PRId32 " moved %"
		B_PRId32 " px, %" B_PRId32 " moved only %" B_PRId32 " px (one step "
		"lost), %" B_PRId32 " elsewhere\n", count, step, whole, 2 * step,
		lost, step, other);
	return whole == count ? 0 : 1;
}


int
main(int argc, char** argv)
{
	BApplication app("application/x-vnd.Haiku-pointer-steps-test");

	if (argc >= 2 && strcmp(argv[1], "pairs") == 0) {
		int32 count = argc >= 3 ? atol(argv[2]) : 200;
		int32 step = argc >= 4 ? atol(argv[3]) : 10;
		return pairs(count, step);
	}
	if (argc == 4 && strcmp(argv[1], "set") == 0) {
		BPoint where;
		BPoint target(atol(argv[2]), atol(argv[3]));
		set_mouse_position((int32)target.x, (int32)target.y);
		if (!wait_for_position(target, where, 2000000)) {
			fprintf(stderr, "the pointer is at %g, %g\n", where.x, where.y);
			return 1;
		}
		return 0;
	}
	if (argc == 2 && strcmp(argv[1], "get") == 0) {
		BPoint where;
		uint32 buttons;
		if (get_mouse(&where, &buttons) != B_OK)
			return 1;
		printf("%g %g %" B_PRIu32 "\n", where.x, where.y, buttons);
		return 0;
	}

	fprintf(stderr, "usage: %s pairs [count [step]] | set <x> <y> | get\n",
		argv[0]);
	return 2;
}
