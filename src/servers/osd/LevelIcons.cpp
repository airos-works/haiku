/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "LevelIcons.h"

#include <math.h>

#include <algorithm>

#include <GradientLinear.h>
#include <GradientRadialFocus.h>
#include <InterfaceDefs.h>
#include <Polygon.h>
#include <View.h>


// The icons are drawn the way the system's vector icons are: a dark outline
// of about a 32nd of their size, and gradients lit from the top left.


static const rgb_color kSunOutline = { 120, 56, 0, 255 };
static const rgb_color kSpeakerOutline = { 32, 32, 40, 255 };


static BPoint
polar(BPoint center, float radius, float angle)
{
	return BPoint(center.x + radius * cosf(angle),
		center.y + radius * sinf(angle));
}


void
DrawSunIcon(BView* view, BRect frame)
{
	float size = frame.Width();
	float outline = std::max(1.0f, size / 32);
	BPoint center(frame.left + size / 2, frame.top + size / 2);

	view->PushState();
	view->SetLineMode(B_ROUND_CAP, B_ROUND_JOIN);
	view->SetPenSize(outline);

	// eight rays, the ones on the axes longer than the diagonal ones
	for (int32 i = 0; i < 8; i++) {
		float angle = i * M_PI / 4;
		float tip = (i % 2) == 0 ? 0.48f * size : 0.43f * size;
		float base = 0.24f * size;
		float halfWidth = 0.1f * size;

		BPoint baseCenter = polar(center, base, angle);
		BPoint offset(halfWidth * cosf(angle + M_PI / 2),
			halfWidth * sinf(angle + M_PI / 2));
		BPoint points[3] = {
			baseCenter + offset,
			polar(center, tip, angle),
			baseCenter - offset
		};

		BGradientLinear gradient(baseCenter, points[1]);
		gradient.AddColor((rgb_color){ 255, 214, 48, 255 }, 0);
		gradient.AddColor((rgb_color){ 244, 132, 0, 255 }, 255);
		view->FillPolygon(points, 3, gradient);

		view->SetHighColor(kSunOutline);
		view->StrokePolygon(points, 3, true);
	}

	// the disc over the inner ends of the rays
	float radius = 0.27f * size;
	BGradientRadialFocus gradient(center, radius,
		center - BPoint(radius * 0.4f, radius * 0.4f));
	gradient.AddColor((rgb_color){ 255, 255, 222, 255 }, 0);
	gradient.AddColor((rgb_color){ 255, 222, 64, 255 }, 110);
	gradient.AddColor((rgb_color){ 240, 150, 8, 255 }, 255);
	view->FillEllipse(center, radius, radius, gradient);

	view->SetHighColor(kSunOutline);
	view->StrokeEllipse(center, radius, radius);

	view->PopState();
}


void
DrawSpeakerIcon(BView* view, BRect frame, float level, bool muted)
{
	float size = frame.Width();
	float outline = std::max(1.0f, size / 32);
	float x = frame.left;
	float y = frame.top;

	view->PushState();
	view->SetLineMode(B_ROUND_CAP, B_ROUND_JOIN);

	// the magnet at the back
	BRect magnet(x + 0.08f * size, y + 0.36f * size, x + 0.26f * size,
		y + 0.64f * size);
	BGradientLinear magnetGradient(magnet.LeftTop(), magnet.LeftBottom());
	magnetGradient.AddColor((rgb_color){ 148, 148, 162, 255 }, 0);
	magnetGradient.AddColor((rgb_color){ 64, 64, 76, 255 }, 255);
	view->FillRoundRect(magnet, outline, outline, magnetGradient);

	view->SetPenSize(outline);
	view->SetHighColor(kSpeakerOutline);
	view->StrokeRoundRect(magnet, outline, outline);

	// the cone, opening to the right
	BPoint cone[4] = {
		BPoint(magnet.right, magnet.top),
		BPoint(x + 0.47f * size, y + 0.14f * size),
		BPoint(x + 0.47f * size, y + 0.86f * size),
		BPoint(magnet.right, magnet.bottom)
	};
	BGradientLinear coneGradient(cone[1], cone[2] + BPoint(-0.1f * size, 0));
	coneGradient.AddColor((rgb_color){ 244, 244, 250, 255 }, 0);
	coneGradient.AddColor((rgb_color){ 176, 176, 192, 255 }, 140);
	coneGradient.AddColor((rgb_color){ 112, 112, 128, 255 }, 255);
	view->FillPolygon(cone, 4, coneGradient);

	view->SetHighColor(kSpeakerOutline);
	view->StrokePolygon(cone, 4, true);

	BPoint mouth(x + 0.47f * size, y + 0.5f * size);
	float stroke = 0.075f * size;

	if (muted) {
		// a red cross where the sound would be
		BPoint crossCenter(x + 0.72f * size, mouth.y);
		float arm = 0.12f * size;
		BPoint a = crossCenter + BPoint(-arm, -arm);
		BPoint b = crossCenter + BPoint(arm, arm);
		BPoint c = crossCenter + BPoint(-arm, arm);
		BPoint d = crossCenter + BPoint(arm, -arm);

		view->SetPenSize(stroke + 2 * outline);
		view->SetHighColor(96, 0, 0);
		view->StrokeLine(a, b);
		view->StrokeLine(c, d);

		view->SetPenSize(stroke);
		view->SetHighColor(232, 40, 24);
		view->StrokeLine(a, b);
		view->StrokeLine(c, d);
	} else {
		// up to three waves, as loud as it is
		int32 lit = level <= 0 ? 0 : std::min(3, (int32)ceilf(level * 3));
		rgb_color faint = tint_color(ui_color(B_PANEL_BACKGROUND_COLOR),
			ui_color(B_PANEL_BACKGROUND_COLOR).IsDark()
				? B_LIGHTEN_1_TINT : B_DARKEN_1_TINT);

		for (int32 i = 0; i < 3; i++) {
			float radius = (0.13f + i * 0.13f) * size;
			if (i < lit) {
				view->SetPenSize(stroke + 2 * outline);
				view->SetHighColor(kSpeakerOutline);
				view->StrokeArc(mouth, radius, radius, -42, 84);

				view->SetPenSize(stroke);
				view->SetHighColor(96, 168, 255);
			} else {
				view->SetPenSize(stroke);
				view->SetHighColor(faint);
			}
			view->StrokeArc(mouth, radius, radius, -42, 84);
		}
	}

	view->PopState();
}
