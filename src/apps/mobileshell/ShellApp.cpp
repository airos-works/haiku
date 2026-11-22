/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */

/*	The mobile shell: a touch desktop for a machine used as a tablet. While
	it runs it stands in for Tracker's desktop and the Deskbar with a home
	screen, a status bar, a navigation panel, an action drawer, a task
	switcher and an on-screen keyboard, and keeps every application window
	full-screen between the panels.

	It takes the desktop over when it starts and gives it back when it is
	asked to quit: the Deskbar and the desktop reappear, and every window
	gets the look and frame it had. "MobileShell --restore" does only the
	giving back, after a shell that died without.
*/


#include "ActionDrawer.h"
#include "AppList.h"
#include "HomeScreen.h"
#include "Keyboard.h"
#include "Panels.h"
#include "SavedState.h"
#include "TaskSwitcher.h"
#include "Theme.h"
#include "WindowTracker.h"

#include <Application.h>
#include <MessageRunner.h>
#include <Roster.h>
#include <Screen.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const uint32 kMsgTrackWindows = 'trck';
static const uint32 kMsgScreenshotNow = 'shtN';

static const bigtime_t kTrackInterval = 300000;
// every so many polls of the windows: the Deskbar may have come back, the
// keyboard add-on may be a new one (input_server was restarted)
static const int32 kHousekeepingPolls = 10;

// Terminal's view takes text without saying so (it isn't
// B_INPUT_METHOD_AWARE), so it is known by its signature.
static const char* kTerminalSignature = "application/x-vnd.Haiku-Terminal";

static volatile sig_atomic_t sQuitSignal = 0;


static void
quit_signal(int)
{
	sQuitSignal = 1;
}


class ShellApp : public BApplication {
public:
								ShellApp();
	virtual						~ShellApp();

	virtual	void				ArgvReceived(int32 argc, char** argv);
	virtual	void				ReadyToRun();
	virtual	bool				QuitRequested();
	virtual	void				MessageReceived(BMessage* message);

private:
			void				_LayoutWindows();
			void				_SetKeyboard(bool show);
			void				_UpdateWorkArea();
			void				_UpdateHoldArea();
			void				_SendHoldArea(const mobile_hold_area& area);
			void				_HelloKeyboard();
			void				_Launch(const entry_ref& ref);
			void				_HideOverlays();
			bool				_OverlayShown();

			AppList				fApps;
			WindowTracker		fTracker;
			BRect				fScreen;
			bool				fRestoreOnly;
			bool				fStarted;

			HomeWindow*			fHome;
			PanelWindow*		fStatusBar;
			StatusBarView*		fStatusView;
			PanelWindow*		fNavBar;
			PanelWindow*		fKeyboard;
			KeyboardView*		fKeyboardView;
			OverlayWindow*		fDrawerWindow;
			ActionDrawerView*	fDrawer;
			OverlayWindow*		fTasksWindow;
			TaskSwitcherView*	fTasks;

			BMessageRunner*		fPulse;
			BMessageRunner*		fTrackRunner;
			int32				fPolls;
			bool				fKeyboardShown;
			mobile_hold_area	fHoldArea;	// as the touch filter has it
};


ShellApp::ShellApp()
	:
	BApplication(MOBILE_SHELL_SIGNATURE),
	fTracker(be_app->Team()),
	fRestoreOnly(false),
	fStarted(false),
	fHome(NULL),
	fStatusBar(NULL),
	fStatusView(NULL),
	fNavBar(NULL),
	fKeyboard(NULL),
	fKeyboardView(NULL),
	fDrawerWindow(NULL),
	fDrawer(NULL),
	fTasksWindow(NULL),
	fTasks(NULL),
	fPulse(NULL),
	fTrackRunner(NULL),
	fPolls(0),
	fKeyboardShown(false)
{
	memset(&fHoldArea, 0, sizeof(fHoldArea));
}


ShellApp::~ShellApp()
{
	delete fPulse;
	delete fTrackRunner;
}


void
ShellApp::ArgvReceived(int32 argc, char** argv)
{
	// Before the shell is up only; a running shell has nothing to restore.
	if (fStarted)
		return;
	for (int32 i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--restore") == 0)
			fRestoreOnly = true;
	}
}


