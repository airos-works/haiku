/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef HINGE_TRACKER_H
#define HINGE_TRACKER_H


#include <SupportDefs.h>


struct accel_vector {
	int32	x;
	int32	y;
	int32	z;
		// mg, in the axes of <accelerometer.h>
};


/*!	Follows the hinge of a 2-in-1 from the accelerometers in its two halves,
	and says what the machine is used as (the HINGE_MODE_* of
	<accelerometer.h>).

	Nothing here needs the kernel (or floating point), so that the host test
	can run it: test/run.sh.
*/
class HingeTracker {
public:
			enum lid_state {
				kLidUnknown = -1,
				kLidClosed = 0,
				kLidOpen = 1
			};
			typedef lid_state (*lid_hook)(void* cookie);

								HingeTracker();

			void				SetLidHook(lid_hook hook, void* cookie);
				// asked only where the accelerometers can't tell: a closed
				// machine and one folded all the way back measure the same

			void				Reset();
				// forgets everything
			void				ForgetAngle();
				// the hinge may have moved unseen (the machine slept); the
				// mode stays until the angle is known again

			bool				Update(const accel_vector& display,
									const accel_vector& base);
				// true when Mode() changed

			uint32				Mode() const { return fMode; }
			int32				Angle() const;
				// degrees, 0 to 360, -1 unknown
			bool				IsAngleValid() const { return fAngleValid; }

	static	int32				Atan2(int32 y, int32 x);
				// tenths of a degree, 0 to 3599

private:
			bool				_Unusable();
			uint32				_ModeFor(int32 angle) const;

			lid_hook			fLidHook;
			void*				fLidCookie;

			accel_vector		fDisplay;
			accel_vector		fBase;
				// low pass filtered, 16 times the mg
			bool				fFiltered;

			int32				fAngle;
				// tenths of a degree, 0 to 3600; -1 unknown
			bool				fAngleValid;
			int32				fUnusableCount;

			uint32				fMode;
			uint32				fPendingMode;
			int32				fPendingCount;
};


#endif	// HINGE_TRACKER_H
