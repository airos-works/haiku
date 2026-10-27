/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "UpdaterWindow.h"

#include <stdio.h>
#include <string.h>

#include <Alert.h>
#include <Application.h>
#include <Button.h>
#include <CheckBox.h>
#include <ControlLook.h>
#include <File.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <MessageRunner.h>
#include <Roster.h>
#include <ScrollView.h>
#include <SeparatorView.h>
#include <StatusBar.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>
#include <TextView.h>

#include <RosterPrivate.h>
#include <airos_boot_control.h>

#include "Constants.h"
#include "InstallWindow.h"
#include "PiBoot.h"
#include "Release.h"
#include "UpdateWorker.h"
#include "Updater.h"


enum {
	kMsgMainButton			= 'mbtn',
	kMsgSecondaryButton		= 'sbtn',
	kMsgCheckButton			= 'cbtn',
	kMsgCancelButton		= 'xbtn',
	kMsgVersionSelected		= 'vsel',
	kMsgRestartInto			= 'rint',
	kMsgGoBackButton		= 'gbtn',
	kMsgAutomaticChanged	= 'auto',
	kMsgFeedChanged			= 'feed',
	kMsgShowFailedInstall	= 'sfin',
	kMsgRestartStalled		= 'rsst'
};


// how long the window with the error of a failed install stays up before
// the restart, and how long a restart may take to begin
static const bigtime_t kFailedRestartDelay = 4000000;
static const bigtime_t kRestartStallDelay = 30000000;


// #pragma mark - VersionItem


class VersionItem : public BListItem {
public:
	VersionItem(const InstalledVersion& version, const BString& detail)
		:
		fState(version.latest ? BString() : version.state),
		fTitle(version.Title()),
		fDetail(detail)
	{
	}

	const BString& State() const
	{
		return fState;
	}

	virtual void Update(BView* owner, const BFont* font)
	{
		BListItem::Update(owner, font);
		font_height height;
		font->GetHeight(&height);
		float line = ceilf(height.ascent + height.descent + height.leading);
		SetHeight(line * 2 + be_control_look->DefaultLabelSpacing() * 2);
	}

	virtual void DrawItem(BView* owner, BRect frame, bool complete)
	{
		rgb_color background = IsSelected()
			? ui_color(B_LIST_SELECTED_BACKGROUND_COLOR)
			: ui_color(B_LIST_BACKGROUND_COLOR);
		rgb_color text = IsSelected()
			? ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR)
			: ui_color(B_LIST_ITEM_TEXT_COLOR);
		owner->SetLowColor(background);
		owner->FillRect(frame, B_SOLID_LOW);

		BFont font;
		owner->GetFont(&font);
		font_height height;
		font.GetHeight(&height);
		float spacing = be_control_look->DefaultLabelSpacing();
		float x = frame.left + spacing;
		float y = frame.top + spacing + ceilf(height.ascent);

		BFont bold(font);
		bold.SetFace(B_BOLD_FACE);
		owner->SetFont(&bold);
		owner->SetHighColor(text);
		owner->DrawString(fTitle, BPoint(x, y));

		owner->SetFont(&font);
		owner->SetHighColor(tint_color(text, IsSelected() ? B_NO_TINT
			: B_DISABLED_LABEL_TINT));
		y += ceilf(height.ascent + height.descent + height.leading);
		owner->DrawString(fDetail, BPoint(x, y));
	}

private:
	BString	fState;
	BString	fTitle;
	BString	fDetail;
};


// #pragma mark - UpdaterWindow


