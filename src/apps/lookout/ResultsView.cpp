/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "ResultsView.h"

#include <algorithm>
#include <math.h>

#include <Catalog.h>
#include <LayoutUtils.h>

#include "Engine.h"
#include "PictureView.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "ResultsView"


ResultsView::ResultsView()
	:
	BView("results", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
	fHasResult(false)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
}


ResultsView::~ResultsView()
{
}


void
ResultsView::SetResult(const BMessage* result)
{
	fHasResult = result != NULL;
	if (result != NULL)
		fResult = *result;
	else
		fResult.MakeEmpty();
	Invalidate();
}


void
ResultsView::Draw(BRect updateRect)
{
	if (!fHasResult)
		return;

	BRect bounds = Bounds();
	BFont font(be_plain_font);
	SetFont(&font);
	font_height fontHeight;
	font.GetHeight(&fontHeight);

	float inset = _Inset();
	float rowHeight = _RowHeight();
	float padding = floorf(font.Size() / 2);
	rgb_color panel = ui_color(B_PANEL_BACKGROUND_COLOR);
	rgb_color textColor = ui_color(B_PANEL_TEXT_COLOR);

	bool detection = fResult.GetInt32("kind", MODEL_CLASSIFICATION)
		== MODEL_DETECTION;

	if (!fResult.HasString("label")) {
		SetHighColor(tint_color(textColor, panel.IsLight()
			? B_LIGHTEN_1_TINT : B_DARKEN_1_TINT));
		DrawString(detection ? B_TRANSLATE("Nothing found that the model "
				"knows") : B_TRANSLATE("The model has no answer"),
			BPoint(bounds.left + inset + padding, bounds.top + inset
				+ floorf((rowHeight - ceilf(fontHeight.ascent)
					- ceilf(fontHeight.descent)) / 2)
				+ ceilf(fontHeight.ascent)));
		return;
	}

	// Scores of classification models are shares of 1 when the model ends
	// in a softmax; some give raw numbers instead, which are shown as
	// they are.
	bool shares = true;
	float score;
	for (int32 i = 0; fResult.FindFloat("score", i, &score) == B_OK; i++) {
		if (score < 0 || score > 1.001f)
			shares = false;
	}

	const char* label;
	for (int32 i = 0; i < kRows
			&& fResult.FindString("label", i, &label) == B_OK; i++) {
		score = fResult.GetFloat("score", i, 0);

		BRect row(bounds.left + inset, bounds.top + inset + i * rowHeight,
			bounds.right - inset, bounds.top + inset + (i + 1) * rowHeight
				- 2);

		// the bar: in the color of the box for things that have one
		rgb_color barColor = detection
			? PictureView::ColorFor(fResult.GetInt32("class", i, 0))
			: ui_color(B_CONTROL_HIGHLIGHT_COLOR);
		barColor = mix_color(panel, barColor, detection ? 150 : 110);
		if (shares && score > 0) {
			BRect bar = row;
			bar.right = bar.left + floorf(row.Width() * std::min(score,
				1.0f));
			SetHighColor(barColor);
			FillRect(bar);
		}

		BString number;
		if (shares)
			number.SetToFormat("%.1f%%", score * 100);
		else
			number.SetToFormat("%.2f", score);
		float numberWidth = ceilf(font.StringWidth(number.String()));

		BString text(label);
		font.TruncateString(&text, B_TRUNCATE_END, std::max(0.0f,
			row.Width() - numberWidth - 3 * padding));

		float baseline = row.top + floorf((row.Height() + 1
			- ceilf(fontHeight.ascent) - ceilf(fontHeight.descent)) / 2)
			+ ceilf(fontHeight.ascent);

		SetHighColor(textColor);
		SetDrawingMode(B_OP_OVER);
		DrawString(text.String(), BPoint(row.left + padding, baseline));
		DrawString(number.String(),
			BPoint(row.right - padding - numberWidth, baseline));
		SetDrawingMode(B_OP_COPY);
	}
}


BSize
ResultsView::MinSize()
{
	return BLayoutUtils::ComposeSize(ExplicitMinSize(),
		BSize(be_plain_font->Size() * 10,
			kRows * _RowHeight() + 2 * _Inset()));
}


BSize
ResultsView::PreferredSize()
{
	return BLayoutUtils::ComposeSize(ExplicitPreferredSize(),
		BSize(be_plain_font->Size() * 30,
			kRows * _RowHeight() + 2 * _Inset()));
}


BSize
ResultsView::MaxSize()
{
	return BLayoutUtils::ComposeSize(ExplicitMaxSize(),
		BSize(B_SIZE_UNLIMITED, kRows * _RowHeight() + 2 * _Inset()));
}


float
ResultsView::_RowHeight() const
{
	font_height fontHeight;
	be_plain_font->GetHeight(&fontHeight);
	return ceilf(fontHeight.ascent) + ceilf(fontHeight.descent)
		+ ceilf(fontHeight.leading) + floorf(be_plain_font->Size() / 2) + 2;
}


float
ResultsView::_Inset() const
{
	return floorf(be_plain_font->Size() / 2);
}
