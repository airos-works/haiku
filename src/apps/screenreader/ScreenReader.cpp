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

			BString				_Describe(const BMessage& node,
									bool withDescription = true) const;
			BString				_DescribeWindow(const BMessage& window) const;
			const char*			_SpokenRole(int32 role) const;
			BString				_SpokenText(const BString& text) const;

			void				_ReadFocus(bool withWindow);
			void				_ReadAll();
			void				_CollectText(const BMessage& node,
									BStringList& parts) const;

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
	fCaretLine(-1)
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


const char*
ScreenReader::_SpokenRole(int32 role) const
{
	switch (role) {
		case B_ACCESSIBLE_ROLE_UNKNOWN:
		case B_ACCESSIBLE_ROLE_GROUP:
		case B_ACCESSIBLE_ROLE_SCROLL_PANE:
		case B_ACCESSIBLE_ROLE_SPLIT_PANE:
		case B_ACCESSIBLE_ROLE_LABEL:
		case B_ACCESSIBLE_ROLE_PARAGRAPH:
		case B_ACCESSIBLE_ROLE_CANVAS:
			return NULL;
		case B_ACCESSIBLE_ROLE_POP_UP_BUTTON:
			return "pop-up menu";
		default:
			return BAccessible::RoleName((accessible_role)role);
	}
}


BString
ScreenReader::_SpokenText(const BString& text) const
{
	if (text == " ")
		return "space";
	if (text == "\n")
		return "new line";
	if (text == "\t")
		return "tab";
	BString trimmed(text);
	trimmed.Trim();
	if (trimmed.IsEmpty())
		return "blank";
	return text;
}


BString
ScreenReader::_Describe(const BMessage& node, bool withDescription) const
{
	int32 role = node.GetInt32(B_ACCESSIBLE_FIELD_ROLE, 0);
	BString name = node.GetString(B_ACCESSIBLE_FIELD_NAME, "");
	BString value = node.GetString(B_ACCESSIBLE_FIELD_VALUE, "");
	uint64 states = (uint64)node.GetInt64(B_ACCESSIBLE_FIELD_STATES, 0);

	BStringList parts;
	if (!name.IsEmpty())
		parts.Add(name);
	if (const char* roleName = _SpokenRole(role))
		parts.Add(roleName);

	bool textRole = role == B_ACCESSIBLE_ROLE_TEXT_FIELD
		|| role == B_ACCESSIBLE_ROLE_TEXT_AREA;
	if ((states & B_ACCESSIBLE_STATE_PROTECTED) != 0) {
		parts.Add("protected");
	} else if (!value.IsEmpty() && value != name) {
		if (textRole && value.Length() > 300) {
			value.Truncate(300);
			value << "…";
		}
		parts.Add(value);
	} else if (textRole && value.IsEmpty())
		parts.Add("blank");

	if ((states & B_ACCESSIBLE_STATE_CHECKABLE) != 0) {
		if ((states & B_ACCESSIBLE_STATE_CHECKED) != 0)
			parts.Add("checked");
		else if ((states & B_ACCESSIBLE_STATE_MIXED) != 0)
			parts.Add("partially checked");
		else
			parts.Add("not checked");
	} else if (role == B_ACCESSIBLE_ROLE_MENU_ITEM
		&& (states & B_ACCESSIBLE_STATE_CHECKED) != 0) {
		parts.Add("checked");
	}
	if (role == B_ACCESSIBLE_ROLE_TOGGLE_BUTTON)
		parts.Add((states & B_ACCESSIBLE_STATE_PRESSED) != 0
			? "pressed" : "not pressed");
	if ((states & B_ACCESSIBLE_STATE_EXPANDABLE) != 0)
		parts.Add((states & B_ACCESSIBLE_STATE_EXPANDED) != 0
			? "expanded" : "collapsed");
	if (role == B_ACCESSIBLE_ROLE_MENU_ITEM
		&& (states & B_ACCESSIBLE_STATE_HAS_POPUP) != 0) {
		parts.Add("submenu");
	}
	if ((states & B_ACCESSIBLE_STATE_DEFAULT) != 0)
		parts.Add("default");
	if ((states & B_ACCESSIBLE_STATE_DISABLED) != 0)
		parts.Add("unavailable");
	if (textRole && (states & B_ACCESSIBLE_STATE_READ_ONLY) != 0)
		parts.Add("read only");

	int32 index;
	if (node.FindInt32(B_ACCESSIBLE_FIELD_INDEX, &index) == B_OK) {
		int32 count = node.GetInt32(B_ACCESSIBLE_FIELD_COUNT,
			node.GetInt32(B_ACCESSIBLE_FIELD_CHILD_COUNT, 0));
		if (count > 0) {
			BString position;
			position << index + 1 << " of " << count;
			parts.Add(position);
		}
	}
	int32 level;
	if (node.FindInt32(B_ACCESSIBLE_FIELD_LEVEL, &level) == B_OK && level > 1) {
		BString text;
		text << "level " << level;
		parts.Add(text);
	}
	const char* shortcut;
	if (node.FindString(B_ACCESSIBLE_FIELD_SHORTCUT, &shortcut) == B_OK)
		parts.Add(shortcut);

	if (withDescription) {
		BString description = node.GetString(B_ACCESSIBLE_FIELD_DESCRIPTION,
			"");
		if (!description.IsEmpty() && description != name)
			parts.Add(description);
	}

	return parts.Join(", ");
}


