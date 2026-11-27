/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	What app_server sends a window whose port is full (#229).

	A window stops reading its port for a while (as a slow Draw() or a file
	operation in MessageReceived() does), and the port fills up (pointer
	moves do that in a second or two; the test fills it itself). Meanwhile
	it is typed to, clicked in, and another window is put over part of it
	and taken away again. app_server used to drop everything it could not
	write into the port at once: the keys, the click, and the _UPDATE_ for
	the uncovered part, which it never sent again, so that part stayed
	unpainted.

	Run it where app_server runs (the test VM at its Try/Install prompt will
	do). Keys and clicks go in through input_server's event port, as from a
	device. With "moves", the pointer also goes back and forth over the
	window 2000 times while it is busy; with "moves-only", that is what
	fills the port. Exit status 0 when everything arrived.
*/


#include <stdio.h>
#include <string.h>

#include <Application.h>
#include <InterfaceDefs.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>
#include <Region.h>
#include <View.h>
#include <Window.h>
#include <WindowScreen.h>

#include <MessagePrivate.h>
#include <MessengerPrivate.h>
#include <TokenSpace.h>


static const uint32 kMsgBlock = 'blck';
static const uint32 kMsgFill = 'fill';
static const bigtime_t kBlockTime = 3000000;
static const char* kTyped = "abcdef";


static int32 sBlocking = 0;
static int32 sBlockEnded = 0;
static int32 sFilled = 0;
static int32 sDrawsAfter = 0;
static int32 sMouseDowns = 0;
static int32 sMouseUps = 0;
static char sKeys[64];
static int32 sKeyCount = 0;
static int32 sDraws = 0;
static BRect sCovered;
	// in the view's coordinates


class TestView : public BView {
public:
	TestView(BRect frame)
		:
		BView(frame, "test", B_FOLLOW_ALL, B_WILL_DRAW)
	{
		SetViewColor(255, 255, 255);
	}

	virtual void Draw(BRect updateRect)
	{
		atomic_add(&sDraws, 1);
		if (atomic_get(&sBlockEnded) != 0 && updateRect.Intersects(sCovered))
			atomic_add(&sDrawsAfter, 1);
		SetHighColor(40, 120, 200);
		FillRect(Bounds().InsetByCopy(10, 10));
	}

	virtual void KeyDown(const char* bytes, int32 numBytes)
	{
		if (numBytes == 1 && sKeyCount < (int32)sizeof(sKeys) - 1)
			sKeys[sKeyCount++] = bytes[0];
	}

	virtual void MouseDown(BPoint where)
	{
		atomic_add(&sMouseDowns, 1);
	}

	virtual void MouseUp(BPoint where)
	{
		atomic_add(&sMouseUps, 1);
	}
};


class TestWindow : public BWindow {
public:
	TestWindow(BRect frame)
		:
		BWindow(frame, "window port full test", B_TITLED_WINDOW,
			B_NOT_ZOOMABLE | B_ASYNCHRONOUS_CONTROLS)
	{
		fView = new TestView(Bounds());
		AddChild(fView);
		fView->MakeFocus();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgBlock:
				// Busy, as a slow Draw() or a file operation would be:
				// nothing reads the port.
				atomic_set(&sBlocking, 1);
				snooze(kBlockTime);
				atomic_set(&sBlockEnded, 1);
				break;
			case kMsgFill:
				atomic_add(&sFilled, 1);
				break;
			default:
				BWindow::MessageReceived(message);
		}
	}

	TestView* fView;
};


static bool
wait_for(int32* value, int32 atLeast, bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	while (atomic_get(value) < atLeast) {
		if (system_time() > deadline)
			return false;
		snooze(5000);
	}
	return true;
}


static status_t
inject(port_id port, BMessage& event)
{
	event.AddInt64("when", system_time());
	ssize_t size = event.FlattenedSize();
	char buffer[size];
	status_t status = event.Flatten(buffer, size);
	if (status != B_OK)
		return status;
	return write_port_etc(port, 0, buffer, size, B_RELATIVE_TIMEOUT, 1000000);
}


static status_t
inject_key(port_id port, char c)
{
	char bytes[2] = { c, 0 };
	BMessage down(B_KEY_DOWN);
	down.AddInt32("key", 0x3c);
		// a key in the letter rows; app_server looks at it only for
		// shortcuts of its own
	down.AddInt32("modifiers", 0);
	down.AddInt32("raw_char", c);
	down.AddInt8("byte", c);
	down.AddString("bytes", bytes);
	status_t status = inject(port, down);
	if (status != B_OK)
		return status;

	BMessage up(B_KEY_UP);
	up.AddInt32("key", 0x3c);
	up.AddInt32("modifiers", 0);
	up.AddInt32("raw_char", c);
	up.AddInt8("byte", c);
	up.AddString("bytes", bytes);
	return inject(port, up);
}


static status_t
inject_click(port_id port, BPoint where)
{
	BMessage down(B_MOUSE_DOWN);
	down.AddPoint("where", where);
	down.AddInt32("buttons", B_PRIMARY_MOUSE_BUTTON);
	down.AddInt32("clicks", 1);
	status_t status = inject(port, down);
	if (status != B_OK)
		return status;

	BMessage up(B_MOUSE_UP);
	up.AddPoint("where", where);
	up.AddInt32("buttons", 0);
	return inject(port, up);
}