void
ShellApp::ReadyToRun()
{
	if (fRestoreOnly) {
		// what a shell that died left behind
		fTracker.RestoreDesktop();
		PostMessage(B_QUIT_REQUESTED);
		return;
	}
	fStarted = true;

	signal(SIGTERM, quit_signal);
	signal(SIGHUP, quit_signal);

	fScreen = BScreen().Frame();
	fApps.Scan();

	fHome = new HomeWindow(fScreen, &fApps);
	fHome->Show();

	fStatusBar = new PanelWindow(BRect(0, 0, 10, 10), "Status bar");
	fStatusView = new StatusBarView(fStatusBar->Bounds());
	fStatusBar->AddChild(fStatusView);

	fNavBar = new PanelWindow(BRect(0, 0, 10, 10), "Navigation");
	fNavBar->AddChild(new NavBarView(fNavBar->Bounds()));

	fKeyboard = new PanelWindow(BRect(0, 0, 10, 10), "Keyboard");
	fKeyboardView = new KeyboardView(fKeyboard->Bounds());
	fKeyboard->AddChild(fKeyboardView);

	fDrawerWindow = new OverlayWindow(BRect(0, 0, 10, 10), "Action drawer");
	fDrawer = new ActionDrawerView(fDrawerWindow->Bounds());
	fDrawerWindow->AddChild(fDrawer);

	fTasksWindow = new OverlayWindow(BRect(0, 0, 10, 10), "Task switcher");
	fTasks = new TaskSwitcherView(fTasksWindow->Bounds());
	fTasksWindow->AddChild(fTasks);

	_LayoutWindows();

	fStatusBar->Show();
	fNavBar->Show();
	// Hide() then Show() starts a window's thread but leaves it hidden.
	fKeyboard->Hide();
	fKeyboard->Show();
	fDrawerWindow->Hide();
	fDrawerWindow->Show();
	fTasksWindow->Hide();
	fTasksWindow->Show();

	fTracker.HideDesktop();
	_UpdateWorkArea();
	fTracker.Update();
	_HelloKeyboard();
	_UpdateHoldArea();

	BMessage pulse(kMsgPulse);
	fPulse = new BMessageRunner(be_app_messenger, &pulse, 1000000);
	BMessage track(kMsgTrackWindows);
	fTrackRunner = new BMessageRunner(be_app_messenger, &track,
		kTrackInterval);
}


bool
ShellApp::QuitRequested()
{
	if (fStarted) {
		fStarted = false;
		delete fPulse;
		fPulse = NULL;
		delete fTrackRunner;
		fTrackRunner = NULL;

		// no more long presses, no more keys
		mobile_hold_area none = {};
		_SendHoldArea(none);
		port_id port = find_port(MOBILE_KEYBOARD_PORT);
		if (port >= 0) {
			write_port_etc(port, kMobileBye, NULL, 0, B_RELATIVE_TIMEOUT,
				100000);
		}

		// The shell's windows go first, so that the desktop comes back
		// from under them in one piece.
		BWindow* windows[] = { fDrawerWindow, fTasksWindow, fKeyboard,
			fStatusBar, fNavBar };
		for (BWindow* window : windows) {
			if (window != NULL && window->Lock()) {
				if (!window->IsHidden())
					window->Hide();
				window->Unlock();
			}
		}
		fTracker.RestoreDesktop();
	}
	return BApplication::QuitRequested();
}


