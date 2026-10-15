/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TRACKER_QUICK_LOOK_H
#define _TRACKER_QUICK_LOOK_H


#include <Messenger.h>
#include <Rect.h>
#include <SupportDefs.h>


class BMessage;


namespace BPrivate {

/*!	Tracker's side of Quick Look: asks the quick_look server to show a
	panel for some entries, launching it when it is not running. */

//! Shows (what = kMsgQuickLookShow) or toggles (kMsgQuickLookToggle) the
//! panel for the "refs" in the message; the owner hears when it closes.
status_t QuickLookShow(uint32 what, const BMessage* refs, BRect ownerFrame,
	BRect iconRect, const BMessenger& owner);
//! Hides the panel.
status_t QuickLookHide();

}	// namespace BPrivate


#endif	// _TRACKER_QUICK_LOOK_H
