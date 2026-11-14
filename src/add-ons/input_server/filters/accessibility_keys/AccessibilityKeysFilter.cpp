/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Global keys of the screen reader (haiku#54):

		Option+Command+S	turn the screen reader on or off
		Option+Command+F	read the focused control again
		Option+Command+W	read the window title and the focus
		Option+Command+R	read the whole window
		Option+Command+X	stop speaking

	(Option is the Windows key, Command the Alt key in the default keymap.)
	The review keys are only taken while the screen reader runs; otherwise
	they reach the application as before.
*/


#include <ctype.h>
#include <new>

#include <InputServerFilter.h>
#include <InterfaceDefs.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <Roster.h>

#include <AccessibilityProtocol.h>


class AccessibilityKeysFilter : public BInputServerFilter {
public:
	virtual	filter_result		Filter(BMessage* message, BList* _list);

private:
	static	status_t			_Toggle(void*);
};


extern "C" _EXPORT BInputServerFilter*
instantiate_input_filter()
{
	return new(std::nothrow) AccessibilityKeysFilter();
}


filter_result
AccessibilityKeysFilter::Filter(BMessage* message, BList* _list)
{
	if (message->what != B_KEY_DOWN)
		return B_DISPATCH_MESSAGE;

	int32 modifiers = message->GetInt32("modifiers", 0);
	if ((modifiers & (B_COMMAND_KEY | B_CONTROL_KEY | B_OPTION_KEY
			| B_MENU_KEY)) != (B_COMMAND_KEY | B_OPTION_KEY)) {
		return B_DISPATCH_MESSAGE;
	}
	const bool shift = (modifiers & B_SHIFT_KEY) != 0;

	// the Option key changes "bytes"; the unmodified character tells the key
	int32 rawChar = tolower(message->GetInt32("raw_char", 0));

	// browse mode (a virtual cursor through what read-all says): arrows
	// step, H K J L D jump to the next heading, link, form field, list,
	// landmark (with Shift: the previous one), Return activates
	int32 browseKind = -1;
	int32 direction = shift ? -1 : 1;
	switch (rawChar) {
		case B_DOWN_ARROW:
			browseKind = 0;
			direction = 1;
			break;
		case B_UP_ARROW:
			browseKind = 0;
			direction = -1;
			break;
		case 'h':
			browseKind = 1;
			break;
		case 'k':
			browseKind = 2;
			break;
		case 'j':
			browseKind = 3;
			break;
		case 'l':
			browseKind = 4;
			break;
		case 'd':
			browseKind = 5;
			break;
	}
	if (browseKind < 0 && rawChar != B_ENTER && shift)
		return B_DISPATCH_MESSAGE;

	uint32 command;
	BMessage request;
	switch (browseKind >= 0 ? -2 : rawChar) {
		case -2:
			command = B_SCREEN_READER_BROWSE;
			request.AddInt32("direction", direction);
			request.AddInt32("kind", browseKind);
			break;
		case B_ENTER:
			command = B_SCREEN_READER_ACTIVATE;
			break;
		case 's':
			if (message->GetInt32("be:key_repeat", 0) == 0) {
				thread_id thread = spawn_thread(_Toggle,
					"screen reader toggle", B_NORMAL_PRIORITY, NULL);
				if (thread >= 0)
					resume_thread(thread);
			}
			return B_SKIP_MESSAGE;
		case 'f':
			command = B_SCREEN_READER_READ_FOCUS;
			break;
		case 'w':
			command = B_SCREEN_READER_READ_WINDOW;
			break;
		case 'r':
			command = B_SCREEN_READER_READ_ALL;
			break;
		case 'x':
			command = B_SCREEN_READER_STOP;
			break;
		default:
			return B_DISPATCH_MESSAGE;
	}

	// Only ask the registrar: BMessenger(signature) would also ask the
	// launch_daemon, which never answers while a job of that name waits for
	// its condition, and this thread delivers all input.
	app_info info;
	if (be_roster->GetAppInfo(B_SCREEN_READER_SIGNATURE, &info) != B_OK)
		return B_DISPATCH_MESSAGE;

	BMessenger reader(NULL, info.team);
	request.what = command;
	reader.SendMessage(&request, (BHandler*)NULL, 0);
	return B_SKIP_MESSAGE;
}


/*static*/ status_t
AccessibilityKeysFilter::_Toggle(void*)
{
	app_info info;
	if (be_roster->GetAppInfo(B_SCREEN_READER_SIGNATURE, &info) == B_OK) {
		BMessenger reader(NULL, info.team);
		reader.SendMessage(B_QUIT_REQUESTED);
	} else
		be_roster->Launch(B_SCREEN_READER_SIGNATURE);
	return B_OK;
}
