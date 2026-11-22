/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */

/*	A touch screen used as one is on a phone: an input_server filter add-on
	for the mobile shell, and for the desktop while the machine is folded
	into a tablet. A finger has no buttons and no wheel, and a mouse's way
	of pressing, dragging and holding is not what it means:

	- A tap is a click. The press is held back until the finger lifts, and
	  the release goes a moment after it: menus, among others, poll for the
	  buttons, and miss a press that is already over.
	- A finger that moves scrolls whatever is under it, following the
	  finger, and glides on when it lifts while still moving. The scrolling
	  goes out as wheel events, with the distance in pixels
	  ("be:wheel_pixel_delta_*", which BView and BScrollBar take), in
	  fractions of a wheel step with the phase of the gesture (as from a
	  touchpad: Summit scrolls by those) and in whole steps for everything
	  else. A finger that comes down on a gliding page stops it, and that
	  touch clicks nothing.
	- A finger held still for half a second selects: the word under it gets
	  a double click (one press, counted as the second: a first click would
	  follow a link at once) that stays down, so dragging on selects more,
	  word by word, as with a mouse. The application that asked for this
	  (the shell) hears of the selection and offers Cut, Copy and Paste.
	  Its release is a click all the same: lifted where it was held on a
	  link, the finger follows the link (WebKit clicks on any release of
	  the primary button over what was pressed). Where it asks for a secondary click instead (over Tracker's
	  windows, and on the desktop), that is what a long press does, and the
	  rest of that touch is dropped.

	It only ever does this
	- while the application that asked for it runs (it writes the rectangle
	  its applications' windows are in, and what to do there, to the
	  filter's port; the filter forgets it when that team is gone),
	- to presses inside that rectangle (the shell's own panels and keyboard
	  are made for fingers already),
	- to events of absolute pointing devices (touch screens and pens; the
	  tablet device add-on marks them B_TABLET_POINTING_DEVICE). Mice and
	  touchpads pass through untouched, as does everything while nobody
	  asked: then the filter looks at the subtype of a press and nothing
	  else.
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
#include <Messenger.h>
#include <OS.h>
#include <Rect.h>
#include <View.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>


// Held this long without moving, a finger selects (or clicks the secondary
// button).
static const bigtime_t kLongPress = 500000;
// How long a held back tap stays pressed: longer than BMenu's 50 ms polls.
static const bigtime_t kTapLength = 100000;
// A finger that moves less than this has not moved.
static const float kTapSlop = 20;
// A wheel step in pixels, what Summit (like Chrome) scrolls for a notch.
static const float kPixelsPerStep = 100;
// Moving at least twice as far one way as the other, a finger scrolls only
// that way.
static const float kAxisRatio = 2;
// A finger lifting faster than this flings the page; a gliding page slows
// down by e every kGlideDecay and stops below kGlideStop.
static const float kFlingMin = 250;		// pixels per second
static const float kFlingMax = 6000;
static const float kGlideStop = 20;
static const bigtime_t kGlideDecay = 350000;
static const bigtime_t kGlideFrame = 16667;
// the movement that counts for the speed at the lift
static const bigtime_t kVelocityWindow = 100000;
static const int32 kSampleCount = 8;
// Marks the events this sends late, which come back through the filters.
static const char* kLateField = "mobile:touch";


class MobileTouchFilter : public BInputServerFilter {
public:
								MobileTouchFilter();
	virtual						~MobileTouchFilter();

	virtual	status_t			InitCheck();
	virtual	filter_result		Filter(BMessage* message, BList* outList);

private:
			enum State {
				kIdle,
				kPending,		// a press held back
				kScrolling,		// the finger scrolls
				kGliding,		// it has lifted, the page glides on
				kMenuPressed,	// a long press clicked the secondary
								// button; the rest of the touch goes
				kSelecting		// a long press selects; the rest of the
								// touch drags as it is
			};

			bool				_Wanted(BPoint where);
			void				_Wake();
	static	status_t			_Watcher(void* data);
			bigtime_t			_NextDeadline() const;
			void				_ReadPort(const port_message_info& info);
			void				_Tick();
			void				_Enqueue(BMessage* message);
			void				_Tell(BMessage& message);

			void				_StartScrolling(BPoint where, BList* outList);
			void				_Scroll(BPoint where, BList* outList);
			void				_AddSample(BPoint where, bigtime_t when);
			BPoint				_Velocity(bigtime_t now) const;
			BMessage*			_Wheel(float x, float y, int32 phase);

			port_id				fPort;
			// who asked, for where and what
			team_id				fOwner;
			BMessenger			fOwnerMessenger;
			BRect				fArea;
			uint32				fFlags;

			// Never started: it is only here to enqueue events from the
			// watcher thread.
			BInputServerDevice*	fInjector;

			BLocker				fLock;
			thread_id			fWatcher;

			State				fState;
			// the touch: where it came down, and when
			BMessage*			fHeld;
			BPoint				fOrigin;
			bigtime_t			fDownTime;
			// it stopped a glide, and clicks nothing
			bool				fStopper;

			// the release of a held back tap, and when it goes
			BMessage*			fRelease;
			bigtime_t			fReleaseAt;

			// scrolling: which ways, where the finger was, how it moved
			bool				fScrollX;
			bool				fScrollY;
			BPoint				fLast;
			BPoint				fSamples[kSampleCount];
			bigtime_t			fSampleTimes[kSampleCount];
			int32				fSampleCount;
			// the whole steps' leftovers
			float				fStepX;
			float				fStepY;
			// gliding: pixels per second, and the last frame
			BPoint				fVelocity;
			bigtime_t			fGlideTime;
};


MobileTouchFilter::MobileTouchFilter()
	:
	fOwner(-1),
	fFlags(0),
	fInjector(NULL),
	fLock("mobile touch"),
	fWatcher(-1),
	fState(kIdle),
	fHeld(NULL),
	fDownTime(0),
	fStopper(false),
	fRelease(NULL),
	fReleaseAt(0),
	fScrollX(true),
	fScrollY(true),
	fSampleCount(0),
	fStepX(0),
	fStepY(0),
	fGlideTime(0)
{
	fPort = create_port(16, MOBILE_TOUCH_PORT);
	if (fPort < 0)
		return;

	fInjector = new BInputServerDevice();
	fWatcher = spawn_thread(_Watcher, "mobile touch timer",
		B_URGENT_DISPLAY_PRIORITY, this);
	if (fWatcher >= 0)
		resume_thread(fWatcher);
}


MobileTouchFilter::~MobileTouchFilter()
{
	// the watcher ends with the port
	delete_port(fPort);
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
	if (fPort < 0)
		return fPort;
	return fWatcher >= 0 ? B_OK : fWatcher;
}


filter_result
MobileTouchFilter::Filter(BMessage* message, BList* outList)
{
	if (message->what != B_MOUSE_DOWN && message->what != B_MOUSE_MOVED
		&& message->what != B_MOUSE_UP) {
		return B_DISPATCH_MESSAGE;
	}

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

	// Nothing going on: only a press can change that.
	if (message->what != B_MOUSE_DOWN && fState == kIdle)
		return B_DISPATCH_MESSAGE;

	switch (message->what) {
		case B_MOUSE_DOWN:
		{
			bool stopped = false;
			if (fState == kGliding) {
				// a finger on a gliding page holds it
				outList->AddItem(_Wheel(0, 0, B_WHEEL_PHASE_ENDED));
				fState = kIdle;
				stopped = true;
			}

			if (fState != kIdle || !_Wanted(where)
				|| message->GetInt32("buttons", 0) != B_PRIMARY_MOUSE_BUTTON) {
				if (!outList->IsEmpty())
					outList->AddItem(new BMessage(*message));
				return B_DISPATCH_MESSAGE;
			}

			// a new tap can't wait for the last one's release
			if (fRelease != NULL) {
				outList->AddItem(fRelease);
				fRelease = NULL;
			}

			if ((fFlags & kTouchSelect) != 0) {
				BMessage began(kMsgTouchBegan);
				began.AddPoint("where", where);
				_Tell(began);
			}

			fHeld = new BMessage(*message);
			fOrigin = where;
			fDownTime = system_time();
			fStopper = stopped;
			fState = kPending;
			_Wake();
			// the list replaces the event, and it has been copied
			return outList->IsEmpty() ? B_SKIP_MESSAGE : B_DISPATCH_MESSAGE;
		}

		case B_MOUSE_MOVED:
			switch (fState) {
				case kMenuPressed:
					return B_SKIP_MESSAGE;

				case kPending:
					if (fabsf(where.x - fOrigin.x) < kTapSlop
						&& fabsf(where.y - fOrigin.y) < kTapSlop) {
						return B_SKIP_MESSAGE;
					}
					if ((fFlags & kTouchScroll) != 0) {
						_StartScrolling(where, outList);
						return B_DISPATCH_MESSAGE;
					}
					// the finger drags: the press goes out as it was, then
					// the move
					outList->AddItem(fHeld);
					fHeld = NULL;
					outList->AddItem(new BMessage(*message));
					fState = kIdle;
					return B_DISPATCH_MESSAGE;

				case kScrolling:
					_Scroll(where, outList);
					return B_DISPATCH_MESSAGE;

				default:
					return B_DISPATCH_MESSAGE;
			}

		case B_MOUSE_UP:
			switch (fState) {
				case kMenuPressed:
					fState = kIdle;
					return B_SKIP_MESSAGE;

				case kSelecting:
				{
					fState = kIdle;
					BMessage selected(kMsgTouchSelected);
					selected.AddPoint("where", where);
					selected.AddPoint("from", fOrigin);
					_Tell(selected);
					return B_DISPATCH_MESSAGE;
				}

				case kScrolling:
				{
					_Scroll(where, outList);
					bigtime_t now = system_time();
					BPoint velocity = _Velocity(now);
					float speed = sqrtf(velocity.x * velocity.x
						+ velocity.y * velocity.y);
					if (speed < kFlingMin) {
						outList->AddItem(_Wheel(0, 0, B_WHEEL_PHASE_ENDED));
						fState = kIdle;
					} else {
						if (speed > kFlingMax) {
							velocity.x *= kFlingMax / speed;
							velocity.y *= kFlingMax / speed;
						}
						fVelocity = velocity;
						fGlideTime = now;
						fState = kGliding;
						_Wake();
					}
					return outList->IsEmpty()
						? B_SKIP_MESSAGE : B_DISPATCH_MESSAGE;
				}

				case kPending:
				{
					fState = kIdle;
					if (fStopper) {
						// it only stopped the page
						delete fHeld;
						fHeld = NULL;
						return B_SKIP_MESSAGE;
					}
					// a tap: the press now, its release a moment later
					outList->AddItem(fHeld);
					fHeld = NULL;
					fRelease = new BMessage(*message);
					fRelease->AddBool(kLateField, true);
					fReleaseAt = system_time() + kTapLength;
					_Wake();
					return B_DISPATCH_MESSAGE;
				}

				default:
					return B_DISPATCH_MESSAGE;
			}
	}

	return B_DISPATCH_MESSAGE;
}


/*!	Whether a press at \a where is one to look after: someone who still
	runs asked for it there. Called with the lock held.
*/
bool
MobileTouchFilter::_Wanted(BPoint where)
{
	if (fOwner < 0)
		return false;

	team_info info;
	if (get_team_info(fOwner, &info) != B_OK) {
		// gone without a word
		fOwner = -1;
		return false;
	}
	return fArea.IsValid() && fArea.Contains(where);
}


