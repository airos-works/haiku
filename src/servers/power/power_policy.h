/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _POWER_POLICY_H
#define _POWER_POLICY_H


#include <Locker.h>
#include <SupportDefs.h>


class BServer;


/*!	What the power daemon does when the power source changes or the lid is
	closed, from the settings file the Power preferences write
	(~/config/settings/system/power).
*/
class PowerPolicy {
public:
								PowerPolicy(BServer* server);

			void				LoadSettings();

			void				SetOnAC(bool onAC);
			void				SetLidOpen(bool open);

			bool				JustWokeUp();
				// a power button press right after sleeping woke the machine

	static	const char*			SettingsPath();

private:
			enum lid_action {
				kLidNothing,
				kLidDisplayOff,
				kLidSleep
			};

			void				_ApplyProfile();
			bool				_InitGUI();
			bool				_Sleep();
			bool				_WaitForLidOpen(bigtime_t timeout);

			BServer*			fServer;
			BLocker				fLock;

			bool				fOnAC;
			bool				fKnowSource;
			bool				fLidClosed;
			bool				fKnowLid;

			bool				fPowerSavingOnAC;
			bool				fPowerSavingOnBattery;
			int32				fBatteryBrightness;
				// percent, at most this on battery; 0 leaves it alone
			lid_action			fLidAction;

			float				fACBrightness;
				// what it was on AC, restored when plugged in again
			bigtime_t			fWokeUp;
};


#endif // _POWER_POLICY_H
