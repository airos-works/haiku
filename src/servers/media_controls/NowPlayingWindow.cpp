/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "NowPlayingWindow.h"

#include <math.h>
#include <stdio.h>

#include <algorithm>

#include <Bitmap.h>
#include <Box.h>
#include <Catalog.h>
#include <Control.h>
#include <ControlLook.h>
#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <Screen.h>
#include <StringView.h>

#include <MediaControls.h>

#include "MediaIcons.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "NowPlayingWindow"


enum {
	kMsgTransport	= 'trns',
	kMsgSeek		= 'seek'
};


static bigtime_t sLastClosed = 0;


static float
font_scale()
{
	return be_plain_font->Size() / 12.0f;
}


static BString
format_time(bigtime_t time)
{
	int32 seconds = (int32)(time / 1000000);
	BString text;
	if (seconds >= 3600) {
		text.SetToFormat("%" B_PRId32 ":%02" B_PRId32 ":%02" B_PRId32,
			seconds / 3600, (seconds / 60) % 60, seconds % 60);
	} else {
		text.SetToFormat("%" B_PRId32 ":%02" B_PRId32, seconds / 60,
			seconds % 60);
	}
	return text;
}


//	#pragma mark - ArtworkView


class ArtworkView : public BView {
public:
	ArtworkView(float size)
		:
		BView("artwork", B_WILL_DRAW),
		fBitmap(NULL)
	{
		SetExplicitSize(BSize(size, size));
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	}

	~ArtworkView()
	{
		delete fBitmap;
	}

	void SetArtwork(const BMessage* archive)
	{
		delete fBitmap;
		fBitmap = NULL;
		if (archive != NULL) {
			fBitmap = new BBitmap(const_cast<BMessage*>(archive));
			if (fBitmap->InitCheck() != B_OK) {
				delete fBitmap;
				fBitmap = NULL;
			}
		}
		Invalidate();
	}

	virtual void Draw(BRect updateRect)
	{
		BRect bounds = Bounds();
		if (fBitmap == NULL) {
			SetHighColor(tint_color(ViewColor(), B_DARKEN_1_TINT));
			FillRoundRect(bounds, 4, 4);
			BRect icon = bounds.InsetByCopy(bounds.Width() / 5,
				bounds.Height() / 5);
			DrawNoteIcon(this, icon, true);
			return;
		}

		// keep the aspect ratio, centered
		BRect source = fBitmap->Bounds();
		float scale = std::min((bounds.Width() + 1) / (source.Width() + 1),
			(bounds.Height() + 1) / (source.Height() + 1));
		float width = roundf((source.Width() + 1) * scale);
		float height = roundf((source.Height() + 1) * scale);
		BRect target(0, 0, width - 1, height - 1);
		target.OffsetTo(roundf((bounds.Width() + 1 - width) / 2),
			roundf((bounds.Height() + 1 - height) / 2));
		SetDrawingMode(B_OP_ALPHA);
		DrawBitmap(fBitmap, source, target, B_FILTER_BITMAP_BILINEAR);
		SetDrawingMode(B_OP_COPY);
	}

private:
	BBitmap*	fBitmap;
};


//	#pragma mark - ProgressView


/*!	Elapsed time, a bar, and the duration. The position is extrapolated
	between the player's updates. A click seeks when the player can.
*/
class ProgressView : public BView {
public:
	ProgressView()
		:
		BView("progress", B_WILL_DRAW | B_PULSE_NEEDED
			| B_FULL_UPDATE_ON_RESIZE),
		fPosition(-1),
		fPositionTime(0),
		fDuration(0),
		fRate(1),
		fPlaying(false),
		fCanSeek(false)
	{
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		BFont font(be_plain_font);
		font.SetSize(ceilf(font.Size() * 0.85f));
		SetFont(&font);
	}

	void SetPlayback(const BMessage& player)
	{
		fPosition = player.GetInt64("position", -1);
		fPositionTime = player.GetInt64("position_time", system_time());
		fDuration = player.GetInt64("duration", 0);
		fRate = player.GetFloat("rate", 1.0f);
		fPlaying = player.GetInt32("state", B_MEDIA_SESSION_STOPPED)
			== B_MEDIA_SESSION_PLAYING;
		fCanSeek = (player.GetUInt32("actions", 0)
			& B_MEDIA_ACTION_FLAG(B_MEDIA_ACTION_SEEK)) != 0;
		Invalidate();
	}

	virtual BSize MinSize()
	{
		font_height height;
		GetFontHeight(&height);
		return BSize(200 * font_scale(),
			ceilf(height.ascent + height.descent) + 2);
	}

