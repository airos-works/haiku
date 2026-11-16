/*
 * mcp_server - the user interface of running applications for agents:
 * windows, control trees, actions and events, without screenshots.
 *
 * Built on the system accessibility interface (haiku#54,
 * docs/airos/ACCESSIBILITY.md): mcp_server subscribes to the
 * accessibility_server like a screen reader does, which turns on the
 * accessibility bridge of every application. Every Interface Kit view then
 * answers as a node (role, name, value, states, actions), so existing
 * applications need no changes. While no agent uses these tools for ten
 * minutes, mcp_server unsubscribes and the bridges stop again.
 *
 * Copyright 2026 air/OS contributors. MIT license.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>

#include <deque>
#include <map>
#include <string>
#include <vector>

#include <Application.h>
#include <Bitmap.h>
#include <Entry.h>
#include <InterfaceDefs.h>
#include <Looper.h>
#include <Message.h>
#include <Messenger.h>
#include <Roster.h>
#include <String.h>
#include <Window.h>

#include <AccessibilityProtocol.h>
#include <Accessible.h>
#include <WindowInfo.h>

#include "Mcp.h"
#include "Tools.h"
#include "Util.h"


bool gUIAccessEnabled = true;


namespace {

static const bigtime_t kQueryTimeout = 3000000;
static const bigtime_t kIdleUnsubscribe = 10 * 60 * 1000000LL;
static const size_t kMaxEvents = 1000;
static const int32 kMaxTreeNodes = 2000;


struct AppEntry {
	team_id		team;
	std::string	signature;
	std::string	name;
	BMessenger	bridge;
};

struct EventRecord {
	int64		seq;
	bigtime_t	when;
	team_id		team;
	std::string	app;
	std::string	text;
};


class UIClient : public BLooper {
public:
	UIClient() : BLooper("mcp ui client") {}
	virtual void MessageReceived(BMessage* message);
};


static sem_id sStateLock = create_sem(1, "mcp ui state");
static UIClient* sClient = NULL;
static bool sSubscribed = false;
static BMessenger sServer;
static std::map<team_id, AppEntry> sApps;
static std::deque<EventRecord> sEvents;
static int64 sNextSeq = 1;
static bigtime_t sLastUse = 0;
static bigtime_t sStartTime = system_time();


// #pragma mark - names


static const char*
EventName(int32 event)
{
	switch (event) {
		case B_ACCESSIBLE_EVENT_FOCUS_CHANGED: return "focus";
		case B_ACCESSIBLE_EVENT_WINDOW_ACTIVATED: return "window activated";
		case B_ACCESSIBLE_EVENT_WINDOW_DEACTIVATED: return "window deactivated";
		case B_ACCESSIBLE_EVENT_VALUE_CHANGED: return "value";
		case B_ACCESSIBLE_EVENT_NAME_CHANGED: return "name";
		case B_ACCESSIBLE_EVENT_STATE_CHANGED: return "state";
		case B_ACCESSIBLE_EVENT_SELECTION_CHANGED: return "selection";
		case B_ACCESSIBLE_EVENT_TEXT_INSERTED: return "text inserted";
		case B_ACCESSIBLE_EVENT_TEXT_DELETED: return "text deleted";
		case B_ACCESSIBLE_EVENT_CARET_MOVED: return "caret";
		case B_ACCESSIBLE_EVENT_CHILDREN_CHANGED: return "children";
		case B_ACCESSIBLE_EVENT_ANNOUNCEMENT: return "announcement";
		case B_ACCESSIBLE_EVENT_DOCUMENT_LOADED: return "document loaded";
		case B_ACCESSIBLE_EVENT_APP_REGISTERED: return "app registered";
		case B_ACCESSIBLE_EVENT_APP_QUIT: return "app quit";
	}
	return "event";
}


struct StateName {
	uint64		bit;
	const char*	name;
};

static const StateName kStateNames[] = {
	{ B_ACCESSIBLE_STATE_FOCUSED, "focused" },
	{ B_ACCESSIBLE_STATE_DISABLED, "disabled" },
	{ B_ACCESSIBLE_STATE_CHECKED, "checked" },
	{ B_ACCESSIBLE_STATE_MIXED, "mixed" },
	{ B_ACCESSIBLE_STATE_PRESSED, "pressed" },
	{ B_ACCESSIBLE_STATE_SELECTED, "selected" },
	{ B_ACCESSIBLE_STATE_EXPANDED, "expanded" },
	{ B_ACCESSIBLE_STATE_READ_ONLY, "read-only" },
	{ B_ACCESSIBLE_STATE_PROTECTED, "password" },
	{ B_ACCESSIBLE_STATE_ACTIVE, "active" },
	{ B_ACCESSIBLE_STATE_MODAL, "modal" },
	{ B_ACCESSIBLE_STATE_DEFAULT, "default" },
	{ B_ACCESSIBLE_STATE_INVISIBLE, "hidden" },
	{ B_ACCESSIBLE_STATE_OFFSCREEN, "minimized" },
	{ B_ACCESSIBLE_STATE_BUSY, "busy" },
	{ B_ACCESSIBLE_STATE_INVALID, "invalid" },
	{ B_ACCESSIBLE_STATE_REQUIRED, "required" },
};


struct ActionName {
	uint32		bit;
	const char*	name;
};

static const ActionName kActionNames[] = {
	{ B_ACCESSIBLE_ACTION_PRESS, "press" },
	{ B_ACCESSIBLE_ACTION_FOCUS, "focus" },
	{ B_ACCESSIBLE_ACTION_INCREMENT, "increment" },
	{ B_ACCESSIBLE_ACTION_DECREMENT, "decrement" },
	{ B_ACCESSIBLE_ACTION_SELECT, "select" },
	{ B_ACCESSIBLE_ACTION_EXPAND, "expand" },
	{ B_ACCESSIBLE_ACTION_COLLAPSE, "collapse" },
	{ B_ACCESSIBLE_ACTION_SHOW_MENU, "show_menu" },
	{ B_ACCESSIBLE_ACTION_SCROLL_TO, "scroll_to" },
};


static std::string
StatesText(uint64 states, bool includeExpandable)
{
	std::string text;
	for (size_t i = 0; i < sizeof(kStateNames) / sizeof(kStateNames[0]); i++) {
		if ((states & kStateNames[i].bit) == 0)
			continue;
		if (!text.empty())
			text += ", ";
		text += kStateNames[i].name;
	}
	if (includeExpandable && (states & B_ACCESSIBLE_STATE_EXPANDABLE) != 0
		&& (states & B_ACCESSIBLE_STATE_EXPANDED) == 0) {
		if (!text.empty())
			text += ", ";
		text += "collapsed";
	}
	return text;
}


static std::string
ActionsText(uint32 actions)
{
	std::string text;
	for (size_t i = 0; i < sizeof(kActionNames) / sizeof(kActionNames[0]); i++) {
		if ((actions & kActionNames[i].bit) == 0
			|| kActionNames[i].bit == B_ACCESSIBLE_ACTION_FOCUS)
			continue;
		if (!text.empty())
			text += ",";
		text += kActionNames[i].name;
	}
	return text;
}


static std::string
Lower(std::string text)
{
	for (size_t i = 0; i < text.size(); i++)
		text[i] = tolower((unsigned char)text[i]);
	return text;
}


//!	A label without case, trailing colon or ellipsis and mnemonic marks.
static std::string
PlainLabel(const std::string& label)
{
	std::string text = TrimString(label);
	if (EndsWith(text, "\xE2\x80\xA6"))
		text.resize(text.size() - 3);
	else if (EndsWith(text, "..."))
		text.resize(text.size() - 3);
	if (EndsWith(text, ":"))
		text.resize(text.size() - 1);
	std::string plain;
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] != '_')
			plain += text[i];
	}
	return Lower(TrimString(plain));
}


static std::string
OneLine(const char* text, size_t maxLength = 200)
{
	std::string line;
	for (const char* c = text; *c != '\0'; c++) {
		if (*c == '\n')
			line += "\\n";
		else if (*c == '\t')
			line += ' ';
		else
			line += *c;
		if (line.size() >= maxLength) {
			line += "…";
			break;
		}
	}
	return line;
}


static std::string
Quote(const char* text, size_t maxLength = 200)
{
	return "\"" + OneLine(text, maxLength) + "\"";
}


// #pragma mark - refs


static std::string
MakeRef(team_id team, int64 id)
{
	int32 object = (int32)(id >> 32);
	int32 sub = (int32)(id & 0xffffffff);
	if (sub == 0)
		return Format("%d:%d", (int)team, (int)object);
	return Format("%d:%d.%d", (int)team, (int)object, (int)sub);
}


static bool
ParseRef(const std::string& ref, team_id& team, int64& id)
{
	int teamValue, object, sub = 0;
	char extra;
	if (sscanf(ref.c_str(), "%d:%d.%d%c", &teamValue, &object, &sub, &extra) == 3
		|| (sscanf(ref.c_str(), "%d:%d%c", &teamValue, &object, &extra) == 2
			&& (sub = 0) == 0)) {
		if (teamValue <= 0 || object <= 0 || sub < 0)
			return false;
		team = teamValue;
		id = ((int64)object << 32) | (uint32)sub;
		return true;
	}
	return false;
}


// #pragma mark - subscription


static std::string
AppName(team_id team, const std::string& signature)
{
	app_info info;
	if (be_roster != NULL && be_roster->GetRunningAppInfo(team, &info) == B_OK)
		return info.ref.name;
	return signature;
}


//!	sStateLock must be held.
static void
ReadAppList(const BMessage& reply)
{
	sApps.clear();
	BMessage app;
	for (int32 i = 0; reply.FindMessage("app", i, &app) == B_OK; i++) {
		AppEntry entry;
		entry.team = app.GetInt32("team", -1);
		entry.signature = app.GetString("signature", "");
		app.FindMessenger("bridge", &entry.bridge);
		if (entry.team < 0 || !entry.bridge.IsValid())
			continue;
		entry.name = AppName(entry.team, entry.signature);
		sApps[entry.team] = entry;
	}
}


static void
AddEvent(team_id team, const std::string& app, const std::string& text)
{
	// sStateLock must be held
	EventRecord record;
	record.seq = sNextSeq++;
	record.when = system_time();
	record.team = team;
	record.app = app;
	record.text = text;
	sEvents.push_back(record);
	while (sEvents.size() > kMaxEvents)
		sEvents.pop_front();
}


void
UIClient::MessageReceived(BMessage* message)
{
	if (message->what != B_ACCESSIBILITY_EVENT) {
		BLooper::MessageReceived(message);
		return;
	}

	int32 event = message->GetInt32("event", 0);
	team_id team = message->GetInt32("team", -1);
	std::string signature = message->GetString("signature", "");

	std::string text = EventName(event);
	BMessage node;
	if (message->FindMessage("node", &node) == B_OK) {
		text += Format(" %s", node.GetString("role_name", "?"));
		const char* name = node.GetString("name", "");
		if (name[0] != '\0')
			text += " " + Quote(name, 80);
		const char* value;
		if (node.FindString("value", &value) == B_OK && value[0] != '\0'
			&& (node.GetInt64("states", 0) & B_ACCESSIBLE_STATE_PROTECTED) == 0)
			text += " = " + Quote(value, 80);
		text += " [" + MakeRef(team, node.GetInt64("id", 0)) + "]";
	}
	const char* eventText;
	if (message->FindString("text", &eventText) == B_OK
		&& !message->GetBool("hidden", false)
		&& event != B_ACCESSIBLE_EVENT_CARET_MOVED)
		text += " text " + Quote(eventText, 80);
	BMessage window;
	if (message->FindMessage("window", &window) == B_OK)
		text += " in window " + Quote(window.GetString("name", ""), 80);

	AutoLocker locker(sStateLock);
	std::string app;
	if (event == B_ACCESSIBLE_EVENT_APP_REGISTERED) {
		AppEntry entry;
		entry.team = team;
		entry.signature = signature;
		message->FindMessenger("bridge", &entry.bridge);
		entry.name = AppName(team, signature);
		if (entry.bridge.IsValid())
			sApps[team] = entry;
		app = entry.name;
	} else {
		std::map<team_id, AppEntry>::iterator found = sApps.find(team);
		app = found != sApps.end() ? found->second.name : signature;
		if (event == B_ACCESSIBLE_EVENT_APP_QUIT && found != sApps.end())
			sApps.erase(found);
	}
	// caret moves are too many to be useful to an agent
	if (event != B_ACCESSIBLE_EVENT_CARET_MOVED)
		AddEvent(team, app, text);
}


static void
Unsubscribe()
{
	// sStateLock must be held
	if (!sSubscribed)
		return;
	BMessage message(B_ACCESSIBILITY_UNSUBSCRIBE);
	message.AddMessenger("client", BMessenger(sClient));
	sServer.SendMessage(&message, (BHandler*)NULL, 1000000);
	sSubscribed = false;
	sServer = BMessenger();
	sApps.clear();
	syslog(LOG_INFO, "ui: unsubscribed from the accessibility_server");
}


static int32
IdleThread(void*)
{
	while (true) {
		snooze(30000000);
		AutoLocker locker(sStateLock);
		if (sSubscribed && system_time() - sLastUse > kIdleUnsubscribe)
			Unsubscribe();
	}
	return 0;
}


/*!	Makes sure the accessibility bridges run and the application list is
	current. Every UI tool calls this first.
*/
static bool
Connect(std::string& error)
{
	if (!gUIAccessEnabled) {
		error = "Agents may not use applications on this computer: turn on "
			"\"Let agents see and use applications\" in the MCP preferences.";
		return false;
	}
	const char* reason = NULL;
	if (!EnsureApplication(&reason)) {
		error = reason != NULL ? reason : "no connection to the desktop";
		return false;
	}

	AutoLocker locker(sStateLock);
	sLastUse = system_time();
	if (sClient == NULL) {
		sClient = new UIClient;
		sClient->Run();
		thread_id thread = spawn_thread(IdleThread, "mcp ui idle",
			B_LOW_PRIORITY, NULL);
		if (thread >= 0)
			resume_thread(thread);
	}

	bool fresh = false;
	if (!sSubscribed || !sServer.IsValid()) {
		// launch_daemon starts the on-demand accessibility_server
		BMessenger server(B_ACCESSIBILITY_SERVER_SIGNATURE);
		if (!server.IsValid()) {
			error = "the accessibility_server could not be started";
			return false;
		}
		BMessage message(B_ACCESSIBILITY_SUBSCRIBE);
		message.AddMessenger("client", BMessenger(sClient));
		BMessage reply;
		status_t status = server.SendMessage(&message, &reply, kQueryTimeout,
			kQueryTimeout);
		if (status == B_OK)
			status = reply.GetInt32("status", B_ERROR);
		if (status != B_OK) {
			error = "the accessibility_server did not answer: "
				+ StrError(status);
			return false;
		}
		sServer = server;
		sSubscribed = true;
		fresh = true;
		syslog(LOG_INFO, "ui: subscribed to the accessibility_server");
	}

	if (fresh) {
		// the applications register their bridges a moment after the
		// server enabled them
		release_sem(sStateLock);
		snooze(400000);
		acquire_sem(sStateLock);
	}

	BMessage request(B_ACCESSIBILITY_GET_APPS);
	BMessage reply;
	if (sServer.SendMessage(&request, &reply, kQueryTimeout, kQueryTimeout)
			== B_OK)
		ReadAppList(reply);
	return true;
}


