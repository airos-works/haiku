/*
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */


#include "EditMenu.h"

#include "Protocol.h"
#include "Theme.h"

#include <Font.h>
#include <InterfaceDefs.h>

#include <algorithm>
#include <math.h>
#include <string.h>


using Theme::S;
using Theme::F;


namespace {

struct Item {
	const char*	label;
	char		shortcut;	// typed with Command; 0: the context menu
	int32		code;		// its key on the US layout
	bool		stays;		// the menu stays up after it
};

const Item kItems[] = {
	{ "Cut", 'x', 0x4d, false },
	{ "Copy", 'c', 0x4e, false },
	{ "Paste", 'v', 0x4f, false },
	{ "Select all", 'a', 0x3c, true },
	{ "\xe2\x80\xa6", 0, 0, false }		// "…"
};
const int32 kItemCount = sizeof(kItems) / sizeof(kItems[0]);


float
item_height()
{
	return S(56);
}


float
item_padding()
{
	return S(18);
}


// how far from the finger the menu stays
float
menu_gap()
{
	return S(28);
}

}	// namespace


EditMenuView::EditMenuView(BRect frame)
	:
	BView(frame, "edit menu", B_FOLLOW_ALL, B_WILL_DRAW),
	fPressed(-1)
{
	SetViewColor(B_TRANSPARENT_COLOR);
	BFont font(be_plain_font);
	font.SetSize(F(17));
	SetFont(&font);
	for (int32 i = 0; i < kItemCount; i++)
		fWidths.push_back(ceilf(font.StringWidth(kItems[i].label))
			+ 2 * item_padding());
}


void
EditMenuView::AttachedToWindow()
{
	SetLowColor(Theme::kSurfaceRaised);
}


BRect
EditMenuView::PreferredFrame() const
{
	float width = 0;
	for (float itemWidth : fWidths)
		width += itemWidth;
	return BRect(0, 0, width - 1, item_height() - 1);
}


void
EditMenuView::Draw(BRect updateRect)
{
	// square: a window has no see-through corners
	BRect bounds = Bounds();
	SetHighColor(Theme::kSurfaceRaised);
	FillRect(bounds);
	SetHighColor(Theme::kSurface);
	StrokeRect(bounds);

	font_height height;
	GetFontHeight(&height);
	for (int32 i = 0; i < kItemCount; i++) {
		BRect frame = _ItemFrame(i);
		if (i == fPressed) {
			SetHighColor(Theme::kAccent);
			BRect pressed = frame.InsetByCopy(S(4), S(4));
			FillRoundRect(pressed, Theme::Corner() - S(4),
				Theme::Corner() - S(4));
		}
		if (i > 0) {
			SetHighColor(Theme::kSurface);
			StrokeLine(BPoint(frame.left, frame.top + S(14)),
				BPoint(frame.left, frame.bottom - S(14)));
		}
		SetHighColor(Theme::kText);
		SetLowColor(i == fPressed ? Theme::kAccent : Theme::kSurfaceRaised);
		float x = frame.left + (frame.Width() - StringWidth(kItems[i].label))
			/ 2;
		float y = floorf(frame.top + (frame.Height() + height.ascent
			- height.descent) / 2);
		DrawString(kItems[i].label, BPoint(x, y));
	}
}


void
EditMenuView::MouseDown(BPoint where)
{
	fPressed = _ItemAt(where);
	SetMouseEventMask(B_POINTER_EVENTS, B_NO_POINTER_HISTORY);
	Invalidate();
}


void
EditMenuView::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	if (fPressed < 0)
		return;
	int32 under = _ItemAt(where);
	if (under != fPressed) {
		// sliding off a button lets go of it
		fPressed = -1;
		Invalidate();
	}
}


void
EditMenuView::MouseUp(BPoint where)
{
	int32 pressed = fPressed;
	fPressed = -1;
	Invalidate();
	if (pressed >= 0 && _ItemAt(where) == pressed)
		_Invoke(pressed);
}


int32
EditMenuView::_ItemAt(BPoint where) const
{
	for (int32 i = 0; i < kItemCount; i++) {
		if (_ItemFrame(i).Contains(where))
			return i;
	}
	return -1;
}


BRect
EditMenuView::_ItemFrame(int32 index) const
{
	float left = 0;
	for (int32 i = 0; i < index; i++)
		left += fWidths[i];
	return BRect(left, 0, left + fWidths[index] - 1, item_height() - 1);
}


void
EditMenuView::_Invoke(int32 index)
{
	const Item& item = kItems[index];
	EditMenuWindow* window = static_cast<EditMenuWindow*>(Window());

	if (item.shortcut == 0) {
		// the application's own menu, where the finger was held
		window->Dismiss();
		port_id port = find_port(MOBILE_TOUCH_PORT);
		if (port >= 0) {
			float point[2] = { fHeld.x, fHeld.y };
			write_port_etc(port, kMobileMenuAt, point, sizeof(point),
				B_RELATIVE_TIMEOUT, 100000);
		}
		return;
	}

	port_id port = find_port(MOBILE_KEYBOARD_PORT);
	if (port >= 0) {
		mobile_key key = {};
		key.key = item.code;
		key.modifiers = B_COMMAND_KEY | B_LEFT_COMMAND_KEY;
		key.bytes[0] = item.shortcut;
		key.raw = item.shortcut;
		write_port_etc(port, kMobileKey, &key, sizeof(key),
			B_RELATIVE_TIMEOUT, 100000);
	}
	if (!item.stays)
		window->Dismiss();
}


// #pragma mark - EditMenuWindow


EditMenuWindow::EditMenuWindow()
	:
	BWindow(BRect(0, 0, 10, 10), "Edit menu", B_NO_BORDER_WINDOW_LOOK,
		B_FLOATING_ALL_WINDOW_FEEL,
		B_NOT_MOVABLE | B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE
			| B_NOT_CLOSABLE | B_AVOID_FOCUS | B_ASYNCHRONOUS_CONTROLS,
		B_ALL_WORKSPACES)
{
	fView = new EditMenuView(Bounds());
	AddChild(fView);
	BRect frame = fView->PreferredFrame();
	ResizeTo(frame.Width(), frame.Height());
}


void
EditMenuWindow::ShowAt(BPoint held, BPoint lifted, BRect area)
{
	if (!Lock())
		return;

	fView->SetHeld(held);
	BRect frame = fView->PreferredFrame();
	float width = frame.Width() + 1;
	float height = frame.Height() + 1;

	// above the selection, or below it when there is no room above
	float top = std::min(held.y, lifted.y) - menu_gap() - height;
	if (top < area.top)
		top = std::max(held.y, lifted.y) + menu_gap();
	top = std::clamp(top, area.top, std::max(area.top, area.bottom - height));
	float left = (held.x + lifted.x) / 2 - width / 2;
	left = std::clamp(left, area.left, std::max(area.left, area.right - width));

	MoveTo(roundf(left), roundf(top));
	if (IsHidden())
		Show();
	Unlock();
}


void
EditMenuWindow::Dismiss()
{
	if (!Lock())
		return;
	if (!IsHidden())
		Hide();
	Unlock();
}
