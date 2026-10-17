/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "ControlBar.h"

#include <math.h>

#include <Font.h>
#include <Message.h>
#include <Window.h>

#include "AirTimeMessages.h"
#include "TimeFormat.h"


namespace airtime {


static const float kBarHeight = 62;
static const float kHudHeight = 74;


ControlBar::ControlBar(BRect frame, bool hud, const BMessenger& target)
	:
	BView(frame, hud ? "hud controller" : "controller",
		hud ? B_FOLLOW_NONE : B_FOLLOW_LEFT_RIGHT | B_FOLLOW_BOTTOM,
		B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
	fTarget(target),
	fHud(hud),
	fHasMedia(false),
	fHasVideo(false),
	fHasAudio(false),
	fPosition(0),
	fDuration(0),
	fPlaying(false),
	fRate(1.0),
	fScanning(false),
	fVolume(1.0f),
	fMuted(false),
	fFullScreen(hud),
	fTimeDisplay(0),
	fShowEnds(true),
	fShowLCD(true),
	fShowVolume(true),
	fPressed(PART_NONE),
	fHover(PART_NONE),
	fInside(false),
	fPressedAt(0),
	fDragTime(0),
	fLastDragSent(0)
{
	SetViewColor(B_TRANSPARENT_COLOR);
	SetLowColor(look::kSurface);
	_Layout();
}


/*static*/ float
ControlBar::PreferredHeight(bool hud)
{
	return hud ? kHudHeight : kBarHeight;
}


/*static*/ float
ControlBar::MinimumWidth(bool hud)
{
	return hud ? 420 : 300;
}


void
ControlBar::AttachedToWindow()
{
	BView::AttachedToWindow();
	_Layout();
}


void
ControlBar::SetMedia(bool hasMedia, bool hasVideo, bool hasAudio)
{
	fHasMedia = hasMedia;
	fHasVideo = hasVideo;
	fHasAudio = hasAudio;
	_Layout();
	Invalidate();
}


void
ControlBar::SetPosition(bigtime_t position, bigtime_t duration)
{
	if (position == fPosition && duration == fDuration)
		return;
	float oldX = _TimelineX(fPosition);
	BString oldText = format_time(fPosition);
	fPosition = position;
	bool durationChanged = duration != fDuration;
	fDuration = duration;
	if (durationChanged) {
		Invalidate();
		return;
	}
	if (fPressed != PART_TIMELINE && fabsf(_TimelineX(fPosition) - oldX) >= 0.5f)
		Invalidate(fTimelineHit);
	if (format_time(fPosition) != oldText && fShowLCD)
		Invalidate(fLCD.InsetByCopy(-2, -2));
}


void
ControlBar::SetPlaying(bool playing)
{
	if (playing == fPlaying)
		return;
	fPlaying = playing;
	Invalidate(fPlay.InsetByCopy(-3, -3));
}


void
ControlBar::SetRate(double rate, bool scanning)
{
	if (rate == fRate && scanning == fScanning)
		return;
	fRate = rate;
	fScanning = scanning;
	Invalidate(fLCD.InsetByCopy(-2, -2));
	Invalidate(fRewind.InsetByCopy(-3, -3));
	Invalidate(fFastForward.InsetByCopy(-3, -3));
}


void
ControlBar::SetVolume(float volume, bool muted)
{
	if (volume == fVolume && muted == fMuted)
		return;
	fVolume = volume;
	fMuted = muted;
	BRect dirty = fSpeaker | fVolumeTrack;
	Invalidate(dirty.InsetByCopy(-8, -8));
}


void
ControlBar::SetChapters(const std::vector<bigtime_t>& starts)
{
	fChapters = starts;
	Invalidate(fTimelineHit);
}


void
ControlBar::SetFullScreen(bool fullScreen)
{
	if (fullScreen == fFullScreen)
		return;
	fFullScreen = fullScreen;
	Invalidate(fFullScreenButton.InsetByCopy(-3, -3));
}


void
ControlBar::SetTimeDisplay(int mode)
{
	fTimeDisplay = mode;
	Invalidate(fLCD.InsetByCopy(-2, -2));
}


void
ControlBar::SetTitle(const char* title)
{
	fTitle = title;
	Invalidate(fLCD.InsetByCopy(-2, -2));
}


void
ControlBar::FrameResized(float width, float height)
{
	_Layout();
	Invalidate();
}


void
ControlBar::_Layout()
{
	BRect bounds = Bounds();
	float width = bounds.Width() + 1;
	float margin = fHud ? 18 : 12;

	// The timeline across the whole width.
	float trackTop = fHud ? 18 : 14;
	float trackHeight = fHud ? 6 : 8;
	fTrack.Set(margin, trackTop, width - margin - 1, trackTop + trackHeight);
	fTimelineHit.Set(margin - 7, trackTop - 11, width - margin + 6,
		trackTop + trackHeight + 5);

	// The row of controls below it.
	float row = fHud ? 50 : 43;
	fShowEnds = width >= (fHud ? 520 : 400);
	fShowVolume = width >= 260;

	fSpeaker.Set(margin, row - 8, margin + 16, row + 8);
	fVolumeTrack.Set(fSpeaker.right + 5, row - 3, fSpeaker.right + 5
		+ (fHud ? 80 : 64), row + 3);

	float endSize = fHud ? 24 : 22;
	float scanSize = fHud ? 28 : 26;
	float playSize = fHud ? 34 : 32;
	float gap = fHud ? 10 : 5;
	float cluster = scanSize * 2 + playSize + gap * 2;
	if (fShowEnds)
		cluster += endSize * 2 + gap * 2;
	float x = floorf(width / 2 - cluster / 2);
	auto place = [&](BRect& rect, float size, bool shown) {
		if (!shown) {
			rect.Set(-100, -100, -90, -90);
			return;
		}
		rect.Set(x, roundf(row - size / 2), x + size, roundf(row - size / 2)
			+ size);
		x += size + gap;
	};
	place(fToStart, endSize, fShowEnds);
	place(fRewind, scanSize, true);
	place(fPlay, playSize, true);
	place(fFastForward, scanSize, true);
	place(fToEnd, endSize, fShowEnds);
	float clusterRight = x - gap;

	float right = width - margin;
	if (fHasVideo || fHud) {
		fFullScreenButton.Set(right - 20, row - 10, right, row + 10);
		right = fFullScreenButton.left - 10;
	} else
		fFullScreenButton.Set(-100, -100, -90, -90);

	float lcdWidth = right - (clusterRight + 12);
	if (lcdWidth > (fHud ? 150 : 172))
		lcdWidth = fHud ? 150 : 172;
	fShowLCD = lcdWidth >= 96;
	if (fShowLCD)
		fLCD.Set(right - lcdWidth, row - 11, right, row + 11);
	else
		fLCD.Set(-100, -100, -90, -90);

	// Hide the volume slider rather than run it into the buttons.
	float leftLimit = (fShowEnds ? fToStart.left : fRewind.left) - 10;
	if (fVolumeTrack.right > leftLimit) {
		fVolumeTrack.right = leftLimit;
		if (fVolumeTrack.Width() < 30)
			fVolumeTrack.Set(-100, -100, -90, -90);
	}
}


BRect
ControlBar::_PartFrame(part which) const
{
	switch (which) {
		case PART_TIMELINE:		return fTimelineHit;
		case PART_SPEAKER:		return fSpeaker;
		case PART_VOLUME:		return fVolumeTrack.InsetByCopy(-6, -6);
		case PART_TO_START:		return fToStart;
		case PART_REWIND:		return fRewind;
		case PART_PLAY:			return fPlay;
		case PART_FAST_FORWARD:	return fFastForward;
		case PART_TO_END:		return fToEnd;
		case PART_LCD:			return fLCD;
		case PART_FULL_SCREEN:	return fFullScreenButton;
		default:				return BRect();
	}
}


ControlBar::part
ControlBar::_PartAt(BPoint where) const
{
	static const part kParts[] = {PART_PLAY, PART_REWIND, PART_FAST_FORWARD,
		PART_TO_START, PART_TO_END, PART_FULL_SCREEN, PART_LCD, PART_SPEAKER,
		PART_VOLUME, PART_TIMELINE};
	for (part which : kParts) {
		BRect frame = _PartFrame(which);
		if (!frame.IsValid() || frame.left < -50)
			continue;
		if (which == PART_PLAY || which == PART_REWIND
			|| which == PART_FAST_FORWARD || which == PART_TO_START
			|| which == PART_TO_END) {
			// Round buttons: test the circle.
			BPoint center((frame.left + frame.right) / 2,
				(frame.top + frame.bottom) / 2);
			float radius = frame.Width() / 2 + 1;
			float dx = where.x - center.x;
			float dy = where.y - center.y;
			if (dx * dx + dy * dy <= radius * radius)
				return which;
			continue;
		}
		if (frame.Contains(where))
			return which;
	}
	return PART_NONE;
}


button_state
ControlBar::_StateOf(part which) const
{
	if (!fHasMedia)
		return BUTTON_DISABLED;
	if (fPressed == which && fInside)
		return BUTTON_PRESSED;
	if ((which == PART_REWIND && fScanning && fRate < 0)
		|| (which == PART_FAST_FORWARD && fScanning && fRate > 0)) {
		return BUTTON_PRESSED;
	}
	if (fHover == which && fPressed == PART_NONE)
		return BUTTON_HOVER;
	return BUTTON_NORMAL;
}


float
ControlBar::_TimelineX(bigtime_t time) const
{
	if (fDuration <= 0)
		return fTrack.left;
	double fraction = (double)time / fDuration;
	if (fraction < 0)
		fraction = 0;
	if (fraction > 1)
		fraction = 1;
	return fTrack.left + 1 + (fTrack.Width() - 2) * fraction;
}


bigtime_t
ControlBar::_TimeAt(float x) const
{
	if (fDuration <= 0 || fTrack.Width() <= 2)
		return 0;
	double fraction = (x - fTrack.left - 1) / (fTrack.Width() - 2);
	if (fraction < 0)
		fraction = 0;
	if (fraction > 1)
		fraction = 1;
	return (bigtime_t)(fraction * fDuration);
}


float
ControlBar::_VolumeAt(float x) const
{
	if (fVolumeTrack.Width() <= 0)
		return fVolume;
	float fraction = (x - fVolumeTrack.left) / fVolumeTrack.Width();
	return fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
}


// #pragma mark - drawing


void
ControlBar::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	if (fHud) {
		SetHighColor(0, 0, 0);
		FillRect(updateRect);
		look::hud_panel(this, bounds.InsetByCopy(1, 1));
	} else {
		look::toolbar(this, updateRect, bounds);
	}

	if (updateRect.Intersects(fTimelineHit))
		_DrawTimeline();
	if (fShowVolume && updateRect.Intersects((fSpeaker | fVolumeTrack)
			.InsetByCopy(-8, -8)))
		_DrawVolume();

	_DrawButton(PART_TO_START, GLYPH_TO_START, false);
	_DrawButton(PART_REWIND, GLYPH_REWIND, false);
	_DrawButton(PART_PLAY, fPlaying && !fScanning ? GLYPH_PAUSE : GLYPH_PLAY,
		true);
	_DrawButton(PART_FAST_FORWARD, GLYPH_FAST_FORWARD, false);
	_DrawButton(PART_TO_END, GLYPH_TO_END, false);
	if (fFullScreenButton.left > -50) {
		_DrawButton(PART_FULL_SCREEN, fFullScreen
			? GLYPH_EXIT_FULL_SCREEN : GLYPH_FULL_SCREEN, false);
	}
	if (fShowLCD && updateRect.Intersects(fLCD.InsetByCopy(-2, -2)))
		_DrawLCD();
}


void
ControlBar::_DrawButton(part which, glyph_kind glyph, bool prominent)
{
	BRect frame = _PartFrame(which);
	if (frame.left < -50)
		return;
	button_state state = _StateOf(which);

	if (fHud) {
		rgb_color color = look::color(232, 232, 236);
		if (state == BUTTON_HOVER)
			color = look::color(255, 255, 255);
		else if (state == BUTTON_PRESSED)
			color = look::color(150, 170, 200);
		else if (state == BUTTON_DISABLED)
			color = look::color(120, 120, 120);
		if (state == BUTTON_HOVER || state == BUTTON_PRESSED) {
			PushState();
			SetDrawingMode(B_OP_ALPHA);
			SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
			SetHighColor(255, 255, 255, 28);
			FillEllipse(frame.InsetByCopy(-2, -2));
			PopState();
		}
		BRect glyphRect = frame.InsetByCopy(frame.Width() * 0.12f,
			frame.Height() * 0.12f);
		look::glyph(this, glyph, glyphRect, color);
		return;
	}

	if (which == PART_FULL_SCREEN) {
		// A small square button rather than a round one.
		PushState();
		SetDrawingMode(B_OP_ALPHA);
		SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
		if (state == BUTTON_HOVER || state == BUTTON_PRESSED) {
			SetHighColor(state == BUTTON_PRESSED ? look::color(0, 0, 0, 40)
				: look::color(255, 255, 255, 90));
			FillRoundRect(frame, 3, 3);
		}
		PopState();
		look::glyph(this, glyph, frame.InsetByCopy(3, 3),
			state == BUTTON_DISABLED ? look::color(140, 140, 140)
				: look::color(48, 48, 52));
		return;
	}

	look::round_button(this, frame, state, prominent);
	float inset = frame.Width() * (prominent ? 0.22f : 0.24f);
	BRect glyphRect = frame.InsetByCopy(inset, inset);
	if (glyph == GLYPH_PLAY)
		glyphRect.OffsetBy(frame.Width() * 0.03f, 0);
	rgb_color color = state == BUTTON_DISABLED ? look::color(150, 150, 150)
		: look::color(44, 44, 48);
	// An engraved look: a light edge under the dark glyph.
	if (state != BUTTON_DISABLED) {
		look::glyph(this, glyph, glyphRect.OffsetByCopy(0, 1),
			look::color(255, 255, 255, 160));
	}
	look::glyph(this, glyph, glyphRect, color);
}


void
ControlBar::_DrawTimeline()
{
	bigtime_t shown = fPressed == PART_TIMELINE ? fDragTime : fPosition;
	float x = _TimelineX(shown);
	look::timeline_track(this, fTrack, fHasMedia ? x : fTrack.left, fHud);

	// Chapter marks.
	if (fDuration > 0 && !fChapters.empty()) {
		PushState();
		SetDrawingMode(B_OP_ALPHA);
		SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
		SetHighColor(fHud ? look::color(255, 255, 255, 110)
			: look::color(30, 30, 34, 120));
		for (bigtime_t start : fChapters) {
			if (start <= 0)
				continue;
			float chapterX = roundf(_TimelineX(start));
			StrokeLine(BPoint(chapterX, fTrack.top + 1),
				BPoint(chapterX, fTrack.bottom - 1));
		}
		PopState();
	}

	if (fHasMedia && fDuration > 0) {
		button_state state = fPressed == PART_TIMELINE ? BUTTON_PRESSED
			: fHover == PART_TIMELINE ? BUTTON_HOVER : BUTTON_NORMAL;
		look::playhead(this, BPoint(roundf(x) + 0.5f, fTrack.bottom - 1),
			fHud ? 12 : 13, state, fHud);
	}
}


void
ControlBar::_DrawVolume()
{
	button_state state = _StateOf(PART_SPEAKER);
	rgb_color color = fHud ? look::color(232, 232, 236)
		: look::color(48, 48, 52);
	if (!fHasAudio)
		color = fHud ? look::color(110, 110, 110) : look::color(140, 140, 140);
	else if (state == BUTTON_PRESSED)
		color = look::mix(color, look::color(128, 128, 128), 0.5f);
	look::glyph(this, fMuted ? GLYPH_SPEAKER_MUTED : GLYPH_SPEAKER, fSpeaker,
		color);

	if (fVolumeTrack.left < -50)
		return;
	float fraction = fMuted ? 0 : fVolume;
	look::slider_track(this, fVolumeTrack, fraction, fHud);
	float knobX = fVolumeTrack.left + fVolumeTrack.Width() * fraction;
	look::slider_knob(this, BPoint(roundf(knobX), (fVolumeTrack.top
		+ fVolumeTrack.bottom) / 2), fPressed == PART_VOLUME
			? BUTTON_PRESSED : BUTTON_NORMAL, fHud);
}


void
ControlBar::_DrawLCD()
{
	if (!fHud)
		look::lcd(this, fLCD);

	BFont font(be_bold_font);
	font.SetSize(fHud ? 13 : 12);
	SetFont(&font);
	font_height height;
	font.GetHeight(&height);
	float baseline = roundf((fLCD.top + fLCD.bottom) / 2
		+ (height.ascent - height.descent) / 2);

	rgb_color text = fHud ? look::color(240, 240, 244)
		: look::lcd_text_color();
	rgb_color dim = fHud ? look::color(170, 170, 176)
		: look::lcd_dim_text_color();

	PushState();
	SetDrawingMode(B_OP_OVER);
	if (!fHasMedia) {
		SetHighColor(dim);
		const char* label = "airTime";
		DrawString(label, BPoint(fLCD.left + (fLCD.Width()
			- font.StringWidth(label)) / 2, baseline));
		PopState();
		return;
	}

	bigtime_t shown = fPressed == PART_TIMELINE ? fDragTime : fPosition;
	bool hours = fDuration >= 3600000000LL;
	BString time;
	if (fTimeDisplay == 1 && fDuration > 0) {
		time = "-";
		time << format_time(fDuration - shown, false, hours);
	} else {
		time = format_time(shown, false, hours);
		if (fDuration > 0)
			time << " / " << format_time(fDuration, false, hours);
	}

	// The speed, when it is not normal.
	BString rate;
	if (fScanning || fabs(fRate - 1.0) > 0.001) {
		double shownRate = fRate;
		if (fabs(shownRate - roundf(shownRate)) < 0.001)
			rate.SetToFormat("%s%d×", shownRate < 0 ? "−" : "",
				(int)fabs(roundf(shownRate)));
		else
			rate.SetToFormat("%g×", shownRate);
	}

	float padding = 7;
	float timeWidth = font.StringWidth(time.String());
	BFont small(font);
	small.SetSize(fHud ? 11 : 10);
	float rateWidth = rate.Length() > 0 ? small.StringWidth(rate.String()) : 0;
	if (timeWidth + rateWidth + padding * 3 > fLCD.Width() && fDuration > 0
		&& fTimeDisplay != 1) {
		// Not enough room for both: the elapsed time alone.
		time = format_time(shown, false, hours);
		timeWidth = font.StringWidth(time.String());
	}

	SetHighColor(text);
	DrawString(time.String(), BPoint(fLCD.right - padding - timeWidth,
		baseline));
	if (rate.Length() > 0) {
		SetFont(&small);
		SetHighColor(dim);
		DrawString(rate.String(), BPoint(fLCD.left + padding, baseline));
	}
	PopState();
}


// #pragma mark - mouse


void
ControlBar::_Send(BMessage* message)
{
	fTarget.SendMessage(message);
}


void
ControlBar::_InvalidatePart(part which)
{
	if (which == PART_NONE)
		return;
	BRect frame = _PartFrame(which);
	if (which == PART_SPEAKER || which == PART_VOLUME)
		frame = (fSpeaker | fVolumeTrack).InsetByCopy(-8, -8);
	if (which == PART_TIMELINE)
		Invalidate(fLCD.InsetByCopy(-2, -2));
	Invalidate(frame.InsetByCopy(-3, -3));
}


void
ControlBar::MouseDown(BPoint where)
{
	if (fHud) {
		BMessage activity(kMsgControlsActivity);
		_Send(&activity);
	}
	part which = _PartAt(where);
	if (!fHasMedia && which != PART_NONE)
		return;
	if (which == PART_NONE) {
		if (fHud)
			return;
		// The bar itself drags the window, as QuickTime's did.
		BMessage* current = Window()->CurrentMessage();
		int32 clicks = 1;
		if (current != NULL)
			current->FindInt32("clicks", &clicks);
		BPoint screen = ConvertToScreen(where);
		uint32 buttons;
		BPoint at;
		GetMouse(&at, &buttons, false);
		BPoint last = screen;
		while (buttons != 0) {
			snooze(10000);
			GetMouse(&at, &buttons, true);
			BPoint now = ConvertToScreen(at);
			if (now != last) {
				Window()->MoveBy(now.x - last.x, now.y - last.y);
				last = now;
			}
		}
		return;
	}

	fPressed = which;
	fInside = true;
	fPressedAt = system_time();
	SetMouseEventMask(B_POINTER_EVENTS,
		B_LOCK_WINDOW_FOCUS | B_NO_POINTER_HISTORY);

	switch (which) {
		case PART_TIMELINE:
		{
			fDragTime = _TimeAt(where.x);
			BMessage message(kMsgSeekTo);
			message.AddInt64("time", fDragTime);
			message.AddBool("final", false);
			_Send(&message);
			fLastDragSent = system_time();
			break;
		}
		case PART_VOLUME:
		{
			BMessage message(kMsgSetVolume);
			message.AddFloat("volume", _VolumeAt(where.x));
			message.AddBool("final", false);
			_Send(&message);
			break;
		}
		case PART_REWIND:
		case PART_FAST_FORWARD:
		{
			BMessage message(kMsgScanPress);
			message.AddInt32("direction", which == PART_REWIND ? -1 : 1);
			_Send(&message);
			break;
		}
		default:
			break;
	}
	_InvalidatePart(which);
}


void
ControlBar::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	if (fHud) {
		BMessage activity(kMsgControlsActivity);
		_Send(&activity);
	}

