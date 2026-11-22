/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Glyphs.h"

#include <Shape.h>
#include <View.h>

#include <algorithm>
#include <math.h>


namespace {

struct Pen {
	BView*	view;
	BRect	box;
	float	size;

	BPoint P(float x, float y) const
	{
		return BPoint(box.left + x * box.Width(), box.top + y * box.Height());
	}

	float L(float length) const { return length * box.Width(); }

	void Line(float x1, float y1, float x2, float y2) const
	{
		view->StrokeLine(P(x1, y1), P(x2, y2));
	}

	BRect R(float l, float t, float r, float b) const
	{
		return BRect(P(l, t), P(r, b));
	}
};

}	// namespace


void
draw_glyph(BView* view, Glyph glyph, BRect frame, rgb_color color, float level)
{
	// Square it up and centre it.
	float side = std::min(frame.Width(), frame.Height());
	BRect box(0, 0, side, side);
	box.OffsetTo(frame.left + (frame.Width() - side) / 2,
		frame.top + (frame.Height() - side) / 2);

	Pen pen = { view, box, std::max(1.5f, roundf(side / 11)) };

	view->PushState();
	view->SetHighColor(color);
	view->SetPenSize(pen.size);
	view->SetLineMode(B_ROUND_CAP, B_ROUND_JOIN);
	view->SetDrawingMode(B_OP_OVER);

	switch (glyph) {
		case kGlyphWifi:
		case kGlyphWifiOff:
		{
			BPoint base = pen.P(0.5f, 0.82f);
			for (int i = 1; i <= 3; i++) {
				float r = pen.L(0.14f * i + 0.04f);
				view->StrokeArc(base, r, r, 45, 90);
			}
			view->FillEllipse(base, pen.size * 0.9f, pen.size * 0.9f);
			if (glyph == kGlyphWifiOff)
				pen.Line(0.15f, 0.15f, 0.85f, 0.85f);
			break;
		}

		case kGlyphWired:
			view->StrokeRoundRect(pen.R(0.2f, 0.12f, 0.8f, 0.52f), pen.L(0.05f),
				pen.L(0.05f));
			pen.Line(0.5f, 0.52f, 0.5f, 0.7f);
			pen.Line(0.18f, 0.7f, 0.82f, 0.7f);
			pen.Line(0.18f, 0.7f, 0.18f, 0.86f);
			pen.Line(0.82f, 0.7f, 0.82f, 0.86f);
			break;

		case kGlyphBattery:
		{
			BRect body = pen.R(0.12f, 0.3f, 0.8f, 0.7f);
			view->StrokeRoundRect(body, pen.L(0.06f), pen.L(0.06f));
			view->FillRect(pen.R(0.83f, 0.42f, 0.9f, 0.58f));
			BRect fill = body.InsetByCopy(pen.size * 1.5f, pen.size * 1.5f);
			fill.right = fill.left + fill.Width() * std::clamp(level, 0.0f, 1.0f);
			if (level < 0.15f)
				view->SetHighColor(237, 21, 21);
			view->FillRect(fill);
			break;
		}

		case kGlyphPlug:
			pen.Line(0.38f, 0.15f, 0.38f, 0.35f);
			pen.Line(0.62f, 0.15f, 0.62f, 0.35f);
			view->FillRoundRect(pen.R(0.25f, 0.35f, 0.75f, 0.6f), pen.L(0.08f),
				pen.L(0.08f));
			pen.Line(0.5f, 0.6f, 0.5f, 0.85f);
			break;

		case kGlyphTasks:
			view->StrokeRoundRect(pen.R(0.18f, 0.28f, 0.68f, 0.82f), pen.L(0.06f),
				pen.L(0.06f));
			pen.Line(0.34f, 0.16f, 0.82f, 0.16f);
			pen.Line(0.82f, 0.16f, 0.82f, 0.66f);
			break;

		case kGlyphHome:
		{
			BShape shape;
			shape.MoveTo(pen.P(0.15f, 0.48f));
			shape.LineTo(pen.P(0.5f, 0.16f));
			shape.LineTo(pen.P(0.85f, 0.48f));
			view->StrokeShape(&shape);
			BShape house;
			house.MoveTo(pen.P(0.26f, 0.42f));
			house.LineTo(pen.P(0.26f, 0.84f));
			house.LineTo(pen.P(0.74f, 0.84f));
			house.LineTo(pen.P(0.74f, 0.42f));
			view->StrokeShape(&house);
			break;
		}

		case kGlyphClose:
			pen.Line(0.25f, 0.25f, 0.75f, 0.75f);
			pen.Line(0.75f, 0.25f, 0.25f, 0.75f);
			break;

		case kGlyphKeyboard:
		{
			view->StrokeRoundRect(pen.R(0.08f, 0.26f, 0.92f, 0.74f), pen.L(0.06f),
				pen.L(0.06f));
			for (int row = 0; row < 2; row++) {
				for (int col = 0; col < 5; col++) {
					BPoint p = pen.P(0.22f + col * 0.14f, 0.4f + row * 0.13f);
					view->FillEllipse(p, pen.size * 0.55f, pen.size * 0.55f);
				}
			}
			pen.Line(0.32f, 0.63f, 0.68f, 0.63f);
			break;
		}

		case kGlyphCamera:
			view->StrokeRoundRect(pen.R(0.1f, 0.3f, 0.9f, 0.8f), pen.L(0.08f),
				pen.L(0.08f));
			view->StrokeEllipse(pen.P(0.5f, 0.55f), pen.L(0.15f), pen.L(0.15f));
			pen.Line(0.35f, 0.3f, 0.4f, 0.2f);
			pen.Line(0.4f, 0.2f, 0.6f, 0.2f);
			pen.Line(0.6f, 0.2f, 0.65f, 0.3f);
			break;

		case kGlyphGear:
		{
			BPoint c = pen.P(0.5f, 0.5f);
			for (int i = 0; i < 8; i++) {
				float a = i * M_PI / 4;
				BPoint from(c.x + cosf(a) * pen.L(0.24f), c.y + sinf(a) * pen.L(0.24f));
				BPoint to(c.x + cosf(a) * pen.L(0.36f), c.y + sinf(a) * pen.L(0.36f));
				view->StrokeLine(from, to);
			}
			view->StrokeEllipse(c, pen.L(0.24f), pen.L(0.24f));
			view->StrokeEllipse(c, pen.L(0.09f), pen.L(0.09f));
			break;
		}

		case kGlyphChevronUp:
			pen.Line(0.2f, 0.62f, 0.5f, 0.38f);
			pen.Line(0.5f, 0.38f, 0.8f, 0.62f);
			break;

		case kGlyphShift:
		{
			BPoint arrow[] = { pen.P(0.5f, 0.15f), pen.P(0.85f, 0.52f),
				pen.P(0.65f, 0.52f), pen.P(0.65f, 0.85f), pen.P(0.35f, 0.85f),
				pen.P(0.35f, 0.52f), pen.P(0.15f, 0.52f) };
			if (level > 0.5f)
				view->FillPolygon(arrow, 7);
			else
				view->StrokePolygon(arrow, 7);
			break;
		}

		case kGlyphBackspace:
		{
			BPoint key[] = { pen.P(0.08f, 0.5f), pen.P(0.32f, 0.22f),
				pen.P(0.92f, 0.22f), pen.P(0.92f, 0.78f), pen.P(0.32f, 0.78f) };
			view->StrokePolygon(key, 5);
			pen.Line(0.48f, 0.38f, 0.72f, 0.62f);
			pen.Line(0.72f, 0.38f, 0.48f, 0.62f);
			break;
		}

		case kGlyphReturn:
			pen.Line(0.82f, 0.2f, 0.82f, 0.6f);
			pen.Line(0.82f, 0.6f, 0.2f, 0.6f);
			pen.Line(0.2f, 0.6f, 0.38f, 0.42f);
			pen.Line(0.2f, 0.6f, 0.38f, 0.78f);
			break;

		case kGlyphGlobe:
		{
			BPoint c = pen.P(0.5f, 0.5f);
			// an outline, a meridian and the equator: more runs together small
			view->StrokeEllipse(c, pen.L(0.4f), pen.L(0.4f));
			view->StrokeEllipse(c, pen.L(0.17f), pen.L(0.4f));
			pen.Line(0.1f, 0.5f, 0.9f, 0.5f);
			break;
		}
	}

	view->PopState();
}
