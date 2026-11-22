/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef THEME_H
#define THEME_H

/*	The mobile shell's colours, sizes and messages, shared by every part.

	Sizes are written in units that were pixels on a 720 x 1440 phone panel
	and are scaled to the screen here: with the system's plain font (12 points
	is the system at 100%), times 0.8, which makes a navigation button about a
	centimetre on a laptop panel of 135 dots per inch. S() is a length in
	pixels, F() a font size.

	The look is dark, in the air/OS colours (docs/airos/BRANDING.md): slate
	panels, the breeze teal as the accent, cloud for text.
*/

#include "Protocol.h"

#include <GraphicsDefs.h>
#include <Rect.h>

#include <math.h>


namespace Theme {

float Scale();

inline float S(float units) { return roundf(units * Scale()); }
inline float F(float size) { return size * Scale(); }

// Panels
inline float StatusBarHeight() { return S(44); }
inline float NavBarHeight() { return S(64); }

// The keyboard: a strip of suggestions, four rows of keys, and for the
// Terminal layout a row of extra keys above them.
inline float KeyGap() { return S(8); }
inline float KeyRowHeight() { return S(72); }
inline float SuggestionHeight() { return S(48); }
inline float KeyboardExtrasHeight() { return S(64); }
inline float KeyboardHeight()
	{ return KeyGap() + SuggestionHeight() + 4 * (KeyRowHeight() + KeyGap()); }

// Home grid
inline float IconSize() { return S(88); }
inline float CellHeight() { return S(168); }
// how many columns of applications fit a screen this wide
int32 GridColumns(float width);

inline float Corner() { return S(18); }

// slate, darkened for the panels
const rgb_color kPanel = {24, 30, 34, 255};
const rgb_color kSurface = {47, 58, 64, 255};
const rgb_color kSurfaceRaised = {64, 78, 86, 255};
// breeze
const rgb_color kAccent = {78, 141, 138, 255};
// cloud
const rgb_color kText = {246, 243, 234, 255};
const rgb_color kTextDim = {164, 174, 176, 255};
const rgb_color kDisabled = {92, 104, 110, 255};
const rgb_color kScrim = {0, 0, 0, 190};
// dawn, for what needs attention
const rgb_color kWarning = {227, 162, 91, 255};

}	// namespace Theme


// Messages between the shell's windows and the application.
enum {
	kMsgGoHome = 'goHm',
	kMsgShowTasks = 'tskS',
	kMsgCloseActive = 'clsA',
	kMsgToggleKeyboard = 'kbdT',
	kMsgShowDrawer = 'drwS',
	kMsgLaunch = 'lnch',			// "ref": entry_ref
	kMsgActivateWindow = 'actW',	// "token": int32 server token
	kMsgCloseWindow = 'clsW',		// "token": int32 server token
	kMsgTakeScreenshot = 'shot',
	kMsgShowSettings = 'sett',
	kMsgKeyboardResized = 'kbdR',
	kMsgOverlayChanged = 'ovlC',	// an overlay opened or closed
	kMsgScreenChanged = 'scrC',
	kMsgPulse = 'puls',
	kMsgAnimate = 'anim'
};


#endif	// THEME_H
