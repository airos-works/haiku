/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */


#include "TaskSwitcher.h"

#include "Glyphs.h"
#include "Theme.h"

#include <Application.h>
#include <Bitmap.h>
#include <NodeInfo.h>
#include <Roster.h>
#include <Window.h>

#include <WindowInfo.h>

#include <algorithm>
#include <math.h>


using Theme::S;
using Theme::F;


#define kGap S(24)
#define kTitleHeight S(52)
#define kTop S(80)
#define kIcon S(96)

// how long a covered window gets to draw itself for its card
static const bigtime_t kPreviewTimeout = 150000;


TaskSwitcherView::TaskSwitcherView(BRect frame)
	:
	OverlayView(frame, "task switcher"),
	fPressed(-1),
	fPressedClose(false)
{
}


TaskSwitcherView::~TaskSwitcherView()
{
	_Clear();
	for (auto& preview : fPreviews)
		delete preview.second;
}


void
TaskSwitcherView::Open(const std::vector<TrackedWindow>& windows)
{
	_Clear();
	// The picture of the screen: the window in front is cut from it.
	Prepare();

	// Cards the shape of the windows: three across a wide screen, two
	// across a tall one, in two rows where they fit.
	BRect bounds = Bounds();
	int32 columns = bounds.Width() > bounds.Height() ? 3 : 2;
	float width = floorf((bounds.Width() - (columns + 1) * kGap) / columns);
	float available = bounds.Height() - kTop;
	float height = S(16) + kTitleHeight
		+ floorf((width - S(16)) * (bounds.Height() + 1) / (bounds.Width() + 1));
	height = std::min(height, floorf((available - 2 * kGap) / 2));
	if (height < S(180))
		height = available - kGap;
	int32 rows = std::max((int32)1, (int32)(available / (height + kGap)));

	// No scrolling yet: windows past the grid are left out.
	bool front = true;
	for (const TrackedWindow& window : windows) {
		int32 index = fCards.size();
		if (index >= rows * columns)
			break;

		Card card;
		card.token = window.token;
		card.title = window.title;
		card.icon = NULL;
		app_info info;
		if (be_roster->GetRunningAppInfo(window.team, &info) == B_OK) {
			card.icon = new BBitmap(BRect(0, 0, kIcon - 1, kIcon - 1), B_RGBA32);
			if (BNodeInfo::GetTrackerIcon(&info.ref, card.icon,
					(icon_size)kIcon) != B_OK) {
				delete card.icon;
				card.icon = NULL;
			}
		}
		card.frame = BRect(0, 0, width, height);
		card.frame.OffsetTo(kGap + (index % columns) * (width + kGap),
			kTop + (index / columns) * (height + kGap));

		// The preview, the window's shape, as large as the card allows
		// above the title.
		BRect area = card.frame.InsetByCopy(S(8), S(8));
		area.bottom -= kTitleHeight;
		float scale = std::min((area.Width() + 1) / (window.frame.Width() + 1),
			(area.Height() + 1) / (window.frame.Height() + 1));
		scale = std::min(scale, 1.0f);
		float previewWidth = floorf((window.frame.Width() + 1) * scale);
		float previewHeight = floorf((window.frame.Height() + 1) * scale);
		card.previewFrame = BRect(0, 0, previewWidth - 1, previewHeight - 1);
		card.previewFrame.OffsetTo(
			area.left + floorf((area.Width() + 1 - previewWidth) / 2),
			area.top + floorf((area.Height() + 1 - previewHeight) / 2));

		if (!window.minimized) {
			_TakePreview(window, card, front);
			front = false;
		}
		fCards.push_back(card);
	}

	// The previews of windows that have gone go with them.
	for (auto it = fPreviews.begin(); it != fPreviews.end();) {
		bool open = false;
		for (const TrackedWindow& window : windows)
			open = open || window.token == it->first;
		if (open)
			++it;
		else {
			delete it->second;
			it = fPreviews.erase(it);
		}
	}

	static_cast<OverlayWindow*>(Window())->Open(this, true);
	StartAnimation(true);
}


/*!	The window as it looks now, at the size of its card's preview. */
void
TaskSwitcherView::_TakePreview(const TrackedWindow& window, const Card& card,
	bool front)
{
	BRect frame = window.frame;
	BRect whole = frame.OffsetToCopy(B_ORIGIN);
	const BBitmap* source = NULL;
	BRect sourceRect;
	BBitmap* drawn = NULL;

	if (front && Backdrop() != NULL) {
		source = Backdrop();
		sourceRect = frame;
	} else {
		drawn = new BBitmap(whole, B_RGB32);
		bool wasDrawn = false;
		status_t status = drawn->InitCheck();
		if (status == B_OK) {
			status = BPrivate::read_window_bitmap(window.token, drawn,
				kPreviewTimeout, &wasDrawn);
		}
		if (status == B_OK && wasDrawn) {
			source = drawn;
			sourceRect = whole;
		} else if (status == B_OK && Backdrop() != NULL) {
			// nothing covers it: it's on the screen
			source = Backdrop();
			sourceRect = frame;
		}
	}

	if (source != NULL) {
		BRect bounds = card.previewFrame.OffsetToCopy(B_ORIGIN);
		BBitmap* preview = new BBitmap(bounds, B_BITMAP_ACCEPTS_VIEWS,
			B_RGB32);
		BView* view = new BView(bounds, "preview", B_FOLLOW_NONE, 0);
		preview->AddChild(view);
		if (preview->Lock()) {
			view->DrawBitmap(source, sourceRect, bounds,
				B_FILTER_BITMAP_BILINEAR);
			view->Sync();
			preview->Unlock();
		}
		delete fPreviews[window.token];
		fPreviews[window.token] = preview;
	}
	delete drawn;
}


