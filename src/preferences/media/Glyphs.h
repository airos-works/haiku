/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef GLYPHS_H
#define GLYPHS_H


#include <GraphicsDefs.h>
#include <Rect.h>


class BView;


enum glyph_kind {
	GLYPH_SPEAKER,
	GLYPH_MICROPHONE,
	GLYPH_MIXER,
	GLYPH_MIDI,
	GLYPH_CAMERA,
	GLYPH_DISPLAY,
	GLYPH_WARNING
};


//! The standard badge color of a category page.
rgb_color glyph_color(glyph_kind kind);

//! The color the badges of single devices use.
rgb_color device_glyph_color();

/*!	Draws a rounded, softly shaded badge of \a color into \a frame with the
	white symbol \a kind on it.
*/
void draw_glyph_badge(BView* view, BRect frame, glyph_kind kind,
	rgb_color color);


#endif	// GLYPHS_H