void
ShellApp::MessageReceived(BMessage* message)
{
	if (!fStarted) {
		BApplication::MessageReceived(message);
		return;
	}

	switch (message->what) {
		case kMsgPulse:
			fStatusBar->PostMessage(kMsgPulse, fStatusView);
			fHome->PostMessage(kMsgPulse, fHome->View());
			break;

		case kMsgTrackWindows:
			if (sQuitSignal != 0) {
				PostMessage(B_QUIT_REQUESTED);
				break;
			}
			if (++fPolls % kHousekeepingPolls == 0) {
				fTracker.HideDesktop();
				_HelloKeyboard();
			}
			fTracker.Update();
			// No application on the screen, no keyboard: the windows that
			// going home minimizes pass the focus along as they go, and the
			// last word on it may have been a text field's.
			if (fTracker.Front() == NULL && fKeyboardShown)
				_SetKeyboard(false);
			_UpdateHoldArea();
			break;

		case kMsgScreenChanged:
			_LayoutWindows();
			_UpdateWorkArea();
			_UpdateHoldArea();
			break;

		case kMsgOverlayChanged:
			_UpdateHoldArea();
			break;

		case kMsgLaunch:
		{
			entry_ref ref;
			if (message->FindRef("ref", &ref) == B_OK)
				_Launch(ref);
			break;
		}

		case kMsgGoHome:
			_HideOverlays();
			_SetKeyboard(false);
			fTracker.MinimizeAll();
			if (fHome->Lock()) {
				fHome->View()->ShowHome();
				fHome->Unlock();
			}
			break;

		case kMsgShowSettings:
			_HideOverlays();
			fTracker.MinimizeAll();
			if (fHome->Lock()) {
				fHome->View()->ShowSettings();
				fHome->Unlock();
			}
			break;

		case kMsgShowTasks:
			if (fDrawerWindow->Lock()) {
				if (!fDrawerWindow->IsHidden())
					fDrawer->Close();
				fDrawerWindow->Unlock();
			}
			fTracker.Update();
			if (fTasksWindow->Lock()) {
				if (fTasksWindow->IsHidden())
					fTasks->Open(fTracker.Windows());
				else
					fTasks->Close();
				fTasksWindow->Unlock();
			}
			break;

		case kMsgShowDrawer:
			if (fDrawerWindow->Lock()) {
				if (fDrawerWindow->IsHidden())
					fDrawer->Open();
				else
					fDrawer->Close();
				fDrawerWindow->Unlock();
			}
			break;

		case kMsgCloseActive:
		{
			_HideOverlays();
			fTracker.Update();
			if (const TrackedWindow* front = fTracker.Front())
				fTracker.Close(front->token);
			break;
		}

		case kMsgActivateWindow:
		case kMsgCloseWindow:
		{
			int32 token;
			if (message->FindInt32("token", &token) != B_OK)
				break;
			if (message->what == kMsgActivateWindow)
				fTracker.Activate(token);
			else
				fTracker.Close(token);
			break;
		}

		case kMsgToggleKeyboard:
		{
			bool hidden = true;
			if (fKeyboard->Lock()) {
				hidden = fKeyboard->IsHidden();
				fKeyboard->Unlock();
			}
			_SetKeyboard(hidden);
			break;
		}

		case kMsgTextFocus:
		{
			// From the keyboard add-on: the keyboard comes up with a text
			// field and goes with it.
			bool text = message->GetBool("text", false);
			team_id team = message->GetInt32("team", -1);
			if (team == Team())
				break;
			app_info info;
			if (!text && team >= 0
				&& be_roster->GetRunningAppInfo(team, &info) == B_OK
				&& strcmp(info.signature, kTerminalSignature) == 0) {
				text = true;
			}
			// another field: the word swiped into the last one is done with
			if (fKeyboard->Lock()) {
				fKeyboardView->Reset();
				fKeyboard->Unlock();
			}
			_SetKeyboard(text);
			break;
		}

		case kMsgKeyboardResized:
			_UpdateWorkArea();
			_UpdateHoldArea();
			break;

		case kMsgTakeScreenshot:
		{
			// Wait for the drawer to slide away first.
			BMessage now(kMsgScreenshotNow);
			BMessageRunner::StartSending(be_app_messenger, &now, 500000, 1);
			break;
		}

		case kMsgScreenshotNow:
			system("/bin/screenshot --silent &");
			break;

		default:
			BApplication::MessageReceived(message);
	}
}


/*!	Puts the shell's windows where they belong on the screen as it is now. */
void
ShellApp::_LayoutWindows()
{
	fScreen = BScreen().Frame();

	struct { BWindow* window; BRect frame; } placed[5];
	BRect frame = fScreen;
	frame.bottom = frame.top + Theme::StatusBarHeight() - 1;
	placed[0] = { fStatusBar, frame };

	frame = fScreen;
	frame.top = frame.bottom - Theme::NavBarHeight() + 1;
	placed[1] = { fNavBar, frame };

	// The keyboard keeps the height its layout gave it.
	float keyboardHeight = Theme::KeyboardHeight();
	if (fKeyboard->Lock()) {
		if (fKeyboard->Frame().Height() + 1 > keyboardHeight)
			keyboardHeight = fKeyboard->Frame().Height() + 1;
		fKeyboard->Unlock();
	}
	frame.bottom = frame.top - 1;
	frame.top = frame.bottom - keyboardHeight + 1;
	placed[2] = { fKeyboard, frame };

	// The overlays sit below the status bar and above the navigation panel,
	// so both stay usable while one is open.
	frame = fScreen;
	frame.top += Theme::StatusBarHeight();
	frame.bottom -= Theme::NavBarHeight();
	placed[3] = { fDrawerWindow, frame };
	placed[4] = { fTasksWindow, frame };

	for (auto& place : placed) {
		if (!place.window->Lock())
			continue;
		place.window->MoveTo(place.frame.LeftTop());
		place.window->ResizeTo(place.frame.Width(), place.frame.Height());
		place.window->Unlock();
	}

	if (fHome->Lock()) {
		fHome->MoveTo(fScreen.LeftTop());
		fHome->ResizeTo(fScreen.Width(), fScreen.Height());
		fHome->Unlock();
	}
}


