/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _ACCELEROMETER_H
#define _ACCELEROMETER_H


#include <Drivers.h>
#include <OS.h>


/*	Accelerometers, and the hinge of a 2-in-1 laptop (a "Yoga"), for
	programs.

	/dev/sensor/accelerometer/<n> is one accelerometer. It measures the force
	that keeps the device from falling, in thousandths of the standard
	gravity (mg), along the axes of the part of the machine it sits in:

		x	to the right
		y	to the top (of the screen, or for the base: away from the hinge)
		z	out of the screen (out of the keyboard), towards the user

	A screen that lies on a table, face up, reads (0, 0, 1000); one that stands
	upright reads (0, 1000, 0). (The driver has turned the sensor's own axes
	into these with what the firmware says about how the chip is mounted.)
	ACCELEROMETER_GET_SAMPLE reads the sensor; reading the device as a file
	gives the same as a line of text, "x y z".

	A 2-in-1 has two of them, one behind the screen and one under the
	keyboard, and that is how it knows how far the screen is folded:
	/dev/sensor/hinge/0 is the angle between the two, and what that makes of
	the machine:

		laptop	up to about 190 degrees: the keyboard is in front of the screen
		stand	folded further, the screen upright: the keyboard lies face
				down, or is the foot of the screen
		tent	folded further, the hinge at the top
		tablet	folded all the way back, the keyboard behind the screen

	In all but the first the keyboard and the touchpad are out of reach, which
	is what is_tablet_mode() says. HINGE_GET_STATE returns the current state;
	select() (or wait_for_objects()) reports the device as readable whenever
	the mode changed since the descriptor last asked. Reading it as a file
	gives a line of text, "<mode> <angle>".

	The angle is only known while the hinge is not close to vertical (a
	machine held like an open book says nothing about it); the mode stays what
	it was until it is.
*/

#define ACCELEROMETER_DEVICE_DIRECTORY	"/dev/sensor/accelerometer"
#define HINGE_DEVICE_PATH				"/dev/sensor/hinge/0"

#define ACCELEROMETER_API_VERSION		1

enum {
	ACCELEROMETER_GET_INFO = B_DEVICE_OP_CODES_END + 0x4100,
		// accelerometer_info
	ACCELEROMETER_GET_SAMPLE,
		// accelerometer_sample

	HINGE_GET_STATE = B_DEVICE_OP_CODES_END + 0x4180,
		// hinge_state
	HINGE_SET_OVERRIDE
		// uint32: a mode to report instead of the measured one (to test what
		// follows the mode), HINGE_MODE_UNKNOWN to measure again
};

// accelerometer_info.location
enum {
	ACCELEROMETER_LOCATION_UNKNOWN = 0,
	ACCELEROMETER_LOCATION_DISPLAY,
	ACCELEROMETER_LOCATION_BASE
};

typedef struct accelerometer_info {
	uint32		version;		// ACCELEROMETER_API_VERSION
	uint32		location;
	char		name[32];		// the chip, "BMA255"
	int32		range;			// mg, the largest force it measures
	int32		resolution;		// µg, one step of the sensor
} accelerometer_info;

typedef struct accelerometer_sample {
	bigtime_t	time;			// system_time() when it was read
	int32		x;				// mg
	int32		y;
	int32		z;
	uint32		_reserved;
} accelerometer_sample;

// hinge_state.mode
enum {
	HINGE_MODE_UNKNOWN = 0,		// nothing measured yet
	HINGE_MODE_LAPTOP,
	HINGE_MODE_STAND,
	HINGE_MODE_TENT,
	HINGE_MODE_TABLET
};

// hinge_state.flags
enum {
	HINGE_STATE_OVERRIDDEN = 0x01,
		// the mode was set with HINGE_SET_OVERRIDE
	HINGE_STATE_ANGLE_VALID = 0x02
		// the angle was measured just now, and not carried over because the
		// hinge is too close to vertical or the machine is being moved
};

typedef struct hinge_state {
	uint32		version;		// ACCELEROMETER_API_VERSION
	uint32		mode;
	int32		angle;			// degrees, 0 (closed) to 360; -1 unknown
	uint32		flags;
	bigtime_t	changed;		// system_time() of the last mode change
} hinge_state;


static inline bool
is_tablet_mode(uint32 hingeMode)
{
	return hingeMode >= HINGE_MODE_STAND;
}


#endif	/* _ACCELEROMETER_H */
