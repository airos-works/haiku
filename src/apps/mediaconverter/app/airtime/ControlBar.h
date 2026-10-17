/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIRTIME_CONTROL_BAR_H
#define AIRTIME_CONTROL_BAR_H


#include <vector>

#include <Messenger.h>
#include <String.h>
#include <View.h>

#include "Look.h"


namespace airtime {

/*!	The movie controller: timeline with playhead, transport buttons, volume
	and the LCD. In the window it is Amp's grey; in full screen it floats
	over the picture in dark glass (`hud`). It only reports what the user
	did, as messages to its target; the window decides what that means. */
class ControlBar : public BView {
public:
								ControlBar(BRect frame, bool hud,
									const BMessenger& target);

	static	float				PreferredHeight(bool hud);
	static	float				MinimumWidth(bool hud);

			void				SetMedia(bool hasMedia, bool hasVideo,
									bool hasAudio);
			void				SetPosition(bigtime_t position,
									bigtime_t duration);
			void				SetPlaying(bool playing);
			void				SetRate(double rate, bool scanning);
			void				SetVolume(float volume, bool muted);
			void				SetChapters(
									const std::vector<bigtime_t>& starts);
			void				SetFullScreen(bool fullScreen);
			void				SetTimeDisplay(int mode);
			int					TimeDisplay() const { return fTimeDisplay; }
			void				SetTitle(const char* title);
			bool				IsTracking() const { return fPressed != PART_NONE; }

	virtual	void				AttachedToWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				FrameResized(float width, float height);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				MouseUp(BPoint where);

private:
			enum part {
				PART_NONE,
				PART_TIMELINE,
				PART_SPEAKER,
				PART_VOLUME,
				PART_TO_START,
				PART_REWIND,
				PART_PLAY,
				PART_FAST_FORWARD,
				PART_TO_END,
				PART_LCD,
				PART_FULL_SCREEN
			};

			void				_Layout();
			part				_PartAt(BPoint where) const;
			BRect				_PartFrame(part which) const;
			button_state		_StateOf(part which) const;
			void				_DrawButton(part which, glyph_kind glyph,
									bool prominent);
			void				_DrawTimeline();
			void				_DrawVolume();
			void				_DrawLCD();
			float				_TimelineX(bigtime_t time) const;
			bigtime_t			_TimeAt(float x) const;
			float				_VolumeAt(float x) const;
			void				_Send(BMessage* message);
			void				_InvalidatePart(part which);

			BMessenger			fTarget;
			bool				fHud;
			bool				fHasMedia;
			bool				fHasVideo;
			bool				fHasAudio;
			bigtime_t			fPosition;
			bigtime_t			fDuration;
			bool				fPlaying;
			double				fRate;
			bool				fScanning;
			float				fVolume;
			bool				fMuted;
			bool				fFullScreen;
			int					fTimeDisplay;
			BString				fTitle;
			std::vector<bigtime_t> fChapters;

			BRect				fTrack;
			BRect				fTimelineHit;
			BRect				fSpeaker;
			BRect				fVolumeTrack;
			BRect				fToStart;
			BRect				fRewind;
			BRect				fPlay;
			BRect				fFastForward;
			BRect				fToEnd;
			BRect				fLCD;
			BRect				fFullScreenButton;
			bool				fShowEnds;
			bool				fShowLCD;
			bool				fShowVolume;

			part				fPressed;
			part				fHover;
			bool				fInside;
			bigtime_t			fPressedAt;
			bigtime_t			fDragTime;
			bigtime_t			fLastDragSent;
};

}	// namespace airtime

#endif	// AIRTIME_CONTROL_BAR_H