void
TaskSwitcherView::Close()
{
	StartAnimation(false);
}


void
TaskSwitcherView::MouseDown(BPoint where)
{
	fPressed = -1;
	for (size_t i = 0; i < fCards.size(); i++) {
		if (fCards[i].frame.Contains(where)) {
			fPressed = i;
			fPressedClose = _CloseFrame(fCards[i]).Contains(where);
		}
	}
	SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
	Render();
}


void
TaskSwitcherView::MouseUp(BPoint where)
{
	int32 pressed = fPressed;
	fPressed = -1;
	if (pressed < 0 || pressed >= (int32)fCards.size()
		|| !fCards[pressed].frame.Contains(where)) {
		Close();
		return;
	}

	if (fPressedClose) {
		BMessage message(kMsgCloseWindow);
		message.AddInt32("token", fCards[pressed].token);
		be_app->PostMessage(&message);
		delete fCards[pressed].icon;
		fCards.erase(fCards.begin() + pressed);
		if (fCards.empty())
			Close();
		else
			Render();
	} else {
		BMessage message(kMsgActivateWindow);
		message.AddInt32("token", fCards[pressed].token);
		be_app->PostMessage(&message);
		Close();
	}
}


void
TaskSwitcherView::RenderContent(BView* view)
{
	BRect bounds = view->Bounds();
	// The cards slide up into place.
	float offset = roundf((1 - Progress()) * S(120));

	BFont font(be_bold_font);
	font.SetSize(F(26));
	view->SetFont(&font);
	view->SetHighColor(Theme::kText);
	view->DrawString("Open applications",
		BPoint(kGap + S(4), kTop - S(28) + offset));

	if (fCards.empty()) {
		font.SetSize(F(19));
		font.SetFace(B_REGULAR_FACE);
		view->SetFont(&font);
		const char* text = "Nothing is open";
		view->SetHighColor(Theme::kTextDim);
		view->DrawString(text,
			BPoint(floorf((bounds.Width() - font.StringWidth(text)) / 2),
				floorf(bounds.Height() / 2)));
		return;
	}

	for (size_t i = 0; i < fCards.size(); i++) {
		const Card& card = fCards[i];
		BRect frame = card.frame.OffsetByCopy(0, offset);
		view->SetHighColor((int32)i == fPressed && !fPressedClose
			? Theme::kSurfaceRaised : Theme::kSurface);
		view->FillRoundRect(frame, Theme::Corner(), Theme::Corner());

		// the window as it looked, or the application's icon
		BRect previewFrame = card.previewFrame.OffsetByCopy(0, offset);
		auto preview = fPreviews.find(card.token);
		if (preview != fPreviews.end() && preview->second != NULL) {
			view->DrawBitmap(preview->second, preview->second->Bounds(),
				previewFrame, B_FILTER_BITMAP_BILINEAR);
		} else if (card.icon != NULL) {
			BRect icon(0, 0, kIcon - 1, kIcon - 1);
			icon.OffsetTo(previewFrame.left
					+ floorf((previewFrame.Width() - kIcon) / 2),
				previewFrame.top + floorf((previewFrame.Height() - kIcon) / 2));
			view->DrawBitmap(card.icon, icon);
		}

		BFont title(be_plain_font);
		title.SetSize(F(17));
		BString text = card.title;
		title.TruncateString(&text, B_TRUNCATE_END, frame.Width() - S(32));
		view->SetFont(&title);
		view->SetHighColor(Theme::kText);
		view->DrawString(text.String(), BPoint(frame.left
			+ floorf((frame.Width() - title.StringWidth(text.String())) / 2),
			frame.bottom - S(20)));

		// over the preview, on a dark disc
		BRect close = _CloseFrame(card).OffsetByCopy(0, offset);
		view->SetHighColor((int32)i == fPressed && fPressedClose
			? Theme::kSurfaceRaised : Theme::kPanel);
		view->FillEllipse(close);
		draw_glyph(view, kGlyphClose, close.InsetByCopy(S(8), S(8)),
			Theme::kText);
	}
}


BRect
TaskSwitcherView::_CloseFrame(const Card& card) const
{
	return BRect(card.frame.right - S(56), card.frame.top + S(8),
		card.frame.right - S(8), card.frame.top + S(56));
}


void
TaskSwitcherView::_Clear()
{
	for (Card& card : fCards)
		delete card.icon;
	fCards.clear();
}
