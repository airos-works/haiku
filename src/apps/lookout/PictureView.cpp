/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "PictureView.h"

#include <algorithm>
#include <math.h>

#include <Catalog.h>
#include <LayoutUtils.h>
#include <Region.h>

#include "Engine.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "PictureView"


// Told apart on a photograph and from each other, also by people who do
// not see all colors.
static const rgb_color kClassColors[] = {
	{ 255, 193, 7, 255 },
	{ 0, 230, 118, 255 },
	{ 64, 196, 255, 255 },
	{ 255, 110, 64, 255 },
	{ 234, 128, 252, 255 },
	{ 255, 255, 141, 255 },
	{ 100, 255, 218, 255 },
	{ 255, 138, 128, 255 }
};
static const int32 kClassColorCount
	= sizeof(kClassColors) / sizeof(kClassColors[0]);


PictureView::PictureView()
	:
	BView("picture", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE)
{
	// everything is drawn in Draw(): no flicker between frames
	SetViewColor(B_TRANSPARENT_COLOR);
}


PictureView::~PictureView()
{
}


void
PictureView::SetFrame(Snapshot* frame)
{
	if (frame == fFrame.Get())
		return;

	fFrame.SetTo(frame);
	Invalidate();
}


void
PictureView::SetResult(const BMessage* result)
{
	if (result != NULL)
		fResult = *result;
	else
		fResult.MakeEmpty();
	Invalidate();
}


void
PictureView::SetNote(const char* note)
{
	if (fNote == note)
		return;

	fNote = note;
	Invalidate();
}


void
PictureView::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	rgb_color background = tint_color(ui_color(B_PANEL_BACKGROUND_COLOR),
		B_DARKEN_2_TINT);

	if (!fFrame.IsSet()) {
		SetHighColor(background);
		FillRect(bounds);
		_DrawNote(false);
		return;
	}

	BRect pictureRect = _PictureRect();

	BRegion around(bounds);
	around.Exclude(pictureRect);
	SetHighColor(background);
	FillRegion(&around);

	BBitmap* bitmap = fFrame->Bitmap();
	SetDrawingMode(B_OP_COPY);
	DrawBitmap(bitmap, bitmap->Bounds(), pictureRect,
		B_FILTER_BITMAP_BILINEAR);

	_DrawBoxes(pictureRect);
	_DrawNote(true);
}


BSize
PictureView::MinSize()
{
	float size = be_plain_font->Size();
	return BLayoutUtils::ComposeSize(ExplicitMinSize(),
		BSize(roundf(size * 16), roundf(size * 12)));
}


BSize
PictureView::PreferredSize()
{
	float size = be_plain_font->Size();
	return BLayoutUtils::ComposeSize(ExplicitPreferredSize(),
		BSize(roundf(size * 48), roundf(size * 36)));
}


