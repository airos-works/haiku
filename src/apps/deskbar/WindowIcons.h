/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef WINDOW_ICONS_H
#define WINDOW_ICONS_H


#include <Handler.h>
#include <Locker.h>
#include <Messenger.h>

#include <map>
#include <memory>


class BBitmap;


// Icons that applications give their windows (a web browser: the site icon of
// the page shown), shown in the window lists instead of the generic window
// icon.
//
// The Deskbar asks each window it lists for its "Icon" property
// (B_GET_PROPERTY, direct specifier). A window with an icon of its own replies
// with an archived BBitmap in "result"; one that has none at the moment
// replies without "result" and with "error" B_OK. The Deskbar then watches the
// window (BHandler::StartWatching) for kWindowIconChanged notices, which carry
// the new icon the same way. A window that does not know the property is not
// asked again.
const uint32 kWindowIconChanged = 'wicn';

// To the bar view: an icon arrived or changed, the window lists draw again.
const uint32 kWindowIconsChanged = 'Wics';


class WindowIcons : public BHandler {
public:
								WindowIcons(BMessenger barView);
	virtual						~WindowIcons();

	// Any thread: the window's own icon, or NULL. The first call for a window
	// asks it for one.
			std::shared_ptr<BBitmap> IconFor(int32 serverToken);

	// Application thread: forgets the windows of a team that quit.
			void				RemoveTeam(team_id team);

	virtual	void				MessageReceived(BMessage* message);

private:
	class Watcher;
	friend class Watcher;

			struct Entry {
				team_id			team = -1;
				Watcher*		watcher = NULL;
				std::shared_ptr<BBitmap> icon;
			};

			void				_Watch(int32 serverToken);
			void				_RemoveClosedWindows();
			void				_Remove(int32 serverToken);
			void				_SetIcon(int32 serverToken,
									const BMessage& message);

			BLocker				fLock;
			std::map<int32, Entry> fEntries;
			BMessenger			fBarView;
};


#endif	// WINDOW_ICONS_H
