/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */


#include "ActionDrawer.h"

#include "Theme.h"

#include <Application.h>
#include <Roster.h>
#include <MobileKit.h>
#include <Window.h>

#include <algorithm>
#include <math.h>
#include <time.h>


using Theme::S;
using Theme::F;


#define kTileTop S(196)
#define kTileHeight S(132)
#define kSwipeClose S(40)


ActionDrawerView::ActionDrawerView(BRect frame)
	:
	OverlayView(frame, "action drawer"),
	fKeyboardShown(false),
	fTracking(false),
	fPressed(-1)
{
}


void
ActionDrawerView::Open()
{
	fStatus.Refresh();
	static_cast<OverlayWindow*>(Window())->Open(this);
	StartAnimation(true);
}


void
ActionDrawerView::Close()
{
	StartAnimation(false);
}


void
ActionDrawerView::SetKeyboardShown(bool shown)
{
	fKeyboardShown = shown;
	if (!Window()->IsHidden())
		Render();
}


void
ActionDrawerView::MouseDown(BPoint where)
{
	fTracking = true;
	fDownPoint = where;
	fPressed = _TileAt(where);
	SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
	Render();
}


void
ActionDrawerView::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	if (fTracking && fDownPoint.y - where.y > kSwipeClose) {
		fTracking = false;
		fPressed = -1;
		Close();
	}
}


void
ActionDrawerView::MouseUp(BPoint where)
{
	int32 pressed = fPressed;
	bool tap = fTracking;
	fTracking = false;
	fPressed = -1;
	if (!tap)
		return;

	if (where.y > _PanelHeight()) {
		Close();
		return;
	}
	if (pressed < 0 || _TileAt(where) != pressed) {
		Render();
		return;
	}

	switch (pressed) {
		case kTileNetwork:
		{
			entry_ref ref;
			if (be_roster->FindApp("application/x-vnd.Haiku-Network", &ref) == B_OK
				&& BMobileKit::SupportsMobileView(ref)) {
				BMessage launch(kMsgLaunch);
				launch.AddRef("ref", &ref);
				be_app->PostMessage(&launch);
				Close();
			}
			break;
		}
		case kTileKeyboard:
			be_app->PostMessage(kMsgToggleKeyboard);
			Render();
			break;
		case kTileScreenshot:
			Close();
			be_app->PostMessage(kMsgTakeScreenshot);
			break;
		case kTileSettings:
			Close();
			be_app->PostMessage(kMsgShowSettings);
			break;
	}
}


void
ActionDrawerView::RenderContent(BView* view)
{
	BRect bounds = view->Bounds();
	float height = _PanelHeight();
	float offset = roundf((Progress() - 1) * height);
	float left = _ContentLeft();

	BRect panel(0, offset, bounds.right, offset + height);
	view->SetHighColor(Theme::kPanel);
	view->FillRoundRect(panel.OffsetByCopy(0, -Theme::Corner()),
		Theme::Corner(), Theme::Corner());

	// Date and time
	BString text = clock_text();
	BFont font(be_plain_font);
	font.SetSize(F(48));
	font.SetFace(B_LIGHT_FACE);
	// Subpixel antialiasing fringes in colour at this size.
	font.SetFlags(B_FORCE_ANTIALIASING);
	view->SetFont(&font);
	view->SetHighColor(Theme::kText);
	view->DrawString(text.String(), BPoint(left + S(8), offset + S(104)));
	text = date_text();
	font.SetSize(F(19));
	font.SetFace(B_REGULAR_FACE);
	view->SetFont(&font);
	view->SetHighColor(Theme::kTextDim);
	view->DrawString(text.String(), BPoint(left + S(10), offset + S(146)));

	// Tiles
	struct { Glyph glyph; const char* label; } const kTiles[] = {
		{ fStatus.network == kNetworkWireless ? kGlyphWifi
			: fStatus.network == kNetworkWired ? kGlyphWired : kGlyphWifiOff,
			"Network" },
		{ kGlyphKeyboard, "Keyboard" },
		{ kGlyphCamera, "Screenshot" },
		{ kGlyphGear, "Settings" }
	};
	for (int32 i = 0; i < kTileCount; i++) {
		BRect frame = _TileFrame(i).OffsetByCopy(0, offset);
		bool on = (i == kTileNetwork && fStatus.network != kNetworkOffline)
			|| (i == kTileKeyboard && fKeyboardShown);
		rgb_color fill = on ? Theme::kAccent : Theme::kSurface;
		if (i == fPressed)
			fill = tint_color(fill, B_DARKEN_1_TINT);
		view->SetHighColor(fill);
		view->FillRoundRect(frame, Theme::Corner(), Theme::Corner());

		float glyph = S(36);
		BRect icon(0, 0, glyph, glyph);
		icon.OffsetTo(frame.left + floorf((frame.Width() - glyph) / 2),
			frame.top + S(24));
		draw_glyph(view, kTiles[i].glyph, icon, Theme::kText);

		font.SetSize(F(15));
		view->SetFont(&font);
		float width = font.StringWidth(kTiles[i].label);
		view->SetHighColor(Theme::kText);
		view->DrawString(kTiles[i].label,
			BPoint(frame.left + floorf((frame.Width() - width) / 2),
				frame.top + S(92)));

		if (i == kTileNetwork) {
			BString detail = fStatus.network == kNetworkOffline ? "Offline"
				: fStatus.networkName.String();
			font.SetSize(F(13));
			font.TruncateString(&detail, B_TRUNCATE_END,
				frame.Width() - S(12));
			view->SetFont(&font);
			width = font.StringWidth(detail.String());
			view->SetHighColor(on ? Theme::kText : Theme::kTextDim);
			view->DrawString(detail.String(),
				BPoint(frame.left + floorf((frame.Width() - width) / 2),
					frame.top + S(114)));
		}
	}

	// the grabber, to push the drawer back up
	view->SetHighColor(Theme::kTextDim);
	view->FillRoundRect(BRect(floorf(bounds.Width() / 2) - S(32),
		offset + height - S(22), floorf(bounds.Width() / 2) + S(32),
		offset + height - S(17)), 3, 3);
}


/*!	The drawer's content is a column in the middle of a wide screen. */
float
ActionDrawerView::_ContentWidth() const
{
	return std::min(Bounds().Width() - 2 * S(24), S(672));
}


float
ActionDrawerView::_ContentLeft() const
{
	return floorf((Bounds().Width() - _ContentWidth()) / 2);
}


BRect
ActionDrawerView::_TileFrame(int32 tile) const
{
	float gap = S(16);
	float width = floorf((_ContentWidth() - 3 * gap) / kTileCount);
	float left = _ContentLeft() + tile * (width + gap);
	return BRect(left, kTileTop, left + width, kTileTop + kTileHeight);
}


int32
ActionDrawerView::_TileAt(BPoint where) const
{
	for (int32 i = 0; i < kTileCount; i++) {
		if (_TileFrame(i).Contains(where))
			return i;
	}
	return -1;
}


float
ActionDrawerView::_PanelHeight() const
{
	return kTileTop + kTileHeight + S(56);
}
