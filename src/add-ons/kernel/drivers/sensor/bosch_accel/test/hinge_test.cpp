/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


//!	Host test of the HingeTracker: run.sh builds and runs it.


#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <accelerometer.h>

#include "HingeTracker.h"


static int sFailures;
static HingeTracker::lid_state sLid = HingeTracker::kLidUnknown;


#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			sFailures++; \
			printf("FAILED %s:%d: %s: ", __FILE__, __LINE__, #condition); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (false)


static HingeTracker::lid_state
lid_hook(void*)
{
	return sLid;
}


static const char*
mode_name(uint32 mode)
{
	static const char* const kNames[] = { "unknown", "laptop", "stand", "tent",
		"tablet" };
	return mode < 5 ? kNames[mode] : "?";
}


static double
radians(double degrees)
{
	return degrees * M_PI / 180;
}


/*!	What the two accelerometers measure with the screen opened by \a hinge
	degrees, while the base is tilted by \a baseTilt (90: it lies on the table,
	keyboard up; 0: it stands on its front edge, the hinge at the top; -90:
	keyboard down) and the hinge leans by \a hingeLean degrees out of the
	horizontal.
*/
static void
pose(double hinge, double baseTilt, double hingeLean, accel_vector& display,
	accel_vector& base, int32 noise = 0)
{
	double perpendicular = 1000 * cos(radians(hingeLean));
	double along = 1000 * sin(radians(hingeLean));
	double displayTilt = baseTilt - (180 - hinge);

	base.x = (int32)lround(along);
	base.y = (int32)lround(perpendicular * cos(radians(baseTilt)));
	base.z = (int32)lround(perpendicular * sin(radians(baseTilt)));
	display.x = (int32)lround(along);
	display.y = (int32)lround(perpendicular * cos(radians(displayTilt)));
	display.z = (int32)lround(perpendicular * sin(radians(displayTilt)));

	if (noise > 0) {
		int32* values[] = { &base.x, &base.y, &base.z, &display.x, &display.y,
			&display.z };
		for (int i = 0; i < 6; i++)
			*values[i] += rand() % (2 * noise + 1) - noise;
	}
}


static void
settle(HingeTracker& tracker, double hinge, double baseTilt,
	double hingeLean = 0, int32 noise = 0, int count = 12)
{
	for (int i = 0; i < count; i++) {
		accel_vector display, base;
		pose(hinge, baseTilt, hingeLean, display, base, noise);
		tracker.Update(display, base);
	}
}


static void
test_atan2()
{
	double worst = 0;
	for (int32 y = -1200; y <= 1200; y += 7) {
		for (int32 x = -1200; x <= 1200; x += 7) {
			if (x == 0 && y == 0)
				continue;
			double expected = atan2(y, x) * 180 / M_PI;
			if (expected < 0)
				expected += 360;
			double got = HingeTracker::Atan2(y, x) / 10.0;
			double error = fabs(got - expected);
			if (error > 180)
				error = 360 - error;
			if (error > worst)
				worst = error;
		}
	}
	printf("atan2: at most %.2f degrees off\n", worst);
	CHECK(worst < 0.35, "%.2f degrees", worst);
	CHECK(HingeTracker::Atan2(0, 0) == 0, "of nothing");
	CHECK(HingeTracker::Atan2(0, 1000) == 0, "%d",
		(int)HingeTracker::Atan2(0, 1000));
	CHECK(HingeTracker::Atan2(1000, 0) == 900, "%d",
		(int)HingeTracker::Atan2(1000, 0));
	CHECK(HingeTracker::Atan2(0, -1000) == 1800, "%d",
		(int)HingeTracker::Atan2(0, -1000));
	CHECK(HingeTracker::Atan2(-1000, 0) == 2700, "%d",
		(int)HingeTracker::Atan2(-1000, 0));
}


//!	What the Yoga 11e measured on the desk, opened like a laptop.
static void
test_measured()
{
	HingeTracker tracker;
	accel_vector display = { 20, 1039, -78 };
	accel_vector base = { -13, 85, 1030 };
	bool changed = tracker.Update(display, base);
	printf("measured on the Yoga 11e: %s, %d degrees\n",
		mode_name(tracker.Mode()), (int)tracker.Angle());
	CHECK(changed, "the first mode is a change");
	CHECK(tracker.Mode() == HINGE_MODE_LAPTOP, "%s",
		mode_name(tracker.Mode()));
	CHECK(abs(tracker.Angle() - 90) <= 2, "%d", (int)tracker.Angle());
	CHECK(tracker.IsAngleValid(), "valid");
}


//!	Folds the screen all the way back and forth again, the base on the table.
static void
test_fold(int32 noise)
{
	HingeTracker tracker;
	tracker.SetLidHook(lid_hook, NULL);
	sLid = HingeTracker::kLidOpen;

	uint32 mode = HINGE_MODE_UNKNOWN;
	int changes = 0;
	double foldedAt = -1, tabletAt = -1, noTabletAt = -1, laptopAt = -1;
	int32 worst = 0;

	for (double hinge = 60; hinge <= 360; hinge += 0.5) {
		accel_vector display, base;
		// (past 180 degrees the base is what gets turned over)
		double baseTilt = hinge <= 180 ? 90 : 90 - (hinge - 180);
		pose(hinge, baseTilt, 0, display, base, noise);
		if (tracker.Update(display, base)) {
			changes++;
			if (mode == HINGE_MODE_LAPTOP)
				foldedAt = hinge;
			if (tracker.Mode() == HINGE_MODE_TABLET)
				tabletAt = hinge;
			mode = tracker.Mode();
		}
		int32 error = abs(tracker.Angle() - (int32)lround(hinge));
		if (hinge > 62 && error > worst)
			worst = error;
	}
	CHECK(mode == HINGE_MODE_TABLET, "%s", mode_name(mode));
	CHECK(tracker.Angle() >= 355, "%d", (int)tracker.Angle());
	CHECK(foldedAt > 198 && foldedAt < 206, "%.1f", foldedAt);
	CHECK(tabletAt > 338 && tabletAt < 346, "%.1f", tabletAt);
	CHECK(changes == 3, "%d changes", changes);

	// stays a tablet, however long, and however it lies
	settle(tracker, 360, -90, 0, noise, 200);
	CHECK(tracker.Mode() == HINGE_MODE_TABLET, "%s",
		mode_name(tracker.Mode()));
	settle(tracker, 359, -20, 30, noise, 200);
	CHECK(tracker.Mode() == HINGE_MODE_TABLET, "%s",
		mode_name(tracker.Mode()));
	CHECK(tracker.Angle() >= 350, "%d", (int)tracker.Angle());

	changes = 0;
	mode = tracker.Mode();
	for (double hinge = 360; hinge >= 0; hinge -= 0.5) {
		accel_vector display, base;
		double baseTilt = hinge <= 180 ? 90 : 90 - (hinge - 180);
		pose(hinge, baseTilt, 0, display, base, noise);
		if (tracker.Update(display, base)) {
			changes++;
			if (mode == HINGE_MODE_TABLET)
				noTabletAt = hinge;
			if (tracker.Mode() == HINGE_MODE_LAPTOP)
				laptopAt = hinge;
			mode = tracker.Mode();
		}
		int32 error = abs(tracker.Angle() - (int32)lround(hinge));
		if (error > worst)
			worst = error;
	}
	CHECK(mode == HINGE_MODE_LAPTOP, "%s", mode_name(mode));
	CHECK(tracker.Angle() <= 5, "closed is %d", (int)tracker.Angle());
	CHECK(noTabletAt < 327 && noTabletAt > 319, "%.1f", noTabletAt);
	CHECK(laptopAt < 190 && laptopAt > 182, "%.1f", laptopAt);
	CHECK(changes == 2, "%d changes", changes);
	CHECK(worst <= (noise > 0 ? 8 : 3), "%d degrees off", (int)worst);

	printf("fold (noise %d mg): folded at %.1f, tablet at %.1f, no tablet at "
		"%.1f, laptop at %.1f; the angle at most %d degrees off\n", (int)noise,
		foldedAt, tabletAt, noTabletAt, laptopAt, (int)worst);
}


static void
test_stand_and_tent()
{
	HingeTracker tracker;

	// stand: the keyboard face down on the table, the screen upright
	settle(tracker, 290, -90);
	CHECK(tracker.Mode() == HINGE_MODE_STAND, "%s",
		mode_name(tracker.Mode()));
	CHECK(abs(tracker.Angle() - 290) <= 2, "%d", (int)tracker.Angle());

	// tent: the hinge at the top
	settle(tracker, 300, 30);
	CHECK(tracker.Mode() == HINGE_MODE_TENT, "%s",
		mode_name(tracker.Mode()));

	// the screen on its side between the two: still the tent
	settle(tracker, 300, -30);
	CHECK(tracker.Mode() == HINGE_MODE_TENT, "%s",
		mode_name(tracker.Mode()));

	settle(tracker, 300, -100);
	CHECK(tracker.Mode() == HINGE_MODE_STAND, "%s",
		mode_name(tracker.Mode()));

	settle(tracker, 110, 90);
	CHECK(tracker.Mode() == HINGE_MODE_LAPTOP, "%s",
		mode_name(tracker.Mode()));
	printf("stand and tent: done\n");
}


//!	Closed and folded back measure the same.
static void
test_ambiguous_start()
{
	struct {
		double						hinge;
		double						baseTilt;
		HingeTracker::lid_state		lid;
		uint32						mode;
		const char*					what;
	} kCases[] = {
		{ 0, 90, HingeTracker::kLidClosed, HINGE_MODE_LAPTOP,
			"closed on the table" },
		{ 2, 90, HingeTracker::kLidClosed, HINGE_MODE_LAPTOP,
			"closed, measured as 2 degrees" },
		{ 358, 90, HingeTracker::kLidClosed, HINGE_MODE_LAPTOP,
			"closed, measured as 358 degrees" },
		{ 360, -90, HingeTracker::kLidOpen, HINGE_MODE_TABLET,
			"a tablet on the table" },
		{ 3, -90, HingeTracker::kLidOpen, HINGE_MODE_TABLET,
			"a tablet, measured as 3 degrees" },
		{ 360, -90, HingeTracker::kLidUnknown, HINGE_MODE_TABLET,
			"no lid switch, the screen up" },
		{ 0, 90, HingeTracker::kLidUnknown, HINGE_MODE_LAPTOP,
			"no lid switch, the screen down" },
		{ 360, -40, HingeTracker::kLidUnknown, HINGE_MODE_TABLET,
			"no lid switch, a tablet held in the hands" },
	};

	for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
		HingeTracker tracker;
		tracker.SetLidHook(lid_hook, NULL);
		sLid = kCases[i].lid;
		settle(tracker, kCases[i].hinge, kCases[i].baseTilt);
		CHECK(tracker.Mode() == kCases[i].mode, "%s: %s, %d degrees",
			kCases[i].what, mode_name(tracker.Mode()), (int)tracker.Angle());
	}
	printf("ambiguous start: done\n");
}