static std::vector<AppEntry>
Apps()
{
	AutoLocker locker(sStateLock);
	std::vector<AppEntry> apps;
	for (std::map<team_id, AppEntry>::iterator i = sApps.begin();
			i != sApps.end(); i++)
		apps.push_back(i->second);
	return apps;
}


static bool
FindApp(team_id team, AppEntry& entry)
{
	AutoLocker locker(sStateLock);
	std::map<team_id, AppEntry>::iterator found = sApps.find(team);
	if (found == sApps.end())
		return false;
	entry = found->second;
	return true;
}


/*!	An application by team id, signature or name (exact, then the only one
	that contains it).
*/
static bool
ResolveApp(const std::string& spec, AppEntry& entry, std::string& error)
{
	std::vector<AppEntry> apps = Apps();
	char* end;
	long team = strtol(spec.c_str(), &end, 10);
	if (*end == '\0' && team > 0) {
		if (FindApp(team, entry))
			return true;
		error = Format("team %ld has no accessibility bridge (not a running "
			"application, or it does not answer)", team);
		return false;
	}
	std::string wanted = Lower(spec);
	for (size_t i = 0; i < apps.size(); i++) {
		if (Lower(apps[i].signature) == wanted || Lower(apps[i].name) == wanted) {
			entry = apps[i];
			return true;
		}
	}
	int matches = 0;
	std::string names;
	for (size_t i = 0; i < apps.size(); i++) {
		if (Lower(apps[i].name).find(wanted) != std::string::npos) {
			entry = apps[i];
			matches++;
			names += (names.empty() ? "" : ", ") + apps[i].name;
		}
	}
	if (matches == 1)
		return true;
	if (matches > 1)
		error = "\"" + spec + "\" matches several applications: " + names;
	else {
		names.clear();
		for (size_t i = 0; i < apps.size(); i++)
			names += (names.empty() ? "" : ", ") + apps[i].name;
		error = "no application \"" + spec + "\"; running: " + names;
	}
	return false;
}