/*!	Has the watcher look at the deadlines again. */
void
MobileTouchFilter::_Wake()
{
	// A full port wakes it as well.
	write_port_etc(fPort, kMobileWake, NULL, 0, B_RELATIVE_TIMEOUT, 0);
}


/*!	Reads the port, and sends what is due when it is due: the release of a
	tap, a long press, the frames of a glide.
*/
status_t
MobileTouchFilter::_Watcher(void* data)
{
	MobileTouchFilter* filter = (MobileTouchFilter*)data;
	while (true) {
		filter->fLock.Lock();
		bigtime_t deadline = filter->_NextDeadline();
		filter->fLock.Unlock();

		port_message_info info;
		status_t status = get_port_message_info_etc(filter->fPort, &info,
			B_ABSOLUTE_TIMEOUT, deadline);
		if (status == B_BAD_PORT_ID)
			return B_OK;

		filter->fLock.Lock();
		if (status == B_OK)
			filter->_ReadPort(info);
		filter->_Tick();
		filter->fLock.Unlock();
	}
}


/*!	Called with the lock held. */
bigtime_t
MobileTouchFilter::_NextDeadline() const
{
	bigtime_t deadline = B_INFINITE_TIMEOUT;
	if (fRelease != NULL)
		deadline = fReleaseAt;
	if (fState == kPending && fDownTime + kLongPress < deadline)
		deadline = fDownTime + kLongPress;
	if (fState == kGliding && fGlideTime + kGlideFrame < deadline)
		deadline = fGlideTime + kGlideFrame;
	return deadline;
}


