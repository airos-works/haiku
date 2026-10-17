/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Look.h"

#include <math.h>

#include <Gradient.h>
#include <GradientLinear.h>
#include <Region.h>
#include <Shape.h>
#include <View.h>


namespace airtime {
namespace look {


rgb_color
color(uint8 red, uint8 green, uint8 blue, uint8 alpha)
{
	rgb_color result;
	result.red = red;
	result.green = green;
	result.blue = blue;
	result.alpha = alpha;
	return result;
}


rgb_color
mix(rgb_color a, rgb_color b, float amount)
{
	rgb_color result;
	result.red = (uint8)(a.red + (b.red - a.red) * amount);
	result.green = (uint8)(a.green + (b.green - a.green) * amount);
	result.blue = (uint8)(a.blue + (b.blue - a.blue) * amount);
	result.alpha = (uint8)(a.alpha + (b.alpha - a.alpha) * amount);
	return result;
}


// #pragma mark - surfaces


void
surface(BView* view, BRect rect)
{
	view->PushState();
	view->SetDrawingMode(B_OP_COPY);
	view->SetHighColor(kSurface);
	view->FillRect(rect);
	view->PopState();
}


void
toolbar(BView* view, BRect rect, BRect bar)
{
	view->PushState();
	view->SetDrawingMode(B_OP_COPY);
	BRegion clip(rect);
	view->ConstrainClippingRegion(&clip);
	BGradientLinear gradient(BPoint(bar.left, bar.top),
		BPoint(bar.left, bar.bottom));
	gradient.AddColor(kSurface, 0);
	gradient.AddColor(kSurfaceBottom, 255);
	view->FillRect(bar, gradient);
	// A light line along the top, as Amp's toolbar has.
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	view->SetHighColor(color(255, 255, 255, 140));
	view->StrokeLine(bar.LeftTop(), bar.RightTop());
	view->PopState();
}


void
inset_frame(BView* view, BRect rect, float radius)
{
	view->PushState();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	view->SetPenSize(1);
	BRect outer = rect.InsetByCopy(-1, -1);
	if (radius > 0) {
		view->SetHighColor(color(255, 255, 255, 150));
		view->StrokeRoundRect(outer.OffsetByCopy(0, 1), radius, radius);
		view->SetHighColor(color(40, 40, 40, 170));
		view->StrokeRoundRect(outer, radius, radius);
	} else {
		view->SetHighColor(color(40, 40, 40, 150));
		view->StrokeLine(outer.LeftBottom(), outer.LeftTop());
		view->StrokeLine(outer.LeftTop(), outer.RightTop());
		view->SetHighColor(color(255, 255, 255, 170));
		view->StrokeLine(outer.RightTop() + BPoint(0, 1), outer.RightBottom());
		view->StrokeLine(outer.RightBottom(), outer.LeftBottom()
			+ BPoint(1, 0));
	}
	view->PopState();
}


// #pragma mark - buttons


void
round_button(BView* view, BRect rect, button_state state, bool prominent)
{
	// Amp's round button: a soft shadow, a face lit from above and a thin
	// grey rim; pressed, it darkens.
	view->PushState();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	if (state != BUTTON_DISABLED) {
		view->SetHighColor(color(0, 0, 0, 40));
		view->FillEllipse(rect.OffsetByCopy(0, 1.5f));
	}

	rgb_color top;
	rgb_color bottom;
	rgb_color rim = color(118, 118, 118);
	switch (state) {
		case BUTTON_PRESSED:
			top = color(190, 190, 190);
			bottom = color(150, 150, 150);
			break;
		case BUTTON_HOVER:
			top = color(255, 255, 255);
			bottom = color(210, 210, 210);
			break;
		case BUTTON_DISABLED:
			top = color(240, 240, 240);
			bottom = color(216, 216, 216);
			rim = color(164, 164, 164);
			break;
		default:
			top = color(252, 252, 252);
			bottom = color(196, 196, 196);
			break;
	}
	view->SetDrawingMode(B_OP_COPY);
	BGradientLinear face(rect.LeftTop(), rect.LeftBottom());
	face.AddColor(top, 0);
	face.AddColor(bottom, 255);
	view->FillEllipse(rect, face);
	view->SetHighColor(rim);
	view->SetPenSize(1);
	view->StrokeEllipse(rect);
	view->PopState();
}


static BPoint
map_point(BRect rect, float x, float y)
{
	return BPoint(rect.left + x * rect.Width(), rect.top + y * rect.Height());
}


static void
fill_polygon(BView* view, BRect rect, const float* points, int count)
{
	BShape shape;
	shape.MoveTo(map_point(rect, points[0], points[1]));
	for (int i = 1; i < count; i++)
		shape.LineTo(map_point(rect, points[i * 2], points[i * 2 + 1]));
	shape.Close();
	view->FillShape(&shape);
}


static void
fill_unit_rect(BView* view, BRect rect, float left, float top, float right,
	float bottom)
{
	BRect bar(map_point(rect, left, top), map_point(rect, right, bottom));
	view->FillRect(bar);
}


void
glyph(BView* view, glyph_kind kind, BRect rect, rgb_color glyphColor)
{
	view->PushState();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	view->SetHighColor(glyphColor);
	view->SetLowColor(glyphColor);

	switch (kind) {
		case GLYPH_PLAY:
		{
			const float points[] = {0.32f, 0.20f, 0.32f, 0.80f, 0.82f, 0.50f};
			fill_polygon(view, rect, points, 3);
			break;
		}
		case GLYPH_PAUSE:
			fill_unit_rect(view, rect, 0.27f, 0.22f, 0.43f, 0.78f);
			fill_unit_rect(view, rect, 0.57f, 0.22f, 0.73f, 0.78f);
			break;
		case GLYPH_REWIND:
		{
			const float a[] = {0.50f, 0.26f, 0.50f, 0.74f, 0.13f, 0.50f};
			const float b[] = {0.87f, 0.26f, 0.87f, 0.74f, 0.50f, 0.50f};
			fill_polygon(view, rect, a, 3);
			fill_polygon(view, rect, b, 3);
			break;
		}
		case GLYPH_FAST_FORWARD:
		{
			const float a[] = {0.13f, 0.26f, 0.13f, 0.74f, 0.50f, 0.50f};
			const float b[] = {0.50f, 0.26f, 0.50f, 0.74f, 0.87f, 0.50f};
			fill_polygon(view, rect, a, 3);
			fill_polygon(view, rect, b, 3);
			break;
		}
		case GLYPH_TO_START:
		{
			fill_unit_rect(view, rect, 0.20f, 0.26f, 0.30f, 0.74f);
			const float a[] = {0.56f, 0.26f, 0.56f, 0.74f, 0.30f, 0.50f};
			const float b[] = {0.82f, 0.26f, 0.82f, 0.74f, 0.56f, 0.50f};
			fill_polygon(view, rect, a, 3);
			fill_polygon(view, rect, b, 3);
			break;
		}
		case GLYPH_TO_END:
		{
			fill_unit_rect(view, rect, 0.70f, 0.26f, 0.80f, 0.74f);
			const float a[] = {0.18f, 0.26f, 0.18f, 0.74f, 0.44f, 0.50f};
			const float b[] = {0.44f, 0.26f, 0.44f, 0.74f, 0.70f, 0.50f};
			fill_polygon(view, rect, a, 3);
			fill_polygon(view, rect, b, 3);
			break;
		}
		case GLYPH_STEP_BACK:
		{
			fill_unit_rect(view, rect, 0.28f, 0.28f, 0.38f, 0.72f);
			const float a[] = {0.74f, 0.28f, 0.74f, 0.72f, 0.40f, 0.50f};
			fill_polygon(view, rect, a, 3);
			break;
		}
		case GLYPH_STEP_FORWARD:
		{
			fill_unit_rect(view, rect, 0.62f, 0.28f, 0.72f, 0.72f);
			const float a[] = {0.26f, 0.28f, 0.26f, 0.72f, 0.60f, 0.50f};
			fill_polygon(view, rect, a, 3);
			break;
		}
		case GLYPH_FULL_SCREEN:
		case GLYPH_EXIT_FULL_SCREEN:
		{
			// Four corners, pointing out (or in, to leave).
			bool out = kind == GLYPH_FULL_SCREEN;
			float pen = rect.Width() / 9.0f;
			if (pen < 1.5f)
				pen = 1.5f;
			view->SetPenSize(pen);
			view->SetLineMode(B_SQUARE_CAP, B_MITER_JOIN);
			const float near = 0.18f, far = 0.82f, arm = 0.22f;
			for (int corner = 0; corner < 4; corner++) {
				float cx = corner & 1 ? far : near;
				float cy = corner & 2 ? far : near;
				float dx = corner & 1 ? -1 : 1;
				float dy = corner & 2 ? -1 : 1;
				if (!out) {
					// The corner moves inward and the arms point outward.
					cx += dx * arm;
					cy += dy * arm;
					dx = -dx;
					dy = -dy;
				}
				BShape shape;
				shape.MoveTo(map_point(rect, cx + dx * arm, cy));
				shape.LineTo(map_point(rect, cx, cy));
				shape.LineTo(map_point(rect, cx, cy + dy * arm));
				view->StrokeShape(&shape);
			}
			break;
		}
		case GLYPH_SPEAKER:
		case GLYPH_SPEAKER_MUTED:
		{
			fill_unit_rect(view, rect, 0.10f, 0.38f, 0.26f, 0.62f);
			const float cone[] = {0.24f, 0.38f, 0.48f, 0.16f, 0.48f, 0.84f,
				0.24f, 0.62f};
			fill_polygon(view, rect, cone, 4);
			float pen = rect.Width() / 12.0f;
			if (pen < 1.2f)
				pen = 1.2f;
			view->SetPenSize(pen);
			if (kind == GLYPH_SPEAKER) {
				BPoint center = map_point(rect, 0.46f, 0.50f);
				for (int wave = 0; wave < 2; wave++) {
					float radius = rect.Width() * (0.20f + wave * 0.17f);
					view->StrokeArc(center, radius, radius, -45, 90);
				}
			} else {
				view->SetLineMode(B_ROUND_CAP, B_ROUND_JOIN);
				view->StrokeLine(map_point(rect, 0.60f, 0.34f),
					map_point(rect, 0.88f, 0.66f));
				view->StrokeLine(map_point(rect, 0.60f, 0.66f),
					map_point(rect, 0.88f, 0.34f));
			}
			break;
		}
	}
	view->PopState();
}


// #pragma mark - timeline and sliders


void
timeline_track(BView* view, BRect track, float playedTo, bool dark)
{
	view->PushState();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	float radius = track.Height() / 2;

	if (dark) {
		view->SetHighColor(color(20, 20, 20, 230));
		view->FillRoundRect(track, radius, radius);
		view->SetHighColor(color(255, 255, 255, 40));
		view->StrokeRoundRect(track.OffsetByCopy(0, 1), radius, radius);
	} else {
		// A groove: dark above, catching light below.
		BRect outer = track.InsetByCopy(-1, -1);
		BGradientLinear edge(outer.LeftTop(), outer.LeftBottom());
		edge.AddColor(color(60, 60, 64, 200), 0);
		edge.AddColor(color(255, 255, 255, 200), 255);
		view->FillRoundRect(outer, radius + 1, radius + 1, edge);
		BGradientLinear inside(track.LeftTop(), track.LeftBottom());
		inside.AddColor(color(176, 178, 182), 0);
		inside.AddColor(color(226, 228, 232), 120);
		inside.AddColor(color(244, 245, 248), 255);
		view->FillRoundRect(track, radius, radius, inside);
	}

	if (playedTo > track.left + 1) {
		BRect played = track;
		played.right = playedTo < track.right ? playedTo : track.right;
		BGradientLinear fillGradient(played.LeftTop(), played.LeftBottom());
		if (dark) {
			fillGradient.AddColor(color(230, 230, 234), 0);
			fillGradient.AddColor(color(170, 170, 176), 255);
		} else {
			fillGradient.AddColor(color(88, 98, 112), 0);
			fillGradient.AddColor(color(128, 140, 156), 140);
			fillGradient.AddColor(color(156, 168, 184), 255);
		}
		view->FillRoundRect(played, radius, radius, fillGradient);
	}
	view->PopState();
}


void
playhead(BView* view, BPoint tip, float height, button_state state, bool dark)
{
	// The playhead: a little grey pentagon pointing down at the track.
	float width = roundf(height * 0.8f);
	float shoulder = height * 0.52f;
	auto outline = [&](BShape& shape, float dx, float dy) {
		shape.MoveTo(BPoint(tip.x - width / 2 + dx, tip.y - height + dy));
		shape.LineTo(BPoint(tip.x + width / 2 + dx, tip.y - height + dy));
		shape.LineTo(BPoint(tip.x + width / 2 + dx,
			tip.y - height + shoulder + dy));
		shape.LineTo(BPoint(tip.x + dx, tip.y + dy));
		shape.LineTo(BPoint(tip.x - width / 2 + dx,
			tip.y - height + shoulder + dy));
		shape.Close();
	};
	BShape shape;
	outline(shape, 0, 0);
	BShape shadow;
	outline(shadow, 0.8f, 1.2f);

	view->PushState();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	view->SetHighColor(color(0, 0, 0, 60));
	view->FillShape(&shadow);

	BGradientLinear face(BPoint(0, tip.y - height), BPoint(0, tip.y));
	bool pressed = state == BUTTON_PRESSED;
	if (dark) {
		face.AddColor(color(250, 250, 250), 0);
		face.AddColor(color(200, 200, 204), 255);
	} else {
		face.AddColor(pressed ? color(190, 190, 190) : color(252, 252, 252), 0);
		face.AddColor(pressed ? color(150, 150, 150) : color(196, 196, 196),
			255);
	}
	view->FillShape(&shape, face);
	view->SetHighColor(dark ? color(40, 40, 44, 220) : color(118, 118, 118));
	view->SetPenSize(1);
	view->StrokeShape(&shape);
	view->PopState();
}


void
slider_track(BView* view, BRect track, float fraction, bool dark)
{
	float played = track.left + (track.Width()) * fraction;
	timeline_track(view, track, played, dark);
}


void
slider_knob(BView* view, BPoint center, button_state state, bool dark)
{
	BRect knob(center.x - 6, center.y - 6, center.x + 6, center.y + 6);
	if (dark) {
		view->PushState();
		view->SetDrawingMode(B_OP_ALPHA);
		BGradientLinear face(knob.LeftTop(), knob.LeftBottom());
		face.AddColor(color(250, 250, 250), 0);
		face.AddColor(color(190, 190, 196), 255);
		view->FillEllipse(knob, face);
		view->PopState();
		return;
	}
	round_button(view, knob, state, false);
}


// #pragma mark - LCD and HUD


rgb_color
lcd_text_color()
{
	return color(40, 45, 38);
}


rgb_color
lcd_dim_text_color()
{
	return color(90, 98, 86);
}


void
lcd(BView* view, BRect rect)
{
	// Amp's LCD: a pale green glass with a soft shadow under it.
	view->PushState();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	float radius = 6;
	view->SetHighColor(color(0, 0, 0, 40));
	view->FillRoundRect(rect.OffsetByCopy(0, 1), radius, radius);

	view->SetDrawingMode(B_OP_COPY);
	BGradientLinear glass(rect.LeftTop(), rect.LeftBottom());
	glass.AddColor(color(232, 237, 226), 0);
	glass.AddColor(color(200, 210, 190), 255);
	view->FillRoundRect(rect, radius, radius, glass);
	view->SetHighColor(color(143, 151, 138));
	view->SetPenSize(1);
	view->StrokeRoundRect(rect, radius, radius);

	view->SetDrawingMode(B_OP_ALPHA);
	view->SetHighColor(color(255, 255, 255, 120));
	view->StrokeLine(BPoint(rect.left + radius, rect.top + 1),
		BPoint(rect.right - radius, rect.top + 1));
	view->PopState();
}


void
hud_panel(BView* view, BRect rect)
{
	view->PushState();
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	float radius = 10;
	BGradientLinear glass(rect.LeftTop(), rect.LeftBottom());
	glass.AddColor(color(62, 62, 66), 0);
	glass.AddColor(color(34, 34, 38), 120);
	glass.AddColor(color(22, 22, 26), 255);
	view->FillRoundRect(rect, radius, radius, glass);
	view->SetHighColor(color(255, 255, 255, 50));
	view->StrokeRoundRect(rect.InsetByCopy(1, 1), radius - 1, radius - 1);
	view->SetHighColor(color(0, 0, 0, 200));
	view->StrokeRoundRect(rect, radius, radius);
	view->PopState();
}

}	// namespace look
}	// namespace airtime
