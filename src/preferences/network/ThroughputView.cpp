/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "ThroughputView.h"

#include <algorithm>

#include <Catalog.h>
#include <ControlLook.h>
#include <LayoutUtils.h>
#include <Shape.h>
#include <StringForRate.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "ThroughputView"


static const size_t kMaxSamples = 120;
static const double kMinimumScale = 1024.0;


static bool
IsDark(rgb_color color)
{
	return color.Brightness() < 128;
}


/*!	The download/upload colours of AirTop, darkened on light panels to keep
	their contrast.
*/
static rgb_color
ReceiveColor(bool dark)
{
	return dark ? make_color(133, 219, 130) : make_color(40, 150, 60);
}


static rgb_color
SendColor(bool dark)
{
	return dark ? make_color(65, 213, 224) : make_color(0, 140, 170);
}


ThroughputView::ThroughputView()
	:
	BView("throughput", B_WILL_DRAW | B_FRAME_EVENTS
		| B_FULL_UPDATE_ON_RESIZE),
	fLastReceived(0),
	fLastSent(0),
	fLastTime(0),
	fHasLast(false),
	fScale(kMinimumScale)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
}


ThroughputView::~ThroughputView()
{
}


void
ThroughputView::Reset()
{
	fSamples.clear();
	fHasLast = false;
	fScale = kMinimumScale;
	Invalidate();
}


void
ThroughputView::AddSample(uint64 receivedBytes, uint64 sentBytes)
{
	bigtime_t now = system_time();
	if (fHasLast && now > fLastTime && receivedBytes >= fLastReceived
		&& sentBytes >= fLastSent) {
		double seconds = (now - fLastTime) / 1000000.0;
		Sample sample;
		sample.receive = (receivedBytes - fLastReceived) / seconds;
		sample.send = (sentBytes - fLastSent) / seconds;
		fSamples.push_back(sample);
		while (fSamples.size() > kMaxSamples)
			fSamples.pop_front();
	}

	fLastReceived = receivedBytes;
	fLastSent = sentBytes;
	fLastTime = now;
	fHasLast = true;

	// Ease the scale towards the peak, so the curves don't jump
	double target = std::max(kMinimumScale, _Maximum() * 1.15);
	if (target > fScale)
		fScale = target;
	else
		fScale += (target - fScale) * 0.2;

	Invalidate();
}


void
ThroughputView::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	rgb_color panel = ViewColor();
	bool dark = IsDark(panel);
	rgb_color text = ui_color(B_PANEL_TEXT_COLOR);
	rgb_color muted = mix_color(text, panel, 150);
	rgb_color background = tint_color(panel,
		dark ? B_LIGHTEN_1_TINT : 1.06f);
	rgb_color border = tint_color(panel, dark ? B_LIGHTEN_2_TINT
		: B_DARKEN_2_TINT);
	rgb_color grid = mix_color(background, border, 110);

	SetDrawingMode(B_OP_COPY);
	SetHighColor(panel);
	FillRect(bounds);

	float radius = be_control_look->DefaultLabelSpacing();
	BRect box = bounds.InsetByCopy(0.5f, 0.5f);
	SetHighColor(background);
	FillRoundRect(box, radius, radius);
	SetHighColor(border);
	StrokeRoundRect(box, radius, radius);

	font_height height;
	GetFontHeight(&height);
	float lineHeight = ceilf(height.ascent + height.descent);
	float spacing = be_control_look->DefaultLabelSpacing();

	// The current rates as a headline

	Sample current = { 0, 0 };
	if (!fSamples.empty())
		current = fSamples.back();

	char rate[64];
	float baseline = box.top + spacing + ceilf(height.ascent);
	float x = box.left + spacing * 1.5f;

	BFont bold(be_bold_font);
	bold.SetSize(be_plain_font->Size());
	SetFont(&bold);
	SetLowColor(background);

	BString down("↓ ");
	down << string_for_rate(current.receive, rate, sizeof(rate));
	SetHighColor(ReceiveColor(dark));
	DrawString(down, BPoint(x, baseline));
	x += StringWidth(down) + spacing * 3;

	BString up("↑ ");
	up << string_for_rate(current.send, rate, sizeof(rate));
	SetHighColor(SendColor(dark));
	DrawString(up, BPoint(x, baseline));

	SetFont(be_plain_font);
	BFont small(be_plain_font);
	small.SetSize(std::max(9.0f, be_plain_font->Size() * 0.85f));
	SetFont(&small);
	SetHighColor(muted);
	BString scale(string_for_rate(fScale, rate, sizeof(rate)));
	DrawString(scale, BPoint(box.right - spacing * 1.5f
		- StringWidth(scale), baseline));

	// The graph

	BRect graph(box.left + spacing * 1.5f, baseline + ceilf(height.descent)
			+ spacing, box.right - spacing * 1.5f, box.bottom - spacing);
	if (graph.Height() < lineHeight)
		return;

	SetHighColor(grid);
	for (int32 i = 1; i < 4; i++) {
		float y = roundf(graph.top + graph.Height() * i / 4) + 0.5f;
		StrokeLine(BPoint(graph.left, y), BPoint(graph.right, y),
			B_MIXED_COLORS);
	}

	if (fSamples.size() < 2) {
		SetHighColor(muted);
		const char* waiting = B_TRANSLATE("Measuring" B_UTF8_ELLIPSIS);
		DrawString(waiting, BPoint(graph.left + (graph.Width()
				- StringWidth(waiting)) / 2,
			graph.top + graph.Height() / 2 + height.ascent / 2));
		return;
	}

	_DrawCurve(graph, true, fScale, ReceiveColor(dark));
	_DrawCurve(graph, false, fScale, SendColor(dark));
}