BSize
PictureView::MaxSize()
{
	return BLayoutUtils::ComposeSize(ExplicitMaxSize(),
		BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
}


/*static*/ rgb_color
PictureView::ColorFor(int32 classIndex)
{
	if (classIndex < 0)
		classIndex = -classIndex;
	return kClassColors[classIndex % kClassColorCount];
}


/*static*/ rgb_color
PictureView::TextColorOn(rgb_color color)
{
	// black on the light ones, white on the dark ones
	float brightness = 0.299f * color.red + 0.587f * color.green
		+ 0.114f * color.blue;
	if (brightness > 140)
		return make_color(0, 0, 0);
	return make_color(255, 255, 255);
}


//!	Where the picture goes: as large as fits, in the middle.
BRect
PictureView::_PictureRect() const
{
	BRect bounds = Bounds();
	BRect picture = fFrame->Bitmap()->Bounds();
	float pictureWidth = picture.Width() + 1;
	float pictureHeight = picture.Height() + 1;

	float scale = std::min((bounds.Width() + 1) / pictureWidth,
		(bounds.Height() + 1) / pictureHeight);
	float width = std::max(1.0f, floorf(pictureWidth * scale));
	float height = std::max(1.0f, floorf(pictureHeight * scale));

	BRect rect(0, 0, width - 1, height - 1);
	rect.OffsetTo(bounds.left + floorf((bounds.Width() + 1 - width) / 2),
		bounds.top + floorf((bounds.Height() + 1 - height) / 2));
	return rect;
}


void
PictureView::_DrawBoxes(BRect pictureRect)
{
	if (fResult.GetInt32("kind", MODEL_CLASSIFICATION) != MODEL_DETECTION
		|| !fResult.HasRect("box")) {
		return;
	}

	BRect bounds = Bounds();

	// Every measure follows the font, which follows the user's settings
	// and the display's density.
	BFont font(be_bold_font);
	SetFont(&font);
	font_height fontHeight;
	font.GetHeight(&fontHeight);
	float unit = std::max(1.0f, floorf(font.Size() / 8));
	float pen = 2 * unit;
	float padding = std::max(2.0f, floorf(font.Size() / 4));
	float tagHeight = ceilf(fontHeight.ascent) + ceilf(fontHeight.descent)
		+ padding;

	const rgb_color black = make_color(0, 0, 0);

	// first all boxes, then all names: a name is not to be struck through
	// by the box of the thing next to it
	int32 count = 0;
	BRect box;
	for (; fResult.FindRect("box", count, &box) == B_OK; count++) {
		BRect rect = _BoxRect(box, pictureRect);
		if (!rect.IsValid())
			continue;

		// A colored line around the thing, not on it (small things would
		// vanish under it), between two dark ones: seen on snow and on
		// coal.
		SetHighColor(black);
		StrokeRect(rect);
		SetHighColor(ColorFor(fResult.GetInt32("class", count, 0)));
		for (float i = 1; i <= pen; i++)
			StrokeRect(rect.InsetByCopy(-i, -i));
		SetHighColor(black);
		for (float i = pen + 1; i <= pen + unit; i++)
			StrokeRect(rect.InsetByCopy(-i, -i));
	}

	// the names, the surest last: on top where they are crowded
	for (int32 i = count - 1; i >= 0; i--) {
		if (fResult.FindRect("box", i, &box) != B_OK)
			continue;
		BRect rect = _BoxRect(box, pictureRect);
		if (!rect.IsValid())
			continue;

		BString text(B_TRANSLATE_COMMENT("%label% %score%%",
			"A thing found in a picture and how sure the model is of it, in "
			"percent: \"dog 87%\""));
		BString score;
		score.SetToFormat("%d",
			(int)roundf(fResult.GetFloat("score", i, 0) * 100));
		text.ReplaceFirst("%label%", fResult.GetString("label", i, ""));
		text.ReplaceFirst("%score%", score);

		// the name sits on the box, or in it where there is no room above
		float edge = pen + unit;
		BRect tag(0, 0, ceilf(StringWidth(text.String())) + 2 * padding - 1,
			tagHeight - 1);
		tag.OffsetTo(rect.left - edge + unit,
			rect.top - edge - tagHeight - unit);
		if (tag.top - unit < bounds.top)
			tag.OffsetTo(tag.left, rect.top + unit);
		if (tag.right + unit > bounds.right)
			tag.OffsetBy(bounds.right - unit - tag.right, 0);
		if (tag.left - unit < bounds.left)
			tag.OffsetTo(bounds.left + unit, tag.top);

		rgb_color color = ColorFor(fResult.GetInt32("class", i, 0));
		SetHighColor(black);
		FillRect(tag.InsetByCopy(-unit, -unit));
		SetHighColor(color);
		FillRect(tag);

		SetHighColor(TextColorOn(color));
		SetLowColor(color);
		DrawString(text.String(), BPoint(tag.left + padding,
			tag.top + floorf(padding / 2) + ceilf(fontHeight.ascent)));
	}
}


//!	Where a box given in the picture's pixels is in the view.
BRect
PictureView::_BoxRect(BRect box, BRect pictureRect) const
{
	BRect picture = fFrame->Bitmap()->Bounds();
	float scaleX = (pictureRect.Width() + 1) / (picture.Width() + 1);
	float scaleY = (pictureRect.Height() + 1) / (picture.Height() + 1);

	// a pixel's far edge is one further
	BRect rect(roundf(pictureRect.left + box.left * scaleX),
		roundf(pictureRect.top + box.top * scaleY),
		roundf(pictureRect.left + (box.right + 1) * scaleX) - 1,
		roundf(pictureRect.top + (box.bottom + 1) * scaleY) - 1);
	return rect & pictureRect;
}


void
PictureView::_DrawNote(bool plate)
{
	if (fNote.IsEmpty())
		return;

	BRect bounds = Bounds();
	BFont font(be_plain_font);
	SetFont(&font);
	font_height fontHeight;
	font.GetHeight(&fontHeight);
	float padding = ceilf(font.Size() * 0.75f);

	BString text(fNote);
	font.TruncateString(&text, B_TRUNCATE_END,
		std::max(0.0f, bounds.Width() - 4 * padding));
	float width = ceilf(font.StringWidth(text.String()));
	float height = ceilf(fontHeight.ascent) + ceilf(fontHeight.descent);

	BPoint where(bounds.left + floorf((bounds.Width() - width) / 2),
		bounds.top + floorf((bounds.Height() - height) / 2)
			+ ceilf(fontHeight.ascent));

	rgb_color panel = ui_color(B_PANEL_BACKGROUND_COLOR);
	if (plate) {
		BRect rect(where.x - padding, where.y - ceilf(fontHeight.ascent)
			- padding / 2, where.x + width + padding,
			where.y + ceilf(fontHeight.descent) + padding / 2);
		SetHighColor(panel);
		FillRoundRect(rect, padding / 2, padding / 2);
		SetHighColor(tint_color(panel, B_DARKEN_3_TINT));
		StrokeRoundRect(rect, padding / 2, padding / 2);
		SetLowColor(panel);
	} else
		SetLowColor(tint_color(panel, B_DARKEN_2_TINT));

	SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
	DrawString(text.String(), where);
}
