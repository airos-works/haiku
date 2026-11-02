/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "MediaIcons.h"

#include <math.h>

#include <algorithm>

#include <GradientLinear.h>
#include <Polygon.h>
#include <View.h>


static const rgb_color kNoteOutline = { 20, 28, 56, 255 };


static void
fill_and_outline_polygon(BView* view, const BPoint* points, int32 count,
	const BGradient& gradient, float outline)
{
	view->FillPolygon(points, count, gradient);
	view->SetPenSize(outline);
	view->SetHighColor(kNoteOutline);
	view->StrokePolygon(points, count, true);
}


static void
note_head(BView* view, BPoint center, float radiusX, float radiusY,
	const BGradient& gradient, float outline)
{
	// a tilted oval, as a polygon
	BPoint points[24];
	const float tilt = -0.45f;
	for (int32 i = 0; i < 24; i++) {
		float angle = i * 2 * M_PI / 24;
		float x = radiusX * cosf(angle);
		float y = radiusY * sinf(angle);
		points[i] = BPoint(center.x + x * cosf(tilt) - y * sinf(tilt),
			center.y + x * sinf(tilt) + y * cosf(tilt));
	}
	fill_and_outline_polygon(view, points, 24, gradient, outline);
}


void
DrawNoteIcon(BView* view, BRect frame, bool dimmed)
{
	float size = std::min(frame.Width(), frame.Height()) + 1;
	float outline = std::max(1.0f, roundf(size / 16));
	BPoint origin(frame.left + (frame.Width() + 1 - size) / 2,
		frame.top + (frame.Height() + 1 - size) / 2);
	auto P = [origin, size](float x, float y) {
		return BPoint(origin.x + x * size, origin.y + y * size);
	};

	view->PushState();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	view->SetLineMode(B_ROUND_CAP, B_ROUND_JOIN);

	uint8 alpha = dimmed ? 140 : 255;
	BGradientLinear gradient(P(0.1, 0.1), P(0.9, 0.9));
	gradient.AddColor((rgb_color){ 140, 200, 255, alpha }, 0);
	gradient.AddColor((rgb_color){ 40, 110, 230, alpha }, 160);
	gradient.AddColor((rgb_color){ 20, 64, 170, alpha }, 255);

	// the beam, sloping up to the right, and the two stems
	BPoint beam[4] = { P(0.33, 0.20), P(0.86, 0.08), P(0.86, 0.24),
		P(0.33, 0.36) };
	BPoint leftStem[4] = { P(0.33, 0.28), P(0.41, 0.26), P(0.41, 0.78),
		P(0.33, 0.78) };
	BPoint rightStem[4] = { P(0.78, 0.16), P(0.86, 0.14), P(0.86, 0.68),
		P(0.78, 0.68) };
	fill_and_outline_polygon(view, leftStem, 4, gradient, outline);
	fill_and_outline_polygon(view, rightStem, 4, gradient, outline);
	fill_and_outline_polygon(view, beam, 4, gradient, outline);

	note_head(view, P(0.25, 0.78), 0.15 * size, 0.105 * size, gradient,
		outline);
	note_head(view, P(0.70, 0.68), 0.15 * size, 0.105 * size, gradient,
		outline);

	view->PopState();
}


void
DrawTransportSymbol(BView* view, BRect frame, transport_symbol symbol,
	rgb_color color)
{
	float size = std::min(frame.Width(), frame.Height());
	BPoint center(roundf((frame.left + frame.right) / 2),
		roundf((frame.top + frame.bottom) / 2));
	float half = roundf(size / 2);

	view->PushState();
	view->SetDrawingMode(B_OP_OVER);
	view->SetHighColor(color);

	switch (symbol) {
		case kSymbolPlay:
		{
			BPoint triangle[3] = {
				BPoint(center.x - half * 0.7f, center.y - half),
				BPoint(center.x + half * 0.9f, center.y),
				BPoint(center.x - half * 0.7f, center.y + half)
			};
			view->FillPolygon(triangle, 3);
			break;
		}

		case kSymbolPause:
		{
			float barWidth = std::max(2.0f, roundf(half * 0.6f));
			float gap = std::max(2.0f, roundf(half * 0.5f));
			view->FillRect(BRect(center.x - gap / 2 - barWidth,
				center.y - half, center.x - gap / 2 - 1, center.y + half));
			view->FillRect(BRect(center.x + gap / 2, center.y - half,
				center.x + gap / 2 + barWidth - 1, center.y + half));
			break;
		}

		case kSymbolNext:
		case kSymbolPrevious:
		{
			// two triangles and a bar, mirrored for previous
			float direction = symbol == kSymbolNext ? 1 : -1;
			float bar = std::max(2.0f, roundf(half * 0.25f));
			float tip = center.x + direction * (half - bar);
			float middle = center.x;
			float back = center.x - direction * half;
			BPoint first[3] = { BPoint(back, center.y - half * 0.8f),
				BPoint(middle, center.y), BPoint(back, center.y + half * 0.8f) };
			BPoint second[3] = { BPoint(middle, center.y - half * 0.8f),
				BPoint(tip, center.y), BPoint(middle, center.y + half * 0.8f) };
			view->FillPolygon(first, 3);
			view->FillPolygon(second, 3);
			BRect barRect = direction > 0
				? BRect(tip, center.y - half * 0.8f, tip + bar - 1,
					center.y + half * 0.8f)
				: BRect(tip - bar + 1, center.y - half * 0.8f, tip,
					center.y + half * 0.8f);
			view->FillRect(barRect);
			break;
		}
	}

	view->PopState();
}
