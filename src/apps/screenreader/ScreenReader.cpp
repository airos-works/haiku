/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	ScreenReader (haiku#54): speaks what has the keyboard focus.

	It subscribes to the accessibility_server (which then enables the
	accessibility bridge of every application) and turns the events into
	speech: the active window's title, the focused control's name, role,
	value and state, value changes, list selections, menu items, typed and
	deleted text and caret moves. Review commands come from the
	accessibility_keys input filter:

		Option+Command+S	turn the screen reader on and off
		Option+Command+F	read the focused control again
		Option+Command+W	read the window title and the focus
		Option+Command+R	read the whole window
		Option+Command+X	stop speaking

	Browse mode, a virtual cursor through what read-all says:

		Option+Command+Down/Up		next/previous item
		Option+Command+H K J L D	next heading, link, form field, list,
									landmark (with Shift: the previous one)
		Option+Command+Return		activate the item (press; focus a field)

	Everything spoken is also written to the syslog ("ScreenReader: say:")
	and, unless turned off, shown in a caption window. Without a speech
	engine (haiku#52), the captions are the output.
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <Application.h>
#include <FindDirectory.h>
#include <Message.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <Path.h>
#include <String.h>
#include <StringList.h>
#include <driver_settings.h>

#include <Accessible.h>
#include <AccessibilityProtocol.h>

#include "CaptionWindow.h"
#include "NodeText.h"
#include "SpeechOutput.h"


static const uint32 kMsgSubscribe = 'subs';
static const bigtime_t kQueryTimeout = 500000;
static const bigtime_t kSettleTime = 400000;
static const int32 kMaxSubscribeTries = 20;


class ScreenReader : public BApplication {
public:
								ScreenReader();
	virtual						~ScreenReader();

	virtual	void				ReadyToRun();
	virtual	bool				QuitRequested();
	virtual	void				MessageReceived(BMessage* message);

private:
			void				_LoadSettings();
			bool				_Subscribe();
			void				_HandleEvent(BMessage* event);
			void				_Say(const BString& text, bool interrupt);


			void				_ReadFocus(bool withWindow);
			void				_ReadAll();
			bool				_GetWindowTree(BMessage& window);
			void				_Browse(int32 direction, int32 kind);
			void				_Activate();

			bool				_IsFocus(const BMessage* event,
									const BMessage& node) const;

private:
			SpeechOutput*		fSpeech;
			CaptionWindow*		fCaptions;
			BMessenger			fServer;
			BMessageRunner*		fSubscribeRunner;
			int32				fSubscribeTries;
			bool				fSubscribed;

			bool				fShowCaptions;
			BString				fEngine;
			int32				fRate;

			// what the user is in
			team_id				fTeam;
			BMessenger			fBridge;
			int64				fWindowID;
			BString				fWindowTitle;
			int64				fFocusID;
			bigtime_t			fStartTime;
			bigtime_t			fWindowTime;
			bigtime_t			fFocusTime;
			bigtime_t			fTextTime;
			int32				fCaretLine;

			// browse mode
			std::vector<browse_item> fBrowseItems;
			int32				fBrowseIndex;
			int64				fBrowseWindowID;
			int64				fBrowseItemID;
			bigtime_t			fBrowseTime;
};


ScreenReader::ScreenReader()
	:
	BApplication(B_SCREEN_READER_SIGNATURE),
	fSpeech(NULL),
	fCaptions(NULL),
	fSubscribeRunner(NULL),
	fSubscribeTries(0),
	fSubscribed(false),
	fShowCaptions(true),
	fRate(0),
	fTeam(-1),
	fWindowID(0),
	fFocusID(0),
	fStartTime(0),
	fWindowTime(0),
	fFocusTime(0),
	fTextTime(0),
	fCaretLine(-1),
	fBrowseIndex(-1),
	fBrowseWindowID(0),
	fBrowseItemID(0),
	fBrowseTime(0)
{
}


ScreenReader::~ScreenReader()
{
	delete fSpeech;
	delete fSubscribeRunner;
}


void
ScreenReader::ReadyToRun()
{
	_LoadSettings();
	fStartTime = system_time();

	fSpeech = new SpeechOutput(fEngine.String(), fRate);
	syslog(LOG_INFO, "ScreenReader: speech engine: %s",
		fSpeech->EngineName().String());

	if (fShowCaptions || !fSpeech->HasEngine()) {
		fCaptions = new CaptionWindow();
		fCaptions->Show();
	}

	if (fSpeech->HasEngine())
		_Say("Screen reader on", true);
	else
		_Say("Screen reader on, no speech engine, captions only", true);

	if (!_Subscribe()) {
		// the server may still be starting
		BMessage retry(kMsgSubscribe);
		fSubscribeRunner = new BMessageRunner(BMessenger(this), &retry,
			500000, kMaxSubscribeTries);
	}
}


bool
ScreenReader::QuitRequested()
{
	if (fSubscribed) {
		BMessage message(B_ACCESSIBILITY_UNSUBSCRIBE);
		message.AddMessenger("client", BMessenger(this));
		fServer.SendMessage(&message, (BHandler*)NULL, kQueryTimeout);
		fSubscribed = false;
	}

	_Say("Screen reader off", true);
	if (fSpeech != NULL)
		fSpeech->WaitUntilDone(3000000);
	if (fCaptions != NULL)
		snooze(500000);
	return true;
}


void
ScreenReader::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgSubscribe:
			if (fSubscribed || _Subscribe()
				|| ++fSubscribeTries >= kMaxSubscribeTries) {
				delete fSubscribeRunner;
				fSubscribeRunner = NULL;
				if (!fSubscribed)
					_Say("The accessibility server is not available", false);
			}
			break;

		case B_ACCESSIBILITY_EVENT:
			_HandleEvent(message);
			break;

		case B_SCREEN_READER_READ_FOCUS:
			_ReadFocus(false);
			break;

		case B_SCREEN_READER_READ_WINDOW:
			_ReadFocus(true);
			break;

		case B_SCREEN_READER_READ_ALL:
			_ReadAll();
			break;

		case B_SCREEN_READER_STOP:
			if (fSpeech != NULL)
				fSpeech->Stop();
			break;

		case B_SCREEN_READER_BROWSE:
			_Browse(message->GetInt32("direction", 1),
				message->GetInt32("kind", BROWSE_ANY));
			break;

		case B_SCREEN_READER_ACTIVATE:
			_Activate();
			break;

		default:
			BApplication::MessageReceived(message);
			break;
	}
}