	virtual BSize MaxSize()
	{
		return BSize(B_SIZE_UNLIMITED, MinSize().height);
	}

	virtual void Pulse()
	{
		if (fPlaying)
			Invalidate();
	}

	virtual void Draw(BRect updateRect)
	{
		BRect bounds = Bounds();
		bigtime_t position = _Position();

		BString elapsed = position >= 0 ? format_time(position) : "";
		BString total = fDuration > 0 ? format_time(fDuration) : "";
		float labelWidth = ceilf(StringWidth(
			fDuration >= 3600000000LL ? "0:00:00" : "00:00")) + 2;

		font_height height;
		GetFontHeight(&height);
		float baseline = roundf((bounds.top + bounds.bottom + height.ascent
			- height.descent) / 2);

		SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_1_TINT);
		DrawString(elapsed.String(), BPoint(bounds.left + labelWidth
			- StringWidth(elapsed.String()), baseline));
		DrawString(total.String(),
			BPoint(bounds.right - labelWidth + 2, baseline));

		fBar = BRect(bounds.left + labelWidth + 6, 0,
			bounds.right - labelWidth - 4, 0);
		float barHeight = std::max(3.0f, roundf(4 * font_scale()));
		fBar.top = roundf((bounds.top + bounds.bottom - barHeight) / 2);
		fBar.bottom = fBar.top + barHeight - 1;

		rgb_color base = ui_color(B_PANEL_BACKGROUND_COLOR);
		SetHighColor(tint_color(base, B_DARKEN_2_TINT));
		FillRoundRect(fBar, barHeight / 2, barHeight / 2);
		if (fDuration > 0 && position >= 0) {
			float fraction = std::min(1.0f, (float)position / fDuration);
			BRect done = fBar;
			done.right = done.left + roundf(fBar.Width() * fraction);
			SetHighUIColor(B_CONTROL_HIGHLIGHT_COLOR);
			FillRoundRect(done, barHeight / 2, barHeight / 2);
		}
	}

	virtual void MouseDown(BPoint where)
	{
		if (!fCanSeek || fDuration <= 0 || !fBar.IsValid()
			|| where.x < fBar.left - 4 || where.x > fBar.right + 4)
			return;

		float fraction = (where.x - fBar.left) / std::max(1.0f, fBar.Width());
		fraction = std::max(0.0f, std::min(1.0f, fraction));
		BMessage seek(kMsgSeek);
		seek.AddInt64("position", (bigtime_t)(fraction * fDuration));
		Window()->PostMessage(&seek);

		fPosition = (bigtime_t)(fraction * fDuration);
		fPositionTime = system_time();
		Invalidate();
	}

private:
	bigtime_t _Position() const
	{
		if (fPosition < 0)
			return -1;
		bigtime_t position = fPosition;
		if (fPlaying)
			position += (bigtime_t)((system_time() - fPositionTime) * fRate);
		if (fDuration > 0)
			position = std::min(position, fDuration);
		return std::max((bigtime_t)0, position);
	}

private:
	bigtime_t	fPosition;
	bigtime_t	fPositionTime;
	bigtime_t	fDuration;
	float		fRate;
	bool		fPlaying;
	bool		fCanSeek;
	BRect		fBar;
};


//	#pragma mark - TransportButton


class TransportButton : public BControl {
public:
	TransportButton(const char* name, transport_symbol symbol,
		BMessage* message, float size)
		:
		BControl(name, NULL, message, B_WILL_DRAW | B_NAVIGABLE),
		fSymbol(symbol),
		fSize(size)
	{
		SetExplicitSize(BSize(roundf(size * 1.5f), size));
	}

	void SetSymbol(transport_symbol symbol)
	{
		if (symbol != fSymbol) {
			fSymbol = symbol;
			Invalidate();
		}
	}

	virtual void Draw(BRect updateRect)
	{
		BRect bounds = Bounds();
		rgb_color base = ui_color(B_CONTROL_BACKGROUND_COLOR);
		uint32 flags = be_control_look->Flags(this);
		if (Value() == B_CONTROL_ON)
			flags |= BControlLook::B_ACTIVATED;

		be_control_look->DrawButtonFrame(this, bounds, updateRect, base,
			ViewColor(), flags);
		be_control_look->DrawButtonBackground(this, bounds, updateRect, base,
			flags);

		rgb_color color = ui_color(B_CONTROL_TEXT_COLOR);
		if (!IsEnabled())
			color = tint_color(base, B_DISABLED_LABEL_TINT);
		float symbolSize = roundf(fSize * 0.45f);
		BRect symbolFrame(0, 0, symbolSize, symbolSize);
		symbolFrame.OffsetTo(roundf((bounds.Width() - symbolSize) / 2),
			roundf((bounds.Height() - symbolSize) / 2));
		DrawTransportSymbol(this, symbolFrame, fSymbol, color);
	}

