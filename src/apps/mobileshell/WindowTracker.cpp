/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */


#include "WindowTracker.h"

#include "Protocol.h"

#include <MobileKit.h>

#include <InterfaceDefs.h>
#include <Message.h>
#include <Roster.h>
#include <Window.h>

#include <MessengerPrivate.h>
#include <WindowInfo.h>
#include <WindowPrivate.h>

#include <algorithm>
#include <math.h>
#include <stdlib.h>
#include <string.h>


static const char* kDeskbarSignature = "application/x-vnd.Be-TSKB";
static const char* kTrackerSignature = "application/x-vnd.Be-TRAK";

// An application that keeps resizing its window (a media player fitting its
// video) gets this many corrections before the shell lets it be.
static const int32 kMaxFits = 6;
// A window that doesn't say what it looks like (its thread is busy) is asked
// again this often, then taken as it is.
static const int32 kMaxLookTries = 5;
static const bigtime_t kLookTimeout = 250000;
static const bigtime_t kSendTimeout = 100000;


WindowTracker::WindowTracker(team_id self)
	:
	fSelf(self)
{
	// What an earlier shell left: the windows it had changed are still as
	// it made them, and this is what they were like before.
	fState.Load();
}


bool
WindowTracker::PrepareWindow(BMessenger target, BRect frame, int32 look, bool filePanel)
{
	app_info app;
	if (!frame.IsValid() || !fWorkArea.IsValid()
		|| be_roster->GetRunningAppInfo(target.Team(), &app) != B_OK
		|| (!filePanel && !BMobileKit::SupportsMobileView(app.ref)))
		return false;

	int32 count = 0;
	int32* tokens = get_token_list(target.Team(), &count);
	bool found = false;
	for (int32 i = 0; tokens != NULL && i < count && !found; i++) {
		client_window_info* info = get_window_info(tokens[i]);
		if (info == NULL)
			continue;
		BMessenger messenger;
		BMessenger::Private(messenger).SetTo(info->team, info->client_port,
			info->client_token);
		found = target == messenger;
		if (found && fState.FindWindow(tokens[i], info->team) == NULL) {
			SavedWindow saved = {tokens[i], info->team, look, frame, info->is_mini, true};
			fState.windows.push_back(saved);
			fState.Save();
		}
		free(info);
	}
	free(tokens);
	return found;
}


bool
WindowTracker::Update()
{
	int32* tokens = NULL;
	int32 count = 0;
	if (BPrivate::get_window_order(current_workspace(), &tokens, &count)
			!= B_OK) {
		return false;
	}

	for (TrackedWindow& window : fWindows)
		window.seen = false;

	bool changed = false;
	std::vector<TrackedWindow> ordered;
	for (int32 i = 0; i < count; i++) {
		client_window_info* info = get_window_info(tokens[i]);
		if (info == NULL)
			continue;

		// Same test as the Deskbar's window menu, less our own windows and
		// those of programs that run in the background (the Deskbar's own).
		bool wanted = info->team != fSelf
			&& info->show_hide_level <= 0
			&& !fState.IsHidden(info->server_token)
			&& !_IsBackground(info->team)
			&& (info->feel == B_NORMAL_WINDOW_FEEL
				|| ((info->feel == B_FLOATING_ALL_WINDOW_FEEL
					|| info->feel == B_MODAL_APP_WINDOW_FEEL
					|| info->feel == B_MODAL_SUBSET_WINDOW_FEEL)
					&& _IsMobileDialogOrPlayer(*info)));
		if (!wanted) {
			free(info);
			continue;
		}

		BRect frame(info->window_left, info->window_top, info->window_right,
			info->window_bottom);
		TrackedWindow* known = _Find(info->server_token);
		TrackedWindow window;
		if (known != NULL) {
			window = *known;
			known->seen = true;
			if (window.title != info->name || window.minimized != info->is_mini)
				changed = true;
		} else {
			window.token = info->server_token;
			window.team = info->team;
			app_info app;
			if (be_roster->GetRunningAppInfo(window.team, &app) != B_OK) {
				free(info);
				continue;
			}
			window.mobile = info->feel != B_NORMAL_WINDOW_FEEL
				|| BMobileKit::SupportsMobileView(app.ref) || _IsMobileDialogOrPlayer(*info);
			window.fits = 0;
			window.capped = false;
			window.capWidth = window.capHeight = INFINITY;
			window.adopted = false;
			window.lookTries = 0;
			BMessenger::Private(window.messenger).SetTo(info->team,
				info->client_port, info->client_token);
			changed = true;
		}
		window.title = info->name;
		window.minimized = info->is_mini;
		window.resizable = (info->flags & B_NOT_RESIZABLE) == 0;
		window.frame = frame;
		window.seen = true;
		free(info);

		// Fitted twice and still short of the area: its application caps
		// its size (DeskCalc's at 400 pixels wide), so it's centred instead.
		// The size limits aren't in the window list, so this is how they
		// show.
		if (window.resizable && !window.capped && window.fits >= 2
			&& (frame.Width() < fWorkArea.Width() - 1
				|| frame.Height() < fWorkArea.Height() - 1)) {
			window.capped = true;
			if (frame.Width() < fWorkArea.Width() - 1)
				window.capWidth = frame.Width();
			if (frame.Height() < fWorkArea.Height() - 1)
				window.capHeight = frame.Height();
		}

		if (!window.adopted) {
			if (_Adopt(window)) {
				if (window.mobile) {
					BMessage mode(BMobileKit::B_MOBILE_VIEW_CHANGED);
					mode.AddBool("mobile", true);
					window.adopted = window.messenger.SendMessage(&mode,
						(BHandler*)NULL, kSendTimeout) == B_OK;
					if (window.adopted)
						_Fit(window, true);
				} else if (!window.minimized)
					do_window_action(window.token, B_MINIMIZE_WINDOW, BRect(), false);
			}
		} else if (!window.minimized && window.frame != _TargetFrame(window))
			_Fit(window, false);

		ordered.push_back(window);
	}
	free(tokens);

	if (ordered.size() != fWindows.size())
		changed = true;
	else {
		for (size_t i = 0; i < ordered.size(); i++) {
			if (ordered[i].token != fWindows[i].token)
				changed = true;
		}
	}

	fWindows = ordered;
	return changed;
}


