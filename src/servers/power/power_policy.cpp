/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "power_policy.h"

#include <algorithm>
#include <fcntl.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <net/if.h>
#include <net/if_media.h>

#include <Autolock.h>
#include <driver_settings.h>
#include <FindDirectory.h>
#include <NetworkInterface.h>
#include <NetworkRoster.h>
#include <Path.h>
#include <scheduler.h>
#include <Screen.h>
#include <Server.h>

#if defined(__i386__) || defined(__x86_64__)
#	include <arch/x86/arch_suspend.h>

extern "C" status_t _kern_generic_syscall(const char* subsystem,
	uint32 function, void* buffer, size_t bufferSize);
#endif


static const char* kSettingsName = "system/power";


PowerPolicy::PowerPolicy(BServer* server)
	:
	fServer(server),
	fLock("power policy"),
	fOnAC(true),
	fKnowSource(false),
	fLidClosed(false),
	fKnowLid(false),
	fPowerSavingOnAC(false),
	fPowerSavingOnBattery(true),
	fBatteryBrightness(70),
	fLidAction(kLidSleep),
	fSleepCheck(0),
	fDeepSleepMinutes(30),
	fACBrightness(-1),
	fWokeUp(0)
{
	LoadSettings();
}


/*static*/ const char*
PowerPolicy::SettingsPath()
{
	static char path[B_PATH_NAME_LENGTH];
	BPath settings;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &settings) != B_OK)
		return NULL;
	settings.Append(kSettingsName);
	strlcpy(path, settings.Path(), sizeof(path));
	return path;
}


/*!	The settings file has lines like "power_saving_on_battery true",
	"battery_brightness 70", "lid_closed sleep" (or "display_off",
	"nothing"), "deep_sleep_minutes 30" and "sleep_check_seconds 0". A
	changed file applies to the current power source at once.
*/
void
PowerPolicy::LoadSettings()
{
	BAutolock _(fLock);

	const char* path = SettingsPath();
	void* handle = path != NULL ? load_driver_settings(path) : NULL;
	if (handle != NULL) {
		fPowerSavingOnAC = get_driver_boolean_parameter(handle,
			"power_saving_on_ac", false, true);
		fPowerSavingOnBattery = get_driver_boolean_parameter(handle,
			"power_saving_on_battery", true, true);

		const char* value = get_driver_parameter(handle, "battery_brightness",
			NULL, NULL);
		if (value != NULL)
			fBatteryBrightness = atoi(value);

		value = get_driver_parameter(handle, "lid_closed", NULL, NULL);
		if (value != NULL) {
			if (strcmp(value, "nothing") == 0)
				fLidAction = kLidNothing;
			else if (strcmp(value, "display_off") == 0)
				fLidAction = kLidDisplayOff;
			else
				fLidAction = kLidSleep;
		}

		value = get_driver_parameter(handle, "sleep_check_seconds", NULL,
			NULL);
		if (value != NULL)
			fSleepCheck = atoi(value);

		value = get_driver_parameter(handle, "deep_sleep_minutes", NULL,
			NULL);
		if (value != NULL)
			fDeepSleepMinutes = atoi(value);
		unload_driver_settings(handle);
	}

	if (fKnowSource)
		_ApplyProfile();
}


void
PowerPolicy::SetOnAC(bool onAC)
{
	BAutolock _(fLock);

	bool changed = !fKnowSource || onAC != fOnAC;
	bool firstTime = !fKnowSource;
	fOnAC = onAC;
	fKnowSource = true;
	if (!changed)
		return;

	printf("power_daemon: on %s\n", onAC ? "AC" : "battery");
	_ApplyProfile();

	// The display is dimmed on battery, and gets its brightness back when
	// the machine is plugged in again. (Not at start-up: app_server is not
	// there yet, and the brightness is what the user left it at.)
	if (firstTime || fBatteryBrightness <= 0 || !_InitGUI())
		return;

	BScreen screen;
	float brightness;
	if (screen.GetBrightness(&brightness) != B_OK)
		return;

	if (!onAC) {
		fACBrightness = brightness;
		float limit = fBatteryBrightness / 100.f;
		if (brightness > limit)
			screen.SetBrightness(limit);
	} else if (fACBrightness >= 0) {
		screen.SetBrightness(fACBrightness);
		fACBrightness = -1;
	}
}


