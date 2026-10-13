/*
 * Copyright 2005-2013, Haiku, Inc.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *		Nathan Whitehorn
 */
#ifndef _LID_MONITOR_H
#define _LID_MONITOR_H


#include "power_monitor.h"


class PowerPolicy;


class LidMonitor : public PowerMonitor {
public:
								LidMonitor(PowerPolicy* policy);
	virtual	 					~LidMonitor();

	virtual	void				HandleEvent(int fd);

	virtual	const std::set<int>&
								FDs() const { return fFDs; }
private:
			std::set<int>		fFDs;
			PowerPolicy*		fPolicy;
};


#endif // _LID_MONITOR_H
