/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "HingeTracker.h"

#include <accelerometer.h>


// All angles are tenths of a degree.

// The laptop becomes something else when the screen is folded further than
// this, and a laptop again below the second one. A screen laid flat (180
// degrees) has to stay a laptop, and uncalibrated sensors are some degrees
// off.
static const int32 kFoldedAbove = 2000;
static const int32 kLaptopBelow = 1880;

// folded all the way: a tablet
static const int32 kTabletAbove = 3400;
static const int32 kNoTabletBelow = 3250;

// This close to 0 or 360 degrees the two halves are parallel either way.
static const int32 kAmbiguous = 300;

// What is measured has to be gravity: a machine that is carried around or
// shaken measures something else.
static const int32 kMinGravity = 700;
static const int32 kMaxGravity = 1300;

// The hinge angle shows in the part of gravity that is perpendicular to the
// hinge. With the hinge close to vertical little is left of it.
static const int32 kMinPerpendicular = 350;

// between stand and tent the screen is on its side: neither
static const int32 kMinUpright = 300;

// a mode has to be measured this often in a row
static const int32 kStableUpdates = 3;

// after this many updates without an angle, the hinge may be anywhere
static const int32 kForgetAfter = 8;


static inline int64
square(int32 value)
{
	return (int64)value * value;
}


static bool
is_gravity(const accel_vector& vector)
{
	int64 length = square(vector.x) + square(vector.y) + square(vector.z);
	return length >= square(kMinGravity) && length <= square(kMaxGravity);
}


static void
low_pass(accel_vector& filtered, const accel_vector& sample)
{
	filtered.x += (sample.x * 16 - filtered.x) / 2;
	filtered.y += (sample.y * 16 - filtered.y) / 2;
	filtered.z += (sample.z * 16 - filtered.z) / 2;
}


HingeTracker::HingeTracker()
	:
	fLidHook(NULL),
	fLidCookie(NULL)
{
	Reset();
}


void
HingeTracker::SetLidHook(lid_hook hook, void* cookie)
{
	fLidHook = hook;
	fLidCookie = cookie;
}


void
HingeTracker::Reset()
{
	fFiltered = false;
	fAngle = -1;
	fAngleValid = false;
	fUnusableCount = 0;
	fMode = HINGE_MODE_UNKNOWN;
	fPendingMode = HINGE_MODE_UNKNOWN;
	fPendingCount = 0;
}


void
HingeTracker::ForgetAngle()
{
	fFiltered = false;
	fAngle = -1;
	fAngleValid = false;
	fUnusableCount = 0;
	fPendingCount = 0;
}


int32
HingeTracker::Angle() const
{
	if (fAngle < 0)
		return -1;
	return (fAngle + 5) / 10;
}


/*!	atan2() in tenths of a degree, from 0 to 3599, without floating point:
	atan(t) = 45 t + 15.64 t (1 - t) degrees for t from 0 to 1, which is never
	more than a quarter of a degree off.
*/
/*static*/ int32
HingeTracker::Atan2(int32 y, int32 x)
{
	int64 absX = x < 0 ? -(int64)x : x;
	int64 absY = y < 0 ? -(int64)y : y;
	if (absX == 0 && absY == 0)
		return 0;

	// the angle within the first octant, in hundredths of a degree
	int64 small = absX < absY ? absX : absY;
	int64 large = absX < absY ? absY : absX;
	int64 t = (small << 15) / large;
		// 0 to 32768
	int64 angle = (4500 * t + ((1564 * t * (32768 - t)) >> 15)) >> 15;

	if (absY > absX)
		angle = 9000 - angle;
	if (x < 0)
		angle = 18000 - angle;
	if (y < 0)
		angle = 36000 - angle;

	int32 tenths = (int32)((angle + 5) / 10);
	return tenths >= 3600 ? tenths - 3600 : tenths;
}


bool
HingeTracker::Update(const accel_vector& display, const accel_vector& base)
{
	if (!is_gravity(display) || !is_gravity(base))
		return _Unusable();

	if (!fFiltered) {
		fDisplay.x = display.x * 16;
		fDisplay.y = display.y * 16;
		fDisplay.z = display.z * 16;
		fBase.x = base.x * 16;
		fBase.y = base.y * 16;
		fBase.z = base.z * 16;
		fFiltered = true;
	} else {
		low_pass(fDisplay, display);
		low_pass(fBase, base);
	}

	// The hinge is the x axis of both halves, so the angle is the one between
	// what is left of the two vectors without their x.
	int64 minimum = square(kMinPerpendicular * 16);
	if (square(fDisplay.y) + square(fDisplay.z) < minimum
		|| square(fBase.y) + square(fBase.z) < minimum) {
		return _Unusable();
	}

	// With the screen laid flat (180 degrees) both halves measure the same.
	int32 measured = 1800 + Atan2(fDisplay.z, fDisplay.y)
		- Atan2(fBase.z, fBase.y);
	if (measured < 0)
		measured += 3600;
	else if (measured >= 3600)
		measured -= 3600;

	if (measured < kAmbiguous || measured > 3600 - kAmbiguous) {
		// Closed, or folded all the way back? The two halves are parallel
		// either way. The hinge does not jump, so it is where it was closer
		// to before. At the start the lid switch knows, and without one:
		// nobody looks at a screen that faces down.
		bool closed;
		if (fAngle >= 0)
			closed = fAngle < 1800;
		else {
			lid_state lid = fLidHook != NULL
				? fLidHook(fLidCookie) : kLidUnknown;
			closed = lid == kLidUnknown ? fDisplay.z <= 0 : lid == kLidClosed;
		}
		if (closed)
			fAngle = measured < 1800 ? measured : 0;
		else
			fAngle = measured > 1800 ? measured : 3600;
	} else
		fAngle = measured;
	fAngleValid = true;
	fUnusableCount = 0;

	uint32 mode = _ModeFor(fAngle);
	if (mode == fMode) {
		fPendingCount = 0;
		return false;
	}
	if (mode != fPendingMode || fPendingCount == 0) {
		fPendingMode = mode;
		fPendingCount = 1;
	} else
		fPendingCount++;

	// (the first mode is as good as measured once)
	if (fPendingCount < kStableUpdates && fMode != HINGE_MODE_UNKNOWN)
		return false;

	fMode = mode;
	fPendingCount = 0;
	return true;
}


bool
HingeTracker::_Unusable()
{
	fAngleValid = false;
	fPendingCount = 0;
	if (++fUnusableCount >= kForgetAfter) {
		// nobody knows where the hinge went meanwhile
		fAngle = -1;
		fFiltered = false;
	}
	return false;
}


uint32
HingeTracker::_ModeFor(int32 angle) const
{
	bool wasFolded = fMode >= HINGE_MODE_STAND;
	if (angle < (wasFolded ? kLaptopBelow : kFoldedAbove))
		return HINGE_MODE_LAPTOP;

	if (angle > (fMode == HINGE_MODE_TABLET ? kNoTabletBelow : kTabletAbove))
		return HINGE_MODE_TABLET;

	// The hinge is at the top of a tent, which turns the screen upside down.
	if (fDisplay.y <= -kMinUpright * 16)
		return HINGE_MODE_TENT;
	if (fDisplay.y >= kMinUpright * 16)
		return HINGE_MODE_STAND;
	return fMode == HINGE_MODE_TENT ? HINGE_MODE_TENT : HINGE_MODE_STAND;
}