//!	Held like an open book the hinge is vertical, and says nothing.
static void
test_vertical_hinge()
{
	HingeTracker tracker;
	tracker.SetLidHook(lid_hook, NULL);
	sLid = HingeTracker::kLidOpen;

	settle(tracker, 360, -90);
	CHECK(tracker.Mode() == HINGE_MODE_TABLET, "%s",
		mode_name(tracker.Mode()));

	// a tablet turned to portrait: whatever the remainder of gravity says
	for (int i = 0; i < 100; i++) {
		accel_vector display, base;
		pose(i * 37 % 360, i * 53 % 360, 80, display, base);
		CHECK(!tracker.Update(display, base), "changed at %d", i);
	}
	CHECK(tracker.Mode() == HINGE_MODE_TABLET, "%s",
		mode_name(tracker.Mode()));
	CHECK(!tracker.IsAngleValid(), "valid");
	CHECK(tracker.Angle() == -1, "%d", (int)tracker.Angle());

	// and back to landscape
	settle(tracker, 360, -60);
	CHECK(tracker.Mode() == HINGE_MODE_TABLET, "%s",
		mode_name(tracker.Mode()));
	CHECK(tracker.Angle() >= 355, "%d", (int)tracker.Angle());

	// the same for a laptop on its side
	settle(tracker, 100, 90);
	CHECK(tracker.Mode() == HINGE_MODE_LAPTOP, "%s",
		mode_name(tracker.Mode()));
	settle(tracker, 250, 20, 85, 0, 100);
	CHECK(tracker.Mode() == HINGE_MODE_LAPTOP, "%s",
		mode_name(tracker.Mode()));
	printf("vertical hinge: done\n");
}


