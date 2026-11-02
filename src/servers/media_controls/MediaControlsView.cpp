/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "MediaControlsView.h"

#include <Catalog.h>
#include <Deskbar.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Roster.h>
#include <String.h>
#include <Window.h>

#include <MediaControls.h>

#include "MediaIcons.h"
#include "NowPlayingWindow.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "MediaControlsView"


enum {
	kMsgAction			= 'actn',
	kMsgActivatePlayer	= 'actp',
	kMsgShowApplication	= 'shap'
};


extern "C" _EXPORT BView* instantiate_deskbar_item(float maxWidth,
	float maxHeight);


BView*
instantiate_deskbar_item(float maxWidth, float maxHeight)
{
	return new MediaControlsView(BRect(0, 0, maxHeight - 1, maxHeight - 1));
}


static void
send_action(int32 action, int64 session = -1)
{
	BMessage message(kMsgMediaControlsSendAction);
	message.AddInt32("action", action);
	if (session >= 0)
		message.AddInt64("session", session);
	BMessenger(kMediaControlsServerSignature).SendMessage(&message,
		(BHandler*)NULL, 100000);
}


//	#pragma mark -


MediaControlsView::MediaControlsView(BRect frame)
	:
	BView(frame, kMediaControlsReplicantName, B_FOLLOW_NONE, B_WILL_DRAW)
{
}


MediaControlsView::MediaControlsView(BMessage* archive)
	:
	BView(archive)
{
}


MediaControlsView::~MediaControlsView()
{
}


MediaControlsView*
MediaControlsView::Instantiate(BMessage* archive)
{
	if (!validate_instantiation(archive, "MediaControlsView"))
		return NULL;

	return new MediaControlsView(archive);
}


status_t
MediaControlsView::Archive(BMessage* archive, bool deep) const
{
	status_t status = BView::Archive(archive, deep);
	if (status == B_OK)
		status = archive->AddString("add_on", kMediaControlsServerSignature);
	return status;
}


void
MediaControlsView::AttachedToWindow()
{
	BView::AttachedToWindow();
	AdoptParentColors();
	if (ViewUIColor() == B_NO_COLOR)
		SetLowColor(ViewColor());
	else
		SetLowUIColor(ViewUIColor());

	if (!be_roster->IsRunning(kMediaControlsServerSignature)) {
		// left over from before a reboot: nothing plays
		BDeskbar().RemoveItem(kMediaControlsReplicantName);
		return;
	}

	BMessage watch(kMsgMediaControlsStartWatching);
	watch.AddMessenger("target", BMessenger(this));
	BMessenger(kMediaControlsServerSignature).SendMessage(&watch,
		(BHandler*)NULL, 100000);
}


void
MediaControlsView::DetachedFromWindow()
{
	BMessage watch(kMsgMediaControlsStopWatching);
	watch.AddMessenger("target", BMessenger(this));
	if (be_roster->IsRunning(kMediaControlsServerSignature)) {
		BMessenger(kMediaControlsServerSignature).SendMessage(&watch,
			(BHandler*)NULL, 100000);
	}

	if (fPopup.IsValid())
		fPopup.SendMessage(B_QUIT_REQUESTED);

	BView::DetachedFromWindow();
}


void
MediaControlsView::Draw(BRect updateRect)
{
	BMessage player;
	bool playing = fState.FindMessage("player", &player) == B_OK
		&& player.GetInt32("state", B_MEDIA_SESSION_STOPPED)
			== B_MEDIA_SESSION_PLAYING;

	BRect bounds = Bounds();
	float inset = roundf(bounds.Height() / 16);
	bounds.InsetBy(inset, inset);
	DrawNoteIcon(this, bounds, !playing);
}


void
MediaControlsView::MouseDown(BPoint where)
{
	BMessage* current = Window()->CurrentMessage();
	int32 buttons = B_PRIMARY_MOUSE_BUTTON;
	if (current != NULL)
		current->FindInt32("buttons", &buttons);

	if ((buttons & B_SECONDARY_MOUSE_BUTTON) != 0)
		_ShowMenu(ConvertToScreen(where));
	else if ((buttons & B_TERTIARY_MOUSE_BUTTON) != 0)
		send_action(B_MEDIA_ACTION_TOGGLE);
	else
		_TogglePopup();
}


void
MediaControlsView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgMediaControlsState:
			fState = *message;
			_UpdateToolTip();
			Invalidate();
			break;

		case kMsgAction:
			send_action(message->GetInt32("action", -1),
				message->GetInt64("session", -1));
			break;

		case kMsgActivatePlayer:
		{
			BMessage activate(kMsgMediaControlsActivate);
			activate.AddInt64("session", message->GetInt64("session", -1));
			BMessenger(kMediaControlsServerSignature).SendMessage(&activate,
				(BHandler*)NULL, 100000);
			break;
		}

		case kMsgShowApplication:
			be_roster->ActivateApp(message->GetInt32("team", -1));
			break;

		default:
			BView::MessageReceived(message);
			break;
	}
}