/*!	Takes the message waiting at the port. Called with the lock held. */
void
MobileTouchFilter::_ReadPort(const port_message_info& info)
{
	char buffer[sizeof(mobile_hold_area)];
	int32 code;
	ssize_t size = read_port_etc(fPort, &code, buffer, sizeof(buffer),
		B_RELATIVE_TIMEOUT, 0);
	if (size < 0)
		return;

	switch (code) {
		case kMobileHoldArea:
		{
			mobile_hold_area area;
			if (size != (ssize_t)sizeof(area))
				break;
			memcpy(&area, buffer, sizeof(area));
			if (area.enabled != 0) {
				if (info.sender_team != fOwner) {
					fOwner = info.sender_team;
					fOwnerMessenger = BMessenger(NULL, fOwner);
				}
				fArea.Set(area.left, area.top, area.right, area.bottom);
				fFlags = area.flags;
			} else if (info.sender_team == fOwner)
				fOwner = -1;
			break;
		}

		case kMobileMenuAt:
		{
			float point[2];
			if (size != (ssize_t)sizeof(point) || info.sender_team != fOwner)
				break;
			memcpy(point, buffer, sizeof(point));
			// the pointer goes there first: app_server takes a press's
			// window from the last move
			BPoint where(point[0], point[1]);
			uint32 what[] = { B_MOUSE_MOVED, B_MOUSE_DOWN, B_MOUSE_UP };
			for (uint32 type : what) {
				BMessage* event = new BMessage(type);
				event->AddPoint("where", where);
				event->AddInt32("buttons",
					type == B_MOUSE_DOWN ? B_SECONDARY_MOUSE_BUTTON : 0);
				if (type == B_MOUSE_DOWN)
					event->AddInt32("clicks", 1);
				event->AddInt32("be:device_subtype", B_TABLET_POINTING_DEVICE);
				event->AddBool(kLateField, true);
				_Enqueue(event);
			}
			break;
		}
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

	if (fState == kPending && now >= fDownTime + kLongPress) {
		BMessage* down = fHeld;
		fHeld = NULL;
		down->AddBool(kLateField, true);
		if ((fFlags & kTouchSelect) != 0) {
			// A double click selects the word, and held down, dragging
			// selects word by word. It comes on its own, counted (app_server
			// would count it a first click): a first click would follow a
			// link.
			down->SetInt32("clicks", 2);
			down->SetBool("be:clicks_counted", true);
			_Enqueue(down);
			fState = kSelecting;
		} else {
			fState = kMenuPressed;
			down->SetInt32("buttons", B_SECONDARY_MOUSE_BUTTON);
			down->SetInt32("clicks", 1);
			_Enqueue(down);

			BMessage* up = new BMessage(B_MOUSE_UP);
			up->AddInt32("buttons", 0);
			up->AddPoint("where", fOrigin);
			up->AddInt32("be:device_subtype", B_TABLET_POINTING_DEVICE);
			up->AddBool(kLateField, true);
			_Enqueue(up);
		}
	}

	if (fState == kGliding && now >= fGlideTime + kGlideFrame) {
		float decay = expf(-(float)(now - fGlideTime) / kGlideDecay);
		// how far the page goes while slowing down from one speed to the
		// next
		float x = fVelocity.x * (1 - decay) * kGlideDecay / 1000000.0f;
		float y = fVelocity.y * (1 - decay) * kGlideDecay / 1000000.0f;
		fVelocity.x *= decay;
		fVelocity.y *= decay;
		fGlideTime = now;
		_Enqueue(_Wheel(x, y, B_WHEEL_PHASE_CHANGED));

		if (sqrtf(fVelocity.x * fVelocity.x + fVelocity.y * fVelocity.y)
				< kGlideStop) {
			_Enqueue(_Wheel(0, 0, B_WHEEL_PHASE_ENDED));
			fState = kIdle;
		}
	}
}


void
MobileTouchFilter::_Enqueue(BMessage* message)
{
	message->SetInt64("when", system_time());
	if (fInjector->EnqueueMessage(message) != B_OK)
		delete message;
}


/*!	Tells the application that asked, without waiting for it. */
void
MobileTouchFilter::_Tell(BMessage& message)
{
	if (fOwnerMessenger.IsValid())
		fOwnerMessenger.SendMessage(&message, (BHandler*)NULL, 0);
}


/*!	The finger has moved off: from now on it scrolls. The press it came
	down with is not sent; the pointer stays where it came down, so the
	wheel goes to what is under it there. Called with the lock held.
*/
void
MobileTouchFilter::_StartScrolling(BPoint where, BList* outList)
{
	delete fHeld;
	fHeld = NULL;
	fState = kScrolling;

	float dx = fabsf(where.x - fOrigin.x);
	float dy = fabsf(where.y - fOrigin.y);
	fScrollX = dx * kAxisRatio > dy;
	fScrollY = dy * kAxisRatio > dx;

	fStepX = fStepY = 0;
	fSampleCount = 0;
	_AddSample(fOrigin, fDownTime);
	_AddSample(where, system_time());

	// The page follows the finger from where it came down.
	fLast = where;
	outList->AddItem(_Wheel(fScrollX ? fOrigin.x - where.x : 0,
		fScrollY ? fOrigin.y - where.y : 0, B_WHEEL_PHASE_BEGAN));
}


/*!	Called with the lock held. */
void
MobileTouchFilter::_Scroll(BPoint where, BList* outList)
{
	float x = fScrollX ? fLast.x - where.x : 0;
	float y = fScrollY ? fLast.y - where.y : 0;
	fLast = where;
	_AddSample(where, system_time());
	if (x != 0 || y != 0)
		outList->AddItem(_Wheel(x, y, B_WHEEL_PHASE_CHANGED));
}


void
MobileTouchFilter::_AddSample(BPoint where, bigtime_t when)
{
	if (fSampleCount == kSampleCount) {
		for (int32 i = 1; i < kSampleCount; i++) {
			fSamples[i - 1] = fSamples[i];
			fSampleTimes[i - 1] = fSampleTimes[i];
		}
		fSampleCount--;
	}
	fSamples[fSampleCount] = where;
	fSampleTimes[fSampleCount] = when;
	fSampleCount++;
}


/*!	How fast the page goes at the lift, in pixels per second, from how the
	finger moved over the last moment. A finger that stopped before it
	lifted has no speed.
*/
BPoint
MobileTouchFilter::_Velocity(bigtime_t now) const
{
	if (fSampleCount < 2 || now - fSampleTimes[fSampleCount - 1]
			> kVelocityWindow / 2) {
		return BPoint(0, 0);
	}

	int32 first = fSampleCount - 1;
	while (first > 0 && now - fSampleTimes[first - 1] <= kVelocityWindow)
		first--;
	if (first == fSampleCount - 1)
		first--;

	bigtime_t span = fSampleTimes[fSampleCount - 1] - fSampleTimes[first];
	if (span <= 0)
		return BPoint(0, 0);

	BPoint moved = fSamples[fSampleCount - 1] - fSamples[first];
	// the page goes against the finger's way
	return BPoint(fScrollX ? -moved.x * 1000000.0f / span : 0,
		fScrollY ? -moved.y * 1000000.0f / span : 0);
}


/*!	A wheel event that scrolls by \a x, \a y pixels (positive: towards the
	end, as a wheel turned towards one).
*/
BMessage*
MobileTouchFilter::_Wheel(float x, float y, int32 phase)
{
	BMessage* message = new BMessage(B_MOUSE_WHEEL_CHANGED);

	// whole steps for whoever knows only those, the rest kept for the next
	fStepX += x / kPixelsPerStep;
	fStepY += y / kPixelsPerStep;
	float stepsX = truncf(fStepX);
	float stepsY = truncf(fStepY);
	fStepX -= stepsX;
	fStepY -= stepsY;

	message->AddInt64("when", system_time());
	message->AddFloat("be:wheel_delta_x", stepsX);
	message->AddFloat("be:wheel_delta_y", stepsY);
	message->AddFloat("be:wheel_precise_delta_x", x / kPixelsPerStep);
	message->AddFloat("be:wheel_precise_delta_y", y / kPixelsPerStep);
	message->AddInt32("be:wheel_phase", phase);
	message->AddFloat("be:wheel_pixel_delta_x", x);
	message->AddFloat("be:wheel_pixel_delta_y", y);
	message->AddInt32("be:device_subtype", B_TABLET_POINTING_DEVICE);
	message->AddBool(kLateField, true);
	return message;
}


extern "C" BInputServerFilter*
instantiate_input_filter()
{
	return new MobileTouchFilter();
}