void
ScreenReader::_LoadSettings()
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return;
	path.Append("ScreenReader");

	void* handle = load_driver_settings(path.Path());
	if (handle == NULL)
		return;

	fShowCaptions = get_driver_boolean_parameter(handle, "captions", true,
		true);
	const char* engine = get_driver_parameter(handle, "engine", NULL, NULL);
	if (engine != NULL)
		fEngine = engine;
	const char* rate = get_driver_parameter(handle, "rate", NULL, NULL);
	if (rate != NULL)
		fRate = atoi(rate);
	unload_driver_settings(handle);
}


bool
ScreenReader::_Subscribe()
{
	// launch_daemon starts the on-demand server for us
	BMessenger server(B_ACCESSIBILITY_SERVER_SIGNATURE);
	if (!server.IsValid())
		return false;

	BMessage message(B_ACCESSIBILITY_SUBSCRIBE);
	message.AddMessenger("client", BMessenger(this));
	BMessage reply;
	if (server.SendMessage(&message, &reply, kQueryTimeout, 2000000) != B_OK
		|| reply.GetInt32("status", B_ERROR) != B_OK) {
		return false;
	}

	fServer = server;
	fSubscribed = true;
	syslog(LOG_INFO, "ScreenReader: subscribed to the accessibility_server");
	return true;
}


void
ScreenReader::_Say(const BString& text, bool interrupt)
{
	if (text.IsEmpty())
		return;

	syslog(LOG_INFO, "ScreenReader: say: %s", text.String());
	printf("say: %s\n", text.String());
	fflush(stdout);

	if (fSpeech != NULL)
		fSpeech->Speak(text.String(), interrupt);
	if (fCaptions != NULL)
		fCaptions->AddCaption(text.String());
}


bool
ScreenReader::_IsFocus(const BMessage* event, const BMessage& node) const
{
	return event->GetInt32("team", -1) == fTeam
		&& node.GetInt64(B_ACCESSIBLE_FIELD_ID, 0) == fFocusID
		&& fFocusID != 0;
}


