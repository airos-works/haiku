/*
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */


#include "SavedState.h"

#include <Deskbar.h>
#include <File.h>
#include <FindDirectory.h>
#include <InterfaceDefs.h>
#include <Message.h>
#include <Messenger.h>
#include <Path.h>
#include <Roster.h>
#include <Window.h>

#include <MessengerPrivate.h>
#include <WindowInfo.h>

#include <stdio.h>
#include <stdlib.h>


static const uint32 kStateWhat = 'mSst';
static const bigtime_t kSendTimeout = 300000;


static status_t
state_path(BPath& path)
{
	status_t status = find_directory(B_SYSTEM_TEMP_DIRECTORY, &path, true);
	if (status == B_OK)
		status = path.Append("mobile_shell_state");
	return status;
}


/*!	The messenger of the window with \a token, if it is still there and
	still \a team's. */
static bool
window_messenger(int32 token, team_id team, BMessenger& messenger,
	bool* _minimized = NULL)
{
	client_window_info* info = get_window_info(token);
	if (info == NULL)
		return false;
	bool same = info->team == team;
	if (same) {
		BMessenger::Private(messenger).SetTo(info->team, info->client_port,
			info->client_token);
		if (_minimized != NULL)
			*_minimized = info->is_mini;
	}
	free(info);
	return same;
}


bool
SavedState::Load()
{
	windows.clear();
	hidden.clear();

	BPath path;
	BFile file;
	BMessage state;
	if (state_path(path) != B_OK
		|| file.SetTo(path.Path(), B_READ_ONLY) != B_OK
		|| state.Unflatten(&file) != B_OK || state.what != kStateWhat) {
		return false;
	}

	// Tokens mean nothing after a restart.
	system_info info;
	get_system_info(&info);
	if (state.GetInt64("boot", 0) != info.boot_time)
		return false;

	BMessage window;
	for (int32 i = 0; state.FindMessage("window", i, &window) == B_OK; i++) {
		SavedWindow saved;
		saved.token = window.GetInt32("token", -1);
		saved.team = window.GetInt32("team", -1);
		saved.look = window.GetInt32("look", -1);
		saved.frame = window.GetRect("frame", BRect());
		saved.wasMinimized = window.GetBool("minimized", false);
		if (saved.token >= 0 && saved.frame.IsValid())
			windows.push_back(saved);
	}
	int32 token;
	for (int32 i = 0; state.FindInt32("hidden", i, &token) == B_OK; i++) {
		HiddenWindow window = { token, state.GetInt32("hidden team", i, -1) };
		hidden.push_back(window);
	}
	return true;
}


void
SavedState::Save() const
{
	BMessage state(kStateWhat);
	system_info info;
	get_system_info(&info);
	state.AddInt64("boot", info.boot_time);

	for (const SavedWindow& saved : windows) {
		BMessage window;
		window.AddInt32("token", saved.token);
		window.AddInt32("team", saved.team);
		window.AddInt32("look", saved.look);
		window.AddRect("frame", saved.frame);
		window.AddBool("minimized", saved.wasMinimized);
		state.AddMessage("window", &window);
	}
	for (const HiddenWindow& window : hidden) {
		state.AddInt32("hidden", window.token);
		state.AddInt32("hidden team", window.team);
	}

	BPath path;
	if (state_path(path) != B_OK)
		return;
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() == B_OK)
		state.Flatten(&file);
}


void
SavedState::Remove()
{
	windows.clear();
	hidden.clear();
	BPath path;
	if (state_path(path) == B_OK)
		remove(path.Path());
}


SavedWindow*
SavedState::FindWindow(int32 token, team_id team)
{
	for (SavedWindow& saved : windows) {
		if (saved.token == token && saved.team == team)
			return &saved;
	}
	return NULL;
}


bool
SavedState::IsHidden(int32 token) const
{
	for (const HiddenWindow& window : hidden) {
		if (window.token == token)
			return true;
	}
	return false;
}


void
SavedState::Restore()
{
	// The windows that are back to front, so that bringing up those the
	// shell minimized leaves them stacked as they are.
	int32* tokens = NULL;
	int32 count = 0;
	std::vector<int32> order;
	if (BPrivate::get_window_order(current_workspace(), &tokens, &count)
			== B_OK) {
		for (int32 i = count - 1; i >= 0; i--)
			order.push_back(tokens[i]);
		free(tokens);
	}
	// then the ones on other workspaces, which stay where they are
	for (const SavedWindow& saved : windows) {
		bool listed = false;
		for (int32 token : order)
			listed = listed || token == saved.token;
		if (!listed)
			order.insert(order.begin(), saved.token);
	}

	for (int32 token : order) {
		SavedWindow* saved = NULL;
		for (SavedWindow& candidate : windows) {
			if (candidate.token == token)
				saved = &candidate;
		}
		BMessenger messenger;
		bool minimized = false;
		if (saved == NULL
			|| !window_messenger(token, saved->team, messenger, &minimized)) {
			continue;
		}

		BMessage look(B_SET_PROPERTY);
		look.AddSpecifier("Look");
		look.AddInt32("data", saved->look >= 0 ? saved->look
			: (int32)B_TITLED_WINDOW_LOOK);
		messenger.SendMessage(&look, (BHandler*)NULL, kSendTimeout);

		BMessage frame(B_SET_PROPERTY);
		frame.AddSpecifier("Frame");
		frame.AddRect("data", saved->frame);
		messenger.SendMessage(&frame, (BHandler*)NULL, kSendTimeout);

		// Going home minimized it; its owner had it open.
		if (minimized && !saved->wasMinimized)
			do_window_action(token, B_BRING_TO_FRONT, BRect(), false);
	}

	team_id deskbarTeam = be_roster->TeamFor("application/x-vnd.Be-TSKB");
	bool deskbarShown = false;
	for (const HiddenWindow& window : hidden) {
		BMessenger messenger;
		if (!window_messenger(window.token, window.team, messenger))
			continue;
		BMessage show(B_SET_PROPERTY);
		show.AddSpecifier("Hidden");
		show.AddBool("data", false);
		// waited for: the Deskbar is only laid out again once it shows
		BMessage reply;
		messenger.SendMessage(&show, &reply, kSendTimeout, kSendTimeout);
		deskbarShown = deskbarShown || window.team == deskbarTeam;
	}

	// A menu's items are not laid out while its window is hidden: the
	// Deskbar's list of programs is as tall as when it was hidden, whatever
	// came or went since. Being told where it is makes it lay itself out.
	if (deskbarShown) {
		BDeskbar deskbar;
		bool expanded = false;
		deskbar_location location = deskbar.Location(&expanded);
		deskbar.SetLocation(location, expanded);
	}

	Remove();
}
