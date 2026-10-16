/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "WindowIcons.h"

#include <AppDefs.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <Looper.h>
#include <Message.h>

#include <MessengerPrivate.h>
#include <WindowInfo.h>

#include <new>
#include <stdlib.h>
#include <vector>


static const uint32 kMsgWatchWindow = 'Wiww';
static const char* const kWindowIconProperty = "Icon";
// The lists draw icons at menu icon size; larger ones are refused.
static const int32 kMaximumIconSize = 256;


// The icon of an archived BBitmap, in B_RGBA32. Only the frame, the color
// space and the bits are taken: an application's archive must not make the
// Deskbar allocate a huge bitmap, or one that accepts views.
static BBitmap*
icon_from_archive(const BMessage& archive)
{
	BRect frame;
	int32 colorSpace;
	int32 rowBytes;
	const void* data;
	ssize_t size;
	if (archive.FindRect("_frame", &frame) != B_OK || !frame.IsValid()
		|| frame.IntegerWidth() + 1 > kMaximumIconSize
		|| frame.IntegerHeight() + 1 > kMaximumIconSize
		|| archive.FindInt32("_cspace", &colorSpace) != B_OK
		|| (colorSpace != B_RGBA32 && colorSpace != B_RGB32)
		|| archive.FindInt32("_rowbytes", &rowBytes) != B_OK
		|| archive.FindData("_data", B_RAW_TYPE, &data, &size) != B_OK) {
		return NULL;
	}

	frame.OffsetTo(B_ORIGIN);
	BBitmap* icon = new(std::nothrow) BBitmap(frame, B_RGBA32);
	if (icon == NULL || icon->InitCheck() != B_OK
		|| icon->ImportBits(data, size, rowBytes, 0, (color_space)colorSpace)
			!= B_OK) {
		delete icon;
		return NULL;
	}
	return icon;
}


// Asks one window for its icon and then watches it: replies and notices come
// to this handler, which knows the window.
class WindowIcons::Watcher : public BHandler {
public:
	Watcher(WindowIcons* owner, int32 serverToken, const BMessenger& window)
		:
		BHandler("window icon watcher"),
		fOwner(owner),
		fServerToken(serverToken),
		fWindow(window),
		fWatching(false)
	{
	}

	void Start()
	{
		BMessage request(B_GET_PROPERTY);
		request.AddSpecifier(kWindowIconProperty);
		// A window that cannot take the message now is not asked again.
		fWindow.SendMessage(&request, this, 0);
	}

	void Stop()
	{
		if (fWatching)
			StopWatching(fWindow, kWindowIconChanged);
		fWatching = false;
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case B_REPLY:
			{
				// A window that does not know the property answers
				// B_BAD_SCRIPT_SYNTAX.
				int32 error;
				if (message->FindInt32("error", &error) != B_OK
					|| error != B_OK) {
					break;
				}
				fOwner->_SetIcon(fServerToken, *message);
				if (!fWatching
					&& StartWatching(fWindow, kWindowIconChanged) == B_OK) {
					fWatching = true;
				}
				break;
			}

			case B_OBSERVER_NOTICE_CHANGE:
			{
				int32 what;
				if (message->FindInt32(B_OBSERVE_WHAT_CHANGE, &what) == B_OK
					&& (uint32)what == kWindowIconChanged) {
					fOwner->_SetIcon(fServerToken, *message);
				}
				break;
			}

			default:
				BHandler::MessageReceived(message);
				break;
		}
	}

private:
	WindowIcons*	fOwner;
	int32			fServerToken;
	BMessenger		fWindow;
	bool			fWatching;
};


WindowIcons::WindowIcons(BMessenger barView)
	:
	BHandler("window icons"),
	fLock("window icons"),
	fBarView(barView)
{
}


WindowIcons::~WindowIcons()
{
	for (auto& entry : fEntries) {
		Watcher* watcher = entry.second.watcher;
		if (watcher == NULL)
			continue;
		if (BLooper* looper = watcher->Looper())
			looper->RemoveHandler(watcher);
		delete watcher;
	}
}


std::shared_ptr<BBitmap>
WindowIcons::IconFor(int32 serverToken)
{
	BAutolock _(fLock);
	auto found = fEntries.find(serverToken);
	if (found != fEntries.end())
		return found->second.icon;

	// Asked from the application thread: watchers are its handlers.
	fEntries[serverToken] = Entry();
	BMessage watch(kMsgWatchWindow);
	watch.AddInt32("token", serverToken);
	BMessenger(this).SendMessage(&watch);
	return NULL;
}


void
WindowIcons::RemoveTeam(team_id team)
{
	std::vector<int32> tokens;
	{
		BAutolock _(fLock);
		for (const auto& entry : fEntries) {
			if (entry.second.team == team)
				tokens.push_back(entry.first);
		}
	}
	for (int32 token : tokens)
		_Remove(token);
}


void
WindowIcons::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgWatchWindow:
		{
			int32 token;
			if (message->FindInt32("token", &token) == B_OK)
				_Watch(token);
			break;
		}

		default:
			BHandler::MessageReceived(message);
			break;
	}
}


void
WindowIcons::_Watch(int32 serverToken)
{
	_RemoveClosedWindows();

	client_window_info* info = get_window_info(serverToken);
	if (info == NULL) {
		_Remove(serverToken);
		return;
	}
	BMessenger window;
	BMessenger::Private(window).SetTo(info->team, info->client_port,
		info->client_token);
	team_id team = info->team;
	free(info);

	Watcher* watcher = new(std::nothrow) Watcher(this, serverToken, window);
	if (watcher == NULL)
		return;
	Looper()->AddHandler(watcher);
	{
		BAutolock _(fLock);
		auto found = fEntries.find(serverToken);
		if (found == fEntries.end() || found->second.watcher != NULL) {
			Looper()->RemoveHandler(watcher);
			delete watcher;
			return;
		}
		found->second.team = team;
		found->second.watcher = watcher;
	}
	watcher->Start();
}


void
WindowIcons::_RemoveClosedWindows()
{
	std::vector<int32> tokens;
	{
		BAutolock _(fLock);
		for (const auto& entry : fEntries) {
			if (entry.second.watcher != NULL)
				tokens.push_back(entry.first);
		}
	}
	for (int32 token : tokens) {
		client_window_info* info = get_window_info(token);
		if (info == NULL)
			_Remove(token);
		free(info);
	}
}


void
WindowIcons::_Remove(int32 serverToken)
{
	Watcher* watcher = NULL;
	bool hadIcon = false;
	{
		BAutolock _(fLock);
		auto found = fEntries.find(serverToken);
		if (found == fEntries.end())
			return;
		watcher = found->second.watcher;
		hadIcon = found->second.icon != NULL;
		fEntries.erase(found);
	}
	if (watcher != NULL) {
		watcher->Stop();
		Looper()->RemoveHandler(watcher);
		delete watcher;
	}
	if (hadIcon)
		fBarView.SendMessage(kWindowIconsChanged);
}


void
WindowIcons::_SetIcon(int32 serverToken, const BMessage& message)
{
	std::shared_ptr<BBitmap> icon;
	BMessage archive;
	if (message.FindMessage("result", &archive) == B_OK)
		icon.reset(icon_from_archive(archive));

	{
		BAutolock _(fLock);
		auto found = fEntries.find(serverToken);
		if (found == fEntries.end()
			|| (found->second.icon == NULL && icon == NULL)) {
			return;
		}
		found->second.icon = icon;
	}
	fBarView.SendMessage(kWindowIconsChanged);
}
