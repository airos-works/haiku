/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef WIFI_GLYPHS_H
#define WIFI_GLYPHS_H


#include <GradientLinear.h>
#include <InterfaceDefs.h>
#include <Rect.h>
#include <Shape.h>
#include <View.h>

#include <algorithm>
#include <math.h>

#include "WiFiIconPaths.h"


enum signal_glyph_style {
	SIGNAL_GLYPH_NORMAL,
	SIGNAL_GLYPH_DISABLED,
		// Greyed bars, for example while disconnected
	SIGNAL_GLYPH_OFF
		// Outlined bars only
};


inline rgb_color
WiFiGlyphTint(rgb_color foreground, rgb_color background, float amount)
{
	// amount is the share of the foreground color
	return mix_color(background, foreground, (uint8)(255 * amount));
}


/*!	Draws signal strength bars inside \a frame. \a bars of the four bars are
	filled with \a foreground, the others with a faint tint of it over
	\a background.
*/
inline void
DrawWiFiSignalBars(BView* view, BRect frame, int32 bars,
	rgb_color foreground, rgb_color background,
	signal_glyph_style style = SIGNAL_GLYPH_NORMAL)
{
	const int32 kBars = 4;
	float gap = std::max(1.0f, floorf(frame.Width() / 12));
	float width = floorf((frame.Width() + 1 - gap * (kBars - 1)) / kBars);
	if (width < 1)
		width = 1;
	float totalWidth = width * kBars + gap * (kBars - 1);
	float left = floorf(frame.left + (frame.Width() + 1 - totalWidth) / 2);
	float bottom = floorf(frame.bottom);
	float height = frame.Height() + 1;

	rgb_color filled = foreground;
	rgb_color empty = WiFiGlyphTint(foreground, background, 0.25f);
	if (style == SIGNAL_GLYPH_DISABLED) {
		filled = WiFiGlyphTint(foreground, background, 0.45f);
		empty = WiFiGlyphTint(foreground, background, 0.2f);
	} else if (style == SIGNAL_GLYPH_OFF)
		filled = empty = WiFiGlyphTint(foreground, background, 0.45f);

	view->PushState();
	view->SetDrawingMode(B_OP_COPY);
	for (int32 i = 0; i < kBars; i++) {
		float barHeight = std::max(2.0f, floorf(height * (i + 1) / kBars));
		BRect bar(left, bottom - barHeight + 1, left + width - 1, bottom);
		view->SetHighColor(i < bars ? filled : empty);
		if (style == SIGNAL_GLYPH_OFF)
			view->StrokeRect(bar);
		else
			view->FillRect(bar);
		left += width + gap;
	}
	view->PopState();
}


enum tray_glyph_state {
	TRAY_GLYPH_CONNECTED,
		// Green arcs
	TRAY_GLYPH_WEAK,
		// Connected, but no Internet: amber arcs
	TRAY_GLYPH_CONNECTING,
		// Amber arcs, animated by the caller
	TRAY_GLYPH_DISCONNECTED,
		// Empty slots only
	TRAY_GLYPH_OFF,
		// Faded slots
	TRAY_GLYPH_NO_ADAPTER
		// Faded slots and a red cross
};


