/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef NOW_PLAYING_WINDOW_H
#define NOW_PLAYING_WINDOW_H


#include <Window.h>


class BStringView;
class ArtworkView;
class ProgressView;
class TransportButton;


/*!	The replicant's popup: artwork, title, artist, progress, and the
	previous, play/pause and next buttons of the active player. It closes
	when it loses the focus.
*/
class NowPlayingWindow : public BWindow {
public:
								NowPlayingWindow(BRect anchor);
	virtual						~NowPlayingWindow();

	virtual	void				Show();
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				WindowActivated(bool active);
	virtual	bool				QuitRequested();

	static	bigtime_t			LastClosed();

private:
			void				_SetState(const BMessage& state);
			void				_Place();

private:
			BRect				fAnchor;
			int64				fSession;
			uint32				fActions;
			bool				fPlaying;

			ArtworkView*		fArtworkView;
			BStringView*		fTitleView;
			BStringView*		fArtistView;
			BStringView*		fApplicationView;
			ProgressView*		fProgressView;
			TransportButton*	fPreviousButton;
			TransportButton*	fPlayButton;
			TransportButton*	fNextButton;
};


#endif	// NOW_PLAYING_WINDOW_H