static bool
HasPending(const BMessage& node)
{
	if (node.GetBool("drawn_pending", false))
		return true;
	BMessage child;
	for (int32 i = 0; node.FindMessage("child", i, &child) == B_OK; i++) {
		if (HasPending(child))
			return true;
	}
	return false;
}


/*!	Makes the windows of a team draw everything, also what other windows
	(or the screen saver) cover: app_server only asks for the visible
	parts, and the text of custom views is only known once they drew it.
*/
static void
DrawCoveredWindows(team_id team)
{
	int32 count = 0;
	int32* tokens = get_token_list(team, &count);
	for (int32 i = 0; tokens != NULL && i < count; i++) {
		client_window_info* info = get_window_info(tokens[i]);
		if (info == NULL)
			continue;
		BRect frame(0, 0, info->window_right - info->window_left,
			info->window_bottom - info->window_top);
		bool visible = info->show_hide_level <= 0 && !info->is_mini;
		int32 token = info->server_token;
		free(info);
		if (!visible || !frame.IsValid() || frame.Width() > 16384
			|| frame.Height() > 16384)
			continue;
		BBitmap bitmap(frame, B_RGB32);
		bool drawn = false;
		if (bitmap.InitCheck() == B_OK)
			BPrivate::read_window_bitmap(token, &bitmap, 500000, &drawn);
	}
	free(tokens);
}


static status_t
BridgeRequest(const AppEntry& app, BMessage& request, BMessage& reply,
	bool agent = false)
{
	if (agent)
		request.AddBool("agent", true);
	status_t status = app.bridge.SendMessage(&request, &reply, kQueryTimeout,
		kQueryTimeout);
	if (status == B_OK)
		status = reply.GetInt32("status", B_ERROR);

	// views that had not drawn since the bridge started were asked to;
	// what they draw is there a moment later
	BMessage node;
	if (status == B_OK && !agent && reply.FindMessage("node", &node) == B_OK
		&& HasPending(node)) {
		DrawCoveredWindows(app.team);
		snooze(150000);
		BMessage again;
		if (app.bridge.SendMessage(&request, &again, kQueryTimeout,
				kQueryTimeout) == B_OK
			&& again.GetInt32("status", B_ERROR) == B_OK)
			reply = again;
	}
	return status;
}


static std::string
BridgeError(status_t status)
{
	switch (status) {
		case B_BUSY:
			return "the window is busy (it did not answer within 250 ms)";
		case B_ENTRY_NOT_FOUND:
			return "no such node any more (the window or item went away; "
				"read the tree again)";
		case B_NOT_ALLOWED:
			return "not allowed (the control is disabled or read-only)";
		case B_NOT_SUPPORTED:
			return "this node does not support that action";
		case B_NAME_IN_USE:
			return "several items have that label";
		case B_BAD_VALUE:
			return "bad value for this control";
		case B_TIMED_OUT:
		case B_WOULD_BLOCK:
			return "the application did not answer";
	}
	return StrError(status);
}


// #pragma mark - formatting nodes


struct TreeOptions {
	bool		hidden;
	bool		frames;
	int32		maxNodes;
	int32		menuDepth;
	bool		allChildren;
	int32		nodes;
	bool		truncated;

	TreeOptions() : hidden(false), frames(false), maxNodes(400), menuDepth(1),
		allChildren(false), nodes(0), truncated(false) {}
};


static bool
IsStructural(const BMessage& node)
{
	int32 role = node.GetInt32("role", 0);
	if (role != B_ACCESSIBLE_ROLE_GROUP && role != B_ACCESSIBLE_ROLE_SCROLL_PANE
		&& role != B_ACCESSIBLE_ROLE_SPLIT_PANE
		&& role != B_ACCESSIBLE_ROLE_UNKNOWN)
		return false;
	return node.GetString("name", "")[0] == '\0'
		&& !node.HasString(B_ACCESSIBLE_FIELD_VALUE)
		&& !node.HasString(B_ACCESSIBLE_FIELD_DRAWN_TEXT)
		&& !node.HasString(B_ACCESSIBLE_FIELD_CLASS)
		&& !node.HasString(B_ACCESSIBLE_FIELD_DESCRIPTION)
		&& (node.GetInt32("actions", 0) & ~B_ACCESSIBLE_ACTION_FOCUS) == 0;
}


static std::string
NodeLine(const BMessage& node, team_id team, bool frames)
{
	std::string line = node.GetString("role_name", "?");
	const char* name = node.GetString("name", "");
	if (name[0] != '\0')
		line += " " + Quote(name, 120);
	line += " [" + MakeRef(team, node.GetInt64("id", 0)) + "]";

	uint64 states = node.GetInt64("states", 0);
	const char* value;
	if ((states & B_ACCESSIBLE_STATE_PROTECTED) != 0)
		line += " = (hidden)";
	else if (node.FindString("value", &value) == B_OK && value[0] != '\0')
		line += " = " + Quote(value);

	double current, minimum, maximum;
	if (node.FindDouble("minimum", &minimum) == B_OK
		&& node.FindDouble("maximum", &maximum) == B_OK
		&& node.FindDouble("current", &current) == B_OK
		&& node.GetInt32("role", 0) != B_ACCESSIBLE_ROLE_SCROLL_BAR) {
		line += Format(" range %g..%g", minimum, maximum);
	}

	std::string statesText = StatesText(states, true);
	if (!statesText.empty())
		line += " (" + statesText + ")";
	std::string actions = ActionsText(node.GetInt32("actions", 0));
	if (!actions.empty())
		line += " {" + actions + "}";

	const char* shortcut;
	if (node.FindString("shortcut", &shortcut) == B_OK)
		line += Format(" %s", shortcut);
	const char* className;
	if (node.FindString(B_ACCESSIBLE_FIELD_CLASS, &className) == B_OK)
		line += Format(" class %s", className);
	const char* description;
	if (node.FindString("description", &description) == B_OK
		&& description[0] != '\0')
		line += " - " + Quote(description, 120);
	const char* confirm;
	if (node.FindString(B_ACCESSIBLE_FIELD_CONFIRM, &confirm) == B_OK)
		line += " NEEDS CONFIRMATION: " + Quote(confirm, 160);
	BRect frame;
	if (frames && node.FindRect("frame", &frame) == B_OK) {
		line += Format(" @%d,%d %dx%d", (int)frame.left, (int)frame.top,
			(int)frame.IntegerWidth() + 1, (int)frame.IntegerHeight() + 1);
	}
	return line;
}


static void
FormatTree(const BMessage& node, team_id team, int indent,
	TreeOptions& options, std::string& out, int32 menuLevel = 0)
{
	if (options.nodes >= options.maxNodes) {
		options.truncated = true;
		return;
	}
	uint64 states = node.GetInt64("states", 0);
	if ((states & B_ACCESSIBLE_STATE_INVISIBLE) != 0 && !options.hidden
		&& node.GetInt32("role", 0) != B_ACCESSIBLE_ROLE_APPLICATION)
		return;
	if (node.GetInt32("role", 0) == B_ACCESSIBLE_ROLE_SEPARATOR)
		return;

	int childIndent = indent;
	if (!IsStructural(node)) {
		out += std::string(indent * 2, ' ') + "- "
			+ NodeLine(node, team, options.frames) + "\n";
		options.nodes++;
		childIndent = indent + 1;

		const char* drawn;
		if (node.FindString(B_ACCESSIBLE_FIELD_DRAWN_TEXT, &drawn) == B_OK) {
			std::vector<std::string> lines = SplitLines(drawn);
			size_t shown = 0;
			for (size_t i = 0; i < lines.size() && shown < 12; i++) {
				if (TrimString(lines[i]).empty())
					continue;
				out += std::string(childIndent * 2, ' ') + "| "
					+ OneLine(lines[i].c_str(), 160) + "\n";
				shown++;
			}
			if (lines.size() > shown + 0 && lines.size() > 12) {
				out += std::string(childIndent * 2, ' ')
					+ Format("| … %d more lines (ui_text)\n",
						(int)(lines.size() - shown));
			}
		}
	}

	// closed menus only so deep: font menus have hundreds of items
	if (node.GetInt32("role", 0) == B_ACCESSIBLE_ROLE_MENU_ITEM) {
		if (menuLevel >= options.menuDepth) {
			int32 count = node.GetInt32("child_count", 0);
			if (count > 0) {
				out += std::string(childIndent * 2, ' ')
					+ Format("- … %d item%s (ui_tree ref=%s)\n", (int)count,
						count == 1 ? "" : "s",
						MakeRef(team, node.GetInt64("id", 0)).c_str());
			}
			return;
		}
		menuLevel++;
	}

	// long menus and lists: the first ones, and where to read the rest
	int32 count = node.GetInt32("child_count", 0);
	int32 shownLimit = menuLevel > 0 ? 20 : 60;
	if (count <= shownLimit + 5 || (options.allChildren && indent <= 1))
		shownLimit = count;
	int32 described = 0;
	BMessage child;
	for (int32 i = 0; node.FindMessage("child", i, &child) == B_OK; i++) {
		if (described >= shownLimit) {
			out += std::string(childIndent * 2, ' ')
				+ Format("- … %d more (ui_tree ref=%s, or ui_find)\n",
					(int)(count - described),
					MakeRef(team, node.GetInt64("id", 0)).c_str());
			return;
		}
		described++;
		FormatTree(child, team, childIndent, options, out, menuLevel);
	}
	if (count > described) {
		out += std::string(childIndent * 2, ' ')
			+ Format("- … %d children not read (ui_tree ref=%s)\n",
				(int)(count - described),
				MakeRef(team, node.GetInt64("id", 0)).c_str());
	}
}


