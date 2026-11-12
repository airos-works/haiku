/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	touchpad_scroll_test: feeds a two finger scroll gesture to the mouse
	add-on's TouchpadMovement and checks the precise deltas and phases it
	reports next to the whole wheel steps.
*/


#include <stdio.h>
#include <math.h>
#include <string.h>

#include "movement_maker.h"


static const char*
phase_name(int32 phase)
{
	switch (phase) {
		case B_WHEEL_PHASE_NONE: return "none";
		case B_WHEEL_PHASE_BEGAN: return "began";
		case B_WHEEL_PHASE_CHANGED: return "changed";
		case B_WHEEL_PHASE_ENDED: return "ended";
	}
	return "?";
}


static int
run_gesture(TouchpadMovement& maker, int32 stepX, int32 stepY, int events)
{
	touchpad_movement event;
	memset(&event, 0, sizeof(event));
	event.xPosition = 3400;
	event.yPosition = 2900;
	event.zPressure = 50;
	event.fingers = 0x03;
	event.fingerWidth = 0;

	float preciseX = 0, preciseY = 0;
	int32 wholeX = 0, wholeY = 0;
	int began = 0, changed = 0, ended = 0, wheelMessages = 0, preciseMessages = 0;
	int failures = 0;
	int32 lastPhase = B_WHEEL_PHASE_NONE;
	bool scrolling = false;

	for (int i = 0; i <= events + 2; i++) {
		if (i == events + 1) {
			// the fingers leave the touchpad
			event.zPressure = 0;
			event.fingers = 0;
			event.xPosition = event.yPosition = 0;
		} else if (i > 0) {
			event.xPosition += stepX;
			event.yPosition += stepY;
		}

		mouse_movement movement;
		scroll_movement scroll;
		bigtime_t timeout;
		if (maker.EventToMovement(&event, &movement, timeout, &scroll)
				!= B_OK) {
			printf("EventToMovement failed\n");
			return 1;
		}

		switch (scroll.phase) {
			case B_WHEEL_PHASE_BEGAN:
				began++;
				if (scrolling)
					failures++;
				scrolling = true;
				break;
			case B_WHEEL_PHASE_CHANGED:
				changed++;
				if (!scrolling || (scroll.x == 0 && scroll.y == 0))
					failures++;
				break;
			case B_WHEEL_PHASE_ENDED:
				ended++;
				if (!scrolling || scroll.x != 0 || scroll.y != 0)
					failures++;
				scrolling = false;
				break;
		}
		if (i == events + 2 && scroll.phase != B_WHEEL_PHASE_NONE)
			failures++;
		lastPhase = scroll.phase;

		preciseX += scroll.x;
		preciseY += scroll.y;
		wholeX += movement.wheel_xdelta;
		wholeY += movement.wheel_ydelta;
		if (movement.wheel_xdelta != 0 || movement.wheel_ydelta != 0)
			wheelMessages++;
		if (scroll.x != 0 || scroll.y != 0)
			preciseMessages++;
	}

	printf("move %d,%d x %d: phases began %d changed %d ended %d (%s last); "
		"whole steps %d,%d in %d events; precise %.3f,%.3f in %d events\n",
		stepX, stepY, events, began, changed, ended, phase_name(lastPhase),
		wholeX, wholeY, wheelMessages, preciseX, preciseY, preciseMessages);

	// "changed" only comes with movement, and then always
	if (began != 1 || ended != 1 || changed == 0
		|| changed < preciseMessages - 1)
		failures++;
	// the precise deltas add up to the whole steps, plus less than a step
	if (fabsf(preciseX - wholeX) >= 1 || fabsf(preciseY - wholeY) >= 1)
		failures++;
	// and arrive more often than whole steps
	if (preciseMessages <= wheelMessages)
		failures++;
	return failures;
}


/*!	Feeds a gesture whose finger moves by \a pattern (touchpad units, one
	entry a report, repeated) for \a events reports and returns the precise
	vertical scrolling it gave, in wheel steps, and how many "changed"
	messages carried it.
*/
static float
run_pattern(TouchpadMovement& maker, const int32* pattern, int patternLength,
	int events, int& changedMessages)
{
	touchpad_movement event;
	memset(&event, 0, sizeof(event));
	event.xPosition = 600;
	event.yPosition = 300;
	event.zPressure = 50;
	event.fingers = 0x03;
	event.fingerWidth = 0;

	float precise = 0;
	changedMessages = 0;
	for (int i = 0; i <= events + 1; i++) {
		if (i == events + 1) {
			event.zPressure = 0;
			event.fingers = 0;
			event.xPosition = event.yPosition = 0;
		} else if (i > 0)
			event.yPosition += pattern[(i - 1) % patternLength];

		mouse_movement movement;
		scroll_movement scroll;
		bigtime_t timeout;
		maker.EventToMovement(&event, &movement, timeout, &scroll);
		if (scroll.phase == B_WHEEL_PHASE_CHANGED)
			changedMessages++;
		precise += scroll.y;
	}
	return precise;
}