	if (fPressed == PART_TIMELINE) {
		bigtime_t time = _TimeAt(where.x);
		if (time != fDragTime) {
			fDragTime = time;
			Invalidate(fTimelineHit);
			Invalidate(fLCD.InsetByCopy(-2, -2));
			bigtime_t now = system_time();
			if (now - fLastDragSent > 40000) {
				BMessage message(kMsgSeekTo);
				message.AddInt64("time", fDragTime);
				message.AddBool("final", false);
				_Send(&message);
				fLastDragSent = now;
			}
		}
		return;
	}
	if (fPressed == PART_VOLUME) {
		BMessage message(kMsgSetVolume);
		message.AddFloat("volume", _VolumeAt(where.x));
		message.AddBool("final", false);
		_Send(&message);
		return;
	}
	if (fPressed != PART_NONE) {
		bool inside = _PartAt(where) == fPressed;
		if (inside != fInside) {
			fInside = inside;
			_InvalidatePart(fPressed);
		}
		return;
	}

	part hover = transit == B_EXITED_VIEW || transit == B_OUTSIDE_VIEW
		? PART_NONE : _PartAt(where);
	if (hover != fHover) {
		part old = fHover;
		fHover = hover;
		_InvalidatePart(old);
		_InvalidatePart(hover);
	}
}


