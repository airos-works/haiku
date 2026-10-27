/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "InstallWindow.h"

#include <Button.h>
#include <LayoutBuilder.h>
#include <StatusBar.h>
#include <StringView.h>
#include <TextView.h>


InstallWindow::InstallWindow(const char* title, const BMessenger& target)
	:
	BWindow(BRect(0, 0, 400, 150), "Installing the update",
		B_TITLED_WINDOW_LOOK, B_MODAL_ALL_WINDOW_FEEL,
		B_NOT_CLOSABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE
			| B_NOT_RESIZABLE | B_AUTO_UPDATE_SIZE_LIMITS
			| B_ASYNCHRONOUS_CONTROLS, B_ALL_WORKSPACES),
	fTarget(target)
{
	BString heading;
	heading << "Installing " << title;
	fTitleView = new BStringView("title", heading);
	BFont font;
	fTitleView->GetFont(&font);
	font.SetFace(B_BOLD_FACE);
	font.SetSize(font.Size() * 1.2);
	fTitleView->SetFont(&font);
	fTitleView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	fTextView = new BTextView("text");
	fTextView->MakeEditable(false);
	fTextView->MakeSelectable(false);
	fTextView->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	fTextView->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
	fTextView->SetWordWrap(true);
	fTextView->SetText("Your applications are closed. The computer restarts "
		"once the update is installed; the version you run now stays "
		"installed, and air/OS goes back to it by itself if the new one "
		"does not start.");
	fTextView->SetExplicitMinSize(BSize(be_plain_font->StringWidth("M") * 32,
		be_plain_font->Size() * 4));

	fProgressBar = new BStatusBar("progress");
	fProgressBar->SetMaxValue(1.0);
	fProgressBar->SetText("Preparing the update…");

	fCancelButton = new BButton("cancel", "Cancel",
		new BMessage(kMsgCancelInstall));
	fCancelButton->SetTarget(fTarget);
	fRestartButton = new BButton("restart", "Restart",
		new BMessage(kMsgRestartNow));
	fRestartButton->SetTarget(fTarget);
	fRestartButton->Hide();

	BLayoutBuilder::Group<>(this, B_VERTICAL)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(fTitleView)
		.Add(fTextView)
		.Add(fProgressBar)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fCancelButton)
			.Add(fRestartButton)
		.End();

	CenterOnScreen();
}


bool
InstallWindow::QuitRequested()
{
	// not closable: only the application quits it, as the computer restarts
	return true;
}


void
InstallWindow::SetStatus(const char* text, float fraction)
{
	if (fraction >= 0)
		fProgressBar->SetTo(fraction, text);
	else
		fProgressBar->SetText(text);
}


void
InstallWindow::SetCommitting()
{
	fCancelButton->SetEnabled(false);
}


void
InstallWindow::SetRestarting(const char* text)
{
	fCancelButton->SetEnabled(false);
	fProgressBar->SetTo(1.0, text);
}


void
InstallWindow::SetRestartStalled()
{
	fTextView->SetText("The computer did not restart: an application that "
		"is still open may have stopped it. Close it, then restart.");
	if (!fCancelButton->IsHidden(fCancelButton))
		fCancelButton->Hide();
	if (fRestartButton->IsHidden(fRestartButton))
		fRestartButton->Show();
	fRestartButton->MakeDefault(true);
}
