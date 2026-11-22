/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Panels.h"

#include "Glyphs.h"
#include "Theme.h"

#include <Application.h>
#include <Font.h>

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <time.h>


using Theme::S;
using Theme::F;


// A drag at least this long counts as a swipe rather than a tap.
static float
swipe_distance()
{
	return S(24);
}


PanelWindow::PanelWindow(BRect frame, const char* name)
	:
	BWindow(frame, name, B_NO_BORDER_WINDOW_LOOK, B_FLOATING_ALL_WINDOW_FEEL,
		B_NOT_MOVABLE | B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE
			| B_NOT_CLOSABLE | B_AVOID_FOCUS | B_ASYNCHRONOUS_CONTROLS,
		B_ALL_WORKSPACES)
{
}


// #pragma mark - StatusBarView


StatusBarView::StatusBarView(BRect frame)
	:
	BView(frame, "status bar", B_FOLLOW_ALL, B_WILL_DRAW),
	fTracking(false),
	fRefreshCountdown(0)
{
	SetViewColor(B_TRANSPARENT_COLOR);
	fStatus.Refresh();
}


void
StatusBarView::AttachedToWindow()
{
	SetFont(be_bold_font);
	SetFontSize(F(17));
}


void
StatusBarView::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	SetHighColor(Theme::kPanel);
	FillRect(bounds);

	BString text = clock_text();

	font_height height;
	GetFontHeight(&height);
	float baseline = floorf((bounds.Height() + height.ascent - height.descent)
		/ 2) + 1;
	SetHighColor(Theme::kText);
	SetLowColor(Theme::kPanel);
	DrawString(text.String(), BPoint(S(20), baseline));

	// Indicators, right to left.
	float size = S(26);
	BRect icon(0, 0, size, size);
	icon.OffsetTo(bounds.right - S(18) - size,
		floorf((bounds.Height() - size) / 2));

	if (fStatus.hasBattery) {
		char percent[8];
		snprintf(percent, sizeof(percent), "%d%%",
			(int)(fStatus.batteryLevel * 100 + 0.5f));
		float width = StringWidth(percent);
		DrawString(percent, BPoint(icon.right - width, baseline));
		icon.OffsetBy(-width - S(4) - size, 0);
		draw_glyph(this, kGlyphBattery, icon, Theme::kText,
			fStatus.batteryLevel);
	} else
		draw_glyph(this, kGlyphPlug, icon, Theme::kText);
	icon.OffsetBy(-size - S(12), 0);

	Glyph network = kGlyphWifiOff;
	if (fStatus.network == kNetworkWireless)
		network = kGlyphWifi;
	else if (fStatus.network == kNetworkWired)
		network = kGlyphWired;
	draw_glyph(this, network, icon,
		fStatus.network == kNetworkOffline ? Theme::kTextDim : Theme::kText);
}


void
StatusBarView::MouseDown(BPoint where)
{
	fTracking = true;
	fDownPoint = where;
	SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
}


void
StatusBarView::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	if (fTracking && where.y - fDownPoint.y > swipe_distance()) {
		fTracking = false;
		be_app->PostMessage(kMsgShowDrawer);
	}
}


void
StatusBarView::MouseUp(BPoint where)
{
	// A tap opens the drawer as well; a swipe already has.
	if (fTracking)
		be_app->PostMessage(kMsgShowDrawer);
	fTracking = false;
}


void
StatusBarView::MessageReceived(BMessage* message)
{
	if (message->what == kMsgPulse) {
		if (--fRefreshCountdown <= 0) {
			fStatus.Refresh();
			fRefreshCountdown = 5;
		}
		Invalidate();
		return;
	}
	BView::MessageReceived(message);
}


// #pragma mark - NavBarView


NavBarView::NavBarView(BRect frame)
	:
	BView(frame, "navigation", B_FOLLOW_ALL, B_WILL_DRAW),
	fPressed(kNone)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


void
NavBarView::Draw(BRect updateRect)
{
	SetHighColor(Theme::kPanel);
	FillRect(Bounds());

	static const Glyph kGlyphs[] = {
		kGlyphTasks, kGlyphHome, kGlyphClose, kGlyphKeyboard
	};

	for (int32 i = 0; i < kButtonCount; i++) {
		BRect frame = _ButtonFrame(i);
		if (i == fPressed) {
			SetHighColor(Theme::kSurfaceRaised);
			float radius = std::min(frame.Width(), frame.Height()) / 2 - S(4);
			FillEllipse(BPoint((frame.left + frame.right) / 2,
				(frame.top + frame.bottom) / 2), radius * 1.3f, radius);
		}
		float glyph = S(i == kKeyboard ? 26 : 30);
		BRect icon(0, 0, glyph, glyph);
		icon.OffsetTo(frame.left + (frame.Width() - glyph) / 2,
			frame.top + (frame.Height() - glyph) / 2);
		draw_glyph(this, kGlyphs[i], icon,
			i == kKeyboard ? Theme::kTextDim : Theme::kText);
	}
}


void
NavBarView::MouseDown(BPoint where)
{
	fPressed = _ButtonAt(where);
	fDownPoint = where;
	SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
	Invalidate();
}


void
NavBarView::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	// Swiping up from the panel opens the task switcher.
	if (fPressed != kNone && fDownPoint.y - where.y > swipe_distance()) {
		fPressed = kNone;
		Invalidate();
		be_app->PostMessage(kMsgShowTasks);
	}
}


void
NavBarView::MouseUp(BPoint where)
{
	int32 pressed = fPressed;
	fPressed = kNone;
	Invalidate();
	if (pressed == kNone || _ButtonAt(where) != pressed)
		return;

	switch (pressed) {
		case kTasks:
			be_app->PostMessage(kMsgShowTasks);
			break;
		case kHome:
			be_app->PostMessage(kMsgGoHome);
			break;
		case kClose:
			be_app->PostMessage(kMsgCloseActive);
			break;
		case kKeyboard:
			be_app->PostMessage(kMsgToggleKeyboard);
			break;
	}
}


BRect
NavBarView::_ButtonFrame(int32 button) const
{
	BRect bounds = Bounds();
	if (button == kKeyboard) {
		return BRect(bounds.right - S(64), bounds.top, bounds.right,
			bounds.bottom);
	}

	// Three equal buttons in the middle of the panel, leaving the keyboard
	// toggle its corner.
	float width = S(160);
	float center = floorf(bounds.Width() / 2) + (button - kHome) * S(200);
	return BRect(center - width / 2, bounds.top, center + width / 2,
		bounds.bottom);
}


int32
NavBarView::_ButtonAt(BPoint where) const
{
	for (int32 i = 0; i < kButtonCount; i++) {
		if (_ButtonFrame(i).Contains(where))
			return i;
	}
	return kNone;
}
