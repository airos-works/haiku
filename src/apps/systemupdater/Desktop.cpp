/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Desktop.h"

#include <string.h>

#include <Messenger.h>
#include <OS.h>
#include <Roster.h>

#include <LaunchRoster.h>
#include <ScreenSaverSettings.h>

#include "Updater.h"


namespace {

struct DesktopApp {
	const char*	job;
		// its launch_daemon service (data/launch/user)
	const char*	signature;
};

// in the order a shutdown closes them: the latest registered first
const DesktopApp kDesktopApps[] = {
	{ "x-vnd.Be-TSKB", "application/x-vnd.Be-TSKB" },
	{ "x-vnd.Be-TRAK", "application/x-vnd.Be-TRAK" }
};
const int32 kDesktopAppCount
	= (int32)(sizeof(kDesktopApps) / sizeof(kDesktopApps[0]));

// how long each may take to quit (Tracker saves its open windows)
const bigtime_t kQuitTimeout = 15000000;

bool sClosed = false;
port_id sScreenSaverHold = -1;
	// owned by this team: gone with it should it crash


bool
team_alive(team_id team)
{
	team_info info;
	return get_team_info(team, &info) == B_OK;
}

}	// namespace


status_t
Desktop::Close()
{
	BLaunchRoster launchRoster;
	status_t result = B_OK;
	sClosed = true;

	if (sScreenSaverHold < 0) {
		sScreenSaverHold = create_port(1, SCREEN_SAVER_HOLD_PORT_NAME);
		if (sScreenSaverHold < 0) {
			Updater::Log("cannot hold the screen saver off: %s",
				strerror(sScreenSaverHold));
		}
	}

	for (int32 i = 0; i < kDesktopAppCount; i++) {
		const DesktopApp& app = kDesktopApps[i];

		// not started again when it quits
		status_t error = launchRoster.SetEnabled(app.job, false);
		if (error != B_OK) {
			Updater::Log("cannot hold %s back in launch_daemon: %s", app.job,
				strerror(error));
		}

		BMessenger messenger(app.signature);
		team_id team = messenger.Team();
		if (!messenger.IsValid() || team < 0)
			continue;

		// as a shutdown asks it: neither refuses a plain quit request
		error = messenger.SendMessage(B_QUIT_REQUESTED);
		if (error != B_OK) {
			Updater::Log("cannot ask %s to quit: %s", app.signature,
				strerror(error));
			result = error;
			continue;
		}

		bigtime_t deadline = system_time() + kQuitTimeout;
		while (team_alive(team) && system_time() < deadline)
			snooze(50000);
		if (team_alive(team)) {
			Updater::Log("%s did not quit; installing all the same",
				app.signature);
			result = B_TIMED_OUT;
		}
	}

	Updater::Log("closed the desktop for the update");
	return result;
}


void
Desktop::Restore()
{
	if (!sClosed)
		return;
	sClosed = false;

	if (sScreenSaverHold >= 0) {
		delete_port(sScreenSaverHold);
		sScreenSaverHold = -1;
	}

	BLaunchRoster launchRoster;
	// Tracker first, as the system starts them
	for (int32 i = kDesktopAppCount - 1; i >= 0; i--) {
		const DesktopApp& app = kDesktopApps[i];
		if (be_roster->IsRunning(app.signature)) {
			launchRoster.SetEnabled(app.job, true);
			continue;
		}
		// enables the job and starts it
		status_t error = launchRoster.Start(app.job);
		if (error != B_OK) {
			Updater::Log("launch_daemon cannot start %s (%s), launching it "
				"directly", app.job, strerror(error));
			launchRoster.SetEnabled(app.job, true);
			be_roster->Launch(app.signature);
		}
	}
	Updater::Log("restored the desktop");
}
