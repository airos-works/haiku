/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef DESKTOP_H
#define DESKTOP_H


#include <SupportDefs.h>


// Deskbar and Tracker, closed for an update as a shutdown closes them, so
// that the screen shows only the install window over the desktop color.
// launch_daemon starts both again when they quit; their jobs are disabled
// while they are closed.
namespace Desktop {

status_t	Close();
				// blocks until both have quit, or a while has passed
void		Restore();
				// starts them again (an update that did not go ahead)

}	// namespace Desktop


#endif	// DESKTOP_H
