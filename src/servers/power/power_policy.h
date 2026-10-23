/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _POWER_POLICY_H
#define _POWER_POLICY_H


#include <Locker.h>
#include <NetworkAddress.h>
#include <ObjectList.h>
#include <String.h>
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

			enum sleep_result {
				kSleepFailed,
				kWokeUp,
				kSleepTimedOut
			};

			struct sleeping_interface {
				BString			name;
				BNetworkAddress	gateway;
					// its default route, unset when it had none
			};
			typedef BObjectList<sleeping_interface, true> InterfaceList;

			void				_ApplyProfile();
			bool				_InitGUI();
			sleep_result		_Sleep(bool deep, bigtime_t timeout);
			bool				_WaitForLidOpen(bigtime_t timeout);
			void				_TakeDownWiredInterfaces(
									InterfaceList& _interfaces);
			void				_BringUpInterfaces(
									const InterfaceList& interfaces);

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
			int32				fSleepCheck;
				// seconds: a sleeping machine wakes up this often to see
				// whether its lid was opened, 0 never (the default: the kernel
				// looks at the lid whenever the firmware interrupts the sleep)
			int32				fDeepSleepMinutes;
				// with the lid closed this long, the machine sleeps deeper:
				// the devices that take longer to come back (wireless, the
				// GPU) sleep, too; 0 never

			float				fACBrightness;
				// what it was on AC, restored when plugged in again
			bigtime_t			fWokeUp;
};


#endif // _POWER_POLICY_H