void
MediaControlsView::_UpdateToolTip()
{
	BMessage player;
	if (fState.FindMessage("player", &player) != B_OK) {
		SetToolTip((const char*)NULL);
		return;
	}

	BString text = player.GetString("title", "");
	if (text.IsEmpty())
		text = player.GetString("name", "");
	const char* artist = player.GetString("artist", NULL);
	if (artist != NULL && artist[0] != '\0')
		text << "\n" << artist;

	switch (player.GetInt32("state", B_MEDIA_SESSION_STOPPED)) {
		case B_MEDIA_SESSION_PLAYING:
			text << "\n" << B_TRANSLATE("Playing in %app%");
			break;
		case B_MEDIA_SESSION_PAUSED:
			text << "\n" << B_TRANSLATE("Paused in %app%");
			break;
		default:
			text << "\n" << B_TRANSLATE("Stopped in %app%");
			break;
	}
	text.ReplaceFirst("%app%", player.GetString("name", "?"));
	SetToolTip(text.String());
}


void
MediaControlsView::_ShowMenu(BPoint where)
{
	BPopUpMenu* menu = new BPopUpMenu("", false, false);
	menu->SetFont(be_plain_font);

	BMessage player;
	bool hasPlayer = fState.FindMessage("player", &player) == B_OK;
	int64 session = player.GetInt64("session", -1);
	uint32 actions = player.GetUInt32("actions", 0);
	bool playing = player.GetInt32("state", B_MEDIA_SESSION_STOPPED)
		== B_MEDIA_SESSION_PLAYING;

	struct {
		const char*	label;
		int32		action;
		bool		enabled;
	} items[] = {
		{ playing ? B_TRANSLATE("Pause") : B_TRANSLATE("Play"),
			B_MEDIA_ACTION_TOGGLE, (actions & (B_MEDIA_ACTION_FLAG(
				B_MEDIA_ACTION_TOGGLE) | B_MEDIA_ACTION_FLAG(
				playing ? B_MEDIA_ACTION_PAUSE : B_MEDIA_ACTION_PLAY))) != 0 },
		{ B_TRANSLATE("Previous"), B_MEDIA_ACTION_PREVIOUS,
			(actions & B_MEDIA_ACTION_FLAG(B_MEDIA_ACTION_PREVIOUS)) != 0 },
		{ B_TRANSLATE("Next"), B_MEDIA_ACTION_NEXT,
			(actions & B_MEDIA_ACTION_FLAG(B_MEDIA_ACTION_NEXT)) != 0 },
		{ B_TRANSLATE("Stop"), B_MEDIA_ACTION_STOP,
			(actions & B_MEDIA_ACTION_FLAG(B_MEDIA_ACTION_STOP)) != 0 },
	};
	for (size_t i = 0; i < B_COUNT_OF(items); i++) {
		BMessage* message = new BMessage(kMsgAction);
		message->AddInt32("action", items[i].action);
		message->AddInt64("session", session);
		BMenuItem* item = new BMenuItem(items[i].label, message);
		item->SetEnabled(hasPlayer && items[i].enabled);
		menu->AddItem(item);
	}

	// all players: the marked one gets the keys
	BMessage info;
	if (fState.FindMessage("player", 1, &info) == B_OK) {
		menu->AddSeparatorItem();
		for (int32 i = 0; fState.FindMessage("player", i, &info) == B_OK;
				i++) {
			BString label = info.GetString("title", "");
			if (label.IsEmpty())
				label = info.GetString("name", "");
			else
				label << " (" << info.GetString("name", "") << ")";
			BMessage* message = new BMessage(kMsgActivatePlayer);
			message->AddInt64("session", info.GetInt64("session", -1));
			BMenuItem* item = new BMenuItem(label.String(), message);
			item->SetMarked(i == 0);
			menu->AddItem(item);
		}
	}

	if (hasPlayer) {
		menu->AddSeparatorItem();
		BString label(B_TRANSLATE("Show %app%"));
		label.ReplaceFirst("%app%", player.GetString("name", "?"));
		BMessage* message = new BMessage(kMsgShowApplication);
		message->AddInt32("team", player.GetInt32("team", -1));
		menu->AddItem(new BMenuItem(label.String(), message));
	}

	menu->SetTargetForItems(this);
	menu->SetAsyncAutoDestruct(true);
	BRect clickToOpen(where - BPoint(4, 4), where + BPoint(4, 4));
	menu->Go(where, true, true, clickToOpen, true);
}


void
MediaControlsView::_TogglePopup()
{
	if (fPopup.IsValid()) {
		fPopup.SendMessage(B_QUIT_REQUESTED);
		fPopup = BMessenger();
		return;
	}

	// A click on the icon while the popup is up first closes it (it lost
	// the focus): that click must not open it again.
	if (system_time() - NowPlayingWindow::LastClosed() < 300000)
		return;

	NowPlayingWindow* window = new NowPlayingWindow(
		ConvertToScreen(Bounds()));
	fPopup = BMessenger(window);
	window->Show();
}