void
PowerPolicy::SetLidOpen(bool open)
{
	lid_action action;
	{
		BAutolock _(fLock);
		bool firstTime = !fKnowLid;
		fKnowLid = true;
		if (open == !fLidClosed)
			return;
		fLidClosed = !open;
		action = fLidAction;

		// Only closing it does something: a machine that starts with its
		// lid closed (on an external display) stays awake.
		if (firstTime) {
			printf("power_daemon: lid closed at start\n");
			return;
		}
	}
	printf("power_daemon: lid %s\n", open ? "opened" : "closed");

	if (action == kLidNothing || !_InitGUI())
		return;

	BScreen screen;
	if (open) {
		screen.SetDPMS(B_DPMS_ON);
		return;
	}

	screen.SetDPMS(B_DPMS_OFF);
	if (action != kLidSleep)
		return;

	// Wired network interfaces sleep down: their drivers can power the
	// PHY off then (no link, no wake-on-LAN, no link lights), and the
	// routes go with them.
	InterfaceList interfaces;
	_TakeDownWiredInterfaces(interfaces);

	// After a while with the lid closed, the sleep gets deeper. (In real
	// time: the system time may stand still in the deepest sleep.)
	bigtime_t deepAt = 0;
	{
		BAutolock _(fLock);
		if (fDeepSleepMinutes > 0) {
			deepAt = real_time_clock_usecs()
				+ fDeepSleepMinutes * 60000000LL;
		}
	}

	// A power button press wakes the machine, too: sleep again until the
	// lid is opened.
	bool deep = false;
	while (true) {
		bigtime_t timeout = 0;
		if (!deep && deepAt != 0)
			timeout = std::max(deepAt - real_time_clock_usecs(),
				(bigtime_t)1000);

		sleep_result result = _Sleep(deep, timeout);
		if (result == kSleepFailed)
			break;
		if (result == kSleepTimedOut && !deep) {
			// (the lid may have been opened just now)
			if (!_WaitForLidOpen(0)) {
				printf("power_daemon: lid closed for %" B_PRId32 " minutes, "
					"sleeping deeper\n", fDeepSleepMinutes);
				deep = true;
				continue;
			}
		}

		if (_WaitForLidOpen(3000000)) {
			{
				BAutolock _(fLock);
				fLidClosed = false;
			}
			screen.SetDPMS(B_DPMS_ON);
			printf("power_daemon: lid opened\n");
			break;
		}
	}

	_BringUpInterfaces(interfaces);
}


bool
PowerPolicy::JustWokeUp()
{
	BAutolock _(fLock);
	return fWokeUp != 0 && system_time() - fWokeUp < 3000000;
}


void
PowerPolicy::_ApplyProfile()
{
	bool powerSaving = fOnAC ? fPowerSavingOnAC : fPowerSavingOnBattery;
	int32 mode = powerSaving
		? SCHEDULER_MODE_POWER_SAVING : SCHEDULER_MODE_LOW_LATENCY;
	if (get_scheduler_mode() != mode)
		set_scheduler_mode(mode);
}


bool
PowerPolicy::_InitGUI()
{
	return fServer->InitGUIContext() == B_OK;
}