	virtual void MouseDown(BPoint where)
	{
		if (!IsEnabled())
			return;
		SetValue(B_CONTROL_ON);
		SetTracking(true);
		SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
	}

	virtual void MouseMoved(BPoint where, uint32 transit,
		const BMessage* dragMessage)
	{
		if (IsTracking())
			SetValue(Bounds().Contains(where) ? B_CONTROL_ON : B_CONTROL_OFF);
	}

	virtual void MouseUp(BPoint where)
	{
		if (!IsTracking())
			return;
		bool inside = Bounds().Contains(where);
		SetValue(B_CONTROL_OFF);
		SetTracking(false);
		if (inside)
			Invoke();
	}

private:
	transport_symbol	fSymbol;
	float				fSize;
};


//	#pragma mark - NowPlayingWindow


NowPlayingWindow::NowPlayingWindow(BRect anchor)
	:
	BWindow(BRect(0, 0, 100, 100), "NowPlaying", B_BORDERED_WINDOW_LOOK,
		B_FLOATING_ALL_WINDOW_FEEL, B_NOT_MOVABLE | B_NOT_RESIZABLE
			| B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_ASYNCHRONOUS_CONTROLS
			| B_WILL_ACCEPT_FIRST_CLICK | B_AUTO_UPDATE_SIZE_LIMITS
			| B_CLOSE_ON_ESCAPE,
		B_ALL_WORKSPACES),
	fAnchor(anchor),
	fSession(-1),
	fActions(0),
	fPlaying(false)
{
	float scale = font_scale();
	SetPulseRate(500000);

	fArtworkView = new ArtworkView(roundf(72 * scale));

	fTitleView = new BStringView("title", "");
	fTitleView->SetFont(be_bold_font);
	fTitleView->SetTruncation(B_TRUNCATE_END);
	fTitleView->SetExplicitMinSize(BSize(180 * scale, B_SIZE_UNSET));
	fTitleView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	fArtistView = new BStringView("artist", "");
	fArtistView->SetTruncation(B_TRUNCATE_END);
	fArtistView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	fApplicationView = new BStringView("application", "");
	BFont small(be_plain_font);
	small.SetSize(ceilf(small.Size() * 0.85f));
	fApplicationView->SetFont(&small);
	fApplicationView->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_1_TINT);
	fApplicationView->SetTruncation(B_TRUNCATE_END);
	fApplicationView->SetExplicitMaxSize(
		BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	fProgressView = new ProgressView();

	float buttonSize = roundf(26 * scale);
	BMessage* message = new BMessage(kMsgTransport);
	message->AddInt32("action", B_MEDIA_ACTION_PREVIOUS);
	fPreviousButton = new TransportButton("previous", kSymbolPrevious,
		message, buttonSize);
	message = new BMessage(kMsgTransport);
	message->AddInt32("action", B_MEDIA_ACTION_TOGGLE);
	fPlayButton = new TransportButton("play", kSymbolPlay, message,
		buttonSize);
	message = new BMessage(kMsgTransport);
	message->AddInt32("action", B_MEDIA_ACTION_NEXT);
	fNextButton = new TransportButton("next", kSymbolNext, message,
		buttonSize);

	BBox* box = new BBox("box");
	box->SetBorder(B_PLAIN_BORDER);

	BLayoutBuilder::Group<>(box, B_VERTICAL, B_USE_SMALL_SPACING)
		.SetInsets(B_USE_SMALL_SPACING)
		.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
			.Add(fArtworkView)
			.AddGroup(B_VERTICAL, 0)
				.AddGlue()
				.Add(fTitleView)
				.Add(fArtistView)
				.Add(fApplicationView)
				.AddGlue()
			.End()
		.End()
		.Add(fProgressView)
		.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
			.AddGlue()
			.Add(fPreviousButton)
			.Add(fPlayButton)
			.Add(fNextButton)
			.AddGlue()
		.End();

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(box);

	_Place();
}


NowPlayingWindow::~NowPlayingWindow()
{
}


void
NowPlayingWindow::Show()
{
	BMessage watch(kMsgMediaControlsStartWatching);
	watch.AddMessenger("target", BMessenger(this));
	BMessenger(kMediaControlsServerSignature).SendMessage(&watch,
		(BHandler*)NULL, 100000);

	BWindow::Show();
	Activate();
}