UpdaterWindow::UpdaterWindow()
	:
	BWindow(BRect(100, 100, 700, 600), "System Updater", B_TITLED_WINDOW,
		B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS
			| B_QUIT_ON_WINDOW_CLOSE),
	fWorker(NULL),
	fInstallWindow(NULL),
	fRestartRunner(NULL),
	fRestarting(false),
	fMode(kModeIdle),
	fBusy(false)
{
	fSettings.Load();

	fTabView = new BTabView("tabs", B_WIDTH_FROM_LABEL);
	BTab* tab = new BTab();
	fTabView->AddTab(_CreateUpdatesTab(), tab);
	tab->SetLabel("Updates");
	tab = new BTab();
	fTabView->AddTab(_CreateVersionsTab(), tab);
	tab->SetLabel("Versions");

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.AddStrut(B_USE_HALF_ITEM_SPACING)
		.Add(fTabView)
		.SetInsets(0, B_USE_SMALL_SPACING, 0, 0);

	if (fSettings.windowFrame.IsValid()) {
		MoveTo(fSettings.windowFrame.LeftTop());
		ResizeTo(fSettings.windowFrame.Width(), fSettings.windowFrame.Height());
		MoveOnScreen();
	} else
		CenterOnScreen();

	fWorker = new UpdateWorker(BMessenger(this));
	fWorker->Run();

	_Refresh();
	if (fMode == kModeIdle)
		_StartCheck();

	// an update that could not be installed before the last restart
	BString failedTitle, failedError;
	if (SystemState::ReadFailedInstall(failedTitle, failedError))
		PostMessage(kMsgShowFailedInstall);
}


UpdaterWindow::~UpdaterWindow()
{
	delete fRestartRunner;
	if (fWorker != NULL) {
		fWorker->Cancel();
		fWorker->Lock();
		fWorker->Quit();
	}
}


BView*
UpdaterWindow::_CreateUpdatesTab()
{
	fTitleView = new BStringView("title", "");
	BFont font;
	fTitleView->GetFont(&font);
	font.SetFace(B_BOLD_FACE);
	font.SetSize(font.Size() * 1.3);
	fTitleView->SetFont(&font);
	fTitleView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	fSummaryView = new BTextView("summary");
	fSummaryView->MakeEditable(false);
	fSummaryView->MakeSelectable(false);
	fSummaryView->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	fSummaryView->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
	fSummaryView->SetStylable(false);
	fSummaryView->SetWordWrap(true);
	fSummaryView->SetText(" ");
	fSummaryView->SetExplicitMinSize(BSize(be_plain_font->StringWidth(
		"M") * 40, be_plain_font->Size() * 4));

	fChangesView = new BTextView("changes");
	fChangesView->MakeEditable(false);
	fChangesView->SetWordWrap(true);
	BScrollView* changesScroll = new BScrollView("changes scroll",
		fChangesView, 0, false, true);
	changesScroll->SetExplicitMinSize(BSize(B_SIZE_UNSET,
		be_plain_font->Size() * 12));
	fChangesScroll = changesScroll;
	fChangesLabel = new BStringView("changes label", "What's new:");

	fProgressBar = new BStatusBar("progress");
	fProgressBar->SetMaxValue(1.0);
	fProgressBar->Hide();

	fCheckButton = new BButton("check", "Check Again",
		new BMessage(kMsgCheckButton));
	fCancelButton = new BButton("cancel", "Cancel",
		new BMessage(kMsgCancelButton));
	fSecondaryButton = new BButton("secondary", "",
		new BMessage(kMsgSecondaryButton));
	fMainButton = new BButton("main", "Install and Reboot",
		new BMessage(kMsgMainButton));

	BView* view = new BView("Updates", 0);
	BLayoutBuilder::Group<>(view, B_VERTICAL)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(fTitleView)
		.Add(fSummaryView)
		.Add(fChangesLabel)
		.Add(changesScroll, 1.0)
		.AddGlue(0)
		.Add(fProgressBar)
		.AddGroup(B_HORIZONTAL)
			.Add(fCheckButton)
			.AddGlue()
			.Add(fCancelButton)
			.Add(fSecondaryButton)
			.Add(fMainButton)
		.End();
	return view;
}


