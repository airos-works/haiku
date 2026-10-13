/*
 * Copyright 2005-2013, Haiku, Inc.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *		Nathan Whitehorn
 */
#ifndef _AC_MONITOR_H
#define _AC_MONITOR_H


#include "power_monitor.h"


class PowerPolicy;


class ACMonitor : public PowerMonitor {
public:
								ACMonitor(PowerPolicy* policy);
	virtual	 					~ACMonitor();

	virtual	void				HandleEvent(int fd);

	virtual	const std::set<int>&
								FDs() const { return fFDs; }
private:
			std::set<int>		fFDs;
			PowerPolicy*		fPolicy;
};


#endif // _AC_MONITOR_H