void
ThroughputView::FrameResized(float width, float height)
{
	Invalidate();
}


BSize
ThroughputView::MinSize()
{
	font_height height;
	be_plain_font->GetHeight(&height);
	float lineHeight = ceilf(height.ascent + height.descent);
	return BLayoutUtils::ComposeSize(ExplicitMinSize(),
		BSize(lineHeight * 14, lineHeight * 5.5f));
}


BSize
ThroughputView::PreferredSize()
{
	font_height height;
	be_plain_font->GetHeight(&height);
	float lineHeight = ceilf(height.ascent + height.descent);
	return BLayoutUtils::ComposeSize(ExplicitPreferredSize(),
		BSize(lineHeight * 20, lineHeight * 6.5f));
}


void
ThroughputView::_DrawCurve(BRect rect, bool receive, double maximum,
	rgb_color color)
{
	// The newest sample sits at the right edge, older ones scroll left
	float step = rect.Width() / (kMaxSamples - 1);
	size_t count = fSamples.size();

	BShape line;
	BShape area;
	BPoint previous;
	for (size_t i = 0; i < count; i++) {
		double value = receive ? fSamples[i].receive : fSamples[i].send;
		float fraction = (float)std::min(1.0, std::max(0.0, value / maximum));
		BPoint point(rect.right - (count - 1 - i) * step,
			rect.bottom - rect.Height() * fraction);
		if (i == 0) {
			line.MoveTo(point);
			area.MoveTo(BPoint(point.x, rect.bottom));
			area.LineTo(point);
		} else {
			// Horizontal handles round the joins without overshooting the
			// measured values
			float third = (point.x - previous.x) / 3;
			BPoint controls[3] = {
				BPoint(previous.x + third, previous.y),
				BPoint(point.x - third, point.y),
				point
			};
			line.BezierTo(controls);
			area.BezierTo(controls);
		}
		previous = point;
	}
	area.LineTo(BPoint(previous.x, rect.bottom));
	area.Close();

	PushState();
	SetDrawingMode(B_OP_ALPHA);
	SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
	rgb_color fill = color;
	fill.alpha = receive ? 70 : 45;
	SetHighColor(fill);
	MovePenTo(B_ORIGIN);
	FillShape(&area);

	SetHighColor(color);
	SetPenSize(1.5f);
	SetLineMode(B_ROUND_CAP, B_ROUND_JOIN);
	MovePenTo(B_ORIGIN);
	StrokeShape(&line);
	SetPenSize(1.0f);
	FillEllipse(previous, 2.5f, 2.5f);
	PopState();
}


double
ThroughputView::_Maximum() const
{
	double maximum = 0;
	for (size_t i = 0; i < fSamples.size(); i++) {
		maximum = std::max(maximum, fSamples[i].receive);
		maximum = std::max(maximum, fSamples[i].send);
	}
	return maximum;
}
