/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "QuickLook.h"

#include <Message.h>
#include <Roster.h>

#include <QuickLookDefs.h>


namespace BPrivate {


static status_t
send_to_quick_look(BMessage* message, bool launch)
{
	BMessenger server(B_QUICK_LOOK_SIGNATURE);
	if (server.IsValid())
		return server.SendMessage(message);
	if (!launch)
		return B_OK;
	// Not running: the roster starts it and hands it the message.
	status_t status = be_roster->Launch(B_QUICK_LOOK_SIGNATURE, message);
	if (status == B_ALREADY_RUNNING) {
		BMessenger again(B_QUICK_LOOK_SIGNATURE);
		return again.SendMessage(message);
	}
	return status;
}


status_t
QuickLookShow(uint32 what, const BMessage* refs, BRect ownerFrame,
	const BMessenger& owner)
{
	BMessage message(*refs);
	message.what = what;
	message.RemoveName("index");
	message.RemoveName("frame");
	message.RemoveName("owner");
	message.AddInt32("index", 0);
	message.AddRect("frame", ownerFrame);
	message.AddMessenger("owner", owner);
	return send_to_quick_look(&message, true);
}


status_t
QuickLookHide()
{
	BMessage message(kMsgQuickLookHide);
	return send_to_quick_look(&message, false);
}


}	// namespace BPrivate