void
NowPlayingWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgMediaControlsState:
			_SetState(*message);
			break;

		case kMsgTransport:
		case kMsgSeek:
		{
			if (fSession < 0)
				break;
			BMessage request(kMsgMediaControlsSendAction);
			request.AddInt64("session", fSession);
			if (message->what == kMsgSeek) {
				request.AddInt32("action", B_MEDIA_ACTION_SEEK);
				request.AddInt64("position",
					message->GetInt64("position", 0));
			} else
				request.AddInt32("action", message->GetInt32("action", -1));
			BMessenger(kMediaControlsServerSignature).SendMessage(&request,
				(BHandler*)NULL, 100000);
			break;
		}

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


void
NowPlayingWindow::WindowActivated(bool active)
{
	if (!active && !IsHidden())
		PostMessage(B_QUIT_REQUESTED);
}


bool
NowPlayingWindow::QuitRequested()
{
	BMessage watch(kMsgMediaControlsStopWatching);
	watch.AddMessenger("target", BMessenger(this));
	BMessenger(kMediaControlsServerSignature).SendMessage(&watch,
		(BHandler*)NULL, 100000);

	sLastClosed = system_time();
	return true;
}


/*static*/ bigtime_t
NowPlayingWindow::LastClosed()
{
	return sLastClosed;
}


void
NowPlayingWindow::_SetState(const BMessage& state)
{
	BMessage player;
	if (state.FindMessage("player", &player) != B_OK) {
		PostMessage(B_QUIT_REQUESTED);
		return;
	}

	fSession = player.GetInt64("session", -1);
	fActions = player.GetUInt32("actions", 0);
	fPlaying = player.GetInt32("state", B_MEDIA_SESSION_STOPPED)
		== B_MEDIA_SESSION_PLAYING;

	BString title = player.GetString("title", "");
	if (title.IsEmpty())
		title = B_TRANSLATE("Unknown title");
	fTitleView->SetText(title.String());

	BString artist = player.GetString("artist", "");
	const char* album = player.GetString("album", "");
	if (album[0] != '\0') {
		if (!artist.IsEmpty())
			artist << " — ";
		artist << album;
	}
	fArtistView->SetText(artist.String());

	BString application;
	switch (player.GetInt32("state", B_MEDIA_SESSION_STOPPED)) {
		case B_MEDIA_SESSION_PLAYING:
			application = B_TRANSLATE("Playing in %app%");
			break;
		case B_MEDIA_SESSION_PAUSED:
			application = B_TRANSLATE("Paused in %app%");
			break;
		default:
			application = B_TRANSLATE("Stopped in %app%");
			break;
	}
	application.ReplaceFirst("%app%", player.GetString("name", "?"));
	fApplicationView->SetText(application.String());

	BMessage artwork;
	fArtworkView->SetArtwork(player.FindMessage("artwork", &artwork) == B_OK
		? &artwork : NULL);

	fProgressView->SetPlayback(player);

	uint32 playPause = B_MEDIA_ACTION_FLAG(B_MEDIA_ACTION_TOGGLE)
		| B_MEDIA_ACTION_FLAG(fPlaying ? B_MEDIA_ACTION_PAUSE
			: B_MEDIA_ACTION_PLAY);
	fPlayButton->SetSymbol(fPlaying ? kSymbolPause : kSymbolPlay);
	fPlayButton->SetEnabled((fActions & playPause) != 0);
	fPreviousButton->SetEnabled(
		(fActions & B_MEDIA_ACTION_FLAG(B_MEDIA_ACTION_PREVIOUS)) != 0);
	fNextButton->SetEnabled(
		(fActions & B_MEDIA_ACTION_FLAG(B_MEDIA_ACTION_NEXT)) != 0);

	_Place();
}


/*!	Next to the replicant: under it when the Deskbar is at the top of the
	screen, else above it; inside the screen.
*/
void
NowPlayingWindow::_Place()
{
	BSize size = GetLayout()->PreferredSize();
	float width = std::min(std::max(size.width, 300 * font_scale()),
		420 * font_scale());
	ResizeTo(width, size.height);

	BRect screen = BScreen(this).Frame();
	const float kMargin = 3;
	float left = fAnchor.left + (fAnchor.Width() - width) / 2;
	float top;
	if (fAnchor.top < screen.top + screen.Height() / 2)
		top = fAnchor.bottom + kMargin + 2;
	else
		top = fAnchor.top - size.height - kMargin - 2;

	left = std::min(left, screen.right - width - kMargin);
	left = std::max(left, screen.left + kMargin);
	top = std::min(top, screen.bottom - size.height - kMargin);
	top = std::max(top, screen.top + kMargin);
	MoveTo(roundf(left), roundf(top));
}
