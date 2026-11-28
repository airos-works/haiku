/*
 * Copyright 2000, Shamyl Zakariya.
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Keeps the touchpad still while typing ("disable while typing"): a palm
	resting on or brushing the touchpad must neither move the pointer nor
	click.

	Touches that begin within the keyboard lock delay of a key (the
	padblocker_threshold of the touchpad settings, in milliseconds) count for
	nothing until they lift: no movement, scrolling or tap. While typing, a
	run of keys, the delay is longer. Touches that began before the typing,
	the buttons of the touchpad itself and keys pressed with Command, Control
	or Option (shortcuts, used with the pointer) are left alone.

	The touchpad's messages tell when the touch they come of began
	("be:touch_began") and whether a press is a tap ("be:tap").
*/


#include <new>
#include <sys/stat.h>

#include <input/touchpad_settings.h>
#include <File.h>
#include <FindDirectory.h>
#include <Input.h>
#include <InterfaceDefs.h>
#include <List.h>
#include <Message.h>
#include <OS.h>
#include <Path.h>

#include <add-ons/input_server/InputServerDevice.h>
#include <add-ons/input_server/InputServerFilter.h>


extern "C" _EXPORT BInputServerFilter* instantiate_input_filter();


// Keys this close together are typing, which locks the touchpad longer.
static const bigtime_t kTypingGap = 500000;
static const int32 kTypingFactor = 5;
	// in halves: 2.5 times the delay while typing

// how often the settings file is checked for changes, at most
static const bigtime_t kSettingsCheckInterval = 1000000;


class PadBlocker : public BInputServerFilter {
public:
								PadBlocker();
	virtual						~PadBlocker();

	virtual	filter_result		Filter(BMessage* message, BList* outList);

private:
			void				_LoadSettings();
			void				_CheckSettings(bigtime_t now);
			bool				_KeyNear(bigtime_t when) const;
			bool				_IsBlocked(BMessage* message, bigtime_t now);

			bigtime_t			fThreshold;
			time_t				fSettingsTime;
			bigtime_t			fSettingsChecked;

			bigtime_t			fLastKey;
			bigtime_t			fLastKeyDown;
			bool				fTyping;
			bigtime_t			fLockedUntil;

			// the touch whose messages go nowhere, by when it began
			bigtime_t			fBlockedTouch;
			// the touchpad buttons the rest of the system saw go down
			int32				fPassedButtons;
			bool				fScrolling;
};


PadBlocker::PadBlocker()
	:
	fThreshold(kDefaultTouchpadSettings.padblocker_threshold * 1000LL),
	fSettingsTime(0),
	fSettingsChecked(0),
	fLastKey(0),
	fLastKeyDown(0),
	fTyping(false),
	fLockedUntil(0),
	fBlockedTouch(-1),
	fPassedButtons(0),
	fScrolling(false)
{
	_LoadSettings();
}


PadBlocker::~PadBlocker()
{
}


void
PadBlocker::_LoadSettings()
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK
		|| path.Append(TOUCHPAD_SETTINGS_FILE) != B_OK) {
		return;
	}

	struct stat st;
	if (stat(path.Path(), &st) != 0)
		return;
	fSettingsTime = st.st_mtime;

	BFile file(path.Path(), B_READ_ONLY);
	BMessage settings;
	if (settings.Unflatten(&file) != B_OK)
		return;

	int16 threshold;
	if (settings.FindInt16("padblocker_threshold", &threshold) == B_OK)
		fThreshold = touchpad_padblocker_threshold(threshold) * 1000LL;
}


/*!	The preferences save the settings file when they change; the input
	server tells filters nothing.
*/
void
PadBlocker::_CheckSettings(bigtime_t now)
{
	if (now - fSettingsChecked < kSettingsCheckInterval)
		return;
	fSettingsChecked = now;

	BPath path;
	struct stat st;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK
		|| path.Append(TOUCHPAD_SETTINGS_FILE) != B_OK
		|| stat(path.Path(), &st) != 0 || st.st_mtime == fSettingsTime) {
		return;
	}

	_LoadSettings();
}


//!	Whether a touch that began then began while typing.
bool
PadBlocker::_KeyNear(bigtime_t when) const
{
	if (fThreshold <= 0 || fLastKey == 0)
		return false;
	if (when >= fLastKey)
		return when < fLockedUntil;
	// a key soon after the touch began: a palm put down while typing
	return fLastKey - when < fThreshold;
}


