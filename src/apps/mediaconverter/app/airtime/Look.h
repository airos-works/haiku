/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIRTIME_LOOK_H
#define AIRTIME_LOOK_H


#include <GraphicsDefs.h>
#include <Rect.h>


class BView;


namespace airtime {

/*!	The look of the window: the plain grey of Amp's toolbar, an LCD in its
	colours and round buttons, so that the two players of air/OS look alike.
	Everything is drawn here so the window, the audio panel, the Quick Look
	panel and the full screen controller agree. */

enum button_state {
	BUTTON_NORMAL,
	BUTTON_HOVER,
	BUTTON_PRESSED,
	BUTTON_DISABLED
};

enum glyph_kind {
	GLYPH_PLAY,
	GLYPH_PAUSE,
	GLYPH_REWIND,
	GLYPH_FAST_FORWARD,
	GLYPH_TO_START,
	GLYPH_TO_END,
	GLYPH_FULL_SCREEN,
	GLYPH_EXIT_FULL_SCREEN,
	GLYPH_SPEAKER,
	GLYPH_SPEAKER_MUTED,
	GLYPH_STEP_BACK,
	GLYPH_STEP_FORWARD
};

namespace look {

// Amp's toolbar greys: the surface around the picture is the light end,
// the controller runs from it down to the dark end.
const rgb_color kSurface = {236, 236, 236, 255};
const rgb_color kSurfaceBottom = {176, 176, 176, 255};
const rgb_color kBorder = {108, 108, 108, 255};
const rgb_color kText = {70, 70, 70, 255};

// Fills `rect` of `view` with the plain surface grey.
void surface(BView* view, BRect rect);

// The controller's grey, Amp's toolbar: a gradient over `bar` (the whole
// bar in the view's coordinates), drawn where it meets `rect`.
void toolbar(BView* view, BRect rect, BRect bar);

// A thin inset line around `rect`, where the picture sits in the surface.
void inset_frame(BView* view, BRect rect, float radius = 0);

void round_button(BView* view, BRect rect, button_state state,
	bool prominent = false);
void glyph(BView* view, glyph_kind kind, BRect rect, rgb_color color);

// The timeline: a recessed track, the played part, chapter marks and the
// triangular playhead.
void timeline_track(BView* view, BRect track, float playedTo, bool dark);
void playhead(BView* view, BPoint tip, float height, button_state state,
	bool dark);

void slider_track(BView* view, BRect track, float fraction, bool dark);
void slider_knob(BView* view, BPoint center, button_state state, bool dark);

// The LCD panel and the colour of what is written on it.
void lcd(BView* view, BRect rect);
rgb_color lcd_text_color();
rgb_color lcd_dim_text_color();

// The full screen controller's dark glass.
void hud_panel(BView* view, BRect rect);

rgb_color color(uint8 red, uint8 green, uint8 blue, uint8 alpha = 255);
rgb_color mix(rgb_color a, rgb_color b, float amount);

}	// namespace look

}	// namespace airtime

#endif	// AIRTIME_LOOK_H