static void
test_shaking()
{
	HingeTracker tracker;
	settle(tracker, 100, 90);
	CHECK(tracker.Mode() == HINGE_MODE_LAPTOP, "%s",
		mode_name(tracker.Mode()));

	// what a bump makes of it is not gravity
	for (int i = 0; i < 50; i++) {
		accel_vector display = { 0, -1500 - i * 10, 900 };
		accel_vector base = { 0, 200, -400 + i };
		CHECK(!tracker.Update(display, base), "changed at %d", i);
	}
	CHECK(tracker.Mode() == HINGE_MODE_LAPTOP, "%s",
		mode_name(tracker.Mode()));

	// one wrong pair in between changes nothing either
	for (int i = 0; i < 40; i++) {
		accel_vector display, base;
		pose(i % 4 == 0 ? 300 : 100, 90, 0, display, base);
		CHECK(!tracker.Update(display, base), "changed at %d", i);
	}
	printf("shaking: done\n");
}


//!	The edges: flat on the table stays a laptop with sensors that are off.
static void
test_flat()
{
	HingeTracker tracker;
	for (int i = 0; i < 2000; i++) {
		accel_vector display, base;
		pose(180, 90, 0, display, base, 25);
		display.y += 70;
		display.z -= 60;
		base.y -= 70;
		base.z += 60;
		tracker.Update(display, base);
		CHECK(tracker.Mode() == HINGE_MODE_LAPTOP, "%s at %d, %d degrees",
			mode_name(tracker.Mode()), i, (int)tracker.Angle());
		if (tracker.Mode() != HINGE_MODE_LAPTOP)
			break;
	}
	printf("flat: %d degrees with sensors 70 mg off\n", (int)tracker.Angle());
}


