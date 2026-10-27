/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "Glyphs.h"

#include <algorithm>
#include <math.h>
#include <string.h>

#include <Bitmap.h>
#include <GradientLinear.h>
#include <InterfaceDefs.h>
#include <Shape.h>
#include <View.h>


static inline BPoint
at(const BRect& box, float x, float y)
{
	return BPoint(box.left + x * box.Width(), box.top + y * box.Height());
}


static inline BRect
part(const BRect& box, float left, float top, float right, float bottom)
{
	return BRect(at(box, left, top), at(box, right, bottom));
}


static void
draw_speaker(BView* view, const BRect& box, rgb_color badge)
{
	BShape cone;
	cone.MoveTo(at(box, 0.08f, 0.37f));
	cone.LineTo(at(box, 0.27f, 0.37f));
	cone.LineTo(at(box, 0.50f, 0.16f));
	cone.LineTo(at(box, 0.50f, 0.84f));
	cone.LineTo(at(box, 0.27f, 0.63f));
	cone.LineTo(at(box, 0.08f, 0.63f));
	cone.Close();
	view->MovePenTo(B_ORIGIN);
	view->FillShape(&cone);

	BPoint center = at(box, 0.50f, 0.50f);
	view->StrokeArc(center, box.Width() * 0.17f, box.Height() * 0.17f, -50,
		100);
	view->StrokeArc(center, box.Width() * 0.33f, box.Height() * 0.33f, -50,
		100);
}


static void
draw_microphone(BView* view, const BRect& box, rgb_color badge)
{
	float radius = box.Width() * 0.13f;
	view->FillRoundRect(part(box, 0.37f, 0.06f, 0.63f, 0.58f), radius,
		radius);
	view->StrokeArc(at(box, 0.50f, 0.42f), box.Width() * 0.25f,
		box.Height() * 0.25f, 180, 180);
	view->StrokeLine(at(box, 0.50f, 0.70f), at(box, 0.50f, 0.86f));
	view->StrokeLine(at(box, 0.34f, 0.88f), at(box, 0.66f, 0.88f));
}


static void
draw_mixer(BView* view, const BRect& box, rgb_color badge)
{
	const float x[3] = { 0.22f, 0.50f, 0.78f };
	const float knob[3] = { 0.62f, 0.32f, 0.52f };
	float penSize = view->PenSize();

	view->SetPenSize(std::max(1.0f, penSize * 0.6f));
	for (int i = 0; i < 3; i++)
		view->StrokeLine(at(box, x[i], 0.10f), at(box, x[i], 0.90f));
	view->SetPenSize(penSize);

	for (int i = 0; i < 3; i++) {
		BRect knobRect = part(box, x[i] - 0.12f, knob[i] - 0.08f,
			x[i] + 0.12f, knob[i] + 0.08f);
		float radius = knobRect.Height() * 0.35f;
		view->FillRoundRect(knobRect, radius, radius);
	}
}


static void
draw_midi(BView* view, const BRect& box, rgb_color badge)
{
	BRect keys = part(box, 0.06f, 0.18f, 0.94f, 0.82f);
	float radius = box.Width() * 0.06f;
	view->FillRoundRect(keys, radius, radius);

	rgb_color white = view->HighColor();
	view->SetHighColor(badge);
	float penSize = view->PenSize();
	view->SetPenSize(std::max(1.0f, penSize * 0.5f));
	for (int i = 1; i < 4; i++) {
		float x = 0.06f + i * 0.22f;
		view->StrokeLine(at(box, x, 0.50f), at(box, x, 0.82f));
		view->FillRect(part(box, x - 0.06f, 0.18f, x + 0.06f, 0.54f));
	}
	view->SetPenSize(penSize);
	view->SetHighColor(white);
}


static void
draw_camera(BView* view, const BRect& box, rgb_color badge)
{
	BPoint center = at(box, 0.50f, 0.42f);
	view->FillEllipse(center, box.Width() * 0.34f, box.Height() * 0.34f);

	rgb_color white = view->HighColor();
	view->SetHighColor(badge);
	view->FillEllipse(center, box.Width() * 0.17f, box.Height() * 0.17f);
	view->SetHighColor(white);
	view->FillEllipse(center + BPoint(-box.Width() * 0.05f,
			-box.Height() * 0.05f), box.Width() * 0.05f,
		box.Height() * 0.05f);

	BShape stand;
	stand.MoveTo(at(box, 0.42f, 0.74f));
	stand.LineTo(at(box, 0.58f, 0.74f));
	stand.LineTo(at(box, 0.70f, 0.92f));
	stand.LineTo(at(box, 0.30f, 0.92f));
	stand.Close();
	view->MovePenTo(B_ORIGIN);
	view->FillShape(&stand);
}


