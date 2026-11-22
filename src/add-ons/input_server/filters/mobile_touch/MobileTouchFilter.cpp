/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */

/*	The mobile shell's long press: an input_server filter add-on that turns
	a finger held still on an application's window into a click of the
	secondary button there, as on other touch systems: a finger has no
	second button.

	The press is held back until the finger moves or lifts, which then sends
	it as it was; if neither happens in time, a secondary click goes instead
	and the rest of the touch is dropped. A tap that was held back to its
	end goes out with its release a moment after the press: menus, among
	others, poll for the buttons, and miss a press that is already over.

	It only ever does this
	- while the mobile shell runs and has asked for it (the shell writes the
	  rectangle its applications' windows are in to the filter's port, and
	  the filter forgets it when that team is gone),
	- to presses inside that rectangle (the shell's own panels and keyboard
	  react to a press and have no use for a secondary click),
	- to events of absolute pointing devices (touch screens and pens; the
	  tablet device add-on marks them B_TABLET_POINTING_DEVICE). Mice and
	  touchpads pass through untouched, as does everything while no shell
	  runs: then the filter looks at the subtype of a press and nothing else.
*/


#include "Protocol.h"

#include <Autolock.h>
#include <Input.h>
#include <InputServerDevice.h>
#include <InputServerFilter.h>
#include <InterfaceDefs.h>
#include <List.h>
#include <Locker.h>
#include <Message.h>
#include <OS.h>
#include <Rect.h>
#include <View.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>


// Held this long without moving, a finger clicks the secondary button.
static const bigtime_t kLongPress = 500000;
// How long a held back tap stays pressed: longer than BMenu's 50 ms polls.
static const bigtime_t kTapLength = 100000;
// A finger that moves less than this has not moved.
static const float kTapSlop = 20;
// Marks the events this sends late, which come back through the filters.
static const char* kLateField = "mobile:touch";


class MobileTouchFilter : public BInputServerFilter {
public:
								MobileTouchFilter();
	virtual						~MobileTouchFilter();

	virtual	status_t			InitCheck();
	virtual	filter_result		Filter(BMessage* message, BList* outList);

private:
			bool				_Wanted(BPoint where);
			bool				_StartWatcher();
	static	status_t			_Watcher(void* data);
			void				_Tick();
			void				_Enqueue(BMessage* message);

			port_id				fPort;
			// what the shell asked for, and who it was
			team_id				fShell;
			BRect				fHoldArea;

			// Never started: it is only here to enqueue events from the
			// watcher thread.
			BInputServerDevice*	fInjector;

			BLocker				fLock;
			sem_id				fWake;
			thread_id			fWatcher;

			// the press being held back
			BMessage*			fHeld;
			BPoint				fHeldWhere;
			bigtime_t			fHeldSince;
			bool				fLongPressed;

			// the release of a held back tap, and when it goes
			BMessage*			fRelease;
			bigtime_t			fReleaseAt;
};


MobileTouchFilter::MobileTouchFilter()
	:
	fShell(-1),
	fInjector(NULL),
	fLock("mobile touch"),
	fWake(-1),
	fWatcher(-1),
	fHeld(NULL),
	fHeldSince(0),
	fLongPressed(false),
	fRelease(NULL),
	fReleaseAt(0)
{
	fPort = create_port(8, MOBILE_TOUCH_PORT);
}


MobileTouchFilter::~MobileTouchFilter()
{
	delete_port(fPort);
	if (fWake >= 0)
		delete_sem(fWake);
	if (fWatcher >= 0) {
		status_t result;
		wait_for_thread(fWatcher, &result);
	}
	delete fInjector;
	delete fHeld;
	delete fRelease;
}


status_t
MobileTouchFilter::InitCheck()
{
	return fPort >= 0 ? B_OK : fPort;
}


filter_result
MobileTouchFilter::Filter(BMessage* message, BList* outList)
{
	if (message->what != B_MOUSE_DOWN && message->what != B_MOUSE_MOVED
		&& message->what != B_MOUSE_UP) {
		return B_DISPATCH_MESSAGE;
	}
	// Nothing held back: only a press can change that.
	if (message->what != B_MOUSE_DOWN && fHeld == NULL && !fLongPressed)
		return B_DISPATCH_MESSAGE;

	int32 subtype;
	if (message->FindInt32("be:device_subtype", &subtype) != B_OK
		|| subtype != B_TABLET_POINTING_DEVICE
		|| message->HasBool(kLateField)) {
		return B_DISPATCH_MESSAGE;
	}

	BPoint where;
	if (message->FindPoint("where", &where) != B_OK)
		return B_DISPATCH_MESSAGE;

	BAutolock _(fLock);

	switch (message->what) {
		case B_MOUSE_DOWN:
		{
			if (!_Wanted(where) || fHeld != NULL || fLongPressed
				|| message->GetInt32("buttons", 0) != B_PRIMARY_MOUSE_BUTTON
				|| !_StartWatcher()) {
				return B_DISPATCH_MESSAGE;
			}

			// a new tap can't wait for the last one's release
			if (fRelease != NULL) {
				outList->AddItem(fRelease);
				fRelease = NULL;
			}

			fHeld = new BMessage(*message);
			fHeldWhere = where;
			fHeldSince = system_time();
			release_sem(fWake);
			// the list replaces the event, and it has been copied
			return outList->IsEmpty() ? B_SKIP_MESSAGE : B_DISPATCH_MESSAGE;
		}

		case B_MOUSE_MOVED:
			if (fLongPressed)
				return B_SKIP_MESSAGE;
			if (fHeld == NULL)
				return B_DISPATCH_MESSAGE;
			if (fabsf(where.x - fHeldWhere.x) < kTapSlop
				&& fabsf(where.y - fHeldWhere.y) < kTapSlop) {
				return B_SKIP_MESSAGE;
			}
			// the finger moves: the press goes out as it was, then the move
			outList->AddItem(fHeld);
			fHeld = NULL;
			outList->AddItem(new BMessage(*message));
			return B_DISPATCH_MESSAGE;

		case B_MOUSE_UP:
			if (fLongPressed) {
				fLongPressed = false;
				return B_SKIP_MESSAGE;
			}
			if (fHeld == NULL)
				return B_DISPATCH_MESSAGE;
			// a tap: the press now, its release a moment later
			outList->AddItem(fHeld);
			fHeld = NULL;
			fRelease = new BMessage(*message);
			fRelease->AddBool(kLateField, true);
			fReleaseAt = system_time() + kTapLength;
			release_sem(fWake);
			return B_DISPATCH_MESSAGE;
	}

	return B_DISPATCH_MESSAGE;
}


