/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef WINDOW_TRACKER_H
#define WINDOW_TRACKER_H

/*	WindowTracker: the shell's window manager.

	app_server has no pluggable window manager, so the shell polls the
	window list (the private calls the Deskbar uses) and, through each
	window's scripting suite, makes mobile application windows borderless and
	sizes it to the work area between the status bar and the navigation panel
	(and the keyboard, when it is up). Windows that can't take that size,
	fixed-size ones or ones whose application caps their size, keep theirs
	and are centred, with a line around them. Desktop-only windows stay in
	the switcher with a close action.

	What a window was like before is kept (SavedState) and put back by
	RestoreDesktop(), as are the Deskbar's windows and Tracker's desktop,
	which HideDesktop() hides while the shell is up.
*/

#include "SavedState.h"

#include <Messenger.h>
#include <OS.h>
#include <Rect.h>
#include <String.h>

#include <map>
#include <vector>


struct TrackedWindow {
	int32		token;		// app_server token
	team_id		team;
	BString		title;
	bool		minimized;
	bool		resizable;
	BMessenger	messenger;	// the BWindow, for scripting
	BRect		frame;
	int32		fits;		// how often we've had to resize it
	bool		capped;		// resizable, but its app won't let it fill the area
	float		capWidth;	// ... beyond this size (INFINITY: that way it does)
	float		capHeight;
	bool		mobile;		// executable declares a Mobile Kit view
	bool		adopted;	// what it was like is saved, and it's ours to size
	int32		lookTries;	// how often it didn't say what it looks like
	bool		seen;
};


struct client_window_info;

class WindowTracker {
public:
								WindowTracker(team_id self);

			// Polls app_server. Returns true when the list changed.
			bool				Update();
			bool				PrepareWindow(BMessenger target, BRect frame,
									int32 look);

			void				SetWorkArea(BRect area);
			BRect				WorkArea() const { return fWorkArea; }

			const std::vector<TrackedWindow>& Windows() const
									{ return fWindows; }
			const TrackedWindow* Front() const;

			void				Activate(int32 token);
			void				Close(int32 token);
			void				MinimizeAll();

			// The Deskbar and Tracker's desktop make way for the shell...
			void				HideDesktop();
			// ...and come back, with every window as it was.
			void				RestoreDesktop();

private:
			bool				_Adopt(TrackedWindow& window);
			void				_Fit(TrackedWindow& window, bool force);
			BRect				_TargetFrame(const TrackedWindow& window) const;
			TrackedWindow*		_Find(int32 token);
			void				_Prune();
			bool				_IsBackground(team_id team);
			bool				_IsMobileFloating(const client_window_info& info);
			std::map<int32, team_id> fAuxiliary;
			void				_HideWindowsOf(const char* signature,
									bool desktopOnly);

			team_id				fSelf;
			BRect				fWorkArea;
			std::vector<TrackedWindow> fWindows;	// front to back
			SavedState			fState;
			std::map<team_id, bool> fBackground;
};


#endif	// WINDOW_TRACKER_H
