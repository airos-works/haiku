/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "LevelView.h"

#include <math.h>

#include <algorithm>

#include <Catalog.h>
#include <GradientLinear.h>
#include <NumberFormat.h>
#include <Window.h>

#include "LevelIcons.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "LevelView"


// sizes for a 12 point system font, scaled with it
static const float kMargin = 16;
static const float kTopMargin = 14;
static const float kIconSize = 64;
static const float kIconSpacing = 8;
static const float kTextSpacing = 5;
static const int32 kSegments = 16;
static const float kSegmentWidth = 9;
static const float kSegmentHeight = 10;
static const float kSegmentSpacing = 3;


LevelView::LevelView()
	:
	BView("level", B_WILL_DRAW | B_SUBPIXEL_PRECISE),
	fScale(std::max(1.0f, be_plain_font->Size() / 12)),
	fKind(B_OSD_VOLUME),
	fLevel(0),
	fMuted(false)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


void
LevelView::SetLevel(const BMessage* message)
{
	int32 kind;
	if (message->FindInt32("kind", &kind) != B_OK)
		kind = B_OSD_VOLUME;
	fKind = (osd_level_kind)kind;

	float level;
	if (message->FindFloat("level", &level) != B_OK || isnan(level))
		level = 0;
	fLevel = std::min(1.0f, std::max(0.0f, level));
	fMuted = message->GetBool("muted", false);

	fLabel = fKind == B_OSD_BRIGHTNESS
		? B_TRANSLATE("Brightness") : B_TRANSLATE("Volume");

	float gain;
	if (fMuted)
		fValue = B_TRANSLATE("Muted");
	else if (message->FindFloat("gain", &gain) == B_OK) {
		// whole decibels for the volume keys' steps, a tenth otherwise
		BString number;
		if (fabsf(gain - roundf(gain)) < 0.05f)
			number.SetToFormat("%d", (int)roundf(gain));
		else
			number.SetToFormat("%.1f", gain);
		number.ReplaceFirst("-", "\xe2\x88\x92");
			// a real minus sign
		fValue.SetToFormat(B_TRANSLATE_COMMENT("%s dB",
			"The volume in decibels, like \"-12 dB\""), number.String());
	} else
		BNumberFormat().FormatPercent(fValue, roundf(fLevel * 100) / 100);

	Invalidate();
}


void
LevelView::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	rgb_color base = ui_color(B_PANEL_BACKGROUND_COLOR);

	SetHighColor(base);
	FillRect(bounds);
	_DrawFrame(bounds, base);

	float iconSize = _Scaled(kIconSize);
	BRect iconFrame(0, 0, iconSize, iconSize);
	iconFrame.OffsetTo(roundf((bounds.Width() - iconSize) / 2),
		_Scaled(kTopMargin));

	SetDrawingMode(B_OP_ALPHA);
	SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	if (fKind == B_OSD_BRIGHTNESS)
		DrawSunIcon(this, iconFrame);
	else
		DrawSpeakerIcon(this, iconFrame, fLevel, fMuted);
	SetDrawingMode(B_OP_OVER);

	// the label and the value above the meter, at its ends
	float left = _Scaled(kMargin);
	float right = bounds.Width() - _Scaled(kMargin);

	font_height fontHeight;
	be_bold_font->GetHeight(&fontHeight);
	float baseline = ceilf(iconFrame.bottom + _Scaled(kIconSpacing)
		+ fontHeight.ascent);

	SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
	SetLowColor(base);
	SetFont(be_bold_font);
	DrawString(fLabel.String(), BPoint(left, baseline));

	SetFont(be_plain_font);
	DrawString(fValue.String(),
		BPoint(right - StringWidth(fValue.String()), baseline));

	BRect meter(left, baseline + fontHeight.descent + _Scaled(kTextSpacing),
		right, 0);
	meter.bottom = meter.top + _Scaled(kSegmentHeight);
	_DrawMeter(meter, base);
}


void
LevelView::MouseDown(BPoint where)
{
	Window()->Hide();
}


