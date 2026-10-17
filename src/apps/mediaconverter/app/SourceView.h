/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SOURCE_VIEW_H
#define SOURCE_VIEW_H


#include <View.h>

#include "SourceInfo.h"


class AudioPreview;
class BBitmap;
class BColumnListView;
class BGroupLayout;
class BMessageRunner;
class BStringView;
class BTextView;
class PreviewView;

namespace airtime {
	class ControlBar;
}


/*!	What the selected source holds: a picture from it, a summary, and one
	line per track, much as HandBrake's summary shows them. A song shows its
	album artwork instead, and plays with airTime's controller. */
class SourceView : public BView {
public:
								SourceView();
	virtual						~SourceView();

	virtual	void				AttachedToWindow();
	virtual	void				DetachedFromWindow();
	virtual	void				MessageReceived(BMessage* message);

			void				SetSource(const SourceInfo* info,
									int32 itemID);
			int32				ItemID() const { return fItemID; }
			// takes the bitmap
			void				SetPreview(BBitmap* bitmap, int32 itemID);

private:
			void				_AddTracks(const SourceInfo& info);
			void				_StopPlaying();
			bool				_OpenPlayer();
			void				_Seek(bigtime_t time);
			void				_UpdateControls();

			PreviewView*		fPreview;
			BStringView*		fTitle;
			BStringView*		fSummary;
			BStringView*		fExtra;
			BColumnListView*	fTracks;
			BGroupLayout*		fControlsSlot;
			airtime::ControlBar* fControls;
			int32				fItemID;

			// the song
			bool				fAudioSource;
			BString				fAudioPath;
			bigtime_t			fAudioDuration;
			AudioPreview*		fPlayer;
			BMessageRunner*		fPulse;
			float				fVolume;
			bool				fMuted;
			bool				fScrubbing;
			bool				fScrubWasPlaying;
			bool				fScanHeld;
			bool				fScanWasPlaying;
};


BString	format_level(AVCodecID codec, int level);
BString	format_frame_rate(AVRational rate);


#endif	// SOURCE_VIEW_H