static JsonValue
NodeToJson(const BMessage& node, team_id team, TreeOptions& options)
{
	JsonValue json = JsonValue::Object();
	options.nodes++;
	json.Set("ref", MakeRef(team, node.GetInt64("id", 0)));
	json.Set("role", node.GetString("role_name", "?"));
	json.Set("name", node.GetString("name", ""));
	uint64 states = node.GetInt64("states", 0);
	const char* text;
	if ((states & B_ACCESSIBLE_STATE_PROTECTED) == 0
		&& node.FindString("value", &text) == B_OK)
		json.Set("value", text);
	JsonValue stateList = JsonValue::Array();
	for (size_t i = 0; i < sizeof(kStateNames) / sizeof(kStateNames[0]); i++) {
		if ((states & kStateNames[i].bit) != 0)
			stateList.Push(kStateNames[i].name);
	}
	if ((states & B_ACCESSIBLE_STATE_EXPANDABLE) != 0)
		stateList.Push("expandable");
	if ((states & B_ACCESSIBLE_STATE_EDITABLE) != 0)
		stateList.Push("editable");
	json.Set("states", stateList);
	JsonValue actionList = JsonValue::Array();
	uint32 actions = node.GetInt32("actions", 0);
	for (size_t i = 0; i < sizeof(kActionNames) / sizeof(kActionNames[0]); i++) {
		if ((actions & kActionNames[i].bit) != 0)
			actionList.Push(kActionNames[i].name);
	}
	json.Set("actions", actionList);
	const char* fields[] = { "description", "shortcut",
		B_ACCESSIBLE_FIELD_DRAWN_TEXT, B_ACCESSIBLE_FIELD_CLASS,
		B_ACCESSIBLE_FIELD_CONFIRM };
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		if (node.FindString(fields[i], &text) == B_OK)
			json.Set(fields[i], text);
	}
	double number;
	const char* numbers[] = { "current", "minimum", "maximum" };
	for (size_t i = 0; i < 3; i++) {
		if (node.FindDouble(numbers[i], &number) == B_OK)
			json.Set(numbers[i], number);
	}
	BRect frame;
	if (node.FindRect("frame", &frame) == B_OK) {
		JsonValue rect = JsonValue::Object();
		rect.Set("x", (double)frame.left);
		rect.Set("y", (double)frame.top);
		rect.Set("width", (double)frame.Width() + 1);
		rect.Set("height", (double)frame.Height() + 1);
		json.Set("frame", rect);
	}
	JsonValue children = JsonValue::Array();
	BMessage child;
	for (int32 i = 0; node.FindMessage("child", i, &child) == B_OK; i++) {
		if (options.nodes >= options.maxNodes) {
			options.truncated = true;
			break;
		}
		if ((child.GetInt64("states", 0) & B_ACCESSIBLE_STATE_INVISIBLE) != 0
			&& !options.hidden)
			continue;
		children.Push(NodeToJson(child, team, options));
	}
	if (children.Size() > 0)
		json.Set("children", children);
	json.Set("child_count", (int64_t)node.GetInt32("child_count", 0));
	return json;
}


// #pragma mark - searching


struct Match {
	team_id		team;
	std::string	app;
	BMessage	node;
	std::string	path;
	std::string	window;
};


struct Query {
	std::string	text;		// in name, value, description or drawn text
	std::string	name;		// the plain name equals this
	std::string	role;
	std::string	window;		// window title contains this
	bool		hidden;

	Query() : hidden(false) {}
};


static bool
Contains(const char* haystack, const std::string& lowerNeedle)
{
	return Lower(haystack).find(lowerNeedle) != std::string::npos;
}


static bool
Matches(const BMessage& node, const Query& query)
{
	if (!query.role.empty()) {
		std::string role = Lower(node.GetString("role_name", ""));
		if (role != query.role && role.find(query.role) == std::string::npos)
			return false;
	}
	if (!query.name.empty()
		&& PlainLabel(node.GetString("name", "")) != query.name)
		return false;
	if (!query.text.empty()) {
		bool protectedField = (node.GetInt64("states", 0)
			& B_ACCESSIBLE_STATE_PROTECTED) != 0;
		if (!Contains(node.GetString("name", ""), query.text)
			&& (protectedField
				|| !Contains(node.GetString("value", ""), query.text))
			&& !Contains(node.GetString("description", ""), query.text)
			&& !Contains(node.GetString(B_ACCESSIBLE_FIELD_DRAWN_TEXT, ""),
				query.text))
			return false;
	}
	return !query.role.empty() || !query.name.empty() || !query.text.empty();
}


static void
Search(const BMessage& node, const AppEntry& app, const Query& query,
	const std::string& path, const std::string& window,
	std::vector<Match>& matches, size_t limit)
{
	if (matches.size() >= limit)
		return;
	uint64 states = node.GetInt64("states", 0);
	if ((states & B_ACCESSIBLE_STATE_INVISIBLE) != 0 && !query.hidden)
		return;

	int32 role = node.GetInt32("role", 0);
	std::string currentWindow = window;
	if (role == B_ACCESSIBLE_ROLE_WINDOW || role == B_ACCESSIBLE_ROLE_DIALOG
		|| role == B_ACCESSIBLE_ROLE_ALERT) {
		currentWindow = node.GetString("name", "");
		if (!query.window.empty()
			&& Lower(currentWindow).find(query.window) == std::string::npos)
			return;
	}

	if (role != B_ACCESSIBLE_ROLE_APPLICATION && Matches(node, query)) {
		Match match;
		match.team = app.team;
		match.app = app.name;
		match.node = node;
		match.node.RemoveName("child");
		match.path = path;
		match.window = currentWindow;
		matches.push_back(match);
	}

	std::string childPath = path;
	const char* name = node.GetString("name", "");
	if (name[0] != '\0' && role != B_ACCESSIBLE_ROLE_APPLICATION) {
		childPath += (childPath.empty() ? "" : " > ")
			+ std::string(node.GetString("role_name", "")) + " "
			+ Quote(name, 60);
	}
	BMessage child;
	for (int32 i = 0; node.FindMessage("child", i, &child) == B_OK; i++)
		Search(child, app, query, childPath, currentWindow, matches, limit);
}


static void
SearchApps(const std::vector<AppEntry>& apps, const Query& query,
	std::vector<Match>& matches, size_t limit, std::string& notes)
{
	for (size_t i = 0; i < apps.size() && matches.size() < limit; i++) {
		BMessage request(B_ACCESSIBILITY_GET_ROOT);
		request.AddInt32("depth", -1);
		BMessage reply;
		status_t status = BridgeRequest(apps[i], request, reply);
		BMessage root;
		if (status != B_OK || reply.FindMessage("node", &root) != B_OK) {
			notes += Format("%s (team %d): %s\n", apps[i].name.c_str(),
				(int)apps[i].team, BridgeError(status).c_str());
			continue;
		}
		Search(root, apps[i], query, "", "", matches, limit);
	}
}


static std::string
MatchLine(const Match& match)
{
	std::string line = match.app + ": " + NodeLine(match.node, match.team,
		false);
	if (!match.window.empty()
		&& match.node.GetInt32("role", 0) != B_ACCESSIBLE_ROLE_WINDOW)
		line += " in window " + Quote(match.window.c_str(), 60);
	if (!match.path.empty())
		line += "\n    path: " + match.path;
	return line;
}


