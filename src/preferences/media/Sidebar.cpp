/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "Sidebar.h"

#include <algorithm>
#include <math.h>

#include <ControlLook.h>
#include <Window.h>

#include "MediaMessages.h"


static float
ui_scale()
{
	return std::max(1.0f, be_plain_font->Size() / 12.0f);
}


static BFont
subtitle_font()
{
	BFont font(be_plain_font);
	font.SetSize(ceilf(font.Size() * 0.85f));
	return font;
}


static BFont
heading_font()
{
	BFont font(be_bold_font);
	font.SetSize(ceilf(font.Size() * 0.85f));
	return font;
}


static float
font_line_height(const BFont& font)
{
	font_height height;
	font.GetHeight(&height);
	return ceilf(height.ascent + height.descent + height.leading);
}


rgb_color
sidebar_color()
{
	return mix_color(ui_color(B_PANEL_BACKGROUND_COLOR),
		ui_color(B_PANEL_TEXT_COLOR), 14);
}


static rgb_color
selection_color()
{
	return ui_color(B_CONTROL_HIGHLIGHT_COLOR);
}


static rgb_color
selection_text_color()
{
	rgb_color background = selection_color();
	return background.IsLight() ? make_color(0, 0, 0)
		: make_color(255, 255, 255);
}


// #pragma mark - SidebarItem


SidebarItem::SidebarItem(const char* heading)
	:
	BListItem(),
	fTitle(heading),
	fGlyph(GLYPH_SPEAKER),
	fColor(make_color(0, 0, 0))
{
	SetEnabled(false);
}


SidebarItem::SidebarItem(const char* key, const char* title,
	const char* subtitle, glyph_kind glyph, rgb_color color)
	:
	BListItem(),
	fKey(key),
	fTitle(title),
	fSubtitle(subtitle),
	fGlyph(glyph),
	fColor(color)
{
	fTitle.Trim();
}


void
SidebarItem::Update(BView* owner, const BFont* font)
{
	BListItem::Update(owner, font);

	float scale = ui_scale();
	if (IsHeading()) {
		SetHeight(font_line_height(heading_font()) + ceilf(14 * scale));
		return;
	}

	float text = font_line_height(*be_plain_font);
	if (!fSubtitle.IsEmpty())
		text += font_line_height(subtitle_font());
	float icon = ceilf(28 * scale);
	SetHeight(std::max(icon, text) + ceilf(10 * scale));
}


void
SidebarItem::DrawItem(BView* owner, BRect frame, bool complete)
{
	float scale = ui_scale();
	owner->PushState();

	rgb_color background = owner->ViewColor();
	owner->SetLowColor(background);
	owner->FillRect(frame, B_SOLID_LOW);
	owner->SetDrawingMode(B_OP_OVER);

	float inset = ceilf(8 * scale);

	if (IsHeading()) {
		BFont font = heading_font();
		owner->SetFont(&font);
		owner->SetHighColor(mix_color(ui_color(B_PANEL_TEXT_COLOR),
			background, 110));
		font_height height;
		font.GetHeight(&height);
		BString text(fTitle);
		owner->TruncateString(&text, B_TRUNCATE_END,
			frame.Width() - inset * 2);
		owner->DrawString(text, BPoint(frame.left + inset + 2,
			frame.bottom - ceilf(4 * scale) - ceilf(height.descent)));
		owner->PopState();
		return;
	}

	rgb_color textColor = ui_color(B_PANEL_TEXT_COLOR);
	rgb_color subtitleColor = mix_color(textColor, background, 100);
	if (IsSelected()) {
		BRect selection = frame.InsetByCopy(ceilf(5 * scale),
			ceilf(1 * scale));
		float radius = ceilf(6 * scale);
		owner->SetHighColor(selection_color());
		owner->FillRoundRect(selection, radius, radius);
		textColor = selection_text_color();
		subtitleColor = mix_color(textColor, selection_color(), 60);
	}

	float iconSize = ceilf(28 * scale);
	BRect icon(frame.left + inset, 0, frame.left + inset + iconSize - 1, 0);
	icon.top = floorf(frame.top + (frame.Height() - iconSize) / 2);
	icon.bottom = icon.top + iconSize - 1;
	draw_glyph_badge(owner, icon, fGlyph, fColor);

	float left = icon.right + ceilf(8 * scale);
	float width = frame.right - inset - left;

	BFont titleFont(be_plain_font);
	BFont smallFont = subtitle_font();
	font_height titleHeight;
	font_height smallHeight;
	titleFont.GetHeight(&titleHeight);
	smallFont.GetHeight(&smallHeight);

	float textHeight = font_line_height(titleFont);
	if (!fSubtitle.IsEmpty())
		textHeight += font_line_height(smallFont);
	float top = floorf(frame.top + (frame.Height() - textHeight) / 2);

	BString title(fTitle);
	owner->SetFont(&titleFont);
	owner->TruncateString(&title, B_TRUNCATE_END, width);
	owner->SetHighColor(textColor);
	owner->DrawString(title, BPoint(left, top + ceilf(titleHeight.ascent)));

	if (!fSubtitle.IsEmpty()) {
		BString subtitle(fSubtitle);
		owner->SetFont(&smallFont);
		owner->TruncateString(&subtitle, B_TRUNCATE_END, width);
		owner->SetHighColor(subtitleColor);
		owner->DrawString(subtitle, BPoint(left, top
			+ font_line_height(titleFont) + ceilf(smallHeight.ascent)));
	}

	owner->PopState();
}