/*!	A touchpad like the Framework 13's (an I2C HID one of 1337 x 876 units)
	with its owner's settings: slow fingers scroll, steadily and as far as
	they move, resting ones do not.
*/
static int
run_slow_and_resting()
{
	touchpad_specs specs;
	memset(&specs, 0, sizeof(specs));
	specs.areaStartX = 0;
	specs.areaEndX = 1337;
	specs.areaStartY = 0;
	specs.areaEndY = 876;
	specs.minPressure = 0;
	specs.realMaxPressure = 50;
	specs.maxPressure = 255;
	specs.edgeMotionWidth = 26;

	touchpad_settings settings = kDefaultTouchpadSettings;
	settings.scroll_ystepsize = 10;
	settings.scroll_acceleration = 10;
	settings.scroll_twofinger_natural_scrolling = true;

	int failures = 0;
	int changed;

	// half a unit a report: a still report every other one, which made the
	// whole-unit movement start over, so this scrolled nothing at all before
	static const int32 slow[] = { 1, 0 };
	TouchpadMovement maker;
	maker.SetSpecs(specs);
	maker.SetSettings(settings);
	float slowSteps = run_pattern(maker, slow, 2, 120, changed);
	printf("slow, half a unit a report for 120 reports: %.3f steps in %d "
		"messages\n", slowSteps, changed);
	if (fabsf(slowSteps) < 0.9 || changed < 40)
		failures++;

	// a resting finger flickering by a unit
	static const int32 resting[] = { 1, -1, 0, -1, 1, 0 };
	TouchpadMovement restingMaker;
	restingMaker.SetSpecs(specs);
	restingMaker.SetSettings(settings);
	float restingSteps = run_pattern(restingMaker, resting, 6, 120, changed);
	printf("resting, flickering by a unit: %.3f steps in %d messages\n",
		restingSteps, changed);
	if (restingSteps != 0 || changed != 0)
		failures++;

	// as far as they move: 60 units slowly and 60 units at a steady pace
	// (no acceleration, to compare distances) scroll about the same
	settings.scroll_acceleration = 0;
	static const int32 steady[] = { 2 };
	TouchpadMovement slowMaker, steadyMaker;
	slowMaker.SetSpecs(specs);
	slowMaker.SetSettings(settings);
	steadyMaker.SetSpecs(specs);
	steadyMaker.SetSettings(settings);
	float slow60 = run_pattern(slowMaker, slow, 2, 120, changed);
	float steady60 = run_pattern(steadyMaker, steady, 1, 30, changed);
	printf("60 units slowly %.3f steps, steadily %.3f steps\n", slow60,
		steady60);
	if (fabsf(slow60) < 0.9 * fabsf(steady60)
		|| fabsf(slow60) > 1.1 * fabsf(steady60))
		failures++;

	// natural scrolling: moving the fingers down scrolls up
	if (slowSteps <= 0)
		failures++;
	return failures;
}


int
main()
{
	touchpad_specs specs;
	memset(&specs, 0, sizeof(specs));
	specs.areaStartX = 1472;
	specs.areaEndX = 5472;
	specs.areaStartY = 1408;
	specs.areaEndY = 4448;
	specs.minPressure = 30;
	specs.realMaxPressure = 80;
	specs.maxPressure = 110;
	specs.edgeMotionWidth = 50;

	int failures = 0;
	for (int natural = 0; natural < 2; natural++) {
		touchpad_settings settings = kDefaultTouchpadSettings;
		settings.scroll_twofinger_natural_scrolling = natural != 0;
		TouchpadMovement maker;
		maker.SetSpecs(specs);
		maker.SetSettings(settings);
		failures += run_gesture(maker, 0, 7, 60);
		failures += run_gesture(maker, 0, -23, 40);
		failures += run_gesture(maker, 13, 0, 40);
	}

	failures += run_slow_and_resting();

	printf("%s\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