static void
draw_display(BView* view, const BRect& box, rgb_color badge)
{
	float radius = box.Width() * 0.05f;
	view->StrokeRoundRect(part(box, 0.08f, 0.16f, 0.92f, 0.70f), radius,
		radius);
	view->StrokeLine(at(box, 0.50f, 0.72f), at(box, 0.50f, 0.84f));
	view->StrokeLine(at(box, 0.30f, 0.86f), at(box, 0.70f, 0.86f));
}


static void
draw_warning(BView* view, const BRect& box, rgb_color badge)
{
	BShape triangle;
	triangle.MoveTo(at(box, 0.50f, 0.08f));
	triangle.LineTo(at(box, 0.95f, 0.88f));
	triangle.LineTo(at(box, 0.05f, 0.88f));
	triangle.Close();
	view->MovePenTo(B_ORIGIN);
	view->FillShape(&triangle);

	view->SetHighColor(badge);
	view->StrokeLine(at(box, 0.50f, 0.36f), at(box, 0.50f, 0.60f));
	view->FillEllipse(at(box, 0.50f, 0.74f), box.Width() * 0.05f,
		box.Height() * 0.05f);
}


rgb_color
glyph_color(glyph_kind kind)
{
	switch (kind) {
		case GLYPH_SPEAKER:
			return make_color(10, 122, 240);
		case GLYPH_MICROPHONE:
			return make_color(232, 72, 60);
		case GLYPH_MIXER:
			return make_color(166, 82, 214);
		case GLYPH_MIDI:
			return make_color(46, 176, 84);
		case GLYPH_CAMERA:
			return make_color(24, 160, 190);
		case GLYPH_DISPLAY:
			return make_color(88, 86, 214);
		case GLYPH_WARNING:
			return make_color(240, 150, 20);
	}
	return make_color(120, 120, 128);
}


rgb_color
device_glyph_color()
{
	return make_color(108, 118, 132);
}


void
draw_glyph_badge(BView* view, BRect frame, glyph_kind kind, rgb_color color)
{
	view->PushState();

	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);

	float size = std::min(frame.Width(), frame.Height());
	BRect badge(frame.left, frame.top, frame.left + size, frame.top + size);
	float radius = size * 0.24f;

	BGradientLinear gradient(badge.LeftTop(), badge.LeftBottom());
	gradient.AddColor(tint_color(color, 0.82f), 0);
	gradient.AddColor(color, 255);
	view->FillRoundRect(badge, radius, radius, gradient);

	BRect box = badge.InsetByCopy(size * 0.22f, size * 0.22f);
	view->SetHighColor(255, 255, 255);
	view->SetPenSize(std::max(1.0f, roundf(size * 0.075f)));
	view->SetLineMode(B_ROUND_CAP, B_ROUND_JOIN);

	switch (kind) {
		case GLYPH_SPEAKER:
			draw_speaker(view, box, color);
			break;
		case GLYPH_MICROPHONE:
			draw_microphone(view, box, color);
			break;
		case GLYPH_MIXER:
			draw_mixer(view, box, color);
			break;
		case GLYPH_MIDI:
			draw_midi(view, box, color);
			break;
		case GLYPH_CAMERA:
			draw_camera(view, box, color);
			break;
		case GLYPH_DISPLAY:
			draw_display(view, box, color);
			break;
		case GLYPH_WARNING:
			draw_warning(view, box, color);
			break;
	}

	view->PopState();
}


BBitmap*
make_glyph_bitmap(glyph_kind kind, rgb_color color, float size)
{
	BRect bounds(0, 0, size - 1, size - 1);
	BBitmap* bitmap = new BBitmap(bounds, B_BITMAP_ACCEPTS_VIEWS, B_RGBA32);
	if (bitmap->InitCheck() != B_OK) {
		delete bitmap;
		return NULL;
	}

	memset(bitmap->Bits(), 0, bitmap->BitsLength());
	BView* view = new BView(bounds, "glyph", B_FOLLOW_NONE, B_WILL_DRAW);
	bitmap->AddChild(view);
	if (bitmap->Lock()) {
		draw_glyph_badge(view, bounds, kind, color);
		view->Sync();
		bitmap->Unlock();
	}
	bitmap->RemoveChild(view);
	delete view;
	return bitmap;
}