// #pragma mark - Sidebar


Sidebar::Sidebar()
	:
	BListView("sidebar", B_SINGLE_SELECTION_LIST, B_WILL_DRAW | B_NAVIGABLE
		| B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
	fLastSelection(-1),
	fAdjusting(false)
{
	SetViewColor(sidebar_color());
	SetLowColor(sidebar_color());
}


void
Sidebar::AttachedToWindow()
{
	BListView::AttachedToWindow();
	SetViewColor(sidebar_color());
	SetLowColor(sidebar_color());
}


bool
Sidebar::SelectKey(const char* key)
{
	for (int32 i = 0; i < CountItems(); i++) {
		SidebarItem* item = static_cast<SidebarItem*>(ItemAt(i));
		if (!item->IsHeading() && item->Key() == key) {
			fAdjusting = true;
			Select(i);
			// keep the heading above the first page in view
			if (Bounds().Height() < ItemFrame(i).Height()) {
				// not laid out yet
			} else if (i > 0 && static_cast<SidebarItem*>(ItemAt(i - 1))->IsHeading()
				&& ItemFrame(i - 1).top < Bounds().top) {
				ScrollTo(0, ItemFrame(i - 1).top);
			} else if (!Bounds().Contains(ItemFrame(i)))
				ScrollToSelection();
			fLastSelection = i;
			fAdjusting = false;
			return true;
		}
	}
	return false;
}


BString
Sidebar::SelectedKey() const
{
	SidebarItem* item = static_cast<SidebarItem*>(ItemAt(CurrentSelection()));
	if (item == NULL)
		return BString();
	return item->Key();
}


void
Sidebar::SelectionChanged()
{
	BListView::SelectionChanged();
	if (fAdjusting)
		return;

	int32 index = CurrentSelection();
	SidebarItem* item = static_cast<SidebarItem*>(ItemAt(index));
	if (item != NULL && item->IsHeading()) {
		// Skip headings in the direction the selection moved.
		int32 direction = index < fLastSelection ? -1 : 1;
		int32 next = index + direction;
		while (next >= 0 && next < CountItems()
			&& static_cast<SidebarItem*>(ItemAt(next))->IsHeading()) {
			next += direction;
		}
		if (next < 0 || next >= CountItems())
			next = fLastSelection;

		fAdjusting = true;
		if (next >= 0)
			Select(next);
		else
			DeselectAll();
		fAdjusting = false;
		index = next;
		item = static_cast<SidebarItem*>(ItemAt(index));
	}

	if (item == NULL || index == fLastSelection)
		return;
	fLastSelection = index;

	BMessage message(kMsgSidebarSelection);
	message.AddString("key", item->Key());
	Window()->PostMessage(&message);
}


void
Sidebar::_ScrollBy(float delta)
{
	// there is no scroll bar, the wheel and the keyboard scroll
	float height = CountItems() > 0
		? ItemFrame(CountItems() - 1).bottom + 1 : 0;
	float maxTop = std::max(0.0f, height - Bounds().Height() - 1);
	float top = std::max(0.0f, std::min(maxTop, Bounds().top + delta));
	if (top != Bounds().top)
		ScrollTo(0, top);
}


void
Sidebar::MessageReceived(BMessage* message)
{
	if (message->what == B_MOUSE_WHEEL_CHANGED) {
		float delta = 0;
		if (message->FindFloat("be:wheel_delta_y", &delta) == B_OK
			&& delta != 0) {
			_ScrollBy(delta * ceilf(be_plain_font->Size() * 3));
		}
		return;
	}

	if (message->what == B_COLORS_UPDATED) {
		SetViewColor(sidebar_color());
		SetLowColor(sidebar_color());
		Invalidate();
	}
	BListView::MessageReceived(message);
}
