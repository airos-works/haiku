/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef MOBILE_SHELL_PROTOCOL_H
#define MOBILE_SHELL_PROTOCOL_H

/*	Between the mobile shell and its two input_server add-ons.

	The add-ons are in every image and loaded by every input_server, so they
	do nothing on their own: each owns a port and waits. Only a running shell
	writes to the ports, and what it asked for ends with it.

	- mobile_keyboard (a device add-on, though it registers no device)
	  types the keys the shell's on-screen keyboard writes to its port, as
	  if a real keyboard had. The shell first says hello with a messenger;
	  from then on the add-on tells it when a text field gets or loses the
	  focus, and takes keys from that team only.
	- mobile_touch (a filter add-on) turns a finger held still into a click
	  of the secondary button, inside the rectangle the shell gives it (where
	  the applications' windows are) and nowhere else.
*/

#include <OS.h>
#include <SupportDefs.h>


#define MOBILE_SHELL_SIGNATURE	"application/x-vnd.airOS-MobileShell"
#define MOBILE_KEYBOARD_PORT	"mobile shell keyboard"
#define MOBILE_TOUCH_PORT		"mobile shell touch"


// message codes of the ports
enum {
	// to the keyboard port
	kMobileKey			= 'mkey',	// a mobile_key
	kMobileHello		= 'mhlo',	// a flattened BMessage, "shell": the
									// messenger to tell about the focus
	kMobileBye			= 'mbye',	// no data

	// to the touch port
	kMobileHoldArea		= 'mhld'	// a mobile_hold_area
};

// from the keyboard add-on to the shell's messenger
enum {
	kMsgTextFocus		= 'txtF'	// "text": bool, a text field has the
									// focus; "team": int32, whose it is
};


struct mobile_key {
	int32	key;		// key code, 0 when the character has none
	uint32	modifiers;	// held for this key only
	char	bytes[8];	// UTF-8, NUL-terminated
	char	raw;		// the character without the modifiers: 'c' for
						// Control-C, whose byte is 3
};


struct mobile_hold_area {
	int32	enabled;	// 0: no long presses anywhere
	float	left;		// screen coordinates
	float	top;
	float	right;
	float	bottom;
};


#endif	// MOBILE_SHELL_PROTOCOL_H
