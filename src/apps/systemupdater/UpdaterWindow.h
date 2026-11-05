/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef UPDATER_WINDOW_H
#define UPDATER_WINDOW_H


#include <String.h>
#include <Window.h>

#include "Settings.h"
#include "SystemState.h"


class BButton;
class BCheckBox;
class BColumnListView;
class BGridView;
class BSplitView;
class BLayoutItem;
class BListView;
class BStatusBar;
class BStringView;
class BTabView;
class BTextControl;
class BMessageRunner;
class BTextView;
class InstallWindow;
class UpdateWorker;


class UpdaterWindow : public BWindow {
public:
								UpdaterWindow();
	virtual						~UpdaterWindow();

	virtual	bool				QuitRequested();
	virtual	void				MessageReceived(BMessage* message);

private:
			BView*				_CreateUpdatesTab();
			BView*				_CreateVersionsTab();

			void				_Refresh();
									// what runs, from the disk
			void				_ShowState();
			void				_SetBusy(bool busy, const char* text = NULL);
			void				_SetVisible(BView* view, bool visible);
			void				_StartCheck();
			void				_Restart();
			void				_StartInstall();
			void				_InstallCancelled();
			void				_ShowAvailable(BMessage* message);
			void				_ClearAvailable();
			void				_InstallFinished(BMessage* message);
			void				_ShowFailedInstall();
			void				_SelectVersion();
			const InstalledVersion* _SelectedVersion() const;
			void				_SaveSettings();

private:
	enum Mode {
		kModeIdle,
		kModeChecking,
		kModeAvailable,
		kModeClosingApps,
		kModeInstalling,
		kModeRestart,
		kModeFailed,
		kModeOlderVersion
	};

			BTabView*			fTabView;

			// updates
			BStringView*		fTitleView;
			BTextView*			fSummaryView;
			BGridView*			fDetailsView;
			BStringView*		fNewVersionView;
			BStringView*		fRunningVersionView;
			BStringView*		fDownloadView;
			BSplitView*			fChangesSplit;
			BView*				fPackagesGroup;
			BStringView*		fPackagesLabel;
			BColumnListView*	fPackagesView;
			BView*				fNotesGroup;
			BTextView*			fNotesView;
			bool				fHaveDetails;
			BStatusBar*			fProgressBar;
			BLayoutItem*		fChangesGlue;
			BButton*			fCheckButton;
			BButton*			fCancelButton;
			BButton*			fSecondaryButton;
			BButton*			fMainButton;

			// versions
			BListView*			fVersionsView;
			BTextView*			fVersionInfoView;
			BButton*			fRestartIntoButton;
			BButton*			fGoBackButton;
			BCheckBox*			fAutomaticBox;
			BTextControl*		fFeedControl;

			UpdateWorker*		fWorker;
			InstallWindow*		fInstallWindow;
			bool				fDesktopClosed;
			bool				fInstallCancelRequested;
			BMessageRunner*		fRestartRunner;
			bool				fRestarting;
			SystemState			fState;
			TrialInfo			fTrial;
			LastBoot			fLastBoot;
			Settings			fSettings;
			Mode				fMode;
			bool				fBusy;
			BString				fAvailableId;
			BString				fAvailableTitle;
			BString				fCheckError;
			BString				fLastResult;
};


#endif	// UPDATER_WINDOW_H