//!	The apps a tool works on: one ("app") or all.
static bool
SelectApps(const JsonValue& arguments, std::vector<AppEntry>& apps,
	std::string& error)
{
	std::string spec = arguments.GetString("app");
	if (spec.empty()) {
		apps = Apps();
		return true;
	}
	AppEntry entry;
	if (!ResolveApp(spec, entry, error))
		return false;
	apps.push_back(entry);
	return true;
}


// #pragma mark - tools


static ToolResult
UIWindows(const JsonValue& arguments)
{
	std::string error;
	if (!Connect(error))
		return ToolResult::Error(error);
	bool all = arguments.GetBool("all");
	std::string filter = Lower(arguments.GetString("app"));

	// front to back on the current workspace
	std::map<int32, int32> order;
	int32* tokens = NULL;
	int32 tokenCount = 0;
	if (BPrivate::get_window_order(current_workspace(), &tokens, &tokenCount)
			== B_OK) {
		for (int32 i = 0; i < tokenCount; i++)
			order[tokens[i]] = i + 1;
		free(tokens);
	}

	BList teams;
	be_roster->GetAppList(&teams);
	std::string out;
	int32 windowCount = 0;
	for (int32 i = 0; i < teams.CountItems(); i++) {
		team_id team = (team_id)(addr_t)teams.ItemAt(i);
		app_info info;
		if (be_roster->GetRunningAppInfo(team, &info) != B_OK)
			continue;
		if (!filter.empty() && Lower(info.ref.name).find(filter)
				== std::string::npos
			&& Lower(info.signature).find(filter) == std::string::npos
			&& Format("%d", (int)team) != filter)
			continue;

		int32 count = 0;
		int32* windowTokens = get_token_list(team, &count);
		if (windowTokens == NULL || count == 0) {
			free(windowTokens);
			continue;
		}

		// the accessibility nodes of the windows, matched by title
		std::vector<BMessage> nodes;
		AppEntry app;
		bool bridged = FindApp(team, app);
		if (bridged) {
			BMessage request(B_ACCESSIBILITY_GET_ROOT);
			request.AddInt32("depth", 1);
			BMessage reply, root, child;
			if (BridgeRequest(app, request, reply) == B_OK
				&& reply.FindMessage("node", &root) == B_OK) {
				for (int32 j = 0; root.FindMessage("child", j, &child) == B_OK;
						j++)
					nodes.push_back(child);
			}
		}

		std::string lines;
		for (int32 j = 0; j < count; j++) {
			client_window_info* window = get_window_info(windowTokens[j]);
			if (window == NULL)
				continue;
			bool hidden = window->show_hide_level > 0 && !window->is_mini;
			if (hidden && !all) {
				free(window);
				continue;
			}
			std::string title = window->name;
			std::string line = "  window " + Quote(title.c_str(), 120);
			for (size_t k = 0; k < nodes.size(); k++) {
				if (title == nodes[k].GetString("name", "")) {
					line += " [" + MakeRef(team, nodes[k].GetInt64("id", 0))
						+ "]";
					uint64 states = nodes[k].GetInt64("states", 0);
					if ((states & B_ACCESSIBLE_STATE_ACTIVE) != 0)
						line += " active";
					if ((states & B_ACCESSIBLE_STATE_MODAL) != 0)
						line += " modal";
					std::string role = nodes[k].GetString("role_name", "");
					if (role != "window")
						line += " " + role;
					nodes.erase(nodes.begin() + k);
					break;
				}
			}
			if (window->is_mini)
				line += " minimized";
			if (hidden)
				line += " hidden";
			std::map<int32, int32>::iterator front
				= order.find(window->server_token);
			if (front != order.end())
				line += Format(" z%d", (int)front->second);
			line += Format(" %dx%d at %d,%d",
				(int)(window->window_right - window->window_left + 1),
				(int)(window->window_bottom - window->window_top + 1),
				(int)window->window_left, (int)window->window_top);
			if (window->workspaces != B_ALL_WORKSPACES) {
				std::string spaces;
				for (int32 w = 0; w < 32; w++) {
					if ((window->workspaces & (1u << w)) != 0)
						spaces += Format("%s%d", spaces.empty() ? "" : ",",
							(int)w + 1);
				}
				line += " workspace " + spaces;
			} else
				line += " all workspaces";
			lines += line + "\n";
			windowCount++;
			free(window);
		}
		free(windowTokens);
		if (lines.empty())
			continue;
		out += Format("%s (team %d, %s)%s\n", info.ref.name, (int)team,
			info.signature, bridged ? "" : " - no accessibility bridge");
		out += lines;
	}
	if (out.empty())
		return ToolResult::Text("No windows.");
	out += Format("\n%d windows on workspace %d. Read one with ui_tree "
		"ref=<window ref>; z1 is the front window.", (int)windowCount,
		(int)current_workspace() + 1);
	return ToolResult::Text(out);
}


static ToolResult
UITree(const JsonValue& arguments)
{
	std::string error;
	if (!Connect(error))
		return ToolResult::Error(error);

	TreeOptions options;
	options.hidden = arguments.GetBool("hidden");
	options.frames = arguments.GetBool("frames");
	options.maxNodes = (int32)arguments.GetInt("max_nodes", 400);
	if (options.maxNodes < 1 || options.maxNodes > kMaxTreeNodes)
		options.maxNodes = kMaxTreeNodes;
	int32 depth = (int32)arguments.GetInt("depth", -1);
	options.menuDepth = (int32)arguments.GetInt("menu_depth",
		arguments.Has("ref") ? 8 : 1);
	options.allChildren = arguments.Has("ref");

	AppEntry app;
	BMessage request(B_ACCESSIBILITY_GET_ROOT);
	std::string ref = arguments.GetString("ref");
	if (!ref.empty()) {
		team_id team;
		int64 id;
		if (!ParseRef(ref, team, id))
			return ToolResult::Error("bad ref \"" + ref + "\" (team:object or "
				"team:object.item, as ui_windows and ui_tree print them)");
		if (!FindApp(team, app))
			return ToolResult::Error(Format("team %d has no accessibility "
				"bridge (it quit?)", (int)team));
		request.what = B_ACCESSIBILITY_GET_NODE;
		request.AddInt64("id", id);
	} else {
		std::string spec = arguments.GetString("app");
		if (spec.empty())
			return ToolResult::Error("give app (name, signature or team) or "
				"ref (from ui_windows)");
		if (!ResolveApp(spec, app, error))
			return ToolResult::Error(error);
		std::string window = Lower(arguments.GetString("window"));
		if (!window.empty()) {
			// find the window by title, then read only its tree
			BMessage rootRequest(B_ACCESSIBILITY_GET_ROOT);
			rootRequest.AddInt32("depth", 1);
			BMessage reply, root, child;
			status_t status = BridgeRequest(app, rootRequest, reply);
			if (status != B_OK || reply.FindMessage("node", &root) != B_OK)
				return ToolResult::Error(BridgeError(status));
			int64 found = 0;
			std::string titles;
			for (int32 i = 0; root.FindMessage("child", i, &child) == B_OK;
					i++) {
				std::string title = child.GetString("name", "");
				titles += (titles.empty() ? "" : ", ") + Quote(title.c_str());
				if (Lower(title) == window
					|| (found == 0
						&& Lower(title).find(window) != std::string::npos))
					found = child.GetInt64("id", 0);
			}
			if (found == 0)
				return ToolResult::Error("no window \"" + window + "\" in "
					+ app.name + "; its windows: " + titles);
			request.what = B_ACCESSIBILITY_GET_NODE;
			request.AddInt64("id", found);
		}
	}
	request.AddInt32("depth", depth);

	BMessage reply, node;
	status_t status = BridgeRequest(app, request, reply);
	if (status != B_OK || reply.FindMessage("node", &node) != B_OK)
		return ToolResult::Error(app.name + ": " + BridgeError(status));

	if (arguments.GetString("format") == "json") {
		JsonValue json = NodeToJson(node, app.team, options);
		JsonValue result = JsonValue::Object();
		result.Set("app", app.name);
		result.Set("team", (int64_t)app.team);
		result.Set("tree", json);
		result.Set("truncated", options.truncated);
		return ToolResult::Json(result);
	}

	std::string out = Format("%s (team %d)\n", app.name.c_str(), (int)app.team);
	FormatTree(node, app.team, 0, options, out);
	if (options.truncated) {
		out += Format("… stopped at %d nodes: read a part with ui_tree "
			"ref=<ref>, or raise max_nodes\n", (int)options.maxNodes);
	}
	return ToolResult::Text(out);
}


