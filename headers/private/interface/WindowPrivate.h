/*
 * Copyright 2005-2008, Jérôme Duval, jerome.duval@free.fr.
 * Distributed under the terms of the MIT License.
 */
#ifndef _WINDOW_PRIVATE_H
#define _WINDOW_PRIVATE_H


#include <Window.h>


/* Private window looks */

const window_look kDesktopWindowLook = window_look(4);
const window_look kLeftTitledWindowLook = window_look(25);

/* Private window feels */

const window_feel kDesktopWindowFeel = window_feel(1024);
const window_feel kMenuWindowFeel = window_feel(1025);
const window_feel kWindowScreenFeel = window_feel(1026);
const window_feel kPasswordWindowFeel = window_feel(1027);
const window_feel kOffscreenWindowFeel = window_feel(1028);

/* Private window types */

const window_type kWindowScreenWindow = window_type(1026);

/* Private window actions (do_window_action()): another application, such
   as a browser sharing the window in a video call, shows that it is reading
   the window's contents. The window's tab is drawn in the capture color
   until as many stops as starts arrive, or the application quits. */

const int32 kWindowActionCaptureStart = 'wcap';
const int32 kWindowActionCaptureStop = 'wcp0';

/* Private window flags */

const uint32 kWindowScreenFlag = 0x10000;
const uint32 kAcceptKeyboardFocusFlag = 0x40000;
	// Accept keyboard input even if B_AVOID_FOCUS is set

#endif // _WINDOW_PRIVATE_H
