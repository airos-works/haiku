/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef GLYPHS_H
#define GLYPHS_H

/*	Glyphs: the shell's line icons, drawn as vectors so they stay sharp at
	any scale (status indicators, navigation buttons, quick settings).
*/

#include <GraphicsDefs.h>
#include <Rect.h>


class BView;


enum Glyph {
	kGlyphWifi,
	kGlyphWifiOff,
	kGlyphWired,
	kGlyphBattery,		// level from 0 to 1 in "level"
	kGlyphPlug,
	kGlyphTasks,
	kGlyphHome,
	kGlyphClose,
	kGlyphKeyboard,
	kGlyphCamera,
	kGlyphGear,
	kGlyphChevronUp,
	kGlyphShift,
	kGlyphBackspace,
	kGlyphReturn,
	kGlyphGlobe
};


void draw_glyph(BView* view, Glyph glyph, BRect frame, rgb_color color,
	float level = 1.0f);


#endif	// GLYPHS_H