/*!	Whether a press at \a where is one to hold back: the newest word from
	a shell that still runs says so. Called with the lock held.
*/
bool
MobileTouchFilter::_Wanted(BPoint where)
{
	// what the shell wrote since the last press
	while (true) {
		port_message_info info;
		if (get_port_message_info_etc(fPort, &info, B_RELATIVE_TIMEOUT, 0)
				!= B_OK) {
			break;
		}
		mobile_hold_area area;
		int32 code;
		ssize_t size = read_port_etc(fPort, &code, &area, sizeof(area),
			B_RELATIVE_TIMEOUT, 0);
		if (size < 0)
			break;
		if (code != kMobileHoldArea || size != (ssize_t)sizeof(area))
			continue;
		if (area.enabled != 0) {
			fShell = info.sender_team;
			fHoldArea.Set(area.left, area.top, area.right, area.bottom);
		} else if (info.sender_team == fShell)
			fShell = -1;
	}

	if (fShell < 0)
		return false;

	team_info info;
	if (get_team_info(fShell, &info) != B_OK) {
		// the shell is gone without a word
		fShell = -1;
		return false;
	}
	return fHoldArea.IsValid() && fHoldArea.Contains(where);
}


/*!	The timer for what is held back; started when a shell first asks. */
bool
MobileTouchFilter::_StartWatcher()
{
	if (fWatcher >= 0)
		return true;

	if (fInjector == NULL)
		fInjector = new BInputServerDevice();
	if (fWake < 0)
		fWake = create_sem(0, "mobile touch wake");
	if (fWake < 0)
		return false;
	fWatcher = spawn_thread(_Watcher, "mobile touch timer",
		B_URGENT_DISPLAY_PRIORITY, this);
	if (fWatcher < 0)
		return false;
	resume_thread(fWatcher);
	return true;
}


/*!	Sleeps until the held back tap's release or the held press's long press
	is due, or until the filter changes them, and sends what is due.
*/
status_t
MobileTouchFilter::_Watcher(void* data)
{
	MobileTouchFilter* filter = (MobileTouchFilter*)data;
	while (true) {
		filter->fLock.Lock();
		bigtime_t deadline = B_INFINITE_TIMEOUT;
		if (filter->fRelease != NULL)
			deadline = filter->fReleaseAt;
		else if (filter->fHeld != NULL)
			deadline = filter->fHeldSince + kLongPress;
		filter->fLock.Unlock();

		if (acquire_sem_etc(filter->fWake, 1, B_ABSOLUTE_TIMEOUT, deadline)
				== B_BAD_SEM_ID) {
			return B_OK;
		}

		filter->fLock.Lock();
		filter->_Tick();
		filter->fLock.Unlock();
	}
}


/*!	Sends whatever is due. Called with the lock held. */
void
MobileTouchFilter::_Tick()
{
	bigtime_t now = system_time();

	if (fRelease != NULL && now >= fReleaseAt) {
		_Enqueue(fRelease);
		fRelease = NULL;
	}

	if (fHeld != NULL && now >= fHeldSince + kLongPress) {
		// the secondary click
		BMessage* down = fHeld;
		fHeld = NULL;
		fLongPressed = true;
		down->SetInt32("buttons", B_SECONDARY_MOUSE_BUTTON);
		down->SetInt32("clicks", 1);
		down->AddBool(kLateField, true);
		_Enqueue(down);

		BMessage* up = new BMessage(B_MOUSE_UP);
		up->AddInt32("buttons", 0);
		up->AddPoint("where", fHeldWhere);
		up->AddInt32("be:device_subtype", B_TABLET_POINTING_DEVICE);
		up->AddBool(kLateField, true);
		_Enqueue(up);
	}
}


void
MobileTouchFilter::_Enqueue(BMessage* message)
{
	message->SetInt64("when", system_time());
	if (fInjector->EnqueueMessage(message) != B_OK)
		delete message;
}


extern "C" BInputServerFilter*
instantiate_input_filter()
{
	return new MobileTouchFilter();
}