bool
PadBlocker::_IsBlocked(BMessage* message, bigtime_t now)
{
	bigtime_t touchBegan;
	if (message->FindInt64("be:touch_began", &touchBegan) != B_OK) {
		// a touchpad that does not tell: only its presses go, as soon after
		// a key as ever
		return message->what == B_MOUSE_DOWN && _KeyNear(now);
	}

	if (touchBegan == fBlockedTouch)
		return true;
	if (_KeyNear(touchBegan)) {
		fBlockedTouch = touchBegan;
		return true;
	}
	return false;
}


filter_result
PadBlocker::Filter(BMessage* message, BList* outList)
{
	switch (message->what) {
		case B_KEY_DOWN:
		case B_KEY_UP:
		{
			if (message->HasInt32("be:key_repeat"))
				break;
			// shortcuts are used with the pointer
			int32 modifiers = 0;
			message->FindInt32("modifiers", &modifiers);
			if ((modifiers & (B_COMMAND_KEY | B_CONTROL_KEY | B_OPTION_KEY))
					!= 0) {
				break;
			}

			bigtime_t now = system_time();
			_CheckSettings(now);

			// a key going down soon after another is typing
			if (message->what == B_KEY_DOWN) {
				fTyping = fLastKeyDown != 0
					&& now - fLastKeyDown < kTypingGap;
				fLastKeyDown = now;
			}
			bigtime_t lock = fThreshold;
			if (fTyping)
				lock = lock * kTypingFactor / 2;
			fLastKey = now;
			fLockedUntil = max_c(fLockedUntil, now + lock);
			break;
		}

		case B_MOUSE_MOVED:
		case B_MOUSE_DOWN:
		case B_MOUSE_UP:
		case B_MOUSE_WHEEL_CHANGED:
		{
			int32 subtype;
			if (message->FindInt32("be:device_subtype", &subtype) != B_OK
				|| subtype != B_TOUCHPAD_POINTING_DEVICE) {
				break;
			}

			bigtime_t now = system_time();
			bool blocked = _IsBlocked(message, now);
			int32 buttons = 0;
			message->FindInt32("buttons", &buttons);

			if (message->what == B_MOUSE_DOWN) {
				// the touchpad's own buttons are pressed on purpose
				bool tap = message->GetBool("be:tap", true);
				if (blocked && tap)
					return B_SKIP_MESSAGE;
				if (tap && fThreshold > 0 && now < fLockedUntil)
					return B_SKIP_MESSAGE;
				fPassedButtons = buttons;
				break;
			}

			if (message->what == B_MOUSE_UP) {
				// only what went down comes up
				if ((fPassedButtons & ~buttons) == 0)
					return B_SKIP_MESSAGE;
				fPassedButtons &= buttons;
				break;
			}

			if (message->what == B_MOUSE_WHEEL_CHANGED) {
				int32 phase = message->GetInt32("be:wheel_phase",
					B_WHEEL_PHASE_NONE);
				if (blocked) {
					// a scroll gesture under way still ends
					if (fScrolling && phase == B_WHEEL_PHASE_ENDED) {
						fScrolling = false;
						message->ReplaceFloat("be:wheel_delta_x", 0);
						message->ReplaceFloat("be:wheel_delta_y", 0);
						message->ReplaceFloat("be:wheel_precise_delta_x", 0);
						message->ReplaceFloat("be:wheel_precise_delta_y", 0);
						break;
					}
					return B_SKIP_MESSAGE;
				}
				if (phase == B_WHEEL_PHASE_BEGAN)
					fScrolling = true;
				else if (phase == B_WHEEL_PHASE_ENDED)
					fScrolling = false;
				break;
			}

			// B_MOUSE_MOVED
			if (blocked)
				return B_SKIP_MESSAGE;
			// no dragging with a button whose press went nowhere
			if ((buttons & ~fPassedButtons) != 0)
				message->ReplaceInt32("buttons", buttons & fPassedButtons);
			break;
		}

		default:
			break;
	}

	return B_DISPATCH_MESSAGE;
}


//	#pragma mark -


BInputServerFilter*
instantiate_input_filter()
{
	return new(std::nothrow) PadBlocker();
}
