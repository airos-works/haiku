/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "thermal_trip_monitor.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <Directory.h>
#include <Notification.h>
#include <Roster.h>
#include <String.h>

#include <RosterPrivate.h>


static const char* kBasePath = "/dev/power/trip";

enum {
	TRIP_EVENT_HOT		= 1,
	TRIP_EVENT_CRITICAL	= 2
};


ThermalTripMonitor::ThermalTripMonitor()
	:
	fFDs(),
	fShuttingDown(false)
{
	BDirectory dir;
	if (dir.SetTo(kBasePath) != B_OK)
		return;

	entry_ref ref;
	while (dir.GetNextRef(&ref) == B_OK) {
		BString path;
		path.SetToFormat("%s/%s", kBasePath, ref.name);
		int fd = open(path.String(), O_RDONLY);
		if (fd >= 0)
			fFDs.insert(fd);
	}
}


ThermalTripMonitor::~ThermalTripMonitor()
{
	for (std::set<int>::iterator it = fFDs.begin(); it != fFDs.end(); ++it)
		close(*it);
}


static void
notify(const char* title, const char* content)
{
	BNotification notification(B_IMPORTANT_NOTIFICATION);
	notification.SetGroup("Power");
	notification.SetTitle(title);
	notification.SetContent(content);
	notification.Send(30 * 1000000LL);
}


void
ThermalTripMonitor::HandleEvent(int fd)
{
	uint8 event;
	if (read(fd, &event, 1) != 1)
		return;

	if (event == TRIP_EVENT_HOT) {
		syslog(LOG_WARNING, "power_daemon: the system is very hot\n");
		notify("The system is very hot",
			"It runs at its lowest speed until it cools down.");
	} else if (event == TRIP_EVENT_CRITICAL && !fShuttingDown) {
		fShuttingDown = true;
		syslog(LOG_CRIT, "power_daemon: critical temperature, shutting "
			"down\n");
		notify("Shutting down: overheating",
			"The system reached its critical temperature and shuts down "
			"now.");
		// a moment for the notification to show
		snooze(2000000);

		BRoster roster;
		BRoster::Private rosterPrivate(roster);
		status_t status = rosterPrivate.ShutDown(false, false, false);
		if (status != B_OK) {
			syslog(LOG_CRIT, "power_daemon: shutdown failed: %s\n",
				strerror(status));
		}
	}
}