/*! Draws the familiar Wi-Fi dot and three arcs, using the same paths as the
	application icon. The four signal levels light up from the dot outwards.
*/
inline void
DrawWiFiTrayIcon(BView* view, BRect frame, int32 bars, tray_glyph_state state,
	rgb_color background)
{
	float scale = std::min(frame.Width() / 58, frame.Height() / 48);
	BPoint origin(frame.left + frame.Width() / 2 - 32 * scale,
		frame.top + frame.Height() / 2 - 33 * scale);

	rgb_color face, highlight, outline;
	switch (state) {
		case TRAY_GLYPH_CONNECTED:
			face = make_color(70, 190, 50);
			highlight = make_color(190, 245, 150);
			outline = make_color(20, 70, 15);
			break;
		case TRAY_GLYPH_WEAK:
		case TRAY_GLYPH_CONNECTING:
			face = make_color(240, 160, 20);
			highlight = make_color(255, 235, 150);
			outline = make_color(110, 60, 0);
			break;
		default:
			bars = 0;
			face = highlight = outline = background;
			break;
	}
	// Alpha lets the unlit arcs blend with Deskbar's own gradient.
	bool faded = state == TRAY_GLYPH_OFF || state == TRAY_GLYPH_NO_ADAPTER;
	bool darkBackground = background.IsDark();
	rgb_color slotFace = darkBackground ? make_color(255, 255, 255,
		faded ? 20 : 40) : make_color(255, 255, 255, faded ? 70 : 120);
	rgb_color slotOutline = darkBackground
		? make_color(255, 255, 255, faded ? 70 : 120)
		: make_color(0, 0, 0, faded ? 60 : 110);

	view->PushState();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	view->SetPenSize(std::max(0.75f, 2 * scale));
	for (int32 i = 0; i < 4; i++) {
		const WiFiIconPath& path = kWiFiIconPaths[i];
		BShape shape;
		shape.MoveTo(origin + BPoint(path.points[0].x * scale,
			path.points[0].y * scale));
		for (int32 j = 1; j < path.count; j += 3) {
			BPoint points[3];
			for (int32 k = 0; k < 3; k++) {
				points[k] = origin + BPoint(path.points[j + k].x * scale,
					path.points[j + k].y * scale);
			}
			shape.BezierTo(points);
		}
		shape.Close();
		if (i < bars) {
			BRect bounds = shape.Bounds();
			BGradientLinear gradient(bounds.LeftTop(), bounds.RightBottom());
			gradient.AddColor(highlight, 0);
			gradient.AddColor(face, 255);
			view->SetHighColor(outline);
			view->StrokeShape(&shape);
			view->FillShape(&shape, gradient);
		} else {
			view->SetHighColor(slotOutline);
			view->StrokeShape(&shape);
			view->SetHighColor(slotFace);
			view->FillShape(&shape);
		}
	}

	if (state == TRAY_GLYPH_NO_ADAPTER) {
		float size = floorf(std::min(frame.Width(), frame.Height()) / 3);
		BRect cross(frame.right - size, frame.bottom - size,
			frame.right, frame.bottom);
		view->SetHighColor(200, 32, 32);
		view->SetPenSize(std::max(1.5f, size / 4));
		view->StrokeLine(cross.LeftTop(), cross.RightBottom());
		view->StrokeLine(cross.RightTop(), cross.LeftBottom());
	}
	view->PopState();
}


//!	Draws a small padlock inside \a frame.
inline void
DrawWiFiLock(BView* view, BRect frame, rgb_color color)
{
	float size = std::min(frame.Width(), frame.Height()) + 1;
	float bodyWidth = floorf(size * 0.75f);
	float bodyHeight = floorf(size * 0.5f);
	float left = floorf(frame.left + (frame.Width() + 1 - bodyWidth) / 2);
	float bottom = floorf(frame.top + (frame.Height() + size) / 2) - 1;
	BRect body(left, bottom - bodyHeight + 1, left + bodyWidth - 1, bottom);

	float shackleInset = std::max(1.0f, floorf(bodyWidth / 5));
	float shackleHeight = floorf(size * 0.45f);
	BRect shackle(body.left + shackleInset, body.top - shackleHeight,
		body.right - shackleInset, body.top + shackleHeight / 2);

	view->PushState();
	view->SetDrawingMode(B_OP_OVER);
	view->SetHighColor(color);
	view->SetPenSize(std::max(1.0f, floorf(size / 9)));
	view->StrokeArc(shackle, 0, 180);
	view->StrokeLine(BPoint(shackle.left, shackle.top + shackle.Height() / 2),
		BPoint(shackle.left, body.top));
	view->StrokeLine(BPoint(shackle.right, shackle.top + shackle.Height() / 2),
		BPoint(shackle.right, body.top));
	view->SetPenSize(1);
	view->FillRoundRect(body, 1, 1);
	view->PopState();
}


#endif	// WIFI_GLYPHS_H