void
ScreenReader::_HandleEvent(BMessage* event)
{
	int32 what = event->GetInt32("event", 0);
	team_id team = event->GetInt32("team", -1);
	bool windowActive = event->GetBool("window_active", false);
	bigtime_t now = system_time();

	BMessage node;
	bool hasNode = event->FindMessage("node", &node) == B_OK;
	BMessage window;
	bool hasWindow = event->FindMessage("window", &window) == B_OK;

	switch (what) {
		case B_ACCESSIBLE_EVENT_WINDOW_ACTIVATED:
		{
			if (!hasWindow)
				break;
			fTeam = team;
			event->FindMessenger("bridge", &fBridge);
			fWindowID = window.GetInt64(B_ACCESSIBLE_FIELD_ID, 0);
			fWindowTitle = window.GetString(B_ACCESSIBLE_FIELD_NAME, "");
			fWindowTime = now;
			fCaretLine = -1;

			BString text = describe_window(window);
			if (hasNode) {
				fFocusID = node.GetInt64(B_ACCESSIBLE_FIELD_ID, 0);
				fFocusTime = now;
				text << ". " << describe_node(node);
			} else
				fFocusID = 0;
			// don't cut off "Screen reader on"
			_Say(text, now - fStartTime > 1500000);
			break;
		}

		case B_ACCESSIBLE_EVENT_FOCUS_CHANGED:
		{
			if (!hasNode || !windowActive)
				break;
			int64 id = node.GetInt64(B_ACCESSIBLE_FIELD_ID, 0);
			bool menu = event->GetBool("menu", false);
			if (team == fTeam && id == fFocusID && !menu) {
				// already said (with the window, or a part of the same
				// control took the focus)
				break;
			}
			if (!menu && hasWindow && team == fTeam) {
				fWindowID = window.GetInt64(B_ACCESSIBLE_FIELD_ID, 0);
				fWindowTitle = window.GetString(B_ACCESSIBLE_FIELD_NAME, "");
			}
			if (team != fTeam)
				event->FindMessenger("bridge", &fBridge);
			fTeam = team;
			fFocusID = id;
			fFocusTime = now;
			fCaretLine = -1;
			// don't cut off the window title that was just started
			_Say(describe_node(node), now - fWindowTime > kSettleTime);
			break;
		}

		case B_ACCESSIBLE_EVENT_VALUE_CHANGED:
		case B_ACCESSIBLE_EVENT_STATE_CHANGED:
		{
			if (!hasNode || !_IsFocus(event, node))
				break;
			uint64 states = (uint64)node.GetInt64(B_ACCESSIBLE_FIELD_STATES,
				0);
			int32 role = node.GetInt32(B_ACCESSIBLE_FIELD_ROLE, 0);
			BString text;
			if (what == B_ACCESSIBLE_EVENT_STATE_CHANGED) {
				// "changed": the state bits that changed; without it the
				// event is an Interface Kit control enabled or disabled
				int64 changed;
				if (event->FindInt64("changed", &changed) != B_OK)
					changed = B_ACCESSIBLE_STATE_DISABLED;
				text = describe_state_change(node, (uint64)changed);
			} else if ((states & B_ACCESSIBLE_STATE_CHECKABLE) != 0) {
				if ((states & B_ACCESSIBLE_STATE_CHECKED) != 0)
					text = "checked";
				else if ((states & B_ACCESSIBLE_STATE_MIXED) != 0)
					text = "partially checked";
				else
					text = "not checked";
			} else if (role == B_ACCESSIBLE_ROLE_TOGGLE_BUTTON) {
				text = (states & B_ACCESSIBLE_STATE_PRESSED) != 0
					? "pressed" : "not pressed";
			} else if (role == B_ACCESSIBLE_ROLE_TEXT_FIELD
				|| role == B_ACCESSIBLE_ROLE_TEXT_AREA) {
				// text changes are spoken as they are typed
				break;
			} else
				text = node.GetString(B_ACCESSIBLE_FIELD_VALUE, "");
			_Say(text, true);
			break;
		}

		case B_ACCESSIBLE_EVENT_NAME_CHANGED:
			if (hasNode && _IsFocus(event, node))
				_Say(describe_node(node, false), true);
			break;

		case B_ACCESSIBLE_EVENT_SELECTION_CHANGED:
		{
			BMessage container;
			if (!hasNode || event->FindMessage("container", &container) != B_OK
				|| !_IsFocus(event, container) || !windowActive) {
				break;
			}
			_Say(describe_node(node), true);
			break;
		}

		case B_ACCESSIBLE_EVENT_TEXT_INSERTED:
		case B_ACCESSIBLE_EVENT_TEXT_DELETED:
		{
			if (!hasNode || !_IsFocus(event, node))
				break;
			fTextTime = now;
			if (now - fFocusTime < kSettleTime)
				break;
			BString text;
			if (event->GetBool("hidden", false)) {
				int32 length = event->GetInt32("length", 1);
				text = length == 1 ? "star" : "stars";
			} else
				text = spoken_text(event->GetString("text", ""));
			if (what == B_ACCESSIBLE_EVENT_TEXT_DELETED
				&& event->GetInt32("length", 0) > 1) {
				text.Prepend("deleted ");
			}
			_Say(text, true);
			break;
		}

		case B_ACCESSIBLE_EVENT_CARET_MOVED:
		{
			if (!hasNode || !_IsFocus(event, node))
				break;
			if (now - fTextTime < 150000 || now - fFocusTime < kSettleTime) {
				fCaretLine = event->GetInt32("line", fCaretLine);
				break;
			}
			int32 start = event->GetInt32(B_ACCESSIBLE_FIELD_SEL_START, 0);
			int32 end = event->GetInt32(B_ACCESSIBLE_FIELD_SEL_END, 0);
			if (start != end) {
				BString text("selected ");
				text << event->GetString("text", "");
				_Say(text, true);
				break;
			}
			int32 line = event->GetInt32("line", -1);
			if (line != fCaretLine && fCaretLine >= 0) {
				_Say(spoken_text(event->GetString("line_text", "")), true);
			} else {
				BString character = event->GetString("char", "");
				_Say(character.IsEmpty() ? BString("end")
					: spoken_text(character), true);
			}
			fCaretLine = line;
			break;
		}

		case B_ACCESSIBLE_EVENT_ANNOUNCEMENT:
			_Say(event->GetString("text", ""), event->GetBool("interrupt",
				false));
			break;

		case B_ACCESSIBLE_EVENT_APP_QUIT:
			if (team == fTeam) {
				fTeam = -1;
				fBridge = BMessenger();
				fFocusID = 0;
				fWindowID = 0;
			}
			break;

		default:
			break;
	}
}