void
ControlBar::MouseUp(BPoint where)
{
	part which = fPressed;
	bool inside = fInside && _PartAt(where) == which;
	fPressed = PART_NONE;
	fInside = false;

	switch (which) {
		case PART_TIMELINE:
		{
			fDragTime = _TimeAt(where.x);
			BMessage message(kMsgSeekTo);
			message.AddInt64("time", fDragTime);
			message.AddBool("final", true);
			_Send(&message);
			// Until the player reports it, show where the user let go.
			fPosition = fDragTime;
			break;
		}
		case PART_VOLUME:
		{
			BMessage message(kMsgSetVolume);
			message.AddFloat("volume", _VolumeAt(where.x));
			message.AddBool("final", true);
			_Send(&message);
			break;
		}
		case PART_REWIND:
		case PART_FAST_FORWARD:
		{
			BMessage message(kMsgScanRelease);
			message.AddInt32("direction", which == PART_REWIND ? -1 : 1);
			message.AddInt64("held", system_time() - fPressedAt);
			_Send(&message);
			break;
		}
		case PART_PLAY:
			if (inside) {
				BMessage message(kMsgTogglePlay);
				_Send(&message);
			}
			break;
		case PART_TO_START:
			if (inside) {
				BMessage message(kMsgGoToStart);
				_Send(&message);
			}
			break;
		case PART_TO_END:
			if (inside) {
				BMessage message(kMsgGoToEnd);
				_Send(&message);
			}
			break;
		case PART_SPEAKER:
			if (inside) {
				BMessage message(kMsgToggleMute);
				_Send(&message);
			}
			break;
		case PART_LCD:
			if (inside) {
				fTimeDisplay = fTimeDisplay == 0 ? 1 : 0;
				BMessage message(kMsgToggleTimeDisplay);
				message.AddInt32("mode", fTimeDisplay);
				_Send(&message);
			}
			break;
		case PART_FULL_SCREEN:
			if (inside) {
				BMessage message(kMsgToggleFullScreen);
				_Send(&message);
			}
			break;
		default:
			break;
	}
	_InvalidatePart(which);
}

}	// namespace airtime