BString
ScreenReader::_DescribeWindow(const BMessage& window) const
{
	int32 role = window.GetInt32(B_ACCESSIBLE_FIELD_ROLE,
		B_ACCESSIBLE_ROLE_WINDOW);
	BString text = window.GetString(B_ACCESSIBLE_FIELD_NAME, "");
	if (text.IsEmpty())
		text = "Untitled";
	text << ", " << BAccessible::RoleName((accessible_role)role);
	BString description = window.GetString(B_ACCESSIBLE_FIELD_DESCRIPTION, "");
	if (!description.IsEmpty())
		text << ", " << description;
	return text;
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

			BString text = _DescribeWindow(window);
			if (hasNode) {
				fFocusID = node.GetInt64(B_ACCESSIBLE_FIELD_ID, 0);
				fFocusTime = now;
				text << ". " << _Describe(node);
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
			_Say(_Describe(node), now - fWindowTime > kSettleTime);
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
				text = (states & B_ACCESSIBLE_STATE_DISABLED) != 0
					? "unavailable" : "available";
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
				_Say(_Describe(node, false), true);
			break;

		case B_ACCESSIBLE_EVENT_SELECTION_CHANGED:
		{
			BMessage container;
			if (!hasNode || event->FindMessage("container", &container) != B_OK
				|| !_IsFocus(event, container) || !windowActive) {
				break;
			}
			_Say(_Describe(node), true);
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
				text = _SpokenText(event->GetString("text", ""));
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
				_Say(_SpokenText(event->GetString("line_text", "")), true);
			} else {
				BString character = event->GetString("char", "");
				_Say(character.IsEmpty() ? BString("end")
					: _SpokenText(character), true);
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
		text = _DescribeWindow(window);
	BMessage node;
	if (reply.FindMessage("node", &node) == B_OK) {
		fFocusID = node.GetInt64(B_ACCESSIBLE_FIELD_ID, 0);
		if (!text.IsEmpty())
			text << ". ";
		text << _Describe(node);
	} else if (text.IsEmpty())
		text = "Nothing has the focus";
	_Say(text, true);
}


void
ScreenReader::_CollectText(const BMessage& node, BStringList& parts) const
{
	uint64 states = (uint64)node.GetInt64(B_ACCESSIBLE_FIELD_STATES, 0);
	if ((states & B_ACCESSIBLE_STATE_INVISIBLE) != 0)
		return;

	int32 role = node.GetInt32(B_ACCESSIBLE_FIELD_ROLE, 0);
	BString name = node.GetString(B_ACCESSIBLE_FIELD_NAME, "");
	BString value = node.GetString(B_ACCESSIBLE_FIELD_VALUE, "");

	switch (role) {
		case B_ACCESSIBLE_ROLE_SCROLL_BAR:
		case B_ACCESSIBLE_ROLE_SEPARATOR:
			return;

		case B_ACCESSIBLE_ROLE_LIST:
		case B_ACCESSIBLE_ROLE_TREE:
		case B_ACCESSIBLE_ROLE_TABLE:
		case B_ACCESSIBLE_ROLE_MENU_BAR:
		{
			BString text = _Describe(node, false);
			int32 count = node.GetInt32(B_ACCESSIBLE_FIELD_CHILD_COUNT, 0);
			text << ", " << count << (count == 1 ? " item" : " items");
			parts.Add(text);
			return;
		}

		case B_ACCESSIBLE_ROLE_UNKNOWN:
		case B_ACCESSIBLE_ROLE_GROUP:
		case B_ACCESSIBLE_ROLE_SCROLL_PANE:
		case B_ACCESSIBLE_ROLE_SPLIT_PANE:
		case B_ACCESSIBLE_ROLE_TAB_PANEL:
			if (!name.IsEmpty() || !value.IsEmpty())
				parts.Add(_Describe(node, false));
			break;

		default:
			parts.Add(_Describe(node, false));
			break;
	}

	BMessage child;
	for (int32 i = 0; node.FindMessage(B_ACCESSIBLE_FIELD_CHILD, i, &child)
			== B_OK; i++) {
		_CollectText(child, parts);
	}
}


void
ScreenReader::_ReadAll()
{
	if (!fBridge.IsValid() || fWindowID == 0) {
		_Say("No window", true);
		return;
	}

	BMessage request(B_ACCESSIBILITY_GET_NODE);
	request.AddInt64("id", fWindowID);
	request.AddInt32("depth", -1);
	BMessage reply;
	BMessage window;
	if (fBridge.SendMessage(&request, &reply, kQueryTimeout, kQueryTimeout)
			!= B_OK || reply.GetInt32("status", B_ERROR) != B_OK
		|| reply.FindMessage("node", &window) != B_OK) {
		_Say("The window does not answer", true);
		return;
	}

	BStringList parts;
	parts.Add(_DescribeWindow(window));
	BMessage child;
	for (int32 i = 0; window.FindMessage(B_ACCESSIBLE_FIELD_CHILD, i, &child)
			== B_OK; i++) {
		_CollectText(child, parts);
	}
	_Say(parts.Join(". "), true);
}


//	#pragma mark -


int
main()
{
	ScreenReader reader;
	reader.Run();
	return 0;
}
