/*
 * Copyright 2013, Jérôme Duval, korli@users.berlios.de.
 * Copyright 2005, Nathan Whitehorn.
 *
 * Distributed under the terms of the MIT License.
 */


#include "lid_monitor.h"

#include "power_policy.h"

#include <Messenger.h>
#include <Roster.h>

#include <stdio.h>

#include <RosterPrivate.h>


LidMonitor::LidMonitor(PowerPolicy* policy)
	:
	fPolicy(policy)
{
	int fd = open("/dev/power/acpi_lid/0", O_RDONLY);
	if (fd > 0) {
		fFDs.insert(fd);
		HandleEvent(fd);
	}
}


LidMonitor::~LidMonitor()
{
	for (std::set<int>::iterator it = fFDs.begin(); it != fFDs.end(); ++it)
		close(*it);
}


void
LidMonitor::HandleEvent(int fd)
{
	uint8 status;
	if (read(fd, &status, 1) != 1)
		return;

	fPolicy->SetLidOpen(status != 0);
}
