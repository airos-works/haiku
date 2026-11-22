/*
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */
#ifndef SAVED_STATE_H
#define SAVED_STATE_H

/*	What the desktop was like before the mobile shell changed it: the look
	and frame of every window it made full-screen, and the windows it hid
	(the Deskbar's, and Tracker's desktop). The shell puts all of it back when
	it quits.

	It is also kept in a file in the temporary directory, written whenever it
	changes, so that a shell that died can still be cleaned up after
	("MobileShell --restore"), and a shell started again knows what the
	windows it finds full-screen were like before.
*/

#include <OS.h>
#include <Rect.h>

#include <vector>


struct SavedWindow {
	int32		token;			// app_server's
	team_id		team;
	int32		look;			// -1: could not be asked
	BRect		frame;
	bool		wasMinimized;
};


struct HiddenWindow {
	int32		token;
	team_id		team;
};


class SavedState {
public:
			// false when there is none (or one from before the last boot)
			bool				Load();
			void				Save() const;
			void				Remove();

			SavedWindow*		FindWindow(int32 token, team_id team);
			bool				IsHidden(int32 token) const;

			// Puts everything back and forgets it; the file goes too.
			void				Restore();

			std::vector<SavedWindow> windows;
			std::vector<HiddenWindow> hidden;
};


#endif	// SAVED_STATE_H