void
WindowTracker::SetWorkArea(BRect area)
{
	if (area == fWorkArea)
		return;

	fWorkArea = area;
	for (TrackedWindow& window : fWindows) {
		if (!window.adopted)
			continue;
		window.fits = 0;
		window.capped = false;
		window.capWidth = window.capHeight = INFINITY;
		_Fit(window, true);
	}
}


const TrackedWindow*
WindowTracker::Front() const
{
	for (const TrackedWindow& window : fWindows) {
		if (!window.minimized)
			return &window;
	}
	return NULL;
}


void
WindowTracker::Activate(int32 token)
{
	TrackedWindow* window = _Find(token);
	if (window != NULL && window->mobile)
		do_window_action(token, B_BRING_TO_FRONT, BRect(), false);
}


void
WindowTracker::Close(int32 token)
{
	TrackedWindow* window = _Find(token);
	if (window != NULL)
		window->messenger.SendMessage(B_QUIT_REQUESTED);
}


void
WindowTracker::MinimizeAll()
{
	for (const TrackedWindow& window : fWindows) {
		if (!window.minimized)
			do_window_action(window.token, B_MINIMIZE_WINDOW, BRect(), false);
	}
}


void
WindowTracker::HideDesktop()
{
	_Prune();
	size_t hidden = fState.hidden.size();
	_HideWindowsOf(kDeskbarSignature, false);
	_HideWindowsOf(kTrackerSignature, true);
	if (fState.hidden.size() != hidden)
		fState.Save();
}


void
WindowTracker::RestoreDesktop()
{
	fState.Restore();
	fWindows.clear();
}


/*!	Takes note of what \a window is like before the shell changes it. False
	when the window can't say yet; it is asked again at the next poll.
*/
bool
WindowTracker::_Adopt(TrackedWindow& window)
{
	if (fState.FindWindow(window.token, window.team) != NULL) {
		// an earlier shell took it
		window.adopted = true;
		return true;
	}

	BMessage get(B_GET_PROPERTY);
	get.AddSpecifier("Look");
	BMessage reply;
	int32 look = -1;
	if (window.mobile && (window.messenger.SendMessage(&get, &reply, kLookTimeout, kLookTimeout)
			!= B_OK
		|| reply.FindInt32("result", &look) != B_OK)) {
		if (++window.lookTries < kMaxLookTries)
			return false;
		look = -1;
	}

	SavedWindow saved;
	saved.token = window.token;
	saved.team = window.team;
	saved.look = look;
	saved.frame = window.frame;
	saved.wasMinimized = window.minimized;
	saved.managed = window.mobile;
	fState.windows.push_back(saved);
	fState.Save();

	window.adopted = true;
	return true;
}


void
WindowTracker::_Fit(TrackedWindow& window, bool force)
{
	if (!window.mobile || !fWorkArea.IsValid() || (!force && window.fits >= kMaxFits))
		return;

	window.fits++;
	// No border around a window that fills the area; a line around one
	// that doesn't, to tell it from what is behind it.
	bool fills = window.resizable && !window.capped;
	BMessage look(B_SET_PROPERTY);
	look.AddSpecifier("Look");
	look.AddInt32("data", fills ? B_NO_BORDER_WINDOW_LOOK
		: B_BORDERED_WINDOW_LOOK);
	window.messenger.SendMessage(&look, (BHandler*)NULL, kSendTimeout);

	BMessage frame(B_SET_PROPERTY);
	frame.AddSpecifier("Frame");
	frame.AddRect("data", _TargetFrame(window));
	window.messenger.SendMessage(&frame, (BHandler*)NULL, kSendTimeout);
}


