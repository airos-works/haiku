/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "TabBarView.h"

#include <ControlLook.h>
#include <Window.h>

#include <algorithm>


namespace BPrivate {


TabBarView::TabBarView()
	:
	BView("tabs", B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
	fFrames(8),
	fSelected(0),
	fHover(-1),
	fDropTarget(-1)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
}


void
TabBarView::SetTabs(const BStringList& labels, int32 selected)
{
	fLabels = labels;
	fSelected = selected;
	fHover = -1;
	_Layout();
	Invalidate();
}


BSize
TabBarView::MinSize()
{
	font_height height;
	be_plain_font->GetHeight(&height);
	return BSize(100, ceilf(height.ascent + height.descent) + 12);
}


BSize
TabBarView::MaxSize()
{
	return BSize(B_SIZE_UNLIMITED, MinSize().height);
}


BSize
TabBarView::PreferredSize()
{
	return BSize(400, MinSize().height);
}


void
TabBarView::FrameResized(float width, float height)
{
	_Layout();
}


void
TabBarView::_Layout()
{
	// tabs share the width, none wider than a comfortable title
	BRect bounds(Bounds());
	float plusWidth = bounds.Height();
	int32 count = fLabels.CountStrings();
	float available = bounds.Width() - plusWidth - 4;
	float maxWidth = be_plain_font->StringWidth("M") * 18;
	float width = count > 0 ? std::min(maxWidth, available / count) : 0;

	fFrames.MakeEmpty();
	float x = bounds.left + 2;
	for (int32 i = 0; i < count; i++) {
		fFrames.AddItem(new BRect(x, bounds.top + 2, x + width - 1,
			bounds.bottom));
		x += width;
	}
	fPlusFrame.Set(x + 2, bounds.top + 2, x + 2 + plusWidth - 4,
		bounds.bottom - 2);
}


BRect
TabBarView::_CloseRect(int32 index) const
{
	BRect* frame = fFrames.ItemAt(index);
	if (frame == NULL)
		return BRect();
	float size = std::min(frame->Height() - 8, 12.0f);
	float top = frame->top + floorf((frame->Height() - size) / 2);
	return BRect(frame->right - size - 6, top, frame->right - 6, top + size);
}


void
TabBarView::Draw(BRect updateRect)
{
	_Layout();
	rgb_color base = ui_color(B_PANEL_BACKGROUND_COLOR);
	rgb_color text = ui_color(B_PANEL_TEXT_COLOR);
	font_height height;
	GetFontHeight(&height);

	// the line under the tabs, broken by the shown one
	BRect bounds(Bounds());
	SetHighColor(tint_color(base, B_DARKEN_2_TINT));
	StrokeLine(BPoint(bounds.left, bounds.bottom),
		BPoint(bounds.right, bounds.bottom));

	for (int32 i = 0; i < fLabels.CountStrings(); i++) {
		BRect frame(*fFrames.ItemAt(i));
		bool selected = i == fSelected;
		BRect tab(frame);
		if (selected) {
			be_control_look->DrawActiveTab(this, tab, updateRect, base, 0,
				BControlLook::B_ALL_BORDERS, BControlLook::B_TOP_BORDER, i, fSelected, 0,
				fLabels.CountStrings() - 1);
		} else {
			uint32 flags = i == fDropTarget ? BControlLook::B_ACTIVATED : 0;
			be_control_look->DrawInactiveTab(this, tab, updateRect, base,
				flags, BControlLook::B_ALL_BORDERS, BControlLook::B_TOP_BORDER, i, fSelected,
				0, fLabels.CountStrings() - 1);
		}

		BRect close(_CloseRect(i));
		BString label(fLabels.StringAt(i));
		float spacing = be_control_look->DefaultLabelSpacing();
		TruncateString(&label, B_TRUNCATE_END,
			close.left - frame.left - 2 * spacing);
		SetHighColor(text);
		SetLowColor(base);
		float baseline = floorf(frame.top + (frame.Height() + height.ascent
			- height.descent) / 2);
		DrawString(label.String(), BPoint(frame.left + spacing, baseline));

		if (selected || i == fHover) {
			// the close mark
			SetHighColor(tint_color(text, text.IsDark() ? B_LIGHTEN_1_TINT
				: B_DARKEN_1_TINT));
			BRect mark(close.InsetByCopy(2, 2));
			StrokeLine(mark.LeftTop(), mark.RightBottom());
			StrokeLine(mark.LeftBottom(), mark.RightTop());
		}
	}

	// the plus for another tab
	SetHighColor(tint_color(text, text.IsDark() ? B_LIGHTEN_1_TINT
		: B_DARKEN_1_TINT));
	BRect plus(fPlusFrame.InsetByCopy(fPlusFrame.Width() / 4,
		fPlusFrame.Height() / 4));
	float middleX = floorf((plus.left + plus.right) / 2);
	float middleY = floorf((plus.top + plus.bottom) / 2);
	SetPenSize(1.5);
	StrokeLine(BPoint(plus.left, middleY), BPoint(plus.right, middleY));
	StrokeLine(BPoint(middleX, plus.top), BPoint(middleX, plus.bottom));
	SetPenSize(1);
}


int32
TabBarView::_TabAt(BPoint where) const
{
	for (int32 i = 0; i < fFrames.CountItems(); i++) {
		if (fFrames.ItemAt(i)->Contains(where))
			return i;
	}
	return -1;
}


void
TabBarView::MouseDown(BPoint where)
{
	_Layout();
	int32 buttons = 0;
	if (Window()->CurrentMessage() != NULL)
		buttons = Window()->CurrentMessage()->GetInt32("buttons", 0);

	if (fPlusFrame.Contains(where)) {
		Window()->PostMessage(kNewTab);
		return;
	}

	int32 index = _TabAt(where);
	if (index < 0)
		return;

	if ((buttons & B_TERTIARY_MOUSE_BUTTON) != 0
		|| _CloseRect(index).Contains(where)) {
		BMessage close(kCloseTab);
		close.AddInt32("index", index);
		Window()->PostMessage(&close);
		return;
	}

	BMessage select(kSelectTab);
	select.AddInt32("index", index);
	Window()->PostMessage(&select);
}


void
TabBarView::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	int32 index = transit == B_EXITED_VIEW ? -1 : _TabAt(where);
	int32 drop = dragMessage != NULL && dragMessage->HasRef("refs") ? index : -1;
	if (index != fHover || drop != fDropTarget) {
		fHover = index;
		fDropTarget = drop;
		Invalidate();
	}
}


void
TabBarView::MessageReceived(BMessage* message)
{
	if (message->WasDropped() && message->HasRef("refs")) {
		BPoint where = message->DropPoint();
		ConvertFromScreen(&where);
		int32 index = _TabAt(where);
		fDropTarget = -1;
		Invalidate();
		if (index < 0)
			return;

		BMessage drop(*message);
		drop.what = kTabDrop;
		drop.AddInt32("index", index);
		Window()->PostMessage(&drop);
		return;
	}

	BView::MessageReceived(message);
}


} // namespace BPrivate