void
ScreenReader::_ReadFocus(bool withWindow)
{
	if (!fBridge.IsValid()) {
		_Say("No window", true);
		return;
	}

	BMessage request(B_ACCESSIBILITY_GET_FOCUS);
	BMessage reply;
	if (fBridge.SendMessage(&request, &reply, kQueryTimeout, kQueryTimeout)
			!= B_OK || reply.GetInt32("status", B_ERROR) != B_OK) {
		_Say("The window does not answer", true);
		return;
	}

	BString text;
	BMessage window;
	if (withWindow && reply.FindMessage("window", &window) == B_OK)
		text = describe_window(window);
	BMessage node;
	if (reply.FindMessage("node", &node) == B_OK) {
		fFocusID = node.GetInt64(B_ACCESSIBLE_FIELD_ID, 0);
		if (!text.IsEmpty())
			text << ". ";
		text << describe_node(node);
	} else if (text.IsEmpty())
		text = "Nothing has the focus";
	_Say(text, true);
}


void
ScreenReader::_ReadAll()
{
	if (!fBridge.IsValid() || fWindowID == 0) {
		_Say("No window", true);
		return;
	}

	BMessage window;
	if (!_GetWindowTree(window)) {
		_Say("The window does not answer", true);
		return;
	}

	_Say(read_all_text(window), true);
}


bool
ScreenReader::_GetWindowTree(BMessage& window)
{
	BMessage request(B_ACCESSIBILITY_GET_NODE);
	request.AddInt64("id", fWindowID);
	request.AddInt32("depth", -1);
	BMessage reply;
	return fBridge.SendMessage(&request, &reply, kQueryTimeout, kQueryTimeout)
			== B_OK && reply.GetInt32("status", B_ERROR) == B_OK
		&& reply.FindMessage("node", &window) == B_OK;
}