BRect
WindowTracker::_TargetFrame(const TrackedWindow& window) const
{
	if (window.resizable && !window.capped)
		return fWorkArea;

	// Fixed-size windows (small tools, some preferences) keep their size
	// and capped ones are as large as their application lets them be in the
	// area; both are centred, or pinned to the top left when they don't fit.
	BRect frame = window.frame;
	if (window.resizable) {
		frame.right = frame.left + std::min(window.capWidth,
			fWorkArea.Width());
		frame.bottom = frame.top + std::min(window.capHeight,
			fWorkArea.Height());
	}
	float left = fWorkArea.left + std::max(0.0f,
		(fWorkArea.Width() - frame.Width()) / 2);
	float top = fWorkArea.top + std::max(0.0f,
		(fWorkArea.Height() - frame.Height()) / 2);
	frame.OffsetTo(floorf(left), floorf(top));
	return frame;
}


TrackedWindow*
WindowTracker::_Find(int32 token)
{
	for (TrackedWindow& window : fWindows) {
		if (window.token == token)
			return &window;
	}
	return NULL;
}


/*!	Forgets what windows that are gone were like: their tokens come back
	as other windows'. */
void
WindowTracker::_Prune()
{
	bool pruned = false;
	for (size_t i = 0; i < fState.windows.size();) {
		const SavedWindow& saved = fState.windows[i];
		client_window_info* info = get_window_info(saved.token);
		bool there = info != NULL && info->team == saved.team;
		free(info);
		if (there)
			i++;
		else {
			fState.windows.erase(fState.windows.begin() + i);
			pruned = true;
		}
	}
	if (pruned)
		fState.Save();
}


bool
WindowTracker::_IsMobileDialogOrPlayer(const client_window_info& info)
{
	// A prepared window has already opted in. This also avoids asking a
	// modal panel on every poll while its owner is waiting for a reply.
	SavedWindow* saved = fState.FindWindow(info.server_token, info.team);
	if (saved != NULL)
		return saved->managed;
	auto known = fAuxiliary.find(info.server_token);
	if (known != fAuxiliary.end() && known->second == info.team)
		return false;

	BMessenger window;
	BMessenger::Private(window).SetTo(info.team, info.client_port, info.client_token);
	BMessage query(BMobileKit::B_MOBILE_WINDOW_QUERY), reply;
	if (window.SendMessage(&query, &reply, kSendTimeout, kSendTimeout) != B_OK)
		return false;
	if (reply.GetBool("file panel", false))
		return true;
	if (info.feel == B_FLOATING_ALL_WINDOW_FEEL && reply.GetBool("primary", false)) {
		app_info app;
		return be_roster->GetRunningAppInfo(info.team, &app) == B_OK
			&& BMobileKit::SupportsMobileView(app.ref);
	}
	if (fAuxiliary.size() > 512)
		fAuxiliary.clear();
	fAuxiliary[info.server_token] = info.team;
	return false;
}


/*!	Whether \a team is a program without a place among the applications:
	one that runs in the background, or the Deskbar. */
bool
WindowTracker::_IsBackground(team_id team)
{
	auto found = fBackground.find(team);
	if (found != fBackground.end())
		return found->second;

	app_info info;
	if (be_roster->GetRunningAppInfo(team, &info) != B_OK)
		return false;
			// not registered yet: asked again next time
	bool background = (info.flags & B_BACKGROUND_APP) != 0
		|| strcasecmp(info.signature, kDeskbarSignature) == 0;
	// Team IDs come back in time; the few of a session don't.
	if (fBackground.size() > 512)
		fBackground.clear();
	fBackground[team] = background;
	return background;
}


/*!	Hides the visible windows of the program with \a signature (only its
	desktop window with \a desktopOnly) and takes note of them.
*/
void
WindowTracker::_HideWindowsOf(const char* signature, bool desktopOnly)
{
	team_id team = be_roster->TeamFor(signature);
	if (team < 0)
		return;

	int32 count = 0;
	int32* tokens = get_token_list(team, &count);
	if (tokens == NULL)
		return;

	for (int32 i = 0; i < count; i++) {
		client_window_info* info = get_window_info(tokens[i]);
		if (info == NULL)
			continue;
		bool hide = info->team == team && info->show_hide_level <= 0
			&& (!desktopOnly || info->feel == (uint32)kDesktopWindowFeel)
			// menus and tool tips come and go by themselves
			&& (info->feel == B_NORMAL_WINDOW_FEEL
				|| info->feel == B_FLOATING_ALL_WINDOW_FEEL
				|| info->feel == (uint32)kDesktopWindowFeel)
			&& !fState.IsHidden(info->server_token);
		if (hide) {
			BMessenger messenger;
			BMessenger::Private(messenger).SetTo(info->team, info->client_port,
				info->client_token);
			BMessage message(B_SET_PROPERTY);
			message.AddSpecifier("Hidden");
			message.AddBool("data", true);
			if (messenger.SendMessage(&message, (BHandler*)NULL, kSendTimeout)
					== B_OK) {
				HiddenWindow hidden = { info->server_token, info->team };
				fState.hidden.push_back(hidden);
			}
		}
		free(info);
	}
	free(tokens);
}