/*!	Puts the machine to sleep until something wakes it (suspend to idle),
	where the platform supports it, or until \a timeout passed (0 never).
	A \a deep sleep takes the devices along that take longer to come back.
*/
PowerPolicy::sleep_result
PowerPolicy::_Sleep(bool deep, bigtime_t timeout)
{
#if defined(__i386__) || defined(__x86_64__)
	x86_suspend_s2idle_args args = {};
	// The devices with suspend hooks (USB, NVMe) go to sleep, too. The power
	// hooks of the legacy drivers (Wi-Fi) only run for a deep sleep: those
	// drivers take seconds to come back.
	if (!deep)
		args.flags = X86_SUSPEND_SKIP_POWER_HOOKS;
	{
		BAutolock _(fLock);
		if (fSleepCheck > 0)
			args.timeout_ms = fSleepCheck * 1000;
	}
	bool forTimeout = false;
	if (timeout > 0 && (args.timeout_ms == 0
			|| timeout / 1000 < (bigtime_t)args.timeout_ms)) {
		args.timeout_ms = std::max(timeout / 1000, (bigtime_t)1);
		forTimeout = true;
	}
	status_t status = _kern_generic_syscall(X86_SUSPEND_SYSCALLS,
		X86_SUSPEND_ENTER_S2IDLE, &args, sizeof(args));
	if (status != B_OK) {
		printf("power_daemon: cannot sleep: %s\n", strerror(status));
		return kSleepFailed;
	}

	BAutolock _(fLock);
	fWokeUp = system_time();
	printf("power_daemon: woke up from a%s sleep after %" B_PRIu64 " s, S0i3 "
		"%s (kept out by %#" B_PRIx32 ")\n", deep ? " deep" : "",
		args.slept_us / 1000000, args.s0i3_entered ? "entered" : "not entered",
		args.s0i3_blockers);
	// (the RTC alarm or the deadline, which do not come early)
	if (args.wake_reason == X86_S2IDLE_WAKE_TIMEOUT && forTimeout)
		return kSleepTimedOut;
	return kWokeUp;
#else
	return kSleepFailed;
#endif
}


void
PowerPolicy::_TakeDownWiredInterfaces(InterfaceList& _interfaces)
{
	BNetworkRoster& roster = BNetworkRoster::Default();
	BNetworkInterface interface;
	uint32 cookie = 0;
	while (roster.GetNextInterface(&cookie, interface) == B_OK) {
		uint32 flags = interface.Flags();
		if ((flags & IFF_UP) == 0 || (flags & IFF_LOOPBACK) != 0
			|| IFM_TYPE(interface.Media()) != IFM_ETHER) {
			continue;
		}

		sleeping_interface* sleeping = new(std::nothrow) sleeping_interface;
		if (sleeping == NULL)
			break;
		sleeping->name = interface.Name();
		interface.GetDefaultGateway(AF_INET, sleeping->gateway);

		if (interface.SetFlags(flags & ~IFF_UP) != B_OK
			|| !_interfaces.AddItem(sleeping)) {
			delete sleeping;
			continue;
		}
		printf("power_daemon: %s down for the sleep\n", interface.Name());
	}
}


void
PowerPolicy::_BringUpInterfaces(const InterfaceList& interfaces)
{
	for (int32 i = 0; i < interfaces.CountItems(); i++) {
		const sleeping_interface* sleeping = interfaces.ItemAt(i);
		BNetworkInterface interface(sleeping->name);
		if (interface.SetFlags(interface.Flags() | IFF_UP) != B_OK)
			continue;

		// Taking the interface down removed its default route.
		BNetworkAddress gateway;
		if (!sleeping->gateway.IsEmpty()
			&& interface.GetDefaultGateway(AF_INET, gateway) != B_OK) {
			interface.AddDefaultRoute(sleeping->gateway);
		}
		printf("power_daemon: %s up again\n", interface.Name());
	}
}


/*!	Waits until the lid reports itself open (its notification follows the
	wake-up); a \a timeout of 0 only looks. */
bool
PowerPolicy::_WaitForLidOpen(bigtime_t timeout)
{
	int fd = open("/dev/power/acpi_lid/0", O_RDONLY);
	if (fd < 0)
		return true;

	bigtime_t end = system_time() + timeout;
	bool lidOpen = false;
	while (true) {
		uint8 status;
		if (read(fd, &status, 1) == 1 && status != 0)
			lidOpen = true;
		if (lidOpen || system_time() >= end)
			break;
		snooze(100000);
	}
	close(fd);
	return lidOpen;
}