void
ShellApp::_SetKeyboard(bool show)
{
	if (fKeyboard->Lock()) {
		if (show && fKeyboard->IsHidden())
			fKeyboard->Show();
		else if (!show && !fKeyboard->IsHidden()) {
			fKeyboardView->Reset();
			fKeyboard->Hide();
		}
		fKeyboard->Unlock();
	}
	fKeyboardShown = show;
	if (fDrawerWindow->Lock()) {
		fDrawer->SetKeyboardShown(show);
		fDrawerWindow->Unlock();
	}
	_UpdateWorkArea();
	_UpdateHoldArea();
}


void
ShellApp::_UpdateWorkArea()
{
	BRect area = fScreen;
	area.top += Theme::StatusBarHeight();
	area.bottom -= Theme::NavBarHeight();
	if (fKeyboard->Lock()) {
		// as tall as its layout makes it
		if (!fKeyboard->IsHidden())
			area.bottom -= fKeyboard->Frame().Height() + 1;
		fKeyboard->Unlock();
	}
	fTracker.SetWorkArea(area);
}


/*!	Tells the touch filter where a finger held still means a secondary
	click: on the applications' windows, while one is up and nothing of the
	shell's covers them.
*/
void
ShellApp::_UpdateHoldArea()
{
	mobile_hold_area area = {};
	if (fTracker.Front() != NULL && !_OverlayShown()) {
		BRect work = fTracker.WorkArea();
		area.enabled = 1;
		area.left = work.left;
		area.top = work.top;
		area.right = work.right;
		area.bottom = work.bottom;
	}
	if (memcmp(&area, &fHoldArea, sizeof(area)) != 0)
		_SendHoldArea(area);
}


void
ShellApp::_SendHoldArea(const mobile_hold_area& area)
{
	port_id port = find_port(MOBILE_TOUCH_PORT);
	if (port < 0)
		return;
	// The filter reads its port at a press: a full port means it has not
	// seen one since, and the newest word is what counts. Try again later.
	if (write_port_etc(port, kMobileHoldArea, &area, sizeof(area),
			B_RELATIVE_TIMEOUT, 0) == B_OK) {
		fHoldArea = area;
	}
}


/*!	Says hello to the keyboard add-on: who to tell about the focus, and
	whose keys to type. Said again now and then, in case it forgot.
*/
void
ShellApp::_HelloKeyboard()
{
	port_id port = find_port(MOBILE_KEYBOARD_PORT);
	if (port < 0)
		return;

	BMessage hello(kMobileHello);
	hello.AddMessenger("shell", be_app_messenger);
	char buffer[512];
	ssize_t size = hello.FlattenedSize();
	if (size > (ssize_t)sizeof(buffer)
		|| hello.Flatten(buffer, size) != B_OK) {
		return;
	}
	write_port_etc(port, kMobileHello, buffer, size, B_RELATIVE_TIMEOUT,
		100000);
}


void
ShellApp::_Launch(const entry_ref& ref)
{
	// Single-launch applications that already run: bring their newest
	// window forward.
	team_id team;
	if (be_roster->Launch(&ref, (BMessage*)NULL, &team) != B_ALREADY_RUNNING)
		return;
	fTracker.Update();
	for (const TrackedWindow& window : fTracker.Windows()) {
		if (window.team == team) {
			fTracker.Activate(window.token);
			return;
		}
	}
	be_roster->ActivateApp(team);
}


void
ShellApp::_HideOverlays()
{
	if (fDrawerWindow->Lock()) {
		if (!fDrawerWindow->IsHidden())
			fDrawer->Close();
		fDrawerWindow->Unlock();
	}
	if (fTasksWindow->Lock()) {
		if (!fTasksWindow->IsHidden())
			fTasks->Close();
		fTasksWindow->Unlock();
	}
}


bool
ShellApp::_OverlayShown()
{
	bool shown = false;
	BWindow* windows[] = { fDrawerWindow, fTasksWindow };
	for (BWindow* window : windows) {
		if (window->Lock()) {
			shown = shown || !window->IsHidden();
			window->Unlock();
		}
	}
	return shown;
}


int
main()
{
	ShellApp app;
	app.Run();
	return 0;
}