BView*
UpdaterWindow::_CreateVersionsTab()
{
	fVersionsView = new BListView("versions");
	fVersionsView->SetSelectionMessage(new BMessage(kMsgVersionSelected));
	BScrollView* versionsScroll = new BScrollView("versions scroll",
		fVersionsView, 0, false, true);
	versionsScroll->SetExplicitMinSize(BSize(B_SIZE_UNSET,
		be_plain_font->Size() * 10));

	fVersionInfoView = new BTextView("version info");
	fVersionInfoView->MakeEditable(false);
	fVersionInfoView->MakeSelectable(false);
	fVersionInfoView->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	fVersionInfoView->SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
	fVersionInfoView->SetWordWrap(true);
	if (PiBoot::IsPi()) {
		fVersionInfoView->SetText("air/OS keeps the current version and the "
			"versions before it, so you can go back if something stops "
			"working. The Raspberry Pi starts the version chosen here; if an "
			"update does not start, it goes back to the version before by "
			"itself.");
	} else {
		fVersionInfoView->SetText("air/OS keeps the current version and the "
			"versions before it, so you can go back if something stops "
			"working. You can also pick one while the computer starts: hold "
			"down Shift or Space to open the boot menu, then choose "
			"\"Select boot volume/state\".");
	}
	fVersionInfoView->SetExplicitMinSize(BSize(B_SIZE_UNSET,
		be_plain_font->Size() * 4));

	fRestartIntoButton = new BButton("restart into",
		"Restart into This Version", new BMessage(kMsgRestartInto));
	fGoBackButton = new BButton("go back", "Go Back to This Version…",
		new BMessage(kMsgGoBackButton));

	fAutomaticBox = new BCheckBox("automatic",
		"Check for updates when the computer starts",
		new BMessage(kMsgAutomaticChanged));
	fAutomaticBox->SetValue(fSettings.checkAutomatically
		? B_CONTROL_ON : B_CONTROL_OFF);

	Updater updater;
	fFeedControl = new BTextControl("feed", "Updates from:", "",
		new BMessage(kMsgFeedChanged));
	fFeedControl->SetText(updater.FeedURL());
	fFeedControl->SetModificationMessage(NULL);

	BView* view = new BView("Versions", 0);
	BLayoutBuilder::Group<>(view, B_VERTICAL)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(new BStringView("versions label", "Installed versions:"))
		.Add(versionsScroll, 1.0)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(fRestartIntoButton)
			.Add(fGoBackButton)
		.End()
		.Add(fVersionInfoView)
		.Add(new BSeparatorView(B_HORIZONTAL))
		.Add(fAutomaticBox)
		.Add(fFeedControl);
	return view;
}


