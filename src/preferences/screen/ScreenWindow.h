/*
 * Copyright 2001-2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Stefano Ceccherini, burton666@libero.it
 *		Axel Dörfler, axeld@pinc-software.de
 *		Thomas Kurschel
 *		Rafael Romo
 *		John Scipione, jscipione@gmail.com
 */
#ifndef SCREEN_WINDOW_H
#define SCREEN_WINDOW_H


#include <Window.h>

#include "DisplayLayoutState.h"
#include "ScreenMode.h"


class BButton;
class BCheckBox;
class BMenuField;
class BMenuItem;
class BMessageRunner;
class BPopUpMenu;
class BSlider;
class BSpinner;
class BStringView;

class DisplayArrangementView;
class ScreenSettings;


class ScreenWindow : public BWindow {
public:
								ScreenWindow(ScreenSettings* settings);
	virtual						~ScreenWindow();

	virtual	bool				QuitRequested();
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				WorkspaceActivated(int32 workspace,
									bool state);
	virtual	void				WindowActivated(bool active);
	virtual	void				ScreenChanged(BRect frame, color_space mode);

private:
	// building the UI
			BView*				_BuildDetailsPanel();
			void				_BuildFallbackMenus();
			void				_BuildResolutionMenu(
									const std::vector<display_mode_entry>&
										resolutions);
			void				_BuildSupportedColorSpaces();

	// display layout path
			void				_LoadLayout();
			void				_ReloadLayout(bool resetPending);
			void				_SelectDisplay(int32 id);
			display_state*		_SelectedDisplay();
			void				_UpdateArrangementView();
			void				_UpdateDetails();
			void				_UpdateLayoutMenus(
									const display_state& display);
			void				_UpdateInfo(const display_state& display);
			void				_UpdateTitle(const display_state& display);
			void				_ApplyLayout();
			void				_ApplyLayoutState(
									const DisplayLayoutState& state,
									bool showAlert);
			void				_IdentifyDisplays();
			void				_UpdateMirrorBox();
			void				_LaunchBackgrounds();

	// classic single screen path
			void				_CheckResolutionMenu();
			void				_CheckColorMenu();
			void				_CheckRefreshMenu();
			void				_UpdateActiveMode();
			void				_UpdateActiveMode(int32 workspace);
			void				_UpdateRefreshControl();
			void				_UpdateFallbackControls();
			void				_UpdateOriginal();
			void				_UpdateColorLabel();
			void				_ApplyMode();
			status_t			_WriteVesaModeFile(
									const screen_mode& mode) const;

	// both
			void				_UpdateWorkspaceButtons();
			void				_CheckApplyEnabled();
			void				_ShowError(const char* format,
									status_t status);
			bool				_ScaleChanged() const;

			bool				_IsVesa() const { return fIsVesa; }

private:
			ScreenSettings*		fSettings;
			bool				fIsVesa;
			bool				fHasLayout;
			bool				fUndoIsScale;
									// the last apply changed the scale of
									// the single display, not its mode
			bool				fUndoScaleAndMode;
									// ... or both of them
			bool				fBootWorkspaceApplied;

			DisplayLayoutState	fCurrentLayout;
				// what the app_server has right now
			DisplayLayoutState	fPendingLayout;
				// what the user is editing
			DisplayLayoutState	fOriginalLayout;
				// for Revert: as it was when opened or last kept
			DisplayLayoutState	fUndoLayout;
				// for the countdown: as it was before Apply
			int32				fSelectedID;
			BMessageRunner*		fReloadRunner;

			DisplayArrangementView* fArrangementView;
			BButton*			fIdentifyButton;
			BButton*			fBackgroundsButton;
			BCheckBox*			fZoomBox;
			BCheckBox*			fMirrorBox;
			BCheckBox*			fDarkModeBox;
			bool				fOriginalDarkMode;

			BStringView*		fTitleView;
			BStringView*		fSubtitleView;

			BMenuField*			fWorkspaceField;
			BMenuItem*			fAllWorkspacesItem;

			BPopUpMenu*			fResolutionMenu;
			BMenuField*			fResolutionField;
			bool				fResolutionMatrix;
			BPopUpMenu*			fColorsMenu;
			BMenuField*			fColorsField;
			BPopUpMenu*			fRefreshMenu;
			BMenuField*			fRefreshField;
			BMenuItem*			fOtherRefresh;
			BPopUpMenu*			fScaleMenu;
			BMenuField*			fScaleField;

			BPopUpMenu*			fCombineMenu;
			BMenuField*			fCombineField;
			BPopUpMenu*			fSwapDisplaysMenu;
			BMenuField*			fSwapDisplaysField;
			BPopUpMenu*			fUseLaptopPanelMenu;
			BMenuField*			fUseLaptopPanelField;
			BPopUpMenu*			fTVStandardMenu;
			BMenuField*			fTVStandardField;

			BCheckBox*			fEnabledBox;
			BCheckBox*			fPrimaryBox;

			BStringView*		fConnectorLabel;
			BStringView*		fConnectorView;
			BStringView*		fSerialLabel;
			BStringView*		fSerialView;
			BStringView*		fManufacturedLabel;
			BStringView*		fManufacturedView;
			BStringView*		fSizeLabel;
			BStringView*		fSizeView;
			BStringView*		fDeviceLabel;
			BStringView*		fDeviceInfo;

			BSlider*			fBrightnessSlider;
			BSpinner*			fColumnsControl;
			BSpinner*			fRowsControl;

			BButton*			fDefaultsButton;
			BButton*			fApplyButton;
			BButton*			fRevertButton;

			uint32				fSupportedColorSpaces;
			BMenuItem*			fUserSelectedColorSpace;

			ScreenMode			fScreenMode;
			ScreenMode			fUndoScreenMode;
				// screen modes for all workspaces

			screen_mode			fActive, fSelected, fOriginal;
				// screen modes for the current workspace

			uint32				fOriginalWorkspacesColumns;
			uint32				fOriginalWorkspacesRows;
			float				fOriginalBrightness;
			bool				fModified;
};

#endif	/* SCREEN_WINDOW_H */
