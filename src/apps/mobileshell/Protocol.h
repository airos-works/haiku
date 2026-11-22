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
	- mobile_touch (a filter add-on) makes a touch screen work as one does
	  on a phone, inside the rectangle the shell gives it (where the
	  applications' windows are) and nowhere else: a finger that moves
	  scrolls, and glides on when it is flicked; a finger held still selects
	  the word under it (and dragging on then selects more), or clicks the
	  secondary button where the shell asks for that instead. The filter
	  tells the shell when a touch begins and when a selection is made, so
	  that it can offer Cut, Copy and Paste.

	  tablet_server gives the filter the whole screen while the machine is
	  folded and the shell is not the one to run: scrolling then works on
	  the desktop too, and a long press is the secondary click.
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
	kMobileHoldArea		= 'mhld',	// a mobile_hold_area
	kMobileMenuAt		= 'mmnu',	// float x, y (screen): click the
									// secondary button there (from whoever
									// set the area)
	kMobileWake			= 'mwak'	// no data; the filter to its own timer
};

// from the keyboard add-on to the shell's messenger
enum {
	kMsgTextFocus		= 'txtF'	// "text": bool, a text field has the
									// focus; "team": int32, whose it is
};

// from the touch filter to the application that set the area
enum {
	kMsgTouchBegan		= 'tchB',	// "where": BPoint, a finger came down
	kMsgTouchSelected	= 'tchS'	// "where": BPoint, where the finger
									// lifted after selecting; "from":
									// BPoint, where it was held
};


struct mobile_key {
	int32	key;		// key code, 0 when the character has none
	uint32	modifiers;	// held for this key only
	char	bytes[8];	// UTF-8, NUL-terminated
	char	raw;		// the character without the modifiers: 'c' for
						// Control-C, whose byte is 3
};


// what a finger does in the area
enum {
	kTouchScroll		= 0x01,	// moving scrolls instead of dragging
	kTouchSelect		= 0x02	// held still, it selects text (a double
								// click held down) instead of clicking the
								// secondary button
};


struct mobile_hold_area {
	int32	enabled;	// 0: nothing anywhere
	float	left;		// screen coordinates
	float	top;
	float	right;
	float	bottom;
	uint32	flags;		// kTouch*
};


#endif	// MOBILE_SHELL_PROTOCOL_H