bool
UpdaterWindow::QuitRequested()
{
	if (fRestarting) {
		_SaveSettings();
		return true;
	}
	if (fMode == kModeClosingApps) {
		// the registrar's window has the Cancel button for that
		return false;
	}
	if (fMode == kModeInstalling) {
		BAlert* alert = new BAlert("System Updater",
			"An update is being downloaded or installed. Stop it?",
			"Keep Going", "Stop", NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		if (alert->Go() == 0)
			return false;
		fWorker->Cancel();
	}
	_SaveSettings();
	return true;
}


void
UpdaterWindow::_SaveSettings()
{
	fSettings.windowFrame = Frame();
	fSettings.checkAutomatically = fAutomaticBox->Value() == B_CONTROL_ON;
	fSettings.Save();
}


void
UpdaterWindow::_Refresh()
{
	fState.Load();
	SystemState::ReadTrial(fTrial);
	fLastBoot = SystemState::ReadLastBoot();

	if (!fState.RunningLatest()) {
		const InstalledVersion* running = fState.Running();
		const InstalledVersion* latest = fState.Latest();
		if (!fTrial.exists && running != NULL && latest != NULL
			&& latest->bootable && latest->SameRelease(*running)) {
			// the running version was made the current one again
			fMode = kModeRestart;
		} else if (fLastBoot.mode == AIROS_BOOT_MODE_FALLBACK
			|| fLastBoot.mode == AIROS_BOOT_MODE_RECOVERY) {
			fMode = kModeFailed;
		} else
			fMode = kModeOlderVersion;
	} else if (fState.RestartPending())
		fMode = kModeRestart;
	else if (fMode == kModeRestart || fMode == kModeFailed
		|| fMode == kModeOlderVersion)
		fMode = kModeIdle;

	// the versions list
	BString selected;
	if (const InstalledVersion* version = _SelectedVersion())
		selected = version->state;
	fVersionsView->MakeEmpty();
	BString next = SystemState::NextBoot();
	for (int32 i = 0; const InstalledVersion* version = fState.VersionAt(i);
			i++) {
		// one entry per release: its newest state (package changes of its
		// own, like an application from HaikuDepot, make more of them)
		bool shown = false;
		for (int32 j = 0; j < i; j++) {
			if (fState.VersionAt(j)->SameRelease(*version))
				shown = true;
		}
		if (shown && !version->running)
			continue;

		BString detail;
		if (version->latest)
			detail << "Current version";
		else
			detail << "Installed " << version->StateTitle();
		if (SystemState::IsFailedRelease(version->releaseVersion))
			detail << " · did not start";
		BString revision = version->Revision();
		if (!revision.IsEmpty())
			detail << " · " << revision;
		if (version->running)
			detail << " · running now";
		if (!version->bootable)
			detail << " · cannot start";
		if (fTrial.exists && version->latest && !fTrial.fallback.IsEmpty()
			&& fLastBoot.mode == AIROS_BOOT_MODE_FALLBACK) {
			detail << " · did not start";
		}
		if (!next.IsEmpty() && (next == version->state
				|| (next == "latest" && version->latest))) {
			detail << " · starts next";
		}
		VersionItem* item = new VersionItem(*version, detail);
		fVersionsView->AddItem(item);
		if (version->state == selected)
			fVersionsView->Select(fVersionsView->CountItems() - 1);
	}
	_SelectVersion();
	_ShowState();
}


void
UpdaterWindow::_ShowState()
{
	Updater updater;
	Release installed;
	BString running;
	const InstalledVersion* runningVersion = fState.Running();
	if (updater.LoadInstalledRelease(installed) == B_OK) {
		running.SetToFormat("%s (%s, %s)", installed.Title().String(),
			installed.ShortRevision().String(), installed.Target().String());
	} else if (runningVersion != NULL)
		running = runningVersion->Title();

	BString title, summary, main, secondary;
	bool showMain = false, showSecondary = false;

	switch (fMode) {
		case kModeIdle:
			title = "air/OS is up to date";
			summary << "This computer runs " << running << ".";
			if (!fCheckError.IsEmpty()) {
				title = "Cannot check for updates";
				summary << "\n" << fCheckError;
			}
			if (!fLastResult.IsEmpty())
				summary << "\n" << fLastResult;
			break;
		case kModeChecking:
			title = "Checking for updates…";
			summary << "This computer runs " << running << ".";
			break;
		case kModeAvailable:
			title = "An update is available";
			summary << fAvailableTitle << " is available. This computer runs "
				<< running << ".\nInstall and Reboot first closes your "
				"applications, so that each can ask you to save your work, "
				"then installs the update and restarts the computer. The "
				"version you run now stays installed.";
			if (!fLastResult.IsEmpty())
				summary << "\n" << fLastResult;
			main = "Install and Reboot";
			showMain = true;
			break;
		case kModeClosingApps:
			title = "Closing applications…";
			summary << "Each application may ask you to save your work. If "
				"one of them stays open, nothing is installed.";
			break;
		case kModeInstalling:
			title = "Installing the update…";
			summary << "The computer restarts when the update is installed.";
			break;
		case kModeRestart:
			if (!fState.RunningLatest()) {
				title = "Restart to finish";
				summary << "This version is the current one again from the "
					"next restart.";
			} else {
				title = "Restart to finish the update";
				summary << "The new version is installed and starts with the "
					"next restart. If it does not start, air/OS goes back to "
					"the version you run now by itself.";
			}
			main = "Restart Now";
			showMain = true;
			break;
		case kModeFailed:
			title = "The update did not start";
			if (fState.Latest() != NULL && !fState.Latest()->bootable) {
				// a package transaction cut short: repair, then update again
				summary << "Installing the update was cut short, so air/OS "
					"started the version before it";
				if (runningVersion != NULL)
					summary << ", " << runningVersion->Title();
				summary << ". Repair makes this version the current one "
					"again; then the update can be installed once more.";
				main = "Repair";
				showMain = true;
				break;
			}
			if (fLastBoot.mode == AIROS_BOOT_MODE_RECOVERY) {
				summary << "Installing the update was cut short, so air/OS "
					"started the version before it, ";
			} else {
				summary << "The new version did not reach the desktop, so "
					"air/OS started the version before it, ";
			}
			if (runningVersion != NULL)
				summary << runningVersion->Title();
			summary << ". You can try the update once more, or remove it and "
				"keep this version.";
			main = "Try Again";
			secondary = "Remove the Update";
			showMain = showSecondary = true;
			break;
		case kModeOlderVersion:
			title = "An older version runs";
			summary << "This start runs ";
			if (runningVersion != NULL)
				summary << runningVersion->Title();
			summary << ". The next start is the current version again, "
				"unless you keep this one.";
			main = "Restart into the Current Version";
			secondary = "Keep This Version";
			showMain = showSecondary = true;
			break;
	}

	fTitleView->SetText(title);
	fSummaryView->SetText(summary);
	fMainButton->SetLabel(main);
	fSecondaryButton->SetLabel(secondary);

	bool showChanges = fMode == kModeIdle || fMode == kModeChecking
		|| fMode == kModeAvailable || fMode == kModeClosingApps
		|| fMode == kModeInstalling;
	_SetVisible(fChangesLabel, showChanges);
	_SetVisible(fChangesScroll, showChanges);
	_SetVisible(fMainButton, showMain);
	_SetVisible(fSecondaryButton, showSecondary);
	_SetVisible(fCancelButton, fMode == kModeInstalling
		|| fMode == kModeChecking);
	fMainButton->SetEnabled(!fBusy);
	fSecondaryButton->SetEnabled(!fBusy);
	fCheckButton->SetEnabled(!fBusy && fState.RunningLatest()
		&& fMode != kModeRestart);
	// Not the default button: Enter in the Versions tab's text field would
	// close the applications and install, or restart.
	fMainButton->MakeDefault(false);
	_SelectVersion();
}


void
UpdaterWindow::_SetBusy(bool busy, const char* text)
{
	fBusy = busy;
	if (busy) {
		fProgressBar->Reset();
		fProgressBar->SetText(text);
	}
	_SetVisible(fProgressBar, busy);
	_ShowState();
}


/*!	Shows or hides \a view, looking at its own state only: before the window
	is shown, IsHidden() says every view is hidden.
*/
void
UpdaterWindow::_SetVisible(BView* view, bool visible)
{
	if (visible && view->IsHidden(view))
		view->Show();
	else if (!visible && !view->IsHidden(view))
		view->Hide();
}


void
UpdaterWindow::_StartCheck()
{
	if (!fState.RunningLatest() || fState.RestartPending())
		return;
	fMode = kModeChecking;
	fCheckError.Truncate(0);
	_SetBusy(true, "Asking the build server…");
	fWorker->PostMessage(kMsgCheck);
}


void
UpdaterWindow::_Restart()
{
	_SaveSettings();
	fRestarting = true;
	// the user has just asked for it: no second question (each application
	// is still asked to quit, and may ask to save its work)
	status_t error = BRoster::Private().ShutDown(true, false, false);
	if (error != B_OK) {
		fRestarting = false;
		(new BAlert("System Updater", BString().SetToFormat(
			"The computer cannot restart: %s", strerror(error)), "OK", NULL,
			NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT))->Go(NULL);
		if (fInstallWindow != NULL && fInstallWindow->Lock()) {
			fInstallWindow->SetRestartStalled();
			fInstallWindow->Unlock();
		}
		return;
	}
	if (fInstallWindow != NULL) {
		// an application that would not quit stops the restart, and nobody
		// tells us: offer to try again after a while
		delete fRestartRunner;
		BMessage stalled(kMsgRestartStalled);
		fRestartRunner = new BMessageRunner(BMessenger(this), &stalled,
			kRestartStallDelay, 1);
	}
}


/*!	The applications are closed: download and install in the window that
	stays in front, then restart.
*/
void
UpdaterWindow::_StartInstall()
{
	fMode = kModeInstalling;
	_SetBusy(true, "Preparing the update…");
	if (fInstallWindow == NULL) {
		fInstallWindow = new InstallWindow(fAvailableTitle,
			BMessenger(this));
		fInstallWindow->Show();
	}
	Updater::Log("installing %s after closing the applications",
		fAvailableId.String());
	BMessage install(kMsgInstall);
	install.AddString("id", fAvailableId);
	fWorker->PostMessage(&install);
}


void
UpdaterWindow::_InstallFinished(BMessage* message)
{
	_SetBusy(false);
	const char* error = message->GetString("error", NULL);
	if (message->GetBool("cancelled", false)) {
		// the user stopped it: nothing changed, no restart
		Updater::Log("install of %s cancelled", fAvailableId.String());
		if (fInstallWindow != NULL && fInstallWindow->Lock()) {
			fInstallWindow->Quit();
			fInstallWindow = NULL;
		}
		fMode = kModeAvailable;
		fLastResult = "The update was stopped; nothing was changed.";
		_Refresh();
		return;
	}

	if (error != NULL) {
		// the applications are closed already: restart all the same, and
		// say what happened once the computer is back
		BString title = message->GetString("title", fAvailableTitle);
		Updater::Log("could not install %s: %s; restarting as planned",
			fAvailableId.String(), error);
		SystemState::RecordFailedInstall(title, error);
		if (fInstallWindow != NULL && fInstallWindow->Lock()) {
			fInstallWindow->SetRestarting("The update could not be "
				"installed. Restarting…");
			fInstallWindow->Unlock();
		}
		delete fRestartRunner;
		BMessage restart(kMsgRestartNow);
		fRestartRunner = new BMessageRunner(BMessenger(this), &restart,
			kFailedRestartDelay, 1);
		return;
	}

	_Refresh();
	if (fInstallWindow != NULL && fInstallWindow->Lock()) {
		fInstallWindow->SetRestarting("Restarting…");
		fInstallWindow->Unlock();
	}
	_Restart();
}


void
UpdaterWindow::_ShowFailedInstall()
{
	BString title, error;
	if (!SystemState::ReadFailedInstall(title, error))
		return;
	SystemState::ClearFailedInstall();

	BString text;
	text.SetToFormat("%s could not be installed before the computer "
		"restarted:\n\n%s\n\nNothing was changed: this computer runs the "
		"version it ran before. You can try the update again.",
		title.IsEmpty() ? "The update" : title.String(), error.String());
	BAlert* alert = new BAlert("System Updater", text, "OK", NULL, NULL,
		B_WIDTH_AS_USUAL, B_STOP_ALERT);
	alert->Go(NULL);
}


const InstalledVersion*
UpdaterWindow::_SelectedVersion() const
{
	VersionItem* item = dynamic_cast<VersionItem*>(
		fVersionsView->ItemAt(fVersionsView->CurrentSelection()));
	if (item == NULL)
		return NULL;
	return fState.FindState(item->State());
}


void
UpdaterWindow::_SelectVersion()
{
	const InstalledVersion* version = _SelectedVersion();
	bool canRestartInto = version != NULL && version->bootable && !fBusy
		&& (!version->running || (version->latest && !fState.RunningLatest()));
	fRestartIntoButton->SetEnabled(canRestartInto && !version->running);
	fGoBackButton->SetEnabled(version != NULL && version->bootable
		&& !version->latest && !fBusy && fState.RunningLatest()
		&& !fState.RestartPending() && !fTrial.exists);
}


void
UpdaterWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgStatus:
		{
			const char* text = message->GetString("text", "");
			float fraction = message->GetFloat("fraction", -1);
			if (fraction >= 0) {
				fProgressBar->SetTo(fraction, text);
			} else
				fProgressBar->SetText(text);
			if (fInstallWindow != NULL && fInstallWindow->Lock()) {
				fInstallWindow->SetStatus(text, fraction);
				fInstallWindow->Unlock();
			}
			break;
		}

		case kMsgCloseAppsDone:
		{
			_SetBusy(false);
			status_t error = message->GetInt32("error", B_ERROR);
			if (error == B_OK) {
				_StartInstall();
				break;
			}
			fMode = kModeAvailable;
			_ShowState();
			BString app = message->GetString("app", "");
			BString text;
			if (error == B_SHUTDOWN_CANCELLED && app.IsEmpty()) {
				// the user pressed Cancel
				Updater::Log("closing the applications was cancelled");
				break;
			} else if (error == B_SHUTDOWN_CANCELLED) {
				Updater::Log("%s did not quit; nothing installed",
					app.String());
				text.SetToFormat("\"%s\" did not close, so the update was "
					"not installed. Save your work there, then try again.",
					app.String());
			} else {
				text.SetToFormat("The applications could not be closed (%s), "
					"so the update was not installed.", strerror(error));
			}
			(new BAlert("System Updater", text, "OK", NULL, NULL,
				B_WIDTH_AS_USUAL, B_INFO_ALERT))->Go(NULL);
			break;
		}

		case kMsgInstallCommitting:
			if (fInstallWindow != NULL && fInstallWindow->Lock()) {
				fInstallWindow->SetCommitting();
				fInstallWindow->Unlock();
			}
			break;

		case kMsgCancelInstall:
			fWorker->Cancel();
			break;

		case kMsgRestartNow:
			_Restart();
			break;

		case kMsgRestartStalled:
		{
			bool inProgress = false;
			if (BRoster::Private().IsShutDownInProgress(&inProgress) == B_OK
				&& inProgress) {
				// still asking the applications
				delete fRestartRunner;
				BMessage stalled(kMsgRestartStalled);
				fRestartRunner = new BMessageRunner(BMessenger(this),
					&stalled, kRestartStallDelay, 1);
				break;
			}
			fRestarting = false;
			if (fInstallWindow != NULL && fInstallWindow->Lock()) {
				fInstallWindow->SetRestartStalled();
				fInstallWindow->Unlock();
			}
			break;
		}

		case kMsgShowFailedInstall:
			_ShowFailedInstall();
			break;

		case kMsgCheckDone:
		{
			_SetBusy(false);
			const char* error = message->GetString("error", NULL);
			if (error != NULL) {
				fMode = kModeIdle;
				fCheckError = error;
				fChangesView->SetText("");
			} else if (message->GetBool("available", false)) {
				fMode = kModeAvailable;
				fAvailableId = message->GetString("id", "");
				fAvailableTitle = message->GetString("title", "");
				BString changes = message->GetString("changes", "");
				int64 size = message->GetInt64("size", -1);
				int32 packages = message->GetInt32("packages", -1);
				if (packages >= 0) {
					changes.Prepend(BString().SetToFormat(
						"Download: %s (%" B_PRId32 " package%s)\n\n",
						Updater::FormatSize(size).String(), packages,
						packages == 1 ? "" : "s"));
				} else if (size > 0) {
					changes.Prepend(BString().SetToFormat(
						"Download: up to %s\n\n",
						Updater::FormatSize(size).String()));
				}
				fChangesView->SetText(changes);
			} else {
				fMode = kModeIdle;
				fChangesView->SetText("No newer release.");
			}
			_ShowState();
			break;
		}

		case kMsgInstallDone:
			_InstallFinished(message);
			break;

		case kMsgGoBackDone:
		case kMsgRemoveFailedDone:
		{
			_SetBusy(false);
			const char* error = message->GetString("error", NULL);
			if (error != NULL) {
				BAlert* alert = new BAlert("System Updater",
					BString().SetToFormat("That did not work: %s\n\nNothing "
						"was changed.", error),
					"OK", NULL, NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
				alert->Go(NULL);
				_Refresh();
			} else {
				_Refresh();
				BAlert* alert = new BAlert("System Updater",
					"Done. The change takes effect when the computer "
					"restarts.", "Restart Later", "Restart Now", NULL,
					B_WIDTH_AS_USUAL, B_INFO_ALERT);
				if (alert->Go() == 1)
					_Restart();
			}
			break;
		}

		case kMsgCheckButton:
			_StartCheck();
			break;

		case kMsgCancelButton:
			fWorker->Cancel();
			break;

		case kMsgMainButton:
			switch (fMode) {
				case kModeAvailable:
					// the applications first: each may ask to save its work
					// or refuse, which stops it all before anything changed
					fMode = kModeClosingApps;
					fLastResult.Truncate(0);
					_SetBusy(true, "Closing applications…");
					fWorker->PostMessage(kMsgCloseApps);
					break;
				case kModeRestart:
					_Restart();
					break;
				case kModeFailed:
				{
					if (fState.Latest() != NULL && !fState.Latest()->bootable) {
						_SetBusy(true, "Repairing…");
						fWorker->PostMessage(kMsgRemoveFailed);
						break;
					}
					SystemState::ResetTrialAttempts();
					SystemState::CancelNextBoot();
					Updater::Log("asked to try %s again",
						fTrial.release.String());
					BString report;
					Updater().SyncBootFiles(report);
					_Restart();
					break;
				}
				case kModeOlderVersion:
				{
					SystemState::CancelNextBoot();
					BString report;
					Updater().SyncBootFiles(report);
					_Restart();
					break;
				}
				default:
					break;
			}
			break;

		case kMsgSecondaryButton:
			if (fMode == kModeFailed) {
				BAlert* alert = new BAlert("System Updater",
					"Remove the update that did not start? The version that "
					"runs now becomes the current version again. The update "
					"stays in the versions list until it is cleaned up.",
					"Cancel", "Remove", NULL, B_WIDTH_AS_USUAL,
					B_WARNING_ALERT);
				alert->SetShortcut(0, B_ESCAPE);
				if (alert->Go() == 1) {
					_SetBusy(true, "Removing the update…");
					fWorker->PostMessage(kMsgRemoveFailed);
				}
			} else if (fMode == kModeOlderVersion) {
				BAlert* alert = new BAlert("System Updater",
					"Keep this older version? It becomes the current version; "
					"the newer one stays installed, so you can go forward "
					"again from the Versions tab.",
					"Cancel", "Keep This Version", NULL, B_WIDTH_AS_USUAL,
					B_WARNING_ALERT);
				alert->SetShortcut(0, B_ESCAPE);
				if (alert->Go() == 1) {
					_SetBusy(true, "Making this version the current one…");
					fWorker->PostMessage(kMsgRemoveFailed);
				}
			}
			break;

		case kMsgVersionSelected:
			_SelectVersion();
			break;

		case kMsgRestartInto:
		{
			const InstalledVersion* version = _SelectedVersion();
			if (version == NULL)
				break;
			BString text;
			text.SetToFormat("Restart into %s? It runs for this one start; "
				"the next start is the current version again.",
				version->Title().String());
			BAlert* alert = new BAlert("System Updater", text, "Cancel",
				"Restart", NULL, B_WIDTH_AS_USUAL, B_INFO_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			if (alert->Go() != 1)
				break;
			status_t error = SystemState::RequestNextBoot(version->state);
			if (error != B_OK) {
				(new BAlert("System Updater", BString().SetToFormat(
					"Cannot ask the boot loader: %s", strerror(error)), "OK"))
						->Go(NULL);
				break;
			}
			Updater::Log("asked to start %s once", version->latest
				? "the latest state" : version->state.String());
			BString report;
			Updater().SyncBootFiles(report);
			_Restart();
			break;
		}

		case kMsgGoBackButton:
		{
			const InstalledVersion* version = _SelectedVersion();
			if (version == NULL)
				break;
			BString text;
			text.SetToFormat("Go back to %s? It becomes the current version "
				"when the computer restarts. The version you run now stays "
				"installed, so you can go forward again.",
				version->Title().String());
			BAlert* alert = new BAlert("System Updater", text, "Cancel",
				"Go Back", NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			alert->SetShortcut(0, B_ESCAPE);
			if (alert->Go() != 1)
				break;
			_SetBusy(true, "Going back…");
			BMessage goBack(kMsgGoBack);
			goBack.AddString("state", version->state);
			fWorker->PostMessage(&goBack);
			break;
		}

		case kMsgAutomaticChanged:
			_SaveSettings();
			break;

		case kMsgFeedChanged:
		{
			Updater defaults;
			BString feed = fFeedControl->Text();
			feed.Trim();
			fSettings.feed = feed;
			if (feed.IsEmpty())
				fFeedControl->SetText(defaults.FeedURL());
			_SaveSettings();
			_StartCheck();
			break;
		}

		case B_PACKAGE_UPDATE:
			// another program changed the packages
			_Refresh();
			break;

		default:
			BWindow::MessageReceived(message);
	}
}