static void
test_forget_angle()
{
	HingeTracker tracker;
	tracker.SetLidHook(lid_hook, NULL);
	sLid = HingeTracker::kLidClosed;

	settle(tracker, 360, -90);
		// (the lid switch of a tablet says closed, wrongly, so it starts as
		// a closed laptop)
	settle(tracker, 100, 90);
	settle(tracker, 360, -90, 0, 0, 3);
		// jumped: the hinge can't have gone there, it must be closed
	CHECK(tracker.Angle() <= 5, "%d", (int)tracker.Angle());

	// asleep, the lid was opened and the screen folded back
	sLid = HingeTracker::kLidOpen;
	tracker.ForgetAngle();
	CHECK(tracker.Mode() == HINGE_MODE_LAPTOP, "%s",
		mode_name(tracker.Mode()));
	settle(tracker, 360, -90);
	CHECK(tracker.Mode() == HINGE_MODE_TABLET, "%s",
		mode_name(tracker.Mode()));
	CHECK(tracker.Angle() >= 355, "%d", (int)tracker.Angle());

	tracker.Reset();
	CHECK(tracker.Mode() == HINGE_MODE_UNKNOWN, "%s",
		mode_name(tracker.Mode()));
	CHECK(tracker.Angle() == -1, "%d", (int)tracker.Angle());
	printf("forgetting: done\n");
}


int
main()
{
	srand(1);
	test_atan2();
	test_measured();
	test_fold(0);
	test_fold(30);
	test_stand_and_tent();
	test_ambiguous_start();
	test_vertical_hinge();
	test_shaking();
	test_flat();
	test_forget_angle();

	if (sFailures > 0) {
		printf("%d checks FAILED\n", sFailures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
