/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _THERMAL_TRIP_MONITOR_H
#define _THERMAL_TRIP_MONITOR_H


#include "power_monitor.h"


/*	The thermal drivers' trip events (/dev/power/trip/<driver>): a byte that
	is 1 when the machine got hot (the driver slowed it down) and 2 when it
	reached its critical temperature, which means a shutdown now. */
class ThermalTripMonitor : public PowerMonitor {
public:
								ThermalTripMonitor();
	virtual	 					~ThermalTripMonitor();

	virtual void				HandleEvent(int fd);

	virtual const std::set<int>&
								FDs() const { return fFDs; }
private:
			std::set<int>		fFDs;
			bool				fShuttingDown;
};


#endif // _THERMAL_TRIP_MONITOR_H