static ToolResult
UIFind(const JsonValue& arguments)
{
	std::string error;
	if (!Connect(error))
		return ToolResult::Error(error);
	std::vector<AppEntry> apps;
	if (!SelectApps(arguments, apps, error))
		return ToolResult::Error(error);

	Query query;
	query.text = Lower(arguments.GetString("text"));
	query.name = PlainLabel(arguments.GetString("name"));
	query.role = Lower(arguments.GetString("role"));
	query.window = Lower(arguments.GetString("window"));
	query.hidden = arguments.GetBool("hidden");
	if (query.text.empty() && query.name.empty() && query.role.empty())
		return ToolResult::Error("give text, name or role");
	size_t limit = (size_t)arguments.GetInt("limit", 40);
	if (limit < 1 || limit > 500)
		limit = 40;

	std::vector<Match> matches;
	std::string notes;
	SearchApps(apps, query, matches, limit, notes);
	std::string out;
	for (size_t i = 0; i < matches.size(); i++)
		out += MatchLine(matches[i]) + "\n";
	if (matches.empty())
		out = "Nothing found.\n";
	else if (matches.size() >= limit)
		out += Format("(first %d matches; narrow with app, window or role)\n",
			(int)limit);
	if (!notes.empty())
		out += "Not searched:\n" + notes;
	return ToolResult::Text(out);
}


struct ResolvedNode {
	AppEntry	app;
	int64		id;
	BMessage	node;
};


/*!	Finds the node a step names: "ref", or "name" (with "role", "app",
	"window" to tell equal names apart).
*/
static bool
ResolveNode(const JsonValue& step, ResolvedNode& resolved, std::string& error)
{
	std::string ref = step.GetString("ref");
	if (!ref.empty()) {
		team_id team;
		if (!ParseRef(ref, team, resolved.id)) {
			error = "bad ref \"" + ref + "\"";
			return false;
		}
		if (!FindApp(team, resolved.app)) {
			error = Format("team %d has no accessibility bridge (it quit?)",
				(int)team);
			return false;
		}
		BMessage request(B_ACCESSIBILITY_GET_NODE), reply;
		request.AddInt64("id", resolved.id);
		request.AddInt32("depth", 0);
		status_t status = BridgeRequest(resolved.app, request, reply);
		if (status != B_OK || reply.FindMessage("node", &resolved.node) != B_OK) {
			error = ref + ": " + BridgeError(status);
			return false;
		}
		return true;
	}

	std::string name = step.GetString("name");
	std::string role = Lower(step.GetString("role"));
	if (name.empty() && role.empty()) {
		error = "a step needs ref, name or role";
		return false;
	}
	std::vector<AppEntry> apps;
	if (!SelectApps(step, apps, error))
		return false;
	Query query;
	query.name = PlainLabel(name);
	query.role = role;
	query.window = Lower(step.GetString("window"));
	std::vector<Match> matches;
	std::string notes;
	SearchApps(apps, query, matches, 20, notes);
	if (matches.empty() && !name.empty()) {
		// a label that only contains the words, if just one does
		query.name.clear();
		query.text = PlainLabel(name);
		SearchApps(apps, query, matches, 20, notes);
		std::vector<Match> named;
		for (size_t i = 0; i < matches.size(); i++) {
			if (PlainLabel(matches[i].node.GetString("name", "")).find(
					query.text) != std::string::npos)
				named.push_back(matches[i]);
		}
		// an unnamed custom view: by what it drew, if only one drew it
		if (named.empty() && matches.size() == 1)
			named = matches;
		matches = named;
	}
	if (matches.empty()) {
		// or by its class
		query.text.clear();
		query.role = "unknown";
		std::vector<Match> custom;
		SearchApps(apps, query, custom, 200, notes);
		for (size_t i = 0; i < custom.size(); i++) {
			if (Lower(custom[i].node.GetString(B_ACCESSIBLE_FIELD_CLASS, ""))
					== Lower(name))
				matches.push_back(custom[i]);
		}
	}
	if (matches.empty()) {
		error = (name.empty() ? "no " + role
				: "nothing named \"" + name + "\"")
			+ (query.role.empty() || name.empty() ? ""
				: " with role " + query.role)
			+ "; read the window with ui_tree or search with ui_find";
		return false;
	}
	if (matches.size() > 1) {
		// a window's own controls before its title or other windows'
		error = "\"" + (name.empty() ? role : name)
			+ "\" is ambiguous, use one of these refs:\n";
		for (size_t i = 0; i < matches.size() && i < 10; i++)
			error += "  " + MatchLine(matches[i]) + "\n";
		return false;
	}
	FindApp(matches[0].team, resolved.app);
	resolved.id = matches[0].node.GetInt64("id", 0);
	resolved.node = matches[0].node;
	return true;
}


static bool
ParseModifiers(const JsonValue& value, uint32& modifiers, std::string& error)
{
	modifiers = 0;
	std::vector<std::string> names;
	if (value.IsArray()) {
		for (size_t i = 0; i < value.Size(); i++)
			names.push_back(Lower(value.At(i).AsString()));
	} else if (value.IsString()) {
		std::string text = Lower(value.AsString());
		size_t start = 0;
		while (start <= text.size()) {
			size_t end = text.find_first_of("+, ", start);
			if (end == std::string::npos)
				end = text.size();
			if (end > start)
				names.push_back(text.substr(start, end - start));
			start = end + 1;
		}
	}
	for (size_t i = 0; i < names.size(); i++) {
		const std::string& name = names[i];
		if (name == "command" || name == "cmd" || name == "alt")
			modifiers |= B_COMMAND_KEY | B_LEFT_COMMAND_KEY;
		else if (name == "control" || name == "ctrl")
			modifiers |= B_CONTROL_KEY | B_LEFT_CONTROL_KEY;
		else if (name == "shift")
			modifiers |= B_SHIFT_KEY | B_LEFT_SHIFT_KEY;
		else if (name == "option" || name == "win" || name == "super")
			modifiers |= B_OPTION_KEY | B_LEFT_OPTION_KEY;
		else if (!name.empty()) {
			error = "unknown modifier \"" + name + "\" (command/alt, control, "
				"shift, option)";
			return false;
		}
	}
	return true;
}


static accessible_action
ActionBit(const std::string& action)
{
	for (size_t i = 0; i < sizeof(kActionNames) / sizeof(kActionNames[0]); i++) {
		if (action == kActionNames[i].name)
			return (accessible_action)kActionNames[i].bit;
	}
	if (action == "activate")
		return B_ACCESSIBLE_ACTION_FOCUS;
	return (accessible_action)0;
}