/*!	Browse mode (#119): a virtual cursor through what read-all says, item
	by item, for web pages (where Tab only reaches what takes the focus) and
	any other window. The tree is fetched again when it is older than a
	second (pages change), keeping the place by the item's node.
*/
void
ScreenReader::_Browse(int32 direction, int32 kind)
{
	if (!fBridge.IsValid() || fWindowID == 0) {
		_Say("No window", true);
		return;
	}

	bigtime_t now = system_time();
	if (fBrowseWindowID != fWindowID || now - fBrowseTime > 1000000) {
		BMessage window;
		if (!_GetWindowTree(window)) {
			_Say("The window does not answer", true);
			return;
		}
		if (fBrowseWindowID != fWindowID) {
			fBrowseIndex = -1;
			fBrowseItemID = 0;
		}
		collect_browse_items(window, fBrowseItems);
		fBrowseWindowID = fWindowID;
		fBrowseTime = now;

		// find the place again
		if (fBrowseItemID != 0) {
			int32 index = -1;
			for (size_t i = 0; i < fBrowseItems.size(); i++) {
				if (fBrowseItems[i].id == fBrowseItemID) {
					index = (int32)i;
					break;
				}
			}
			if (index >= 0)
				fBrowseIndex = index;
			else if (fBrowseIndex >= (int32)fBrowseItems.size())
				fBrowseIndex = (int32)fBrowseItems.size() - 1;
		}
	}

	int32 from = fBrowseIndex;
	if (from < 0 && direction < 0)
		from = (int32)fBrowseItems.size();
	int32 index = find_browse_item(fBrowseItems, from, direction, kind);
	if (index < 0) {
		BString text;
		if (kind == BROWSE_ANY)
			text = direction > 0 ? "Bottom" : "Top";
		else {
			text << "No " << (direction > 0 ? "next " : "previous ")
				<< browse_kind_name(kind);
		}
		_Say(text, true);
		return;
	}

	fBrowseIndex = index;
	fBrowseItemID = fBrowseItems[index].id;
	_Say(fBrowseItems[index].text, true);
}


void
ScreenReader::_Activate()
{
	if (fBrowseWindowID != fWindowID || fBrowseIndex < 0
		|| fBrowseIndex >= (int32)fBrowseItems.size()
		|| fBrowseItems[fBrowseIndex].id == 0) {
		_Say("Nothing to activate", true);
		return;
	}

	// a field takes the focus (and is then said); anything else is pressed
	const browse_item& item = fBrowseItems[fBrowseIndex];
	int32 action = B_ACCESSIBLE_ACTION_PRESS;
	switch (item.role) {
		case B_ACCESSIBLE_ROLE_TEXT_FIELD:
		case B_ACCESSIBLE_ROLE_TEXT_AREA:
		case B_ACCESSIBLE_ROLE_SLIDER:
		case B_ACCESSIBLE_ROLE_SPIN_BUTTON:
		case B_ACCESSIBLE_ROLE_COMBO_BOX:
			action = B_ACCESSIBLE_ACTION_FOCUS;
			break;
	}

	BMessage request(B_ACCESSIBILITY_PERFORM_ACTION);
	request.AddInt64("id", item.id);
	request.AddInt32("action", action);
	BMessage reply;
	if (fBridge.SendMessage(&request, &reply, kQueryTimeout, kQueryTimeout)
			!= B_OK || reply.GetInt32("status", B_ERROR) != B_OK) {
		if (action == B_ACCESSIBLE_ACTION_PRESS) {
			// what cannot be pressed may still take the focus
			request.ReplaceInt32("action", B_ACCESSIBLE_ACTION_FOCUS);
			if (fBridge.SendMessage(&request, &reply, kQueryTimeout,
					kQueryTimeout) == B_OK
				&& reply.GetInt32("status", B_ERROR) == B_OK) {
				return;
			}
		}
		_Say("Cannot activate this", true);
	}
}


//	#pragma mark -


int
main()
{
	ScreenReader reader;
	reader.Run();
	return 0;
}
