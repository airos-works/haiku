/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "LevelWindow.h"

#include <MessageRunner.h>
#include <Screen.h>

#include <InterfacePrivate.h>
#include <OnScreenDisplay.h>

#include "LevelView.h"


static const uint32 kMsgHide = 'hide';
static const bigtime_t kShowTime = 1500000;
	// as long as macOS shows its volume and brightness


LevelWindow::LevelWindow()
	:
	BWindow(BRect(0, 0, 0, 0), "Level", B_NO_BORDER_WINDOW_LOOK,
		B_FLOATING_ALL_WINDOW_FEEL, B_AVOID_FOCUS | B_NOT_CLOSABLE
			| B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_RESIZABLE
			| B_NOT_MOVABLE | B_WILL_ACCEPT_FIRST_CLICK, B_ALL_WORKSPACES),
	fHideRunner(NULL)
{
	fView = new LevelView();
	float width, height;
	fView->GetPreferredSize(&width, &height);
	ResizeTo(width - 1, height - 1);
	fView->ResizeTo(width - 1, height - 1);
	AddChild(fView);
}


LevelWindow::~LevelWindow()
{
	delete fHideRunner;
}


void
LevelWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgShowLevel:
			_ShowLevel(message);
			break;

		case kMsgHide:
			if (!IsHidden())
				Hide();
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


void
LevelWindow::_ShowLevel(const BMessage* message)
{
	fView->SetLevel(message);

	if (IsHidden()) {
		_MoveIntoPlace();
		Show();
	}

	// a level that keeps changing keeps the window up
	delete fHideRunner;
	BMessage hide(kMsgHide);
	fHideRunner = new BMessageRunner(BMessenger(this), &hide, kShowTime, 1);
}


void
LevelWindow::_MoveIntoPlace()
{
	// centered, in the lower part of the primary display (where Deskbar is
	// too), where macOS shows it as well
	BRect screen;
	if (BPrivate::get_display_frame(BRect(), false, screen) != B_OK
		|| !screen.IsValid())
		screen = BScreen(this).Frame();
	BRect frame = Frame();
	MoveTo(roundf(screen.left + (screen.Width() - frame.Width()) / 2),
		roundf(screen.bottom - screen.Height() / 7 - frame.Height()));
}