void
LevelView::GetPreferredSize(float* _width, float* _height)
{
	font_height fontHeight;
	be_bold_font->GetHeight(&fontHeight);

	float meterWidth = kSegments * kSegmentWidth
		+ (kSegments - 1) * kSegmentSpacing;

	if (_width != NULL)
		*_width = ceilf(_Scaled(meterWidth + 2 * kMargin));
	if (_height != NULL) {
		*_height = ceilf(_Scaled(kTopMargin + kIconSize + kIconSpacing
				+ kTextSpacing + kSegmentHeight + kMargin)
			+ fontHeight.ascent + fontHeight.descent);
	}
}


float
LevelView::_Scaled(float value) const
{
	return roundf(value * fScale);
}


void
LevelView::_DrawFrame(BRect frame, rgb_color base)
{
	// a raised panel with a dark rim, as BeOS drew its panels
	bool dark = base.IsDark();
	rgb_color rim = tint_color(base, dark ? B_DARKEN_4_TINT : B_DARKEN_3_TINT);
	rgb_color light = tint_color(base,
		dark ? B_LIGHTEN_1_TINT : B_LIGHTEN_MAX_TINT);
	rgb_color shadow = tint_color(base, B_DARKEN_2_TINT);

	SetHighColor(rim);
	StrokeRect(frame);
	frame.InsetBy(1, 1);

	for (int32 i = 0; i < (int32)fScale; i++) {
		BeginLineArray(4);
		AddLine(frame.LeftBottom(), frame.LeftTop(), light);
		AddLine(frame.LeftTop(), frame.RightTop(), light);
		AddLine(frame.RightTop(), frame.RightBottom(), shadow);
		AddLine(frame.RightBottom(), frame.LeftBottom(), shadow);
		EndLineArray();
		frame.InsetBy(1, 1);
	}
}


void
LevelView::_DrawMeter(BRect frame, rgb_color base)
{
	// a row of sunken lights; the last lit one may be lit only in part
	bool dark = base.IsDark();
	rgb_color shadow = tint_color(base, dark ? B_DARKEN_4_TINT
		: B_DARKEN_2_TINT);
	rgb_color light = tint_color(base,
		dark ? B_LIGHTEN_1_TINT : B_LIGHTEN_MAX_TINT);
	rgb_color off = tint_color(base, dark ? B_DARKEN_2_TINT : B_DARKEN_1_TINT);
	rgb_color on = fMuted ? tint_color(base, dark ? B_LIGHTEN_2_TINT
		: B_DARKEN_3_TINT) : ui_color(B_CONTROL_HIGHLIGHT_COLOR);

	float width = (frame.Width() - (kSegments - 1) * _Scaled(kSegmentSpacing))
		/ kSegments;
	float lit = fLevel * kSegments;

	for (int32 i = 0; i < kSegments; i++) {
		BRect segment(frame.left + i * (width + _Scaled(kSegmentSpacing)),
			frame.top, 0, frame.bottom);
		segment.right = segment.left + width;

		BeginLineArray(4);
		AddLine(segment.LeftBottom(), segment.LeftTop(), shadow);
		AddLine(segment.LeftTop(), segment.RightTop(), shadow);
		AddLine(segment.RightTop(), segment.RightBottom(), light);
		AddLine(segment.RightBottom(), segment.LeftBottom(), light);
		EndLineArray();

		BRect inside = segment.InsetByCopy(1, 1);
		SetHighColor(off);
		FillRect(inside);

		float fill = std::min(1.0f, lit - i);
		if (fill <= 0)
			continue;

		BRect litPart = inside;
		litPart.right = litPart.left + (inside.Width() + 1) * fill - 1;
		BGradientLinear gradient(litPart.LeftTop(), litPart.LeftBottom());
		gradient.AddColor(tint_color(on, B_LIGHTEN_1_TINT), 0);
		gradient.AddColor(on, 160);
		gradient.AddColor(tint_color(on, B_DARKEN_1_TINT), 255);
		FillRect(litPart, gradient);
	}
}