static std::string
RunStep(const JsonValue& step, bool& ok)
{
	ok = false;
	std::string action = Lower(step.GetString("action"));
	if (action.empty())
		return "a step needs an action";

	ResolvedNode target;
	std::string error;
	if (!ResolveNode(step, target, error))
		return error;
	std::string ref = MakeRef(target.app.team, target.id);
	std::string what = std::string(target.node.GetString("role_name", "?"))
		+ " " + Quote(target.node.GetString("name", ""), 80) + " [" + ref + "]";

	const char* confirm;
	if (target.node.FindString(B_ACCESSIBLE_FIELD_CONFIRM, &confirm) == B_OK
		&& !step.GetBool("confirmed")) {
		return what + " NEEDS CONFIRMATION: " + Quote(confirm, 200)
			+ ". Ask the person; repeat the step with confirmed=true only "
			"after they agree.";
	}

	BMessage request;
	std::string logValue;
	if (action == "set_value") {
		const JsonValue* value = step.Get("value");
		if (value == NULL)
			return "set_value needs value";
		std::string text = value->IsString() ? value->AsString()
			: value->IsBool() ? (value->AsBool() ? "on" : "off")
			: value->Dump();
		request.what = B_ACCESSIBILITY_SET_VALUE;
		request.AddInt64("id", target.id);
		request.AddString("value", text.c_str());
		logValue = (target.node.GetInt64("states", 0)
			& B_ACCESSIBLE_STATE_PROTECTED) != 0 ? "(hidden)" : text;
	} else if (action == "type" || action == "key" || action == "click") {
		request.what = B_ACCESSIBILITY_INPUT;
		request.AddInt64("id", target.id);
		uint32 modifiers = 0;
		if (const JsonValue* value = step.Get("modifiers")) {
			if (!ParseModifiers(*value, modifiers, error))
				return error;
		}
		request.AddInt32("modifiers", modifiers);
		if (action == "type") {
			std::string text = step.GetString("text", step.GetString("value"));
			if (text.empty())
				return "type needs text";
			request.AddString("text", text.c_str());
			logValue = (target.node.GetInt64("states", 0)
				& B_ACCESSIBLE_STATE_PROTECTED) != 0 ? "(hidden)" : text;
		} else if (action == "key") {
			std::string key = step.GetString("key", step.GetString("value"));
			if (key.empty())
				return "key needs key (enter, escape, tab, f5, s ...)";
			request.AddString("key", key.c_str());
			logValue = key;
		} else {
			request.AddInt32("click", (int32)step.GetInt("clicks", 1));
			request.AddInt32("button", (int32)step.GetInt("button", 1));
			if (step.Has("x") && step.Has("y")) {
				BRect frame;
				if (target.node.FindRect("frame", &frame) != B_OK)
					return what + " has no frame to click in";
				request.AddPoint("point", BPoint(
					frame.left + step.GetDouble("x"),
					frame.top + step.GetDouble("y")));
			}
		}
	} else {
		accessible_action bit = ActionBit(action);
		if (bit == 0) {
			return "unknown action \"" + action + "\" (press, focus, select, "
				"increment, decrement, expand, collapse, show_menu, "
				"scroll_to, set_value, type, key, click)";
		}
		uint32 supported = target.node.GetInt32("actions", 0);
		if ((supported & bit) == 0) {
			std::string actions = ActionsText(supported);
			return what + " does not support " + action + " (it supports: "
				+ (actions.empty() ? std::string("nothing")
					: actions + ((supported & B_ACCESSIBLE_ACTION_FOCUS) != 0
						? ",focus" : ""))
				+ "; type, key and click work on any view)";
		}
		request.what = B_ACCESSIBILITY_PERFORM_ACTION;
		request.AddInt64("id", target.id);
		request.AddInt32("action", bit);
	}

	BMessage reply;
	status_t status = BridgeRequest(target.app, request, reply, true);
	syslog(LOG_INFO, "ui: %s %s %s %s%s%s -> %s", target.app.name.c_str(),
		ref.c_str(), action.c_str(), target.node.GetString("role_name", "?"),
		logValue.empty() ? "" : " value ", logValue.c_str(),
		StrError(status).c_str());
	if (status != B_OK)
		return what + ": " + action + " failed: " + BridgeError(status);
	ok = true;

	// what it looks like now
	snooze(action == "type" || action == "key" || action == "click"
		? 150000 : 50000);
	BMessage nodeRequest(B_ACCESSIBILITY_GET_NODE), nodeReply, node;
	nodeRequest.AddInt64("id", target.id);
	nodeRequest.AddInt32("depth", 0);
	if (BridgeRequest(target.app, nodeRequest, nodeReply) == B_OK
		&& nodeReply.FindMessage("node", &node) == B_OK)
		return "ok " + action + ": " + NodeLine(node, target.app.team, false);
	return "ok " + action + ": " + what + " (it is gone now: its window "
		"closed or changed)";
}


static ToolResult
UIAct(const JsonValue& arguments)
{
	std::string error;
	if (!Connect(error))
		return ToolResult::Error(error);

	JsonValue steps = JsonValue::Array();
	if (const JsonValue* list = arguments.Get("steps"))
		steps = *list;
	else
		steps.Push(arguments);
	if (!steps.IsArray() || steps.Size() == 0)
		return ToolResult::Error("give steps: [{ref or name, action, ...}]");

	std::string out;
	bool failed = false;
	for (size_t i = 0; i < steps.Size(); i++) {
		bool ok;
		std::string result = RunStep(steps.At(i), ok);
		out += Format("%d. ", (int)i + 1) + result + "\n";
		if (!ok) {
			failed = true;
			if (i + 1 < steps.Size())
				out += Format("stopped; %d steps not run\n",
					(int)(steps.Size() - i - 1));
			break;
		}
	}
	ToolResult result = ToolResult::Text(out);
	result.isError = failed;
	return result;
}


static ToolResult
UIFocus(const JsonValue& arguments)
{
	std::string error;
	if (!Connect(error))
		return ToolResult::Error(error);
	std::vector<AppEntry> apps;
	if (!SelectApps(arguments, apps, error))
		return ToolResult::Error(error);

	for (size_t i = 0; i < apps.size(); i++) {
		BMessage request(B_ACCESSIBILITY_GET_FOCUS), reply;
		if (BridgeRequest(apps[i], request, reply) != B_OK)
			continue;
		BMessage window, node;
		if (reply.FindMessage("window", &window) != B_OK)
			continue;
		bool active = (window.GetInt64("states", 0)
			& B_ACCESSIBLE_STATE_ACTIVE) != 0;
		if (!active && apps.size() > 1)
			continue;
		std::string out = Format("%s (team %d)\n", apps[i].name.c_str(),
			(int)apps[i].team);
		out += "  " + NodeLine(window, apps[i].team, false) + "\n";
		if (reply.FindMessage("node", &node) == B_OK)
			out += "  focus: " + NodeLine(node, apps[i].team, false) + "\n";
		else
			out += "  no control has the focus\n";
		return ToolResult::Text(out);
	}
	return ToolResult::Text("No application window is active (the desktop "
		"or a window without a bridge has the focus).");
}


static ToolResult
UIEvents(const JsonValue& arguments)
{
	std::string error;
	if (!Connect(error))
		return ToolResult::Error(error);
	int64 since = arguments.GetInt("since", 0);
	double wait = arguments.GetDouble("wait_s", 0);
	if (wait > 30)
		wait = 30;
	size_t limit = (size_t)arguments.GetInt("limit", 100);
	std::string filter = Lower(arguments.GetString("app"));

	bigtime_t deadline = system_time() + SecondsToMicro(wait);
	std::string out;
	int64 last = since;
	while (true) {
		{
			AutoLocker locker(sStateLock);
			sLastUse = system_time();
			size_t count = 0;
			for (size_t i = 0; i < sEvents.size(); i++) {
				const EventRecord& record = sEvents[i];
				if (record.seq <= since)
					continue;
				last = record.seq;
				if (!filter.empty()
					&& Lower(record.app).find(filter) == std::string::npos)
					continue;
				if (count++ >= limit)
					continue;
				out += Format("#%lld %.1fs %s: %s\n", (long long)record.seq,
					(record.when - sStartTime) / 1e6, record.app.c_str(),
					record.text.c_str());
			}
			if (last == since)
				last = sNextSeq - 1;
		}
		if (!out.empty() || system_time() >= deadline)
			break;
		snooze(100000);
	}
	if (out.empty())
		out = "No events.\n";
	out += Format("next: since=%lld\n", (long long)last);
	out += "(focus, window, value, selection and text events come from the "
		"control that has the keyboard focus)";
	return ToolResult::Text(out);
}


static ToolResult
UIWait(const JsonValue& arguments)
{
	std::string error;
	if (!Connect(error))
		return ToolResult::Error(error);
	std::vector<AppEntry> apps;

	Query query;
	query.text = Lower(arguments.GetString("text"));
	query.name = PlainLabel(arguments.GetString("name"));
	query.role = Lower(arguments.GetString("role"));
	query.window = Lower(arguments.GetString("window"));
	std::string state = Lower(arguments.GetString("state"));
	std::string value = Lower(arguments.GetString("value"));
	bool gone = arguments.GetBool("gone");
	if (query.text.empty() && query.name.empty() && query.role.empty()) {
		if (query.window.empty())
			return ToolResult::Error("give window, name, text or role");
		query.role = "window";
	}
	double timeout = arguments.GetDouble("timeout_s", 10);
	if (timeout > 120)
		timeout = 120;
	bigtime_t deadline = system_time() + SecondsToMicro(timeout);

	while (true) {
		apps.clear();
		if (!Connect(error) || !SelectApps(arguments, apps, error)) {
			if (system_time() >= deadline)
				return ToolResult::Error(error);
		} else {
			std::vector<Match> matches;
			std::string notes;
			SearchApps(apps, query, matches, 50, notes);
			std::vector<Match> good;
			for (size_t i = 0; i < matches.size(); i++) {
				const BMessage& node = matches[i].node;
				if (!value.empty() && Lower(node.GetString("value", "")).find(
						value) == std::string::npos)
					continue;
				if (!state.empty()) {
					uint64 states = node.GetInt64("states", 0);
					std::string text = Lower(StatesText(states, true));
					bool negate = state.compare(0, 4, "not ") == 0;
					std::string name = negate ? state.substr(4) : state;
					bool has = name == "enabled"
						? (states & B_ACCESSIBLE_STATE_DISABLED) == 0
						: (", " + text + ",").find(", " + name + ",")
							!= std::string::npos;
					if (has == negate)
						continue;
				}
				good.push_back(matches[i]);
			}
			if (gone ? good.empty() : !good.empty()) {
				std::string out = gone ? "gone\n" : "found:\n";
				for (size_t i = 0; i < good.size() && i < 10; i++)
					out += MatchLine(good[i]) + "\n";
				return ToolResult::Text(out);
			}
		}
		if (system_time() >= deadline)
			break;
		snooze(200000);
	}
	ToolResult result = ToolResult::Text(Format("timed out after %g s", timeout));
	result.isError = true;
	return result;
}