int
main(int argc, char** argv)
{
	bool moves = argc > 1 && strncmp(argv[1], "moves", 5) == 0;
	bool fill = argc <= 1 || strcmp(argv[1], "moves-only") != 0;

	BApplication app("application/x-vnd.Haiku-window-port-full-test");

	port_id inputPort = find_port("input server events");
	if (inputPort < 0) {
		fprintf(stderr, "input_server's event port is not there\n");
		return 2;
	}

	BRect frame(100, 100, 500, 400);
	TestWindow* window = new TestWindow(frame);
	window->Show();
	window->Activate();

	// Wait until it is on screen, drawn and in front.
	bigtime_t deadline = system_time() + 10000000;
	while (true) {
		if (window->Lock()) {
			bool ready = window->IsActive() && atomic_get(&sDraws) > 0;
			window->Unlock();
			if (ready)
				break;
		}
		if (system_time() > deadline) {
			fprintf(stderr, "the test window did not come up active\n");
			return 2;
		}
		snooze(20000);
	}

	// The part that will be covered, and a point to click at, away from it.
	BRect coverFrame(frame.left + 40, frame.top + 40, frame.left + 200,
		frame.top + 160);
	BPoint clickAt(frame.right - 60, frame.bottom - 60);
	if (window->Lock()) {
		sCovered = window->fView->ConvertFromScreen(coverFrame);
		window->Unlock();
	}

	BMessenger messenger(window);
	messenger.SendMessage(kMsgBlock);
	if (!wait_for(&sBlocking, 1, 2000000)) {
		fprintf(stderr, "the window did not start its busy spell\n");
		return 2;
	}
	bigtime_t blockStart = system_time();

	// Fill its port, as a busy window under a moving pointer has it. A
	// BMessenger of the same team would hand the messages to the window's
	// queue directly; these go through the port, as another team's do.
	port_id windowPort = BMessenger::Private(messenger).Port();
	int32 filled = 0;
	while (fill && filled < 1000) {
		BMessage message(kMsgFill);
		BMessenger replyTo;
		if (BMessage::Private(&message).SendMessage(windowPort, -1,
				B_PREFERRED_TOKEN, 0, false, replyTo) != B_OK)
			break;
		filled++;
	}
	printf("the port took %" B_PRId32 " messages\n", filled);

	// Cover part of the window and uncover it again: app_server sends an
	// _UPDATE_ for that part.
	BWindow* cover = new BWindow(coverFrame, "cover", B_NO_BORDER_WINDOW_LOOK,
		B_FLOATING_ALL_WINDOW_FEEL, B_AVOID_FOCUS);
	BView* coverView = new BView(cover->Bounds(), "cover", B_FOLLOW_ALL,
		B_WILL_DRAW);
	coverView->SetViewColor(200, 40, 40);
	cover->AddChild(coverView);
	cover->Show();
	if (cover->Lock()) {
		cover->Sync();
		cover->Unlock();
	}
	snooze(300000);
	cover->Lock();
	cover->Quit();
	snooze(300000);

	if (moves) {
		// A pointer moving over the busy window: app_server sends it a
		// B_MOUSE_MOVED for each step.
		for (int32 i = 0; i < 2000; i++) {
			set_mouse_position((int32)frame.left + 220 + (i % 2) * 40,
				(int32)frame.top + 200 + (i % 7) * 5);
			if (i % 50 == 0)
				snooze(1000);
		}
	}

	// Type and click.
	for (const char* c = kTyped; *c != '\0'; c++) {
		if (inject_key(inputPort, *c) != B_OK) {
			fprintf(stderr, "could not hand input_server a key\n");
			return 2;
		}
		snooze(20000);
	}
	if (inject_click(inputPort, clickAt) != B_OK) {
		fprintf(stderr, "could not hand input_server a click\n");
		return 2;
	}

	if (system_time() - blockStart > kBlockTime - 200000)
		printf("warning: the input went in late in the busy spell\n");

	// The window wakes up, works through the port and whatever else comes.
	wait_for(&sBlockEnded, 1, kBlockTime + 2000000);
	wait_for(&sFilled, filled, 5000000);
	deadline = system_time() + 3000000;
	while (system_time() < deadline) {
		if (sKeyCount == (int32)strlen(kTyped) && atomic_get(&sMouseUps) > 0
			&& atomic_get(&sDrawsAfter) > 0)
			break;
		snooze(20000);
	}

	window->Lock();
	sKeys[sKeyCount] = '\0';
	bool keysOk = strcmp(sKeys, kTyped) == 0;
	bool clickOk = sMouseDowns == 1 && sMouseUps == 1;
	bool drawOk = sDrawsAfter > 0;
	printf("keys typed while busy: \"%s\" of \"%s\": %s\n", sKeys, kTyped,
		keysOk ? "ok" : "LOST");
	printf("click while busy: %" B_PRId32 " down, %" B_PRId32 " up: %s\n",
		sMouseDowns, sMouseUps, clickOk ? "ok" : "LOST");
	printf("uncovered part redrawn after the busy spell: %" B_PRId32
		" draws: %s\n", sDrawsAfter, drawOk ? "ok" : "NOT REDRAWN");
	window->Quit();

	return keysOk && clickOk && drawOk ? 0 : 1;
}