static ToolResult
UIText(const JsonValue& arguments)
{
	std::string error;
	if (!Connect(error))
		return ToolResult::Error(error);
	ResolvedNode target;
	if (!ResolveNode(arguments, target, error))
		return ToolResult::Error(error);

	BMessage request(B_ACCESSIBILITY_GET_TEXT), reply;
	request.AddInt64("id", target.id);
	request.AddInt32("start", (int32)arguments.GetInt("start", 0));
	request.AddInt32("end", (int32)arguments.GetInt("end", 1 << 20));
	status_t status = BridgeRequest(target.app, request, reply);
	if (status == B_OK)
		return ToolResult::Text(reply.GetString("text", ""));
	const char* drawn;
	if (target.node.FindString(B_ACCESSIBLE_FIELD_DRAWN_TEXT, &drawn) == B_OK)
		return ToolResult::Text(drawn);
	if ((target.node.GetInt64("states", 0) & B_ACCESSIBLE_STATE_PROTECTED) != 0)
		return ToolResult::Error("password fields never give out their text");
	return ToolResult::Error(BridgeError(status));
}

} // namespace


void
SetUIAccessEnabled(bool enabled)
{
	gUIAccessEnabled = enabled;
	if (!enabled) {
		AutoLocker locker(sStateLock);
		Unsubscribe();
	}
}


void
RegisterUITools(McpServer& server)
{
	server.AddTool("ui_windows",
		"Every window on screen, by application, with the ref to read or use "
		"it: title, active/modal, front-to-back order (z1 = front), size, "
		"position, workspace. Faster and more precise than a screenshot. "
		"Applications run unchanged: their Interface Kit views answer "
		"through the system accessibility interface.",
		"{\"type\":\"object\",\"properties\":{"
		"\"app\":{\"type\":\"string\",\"description\":\"only this application "
		"(name, signature or team)\"},"
		"\"all\":{\"type\":\"boolean\",\"description\":\"also hidden windows\"}}}",
		UIWindows);
	server.AddTool("ui_tree",
		"The controls of a window (or an application, or below a ref) as an "
		"indented tree: role \"name\" [ref] = value (states) {actions}. Lines "
		"starting with | are text a custom view drew. Use the refs with "
		"ui_act. Unnamed layout groups are left out. Password fields never "
		"show their text.",
		"{\"type\":\"object\",\"properties\":{"
		"\"app\":{\"type\":\"string\",\"description\":\"application name, "
		"signature or team\"},"
		"\"window\":{\"type\":\"string\",\"description\":\"window title (or "
		"part of it)\"},"
		"\"ref\":{\"type\":\"string\",\"description\":\"a node from "
		"ui_windows, ui_tree or ui_find, e.g. 611:12 or 611:14.3\"},"
		"\"depth\":{\"type\":\"integer\",\"description\":\"levels to read "
		"(default -1: all)\"},"
		"\"max_nodes\":{\"type\":\"integer\",\"description\":\"default 400\"},"
		"\"menu_depth\":{\"type\":\"integer\",\"description\":\"levels of "
		"closed menus under the menu bar (default 1; 8 below a ref)\"},"
		"\"hidden\":{\"type\":\"boolean\",\"description\":\"include hidden "
		"views (other tabs)\"},"
		"\"frames\":{\"type\":\"boolean\",\"description\":\"screen position "
		"and size of each node\"},"
		"\"format\":{\"type\":\"string\",\"enum\":[\"text\",\"json\"]}}}",
		UITree);
	server.AddTool("ui_find",
		"Find controls in all applications (or one) by text in their name, "
		"value, tool tip or drawn text, by exact name, or by role (button, "
		"check box, text field, menu item, list item, window ...). Returns "
		"refs with the path to each.",
		"{\"type\":\"object\",\"properties\":{"
		"\"text\":{\"type\":\"string\"},"
		"\"name\":{\"type\":\"string\",\"description\":\"the whole label, "
		"ignoring case, a trailing colon or ellipsis\"},"
		"\"role\":{\"type\":\"string\"},"
		"\"app\":{\"type\":\"string\"},"
		"\"window\":{\"type\":\"string\",\"description\":\"window title "
		"contains\"},"
		"\"hidden\":{\"type\":\"boolean\"},"
		"\"limit\":{\"type\":\"integer\",\"description\":\"default 40\"}}}",
		UIFind);
	server.AddTool("ui_act",
		"Use controls without the mouse: run steps in order and return each "
		"node's new state. A step names its node by ref, or by name (+ role, "
		"app, window when names repeat; an ambiguous name returns the "
		"candidates). Actions: press (buttons, check boxes, menu and list "
		"items - menu items work while their menu is closed), set_value "
		"(text fields and areas, check boxes on/off, sliders and spinners, "
		"pop-up menus/lists/tabs by item label, color #rrggbb), select, "
		"focus, activate (a window), increment, decrement, expand, collapse, "
		"show_menu, and for views without semantics type (text), key (enter, "
		"escape, tab, f5, or a character with modifiers such as "
		"command+s) and click (clicks, button 1-3, x/y inside the node). "
		"Windows need not be in front. Nodes marked NEEDS CONFIRMATION "
		"refuse until you asked the person and set confirmed=true. The "
		"application sees be:agent in the messages it gets.",
		"{\"type\":\"object\",\"properties\":{"
		"\"steps\":{\"type\":\"array\",\"items\":{\"type\":\"object\","
		"\"properties\":{"
		"\"ref\":{\"type\":\"string\"},"
		"\"name\":{\"type\":\"string\"},"
		"\"role\":{\"type\":\"string\"},"
		"\"app\":{\"type\":\"string\"},"
		"\"window\":{\"type\":\"string\"},"
		"\"action\":{\"type\":\"string\"},"
		"\"value\":{},"
		"\"text\":{\"type\":\"string\"},"
		"\"key\":{\"type\":\"string\"},"
		"\"modifiers\":{\"type\":\"string\",\"description\":\"command+shift "
		"(command = Alt key in the default keymap)\"},"
		"\"clicks\":{\"type\":\"integer\"},"
		"\"button\":{\"type\":\"integer\"},"
		"\"x\":{\"type\":\"number\"},"
		"\"y\":{\"type\":\"number\"},"
		"\"confirmed\":{\"type\":\"boolean\"}},"
		"\"required\":[\"action\"]}}},"
		"\"required\":[\"steps\"]}",
		UIAct);
	server.AddTool("ui_focus",
		"The active window and the control with the keyboard focus.",
		"{\"type\":\"object\",\"properties\":{\"app\":{\"type\":\"string\"}}}",
		UIFocus);
	server.AddTool("ui_events",
		"What happened in the user interface since the last call: windows "
		"activated, focus moves, value and selection changes, typed text, "
		"applications started and quit. Pass since from the previous result; "
		"wait_s waits (up to 30 s) for the first event.",
		"{\"type\":\"object\",\"properties\":{"
		"\"since\":{\"type\":\"integer\"},"
		"\"wait_s\":{\"type\":\"number\"},"
		"\"app\":{\"type\":\"string\"},"
		"\"limit\":{\"type\":\"integer\"}}}",
		UIEvents);
	server.AddTool("ui_wait",
		"Wait until a window or control appears (or, with gone=true, goes "
		"away), optionally with a value or state (enabled, \"not disabled\", "
		"checked, selected, focused, active ...). Instead of sleeping and "
		"taking screenshots.",
		"{\"type\":\"object\",\"properties\":{"
		"\"window\":{\"type\":\"string\"},"
		"\"name\":{\"type\":\"string\"},"
		"\"text\":{\"type\":\"string\"},"
		"\"role\":{\"type\":\"string\"},"
		"\"value\":{\"type\":\"string\"},"
		"\"state\":{\"type\":\"string\"},"
		"\"app\":{\"type\":\"string\"},"
		"\"gone\":{\"type\":\"boolean\"},"
		"\"timeout_s\":{\"type\":\"number\",\"description\":\"default 10\"}}}",
		UIWait);
	server.AddTool("ui_text",
		"The whole text of a text view or field (ui_tree shows the first "
		"4 KB), or what a custom view drew. start/end are byte offsets.",
		"{\"type\":\"object\",\"properties\":{"
		"\"ref\":{\"type\":\"string\"},"
		"\"name\":{\"type\":\"string\"},"
		"\"app\":{\"type\":\"string\"},"
		"\"window\":{\"type\":\"string\"},"
		"\"start\":{\"type\":\"integer\"},"
		"\"end\":{\"type\":\"integer\"}}}",
		UIText);
}
