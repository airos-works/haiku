/*
 * Copyright 2007-2025 Haiku, Inc. All rights reserved.
 * Copyright (c) 2004 Daniel Furrer <assimil8or@users.sourceforge.net>
 * Copyright (c) 2003-2004 Kian Duffy <myob@users.sourceforge.net>
 * Copyright (C) 1998,99 Kazuho Okui and Takashi Murai.
 *
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *		Kian Duffy, myob@users.sourceforge.net
 *		Daniel Furrer, assimil8or@users.sourceforge.net
 *		John Scipione, jscipione@gmail.com
 *		Simon South, simon@simonsouth.net
 *		Siarzhuk Zharski, zharik@gmx.li
 */


#include "TermWindow.h"

#include <algorithm>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <map>
#include <regex>

#include <Alert.h>
#include <Beep.h>
#include <Application.h>
#include <Catalog.h>
#include <CharacterSet.h>
#include <CharacterSetRoster.h>
#include <Clipboard.h>
#include <ColorListView.h>
#include <ControlLook.h>
#include <Dragger.h>
#include <File.h>
#include <FindDirectory.h>
#include <Keymap.h>
#include <LayoutBuilder.h>
#include <LayoutUtils.h>
#include <Locale.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <ObjectList.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <PrintJob.h>
#include <Rect.h>
#include <Roster.h>
#include <Screen.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <String.h>
#include <UTF8.h>
#include <UnicodeChar.h>

#include <AutoLocker.h>
#include <Json.h>

#include "ActiveProcessInfo.h"
#include "Arguments.h"
#include "AppearPrefView.h"
#include "Colors.h"
#include "Globals.h"
#include "PaneView.h"
#include "PrefWindow.h"
#include "PrefHandler.h"
#include "SetTitleDialog.h"
#include "ShellInfo.h"
#include "ShellParameters.h"
#include "TermConst.h"
#include "TermCrypto.h"
#include "TermScrollView.h"
#include "ThemeWindow.h"
#include "ThemeView.h"
#include "TitlePlaceholderMapper.h"


const static int32 kTermViewOffset = 3;

const static int32 kMinimumFontSize = 8;
const static int32 kMaximumFontSize = 36;

// messages constants
static const uint32 kNewTab = 'NTab';
static const uint32 kCloseView = 'ClVw';
static const uint32 kCloseOtherViews = 'CloV';
static const uint32 kIncreaseFontSize = 'InFs';
static const uint32 kDecreaseFontSize = 'DcFs';
static const uint32 kSetActiveTab = 'STab';
static const uint32 kUpdateTitles = 'UPti';
static const uint32 kEditTabTitle = 'ETti';
static const uint32 kEditWindowTitle = 'EWti';
static const uint32 kTabTitleChanged = 'TTch';
static const uint32 kWindowTitleChanged = 'WTch';
static const uint32 kUpdateSwitchTerminalsMenuItem = 'Ustm';
static const uint32 kSplitSideBySide = 'SPsb';
static const uint32 kSplitTopAndBottom = 'SPtb';
static const uint32 kClosePane = 'CPan';
static const uint32 kNextPane = 'NPan';
static const uint32 kPreviousPane = 'PPan';
static const uint32 kSelectPane = 'SPan';
static const uint32 kMaximizePane = 'MPan';
static const uint32 kEqualizePanes = 'EPan';
static const uint32 kFindNext = 'FNxt';
static const uint32 kFindPrevious = 'FPrv';
static const uint32 kFindSelection = 'FSel';
static const uint32 kAlertOnNextMark = 'AoNm';

using namespace BPrivate ; // BCharacterSet stuff

#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Terminal TermWindow"

// actually an arrow
#define UTF8_ENTER "\xe2\x86\xb5"


// #pragma mark - TermViewContainerView


class TermViewContainerView : public BView {
public:
	TermViewContainerView(TermView* termView)
		:
		BView(BRect(), "term view container", B_FOLLOW_ALL, 0),
		fTermView(termView)
	{
		termView->MoveTo(kTermViewOffset, kTermViewOffset);
		BRect frame(termView->Frame());
		ResizeTo(frame.right + kTermViewOffset, frame.bottom + kTermViewOffset);
		AddChild(termView);
	}

	TermView* GetTermView() const	{ return fTermView; }

	virtual void GetPreferredSize(float* _width, float* _height)
	{
		float width, height;
		fTermView->GetPreferredSize(&width, &height);
		*_width = width + 2 * kTermViewOffset;
		*_height = height + 2 * kTermViewOffset;
	}

private:
	TermView*	fTermView;
};


// #pragma mark - SessionID


TermWindow::SessionID::SessionID(int32 id)
	:
	fID(id)
{
}


TermWindow::SessionID::SessionID(const BMessage& message, const char* field)
{
	if (message.FindInt32(field, &fID) != B_OK)
		fID = -1;
}


status_t
TermWindow::SessionID::AddToMessage(BMessage& message, const char* field) const
{
	return message.AddInt32(field, fID);
}


// #pragma mark - Session


struct TermWindow::Session {
	SessionID				id;
	int32					index;
	Title					title;
	PaneView*				paneView;
	TermViewContainerView*	containerView;
		// the active pane
	std::map<TermView*, BString> paneTitles;
		// the titles programs gave their panes
	bool					titleFromPane;
		// the tab's title is a pane's, not one the user gave it

	Session(SessionID id, int32 index, TermViewContainerView* containerView)
		:
		id(id),
		index(index),
		paneView(NULL),
		containerView(containerView),
		titleFromPane(false)
	{
		title.title = B_TRANSLATE("Shell ");
		title.title << index;
		title.patternUserDefined = false;
	}
};


// #pragma mark - TermWindow


TermWindow::TermWindow(const Arguments& args)
	:
	BWindow(BRect(0, 0, 0, 0), args.Title(), B_DOCUMENT_WINDOW,
		B_CURRENT_WORKSPACE | B_QUIT_ON_WINDOW_CLOSE),
	fTitleUpdateRunner(this, BMessage(kUpdateTitles), 1000000),
	fNextSessionID(0),
	fTabView(NULL),
	fMenuBar(NULL),
	fSwitchTerminalsMenuItem(NULL),
	fEncodingMenu(NULL),
	fPrintSettings(NULL),
	fPrefWindow(NULL),
	fThemeWindow(NULL),
	fSavedFrame(0, 0, -1, -1),
	fSetWindowTitleDialog(NULL),
	fSetTabTitleDialog(NULL),
	fIncreaseFontSizeMenuItem(NULL),
	fDecreaseFontSizeMenuItem(NULL),
	fCloseViewMenuItem(NULL),
	fPanesMenu(NULL),
	fMaximizePaneMenuItem(NULL),
	fAlertOnNextMarkMenuItem(NULL),
	fFullScreen(false)
{
	// register this terminal
	fTerminalRoster.Register(Team(), this);
	fTerminalRoster.SetListener(this);
	int32 id = fTerminalRoster.ID();

	// fetch the current keymap
	get_key_map(&fKeymap, &fKeymapChars);

	// apply the title settings
	fTitle.pattern = args.Title();
	if (fTitle.pattern.Length() == 0) {
		fTitle.pattern = B_TRANSLATE_SYSTEM_NAME("Terminal");

		if (id >= 0)
			fTitle.pattern << " " << id + 1;

		fTitle.patternUserDefined = false;
	} else
		fTitle.patternUserDefined = true;

	fTitle.title = fTitle.pattern;
	fTitle.pattern = args.Title();

	_TitleSettingsChanged();

	// get the saved window position and workspaces
	BRect frame;
	uint32 workspaces;
	if (_LoadWindowPosition(&frame, &workspaces) == B_OK) {
		// make sure the window is still on screen
		// (for example if there was a resolution change)
		BRect screenFrame = BScreen(this).Frame();
		if (frame.Width() <= screenFrame.Width()
			&& frame.Height() <= screenFrame.Height())
			ResizeTo(frame.Width(), frame.Height());

		MoveTo(frame.LeftTop());
		MoveOnScreen(B_MOVE_IF_PARTIALLY_OFFSCREEN);

		SetWorkspaces(workspaces);
	} else {
		// use computed defaults
		int row = id / 16;
		int column = id % 16;
		int x = (column * 16) + (row * 64) + 50;
		int y = (column * 16) + 50;

		MoveTo(x, y);
	}

	// init the GUI and add a tab
	_InitWindow();
	_AddTab(&args, args.WorkingDir());

	// Announce our window as no longer minimized. That's not true, since it's
	// still hidden at this point, but it will be shown very soon.
	fTerminalRoster.SetWindowInfo(false, Workspaces());
}


TermWindow::~TermWindow()
{
	fTerminalRoster.Unregister();

	_FinishTitleDialog();

	if (fPrefWindow)
		fPrefWindow->PostMessage(B_QUIT_REQUESTED);

	PrefHandler::DeleteDefault();

	for (int32 i = 0; Session* session = _SessionAt(i); i++)
		delete session;

	delete fKeymap;
	delete[] fKeymapChars;
}


void
TermWindow::SessionChanged()
{
	_UpdateSessionTitle(fTabView->Selection());
}


void
TermWindow::_InitWindow()
{
	// make menu bar
	_SetupMenu();

	// shortcuts to switch tabs
	for (int32 i = 0; i < 9; i++) {
		BMessage* message = new BMessage(kSetActiveTab);
		message->AddInt32("index", i);
		AddShortcut('1' + i, B_COMMAND_KEY, message);
	}

	AddShortcut(B_LEFT_ARROW, B_COMMAND_KEY, new BMessage(MSG_SWITCH_TAB_LEFT));
	AddShortcut(B_RIGHT_ARROW, B_COMMAND_KEY, new BMessage(MSG_SWITCH_TAB_RIGHT));
	AddShortcut(B_LEFT_ARROW, B_COMMAND_KEY | B_SHIFT_KEY, new BMessage(MSG_MOVE_TAB_LEFT));
	AddShortcut(B_RIGHT_ARROW, B_COMMAND_KEY | B_SHIFT_KEY, new BMessage(MSG_MOVE_TAB_RIGHT));

	// panes, as in iTerm2: Command+Option+arrows
	const int32 kPaneKeys[4] = { B_LEFT_ARROW, B_RIGHT_ARROW, B_UP_ARROW,
		B_DOWN_ARROW };
	for (int32 i = 0; i < 4; i++) {
		BMessage* message = new BMessage(kSelectPane);
		message->AddInt32("direction", i);
		AddShortcut(kPaneKeys[i], B_COMMAND_KEY | B_OPTION_KEY, message);
	}

	BRect textFrame = Bounds();
	textFrame.top = fMenuBar->Bounds().bottom + 1.0;

	fTabView = new SmartTabView(textFrame, "tab view", B_WIDTH_FROM_LABEL);
	fTabView->SetListener(this);
	AddChild(fTabView);

	// Make the scroll view one pixel wider than the tab view container view, so
	// the scroll bar will look good.
	fTabView->SetInsets(0, 0, -1, 0);
}


bool
TermWindow::_CanClose(int32 index, TermView* onlyView)
{
	bool warnOnExit = PrefHandler::Default()->getBool(PREF_WARN_ON_EXIT);

	if (!warnOnExit)
		return true;

	// the terminals that would go: one pane, a tab's, or all
	std::vector<TermView*> views;
	if (onlyView != NULL)
		views.push_back(onlyView);
	else if (index != -1)
		_SessionTermViews(_SessionAt(index), views);
	else
		_AllTermViews(views);

	uint32 busyProcessCount = 0;
	BString busyProcessNames;
		// all names, separated by "\n\t"

	for (size_t i = 0; i < views.size(); i++) {
		ShellInfo shellInfo;
		ActiveProcessInfo info;
		TermView* termView = views[i];
		if (termView->GetShellInfo(shellInfo)
			&& termView->GetActiveProcessInfo(info)
			&& (info.ID() != shellInfo.ProcessID()
				|| !shellInfo.IsDefaultShell())) {
			if (++busyProcessCount > 1)
				busyProcessNames << "\n\t";
			busyProcessNames << info.Name();
		}
	}

	if (busyProcessCount == 0)
		return true;

	bool closesWindow = index == -1
		|| (onlyView == NULL && fSessions.CountItems() == 1);
	BString alertMessage;
	if (busyProcessCount == 1) {
		// Only one pending process. Select the alert text depending on whether
		// the terminal will be closed.
		alertMessage = closesWindow
			? B_TRANSLATE("The process \"%1\" is still running.\n"
				"If you close %appname%, the process will be killed.")
			: onlyView != NULL
				? B_TRANSLATE("The process \"%1\" is still running.\n"
					"If you close the pane, the process will be killed.")
				: B_TRANSLATE("The process \"%1\" is still running.\n"
					"If you close the tab, the process will be killed.");
	} else {
		// multiple pending processes
		alertMessage = closesWindow
			? B_TRANSLATE("The following processes are still running:\n\n"
				"\t%1\n\n"
				"If you close %appname%, the processes will be killed.")
			: B_TRANSLATE("The following processes are still running:\n\n"
				"\t%1\n\n"
				"If you close the tab, the processes will be killed.");
	}
	alertMessage.ReplaceFirst("%appname%", B_TRANSLATE_SYSTEM_NAME("Terminal"));
	alertMessage.ReplaceFirst("%1", busyProcessNames);

	BAlert* alert = new BAlert(B_TRANSLATE("Really close?"),
		alertMessage, B_TRANSLATE("Close"), B_TRANSLATE("Cancel"), NULL,
		B_WIDTH_AS_USUAL, B_WARNING_ALERT);
	alert->SetShortcut(1, B_ESCAPE);
	return alert->Go() == 0;
}


bool
TermWindow::QuitRequested()
{
	_FinishTitleDialog();

	if (!_CanClose(-1))
		return false;

	_SaveWindowPosition();

	return BWindow::QuitRequested();
}


void
TermWindow::MenusBeginning()
{
	TermView* view = _ActiveTermView();

	// Syncronize Encode Menu Pop-up menu and Preference.
	const BCharacterSet* charset
		= BCharacterSetRoster::GetCharacterSetByConversionID(view->Encoding());
	if (charset != NULL) {
		BString name(charset->GetPrintName());
		const char* mime = charset->GetMIMEName();
		if (mime)
			name << " (" << mime << ")";

		BMenuItem* item = fEncodingMenu->FindItem(name);
		if (item != NULL)
			item->SetMarked(true);
	}

	BFont font;
	view->GetTermFont(&font);

	float size = font.Size();

	fDecreaseFontSizeMenuItem->SetEnabled(size > kMinimumFontSize);
	fIncreaseFontSizeMenuItem->SetEnabled(size < kMaximumFontSize);

	// panes
	Session* session = _SessionAt(fTabView->Selection());
	int32 paneCount = session != NULL && session->paneView != NULL
		? session->paneView->CountPanes() : 1;
	fCloseViewMenuItem->SetLabel(paneCount > 1
		? B_TRANSLATE("Close active pane") : B_TRANSLATE("Close active tab"));
	for (int32 i = 0; BMenuItem* item = fPanesMenu->ItemAt(i); i++)
		item->SetEnabled(paneCount > 1);
	fMaximizePaneMenuItem->SetMarked(session != NULL
		&& session->paneView != NULL
		&& session->paneView->Maximized() != NULL);
	fAlertOnNextMarkMenuItem->SetMarked(view->AlertOnNextMark());

	BWindow::MenusBeginning();
}


/* static */ void
TermWindow::MakeEncodingMenu(BMenu* menu)
{
	BCharacterSetRoster roster;
	BCharacterSet charset;
	while (roster.GetNextCharacterSet(&charset) == B_OK) {
		int encoding = M_UTF8;
		const char* mime = charset.GetMIMEName();
		if (mime == NULL || strcasecmp(mime, "UTF-8") != 0)
			encoding = charset.GetConversionID();

		// filter out currently (???) not supported USC-2 and UTF-16
		if (encoding == B_UTF16_CONVERSION || encoding == B_UNICODE_CONVERSION)
			continue;

		BString name(charset.GetPrintName());
		if (mime)
			name << " (" << mime << ")";

		BMessage *message = new BMessage(MENU_ENCODING);
		if (message != NULL) {
			message->AddInt32("op", (int32)encoding);
			menu->AddItem(new BMenuItem(name, message));
		}
	}

	menu->SetRadioMode(true);
}


void
TermWindow::_SetupMenu()
{
	fFontSizeMenu = _MakeFontSizeMenu(MSG_HALF_SIZE_CHANGED,
		PrefHandler::Default()->getInt32(PREF_HALF_FONT_SIZE));
	fIncreaseFontSizeMenuItem = new BMenuItem(B_TRANSLATE("Increase"),
		new BMessage(kIncreaseFontSize), '+', B_COMMAND_KEY);
	fDecreaseFontSizeMenuItem = new BMenuItem(B_TRANSLATE("Decrease"),
		new BMessage(kDecreaseFontSize), '-', B_COMMAND_KEY);
	fFontSizeMenu->AddSeparatorItem();
	fFontSizeMenu->AddItem(fIncreaseFontSizeMenuItem);
	fFontSizeMenu->AddItem(fDecreaseFontSizeMenuItem);

	BMenu* windowSize = new(std::nothrow) BMenu(B_TRANSLATE("Window size"));
	if (windowSize != NULL) {
		MakeWindowSizeMenu(windowSize);
		windowSize->AddSeparatorItem();
		windowSize->AddItem(new BMenuItem(B_TRANSLATE("Full screen"),
			new BMessage(FULLSCREEN), B_ENTER));
	}

	fEncodingMenu = new(std::nothrow) BMenu(B_TRANSLATE("Text encoding"));
	if (fEncodingMenu != NULL)
		MakeEncodingMenu(fEncodingMenu);

	BString newTerminal(B_TRANSLATE("New %appname%"));
	newTerminal.ReplaceFirst("%appname%", B_TRANSLATE_SYSTEM_NAME("Terminal"));

	BLayoutBuilder::Menu<>(fMenuBar = new BMenuBar(Bounds(), "mbar"))
		// Terminal
		.AddMenu(B_TRANSLATE_SYSTEM_NAME("Terminal"))
			.AddItem(B_TRANSLATE_COMMENT("Switch Terminals", "'Terminals' being this application's "
				"name"), MENU_SWITCH_TERM, B_TAB)
				.GetItem(fSwitchTerminalsMenuItem)
			.AddItem(newTerminal, MENU_NEW_TERM, 'N')
			.AddItem(B_TRANSLATE("New tab"), kNewTab, 'T')
			.AddItem(B_TRANSLATE("Split vertically"), kSplitSideBySide, 'D')
			.AddItem(B_TRANSLATE("Split horizontally"), kSplitTopAndBottom,
				'D', B_SHIFT_KEY)
			.AddMenu(B_TRANSLATE("Panes"))
				.GetMenu(fPanesMenu)
				.AddItem(B_TRANSLATE("Select next pane"), kNextPane, ']')
				.AddItem(B_TRANSLATE("Select previous pane"), kPreviousPane,
					'[')
				.AddSeparator()
				.AddItem(B_TRANSLATE("Maximize active pane"), kMaximizePane,
					B_ENTER, B_SHIFT_KEY)
					.GetItem(fMaximizePaneMenuItem)
				.AddItem(B_TRANSLATE("Make panes the same size"),
					kEqualizePanes)
				.AddSeparator()
				.AddItem(B_TRANSLATE("Close active pane"), kClosePane)
			.End()
			.AddSeparator()
			.AddItem(B_TRANSLATE("Page setup" B_UTF8_ELLIPSIS), MENU_PAGE_SETUP)
			.AddItem(B_TRANSLATE("Print"), MENU_PRINT, 'P')
			.AddSeparator()
			.AddItem(B_TRANSLATE("Close window"), B_QUIT_REQUESTED, 'W',
				B_SHIFT_KEY)
			.AddItem(B_TRANSLATE("Close active tab"), kCloseView, 'W')
				.GetItem(fCloseViewMenuItem)
			.AddItem(B_TRANSLATE("Quit"), B_QUIT_REQUESTED, 'Q')
		.End()

		// Edit
		.AddMenu(B_TRANSLATE("Edit"))
			.AddItem(B_TRANSLATE("Copy"), B_COPY, 'C')
			.AddItem(B_TRANSLATE("Paste"), B_PASTE, 'V')
			.AddSeparator()
			.AddItem(B_TRANSLATE("Select all"), B_SELECT_ALL, 'A')
			.AddItem(B_TRANSLATE("Clear all"), MENU_CLEAR_ALL, 'L')
			.AddSeparator()
			.AddItem(B_TRANSLATE("Find" B_UTF8_ELLIPSIS), MENU_FIND_STRING, 'F')
			.AddItem(B_TRANSLATE("Find next"), kFindNext, 'G')
			.AddItem(B_TRANSLATE("Find previous"), kFindPrevious, 'G',
				B_SHIFT_KEY)
			.AddItem(B_TRANSLATE("Use selection for find"), kFindSelection,
				'E')
			.AddSeparator()
			.AddItem(B_TRANSLATE("Alert on next mark"), kAlertOnNextMark,
				'A', B_SHIFT_KEY)
				.GetItem(fAlertOnNextMarkMenuItem)
		.End()

		// Settings
		.AddMenu(B_TRANSLATE("Settings"))
			.AddItem(B_TRANSLATE("Window title" B_UTF8_ELLIPSIS),
				kEditWindowTitle)
			.AddItem(windowSize)
			.AddItem(fEncodingMenu)
			.AddItem(fFontSizeMenu)
			.AddItem(B_TRANSLATE("Save as default"), MSG_SAVE_AS_DEFAULT)
			.AddSeparator()
			.AddItem(B_TRANSLATE("Settings" B_UTF8_ELLIPSIS), MENU_PREF_OPEN,
				',')
			.AddItem(B_TRANSLATE("Colors" B_UTF8_ELLIPSIS), MENU_THEME_OPEN)
		.End();

	AddChild(fMenuBar);

	_UpdateSwitchTerminalsMenuItem();

#ifdef USE_DEBUG_SNAPSHOTS
	AddShortcut('S', B_COMMAND_KEY | B_CONTROL_KEY,
		new BMessage(SHORTCUT_DEBUG_SNAPSHOTS));
	AddShortcut('C', B_COMMAND_KEY | B_CONTROL_KEY,
		new BMessage(SHORTCUT_DEBUG_CAPTURE));
#endif

	BKeymap keymap;
	keymap.SetToCurrent();
	BStringList unmodified(3);
	if (keymap.GetModifiedCharacters("+", B_SHIFT_KEY, 0, unmodified)
			== B_OK) {
		int32 count = unmodified.CountStrings();
		for (int32 i = 0; i < count; i++) {
			uint32 key = BUnicodeChar::FromUTF8(unmodified.StringAt(i));
			if (!HasShortcut(key, 0)) {
				// Add semantic + shortcut, bug #7428
				AddShortcut(key, B_COMMAND_KEY,
					new BMessage(kIncreaseFontSize));
			}
		}
	}
	unmodified.MakeEmpty();
}


status_t
TermWindow::_GetWindowPositionFile(BFile* file, uint32 openMode)
{
	BPath path;
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path, true);
	if (status != B_OK)
		return status;

	status = path.Append("Terminal");
	if (status != B_OK)
		return status;

	status = path.Append("Windows");
	if (status != B_OK)
		return status;

	return file->SetTo(path.Path(), openMode);
}


status_t
TermWindow::_LoadWindowPosition(BRect* frame, uint32* workspaces)
{
	status_t status;
	BMessage position;

	BFile file;
	status = _GetWindowPositionFile(&file, B_READ_ONLY);
	if (status != B_OK)
		return status;

	status = position.Unflatten(&file);

	file.Unset();

	if (status != B_OK)
		return status;

	int32 id = fTerminalRoster.ID();
	status = position.FindRect("rect", id, frame);
	if (status != B_OK)
		return status;

	int32 _workspaces;
	status = position.FindInt32("workspaces", id, &_workspaces);
	if (status != B_OK)
		return status;
	if (modifiers() & B_SHIFT_KEY)
		*workspaces = _workspaces;
	else
		*workspaces = B_CURRENT_WORKSPACE;

	return B_OK;
}


status_t
TermWindow::_SaveWindowPosition()
{
	BFile file;
	BMessage originalSettings;

	// Read the settings file if it exists and is a valid BMessage.
	status_t status = _GetWindowPositionFile(&file, B_READ_ONLY);
	if (status == B_OK) {
		status = originalSettings.Unflatten(&file);
		file.Unset();

		if (status != B_OK)
			status = originalSettings.MakeEmpty();

		if (status != B_OK)
			return status;
	}

	// Replace the settings
	int32 id = fTerminalRoster.ID();
	BRect rect(Frame());
	if (originalSettings.ReplaceRect("rect", id, rect) != B_OK)
		originalSettings.AddRect("rect", rect);

	int32 workspaces = Workspaces();
	if (originalSettings.ReplaceInt32("workspaces", id, workspaces) != B_OK)
		originalSettings.AddInt32("workspaces", workspaces);

	// Resave the whole thing
	status = _GetWindowPositionFile (&file,
		B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (status != B_OK)
		return status;

	return originalSettings.Flatten(&file);
}


void
TermWindow::_GetPreferredFont(BFont& font)
{
	// Default to be_fixed_font
	font = be_fixed_font;

	const char* family
		= PrefHandler::Default()->getString(PREF_HALF_FONT_FAMILY);
	const char* style
		= PrefHandler::Default()->getString(PREF_HALF_FONT_STYLE);
	const char* size = PrefHandler::Default()->getString(PREF_HALF_FONT_SIZE);

	font.SetFamilyAndStyle(family, style);
	font.SetSize(atoi(size));

	// mark the font size menu item
	for (int32 i = 0; i < fFontSizeMenu->CountItems(); i++) {
		BMenuItem* item = fFontSizeMenu->ItemAt(i);
		if (item == NULL)
			continue;

		item->SetMarked(false);
		if (strcmp(item->Label(), size) == 0)
			item->SetMarked(true);
	}
}


void
TermWindow::MessageReceived(BMessage *message)
{
	int32 encodingId;

	if (message->WasDropped())
		_SetTermColors();

	switch (message->what) {
		case B_KEY_MAP_LOADED:
			_UpdateKeymap();
			break;

		case B_COPY:
			_ActiveTermView()->Copy(be_clipboard);
			break;

		case B_PASTE:
			_ActiveTermView()->Paste(be_clipboard);
			break;

#ifdef USE_DEBUG_SNAPSHOTS
		case SHORTCUT_DEBUG_SNAPSHOTS:
			_ActiveTermView()->MakeDebugSnapshots();
			break;

		case SHORTCUT_DEBUG_CAPTURE:
			_ActiveTermView()->StartStopDebugCapture();
			break;
#endif

		case B_SELECT_ALL:
			_ActiveTermView()->SelectAll();
			break;

		case MENU_CLEAR_ALL:
			_ActiveTermView()->Clear();
			break;

		case MENU_SWITCH_TERM:
			_SwitchTerminal();
			break;

		case MENU_NEW_TERM:
		{
			// Set our current working directory to that of the active tab, so
			// that the new terminal and its shell inherit it.
			const char* argv[] = {NULL, NULL, NULL};
			int32 argc = 0;

			ActiveProcessInfo activeProcessInfo;
			if (_ActiveTermView()->GetActiveProcessInfo(activeProcessInfo)) {
				argv[0] = "-w";
				argv[1] = activeProcessInfo.CurrentDirectory();
				argc = 2;
			}

			app_info info;
			be_app->GetAppInfo(&info);

			// try launching two different ways to work around possible problems
			if (be_roster->Launch(&info.ref, argc, argv) != B_OK)
				be_roster->Launch(TERM_SIGNATURE, argc, argv);
			break;
		}

		case MENU_PREF_OPEN:
			if (!fPrefWindow) {
				fPrefWindow = new PrefWindow(this);
			} else
				fPrefWindow->Activate();
			break;

		case MSG_PREF_CLOSED:
			fPrefWindow = NULL;
			break;

		case MENU_THEME_OPEN:
			if (!fThemeWindow)
				fThemeWindow = new ThemeWindow(this);
			else
				fThemeWindow->Activate();
			break;

		case MSG_THEME_CLOSED:
			fThemeWindow = NULL;
			break;

		case MSG_WINDOW_TITLE_SETTING_CHANGED:
		case MSG_TAB_TITLE_SETTING_CHANGED:
			_TitleSettingsChanged();
			break;

		case MENU_FIND_STRING:
			_ActiveTermView()->ShowFindBar();
			break;

		case kFindNext:
		case kFindPrevious:
		case MENU_FIND_NEXT:
		case MENU_FIND_PREVIOUS:
			// "next" goes up, to older text, as Terminal always did
			_ActiveTermView()->FindAgain(message->what == kFindNext
				|| message->what == MENU_FIND_NEXT);
			break;

		case kFindSelection:
		{
			BString selection;
			_ActiveTermView()->GetSelection(selection);
			int32 lineEnd = selection.FindFirst('\n');
			if (lineEnd >= 0)
				selection.Truncate(lineEnd);
			if (selection.IsEmpty())
				beep();
			else
				_ActiveTermView()->ShowFindBar(selection);
			break;
		}

		case kAlertOnNextMark:
		{
			TermView* view = _ActiveTermView();
			view->SetAlertOnNextMark(!view->AlertOnNextMark());
			break;
		}

		case kSplitSideBySide:
		case kSplitTopAndBottom:
			_SplitPane(message->what == kSplitSideBySide);
			break;

		case kClosePane:
		{
			Session* session = _SessionAt(fTabView->Selection());
			if (session != NULL)
				_RemovePane(session, session->containerView->GetTermView());
			break;
		}

		case kNextPane:
		case kPreviousPane:
			_CyclePane(message->what == kNextPane);
			break;

		case kSelectPane:
			_NavigatePane(message->GetInt32("direction", 0));
			break;

		case kMaximizePane:
		{
			Session* session = _SessionAt(fTabView->Selection());
			if (session == NULL || session->paneView->CountPanes() < 2)
				break;
			BView* pane = session->containerView->Parent();
			session->paneView->SetMaximized(
				session->paneView->Maximized() == pane ? NULL : pane);
			_UpdateDimming(session);
			break;
		}

		case kEqualizePanes:
		{
			Session* session = _SessionAt(fTabView->Selection());
			if (session != NULL)
				session->paneView->Equalize();
			break;
		}

		case MSG_TERM_VIEW_EVENTS:
		{
			// what a terminal in a hidden tab reported
			TermView* view;
			if (message->FindPointer("view", (void**)&view) == B_OK
				&& _SessionForTermView(view) != NULL) {
				view->HandleDetachedEvents();
			}
			break;
		}

		case MENU_ENCODING:
			if (message->FindInt32("op", &encodingId) == B_OK)
				_ActiveTermView()->SetEncoding(encodingId);
			break;

		case MSG_COLS_CHANGED:
		{
			int32 columns, rows;
			if (message->FindInt32("columns", &columns) != B_OK
				|| message->FindInt32("rows", &rows) != B_OK) {
				break;
			}

			for (int32 i = 0; i < fTabView->CountTabs(); i++) {
				// split tabs keep their panes' sizes
				if (_SessionAt(i)->paneView->CountPanes() > 1)
					continue;
				TermView* view = _TermViewAt(i);
				view->SetTermSize(rows, columns, true);
				_ResizeView(view);
			}
			break;
		}

		case MSG_BLINK_CURSOR_CHANGED:
		{
			bool blinkingCursor
				= PrefHandler::Default()->getBool(PREF_BLINK_CURSOR);

			std::vector<TermView*> views;
			_AllTermViews(views);
			for (size_t i = 0; i < views.size(); i++)
				views[i]->SwitchCursorBlinking(blinkingCursor);
			break;
		}

		case MSG_HALF_FONT_CHANGED:
		case MSG_FULL_FONT_CHANGED:
		case MSG_ALLOW_BOLD_CHANGED:
		{
			BFont font;
			_GetPreferredFont(font);
			_ApplyFont(font);
			break;
		}

		case MSG_HALF_SIZE_CHANGED:
		case MSG_FULL_SIZE_CHANGED:
		{
			const char* size = NULL;
			if (message->FindString("font_size", &size) != B_OK)
				break;

			// mark the font size menu item
			for (int32 i = 0; i < fFontSizeMenu->CountItems(); i++) {
				BMenuItem* item = fFontSizeMenu->ItemAt(i);
				if (item == NULL)
					continue;

				item->SetMarked(false);
				if (strcmp(item->Label(), size) == 0)
					item->SetMarked(true);
			}

			BFont font;
			_ActiveTermView()->GetTermFont(&font);
			font.SetSize(atoi(size));
			PrefHandler::Default()->setInt32(PREF_HALF_FONT_SIZE,
				(int32)atoi(size));
			_ApplyFont(font);
			break;
		}

		case MSG_USE_OPTION_AS_META_CHANGED:
		{
			bool useOptionAsMetaKey
				= PrefHandler::Default()->getBool(PREF_USE_OPTION_AS_META);

			std::vector<TermView*> views;
			_AllTermViews(views);
			for (size_t i = 0; i < views.size(); i++)
				views[i]->SetUseOptionAsMetaKey(useOptionAsMetaKey);
			break;
		}

		case FULLSCREEN:
			if (!fSavedFrame.IsValid()) { // go fullscreen
				_ActiveTermView()->DisableResizeView();
				float mbHeight = fMenuBar->Bounds().Height() + 1;
				fSavedFrame = Frame();
				BScreen screen(this);

				// no resize knob: the scroll bars reach the corner
				for (int32 i = fSessions.CountItems() - 1; i >= 0; i--)
					_SessionAt(i)->paneView->SetCornerInset(0);

				fMenuBar->Hide();
				fTabView->ResizeBy(0, mbHeight);
				fTabView->MoveBy(0, -mbHeight);
				fSavedLook = Look();
				// done before ResizeTo to work around a Dano bug
				// (not erasing the decor)
				SetLook(B_NO_BORDER_WINDOW_LOOK);
				ResizeTo(screen.Frame().Width() + 1, screen.Frame().Height() + 1);
				MoveTo(screen.Frame().left, screen.Frame().top);
				SetFlags(Flags() | (B_NOT_RESIZABLE | B_NOT_MOVABLE));
				fFullScreen = true;
			} else { // exit fullscreen
				_ActiveTermView()->DisableResizeView();
				float mbHeight = fMenuBar->Bounds().Height() + 1;
				fMenuBar->Show();

				for (int32 i = fSessions.CountItems() - 1; i >= 0; i--) {
					_SessionAt(i)->paneView->SetCornerInset(
						be_control_look->GetScrollBarWidth(B_VERTICAL) - 1);
				}

				ResizeTo(fSavedFrame.Width(), fSavedFrame.Height());
				MoveTo(fSavedFrame.left, fSavedFrame.top);
				fTabView->ResizeBy(0, -mbHeight);
				fTabView->MoveBy(0, mbHeight);
				SetLook(fSavedLook);
				fSavedFrame = BRect(0, 0, -1, -1);
				SetFlags(Flags() & ~(B_NOT_RESIZABLE | B_NOT_MOVABLE));
				fFullScreen = false;
			}
			break;

		case MSG_FONT_CHANGED:
			PostMessage(MSG_HALF_FONT_CHANGED);
			break;

		case MSG_COLOR_SCHEME_CHANGED:
		case MSG_UPDATE_COLOR:
			_SetTermColors();
			break;

		case MSG_SAVE_AS_DEFAULT:
		{
			BPath path;
			if (PrefHandler::GetDefaultPath(path) == B_OK) {
				PrefHandler::Default()->SaveAsText(path.Path(),
					PREFFILE_MIMETYPE);
			}
			break;
		}

		case MENU_PAGE_SETUP:
			_DoPageSetup();
			break;

		case MENU_PRINT:
			_DoPrint();
			break;

		case MSG_CHECK_CHILDREN:
			_CheckChildren();
			break;

		case MSG_NOTIFICATION_ACTIVATED:
		{
			// one of our notifications was clicked: show its tab, tell the
			// program
			const char* view;
			const char* identifier;
			int32 actions;
			if (message->FindString("view", &view) != B_OK
				|| message->FindString("id", &identifier) != B_OK
				|| message->FindInt32("actions", &actions) != B_OK) {
				break;
			}
			std::vector<TermView*> views;
			_AllTermViews(views);
			for (size_t i = 0; i < views.size(); i++) {
				TermView* termView = views[i];
				BString key;
				key.SetToFormat("%p", termView);
				if (key != view)
					continue;
				if ((actions & 1) != 0)
					ShowTermView(termView);
				if ((actions & 2) != 0) {
					termView->NotificationActivated(identifier,
						message->GetInt32("button", 0));
				}
				break;
			}
			break;
		}

		case MSG_NOTIFICATION_CLOSED:
		{
			const char* view;
			const char* identifier;
			if (message->FindString("view", &view) != B_OK
				|| message->FindString("id", &identifier) != B_OK) {
				break;
			}
			std::vector<TermView*> views;
			_AllTermViews(views);
			for (size_t i = 0; i < views.size(); i++) {
				BString key;
				key.SetToFormat("%p", views[i]);
				if (key == view) {
					views[i]->NotificationClosed(identifier);
					break;
				}
			}
			break;
		}

		case MSG_MOVE_TAB_LEFT:
		case MSG_MOVE_TAB_RIGHT:
			_NavigateTab(_IndexOfTermView(_ActiveTermView()),
				message->what == MSG_MOVE_TAB_LEFT ? -1 : 1, true);
			break;

		case MSG_SWITCH_TAB_LEFT:
		case MSG_SWITCH_TAB_RIGHT:
			_NavigateTab(_IndexOfTermView(_ActiveTermView()),
				message->what == MSG_SWITCH_TAB_LEFT ? -1 : 1, false);
			break;

		case kTabTitleChanged:
		{
			// tab title changed message from SetTitleDialog
			SessionID sessionID(*message, "session");
			if (Session* session = _SessionForID(sessionID)) {
				BString title;
				if (message->FindString("title", &title) == B_OK) {
					session->title.pattern = title;
					session->title.patternUserDefined = true;
				} else {
					session->title.pattern.Truncate(0);
					session->title.patternUserDefined = false;
				}
				session->titleFromPane = false;
				_UpdateSessionTitle(_IndexOfSession(session));
			}
			break;
		}

		case kWindowTitleChanged:
		{
			// window title changed message from SetTitleDialog
			BString title;
			if (message->FindString("title", &title) == B_OK) {
				fTitle.pattern = title;
				fTitle.patternUserDefined = true;
			} else {
				fTitle.pattern
					= PrefHandler::Default()->getString(PREF_WINDOW_TITLE);
				fTitle.patternUserDefined = false;
			}

			_UpdateSessionTitle(fTabView->Selection());
				// updates the window title as a side effect

			break;
		}

		case kSetActiveTab:
		{
			int32 index;
			if (message->FindInt32("index", &index) == B_OK
					&& index >= 0 && index < fSessions.CountItems()) {
				fTabView->Select(index);
			}
			break;
		}

		case kNewTab:
			_NewTab();
			break;

		case kCloseView:
		{
			// a pane whose shell quit, the menu (the active pane of a
			// split tab, or the tab), or a tab's menu
			SessionID sessionID(*message, "session");
			Session* session = sessionID.IsValid()
				? _SessionForID(sessionID) : _SessionAt(fTabView->Selection());
			if (session == NULL)
				break;
			TermView* view = NULL;
			if (message->FindPointer("view", (void**)&view) != B_OK
				&& !sessionID.IsValid()) {
				view = session->containerView->GetTermView();
			}
			if (view != NULL && session->paneView->CountPanes() > 1) {
				std::vector<TermView*> views;
				_SessionTermViews(session, views);
				if (std::find(views.begin(), views.end(), view) != views.end())
					_RemovePane(session, view, !message->HasInt32("reason"));
				break;
			}
			_RemoveTab(_IndexOfSession(session));
			break;
		}

		case kCloseOtherViews:
		{
			Session* session = _SessionForID(SessionID(*message, "session"));
			if (session == NULL)
				break;

			int32 count = fSessions.CountItems();
			for (int32 i = count - 1; i >= 0; i--) {
				if (_SessionAt(i) != session)
					_RemoveTab(i);
			}

			break;
		}

		case MSG_REMOTE_CONTROL:
			_RemoteControl(message);
			break;

		case kIncreaseFontSize:
		case kDecreaseFontSize:
		{
			BFont font;
			_ActiveTermView()->GetTermFont(&font);
			float size = font.Size();

			if (message->what == kIncreaseFontSize) {
				if (size < 12)
					size += 1;
				else if (size < 24)
					size += 2;
				else
					size += 4;
			} else {
				if (size <= 12)
					size -= 1;
				else if (size <= 24)
					size -= 2;
				else
					size -= 4;
			}

			_SetFontSize(size);
			break;
		}

		case kUpdateTitles:
			_UpdateTitles();
			break;

		case kEditTabTitle:
		{
			SessionID sessionID(*message, "session");
			if (Session* session = _SessionForID(sessionID))
				_OpenSetTabTitleDialog(_IndexOfSession(session));
			break;
		}

		case kEditWindowTitle:
			_OpenSetWindowTitleDialog();
			break;

		case kUpdateSwitchTerminalsMenuItem:
			_UpdateSwitchTerminalsMenuItem();
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


void
TermWindow::WindowActivated(bool activated)
{
	if (activated)
		_UpdateSwitchTerminalsMenuItem();
}


void
TermWindow::_SetTermColors()
{
	std::vector<TermView*> views;
	_AllTermViews(views);
	for (size_t i = 0; i < views.size(); i++) {
		TermViewContainerView* container
			= (TermViewContainerView*)views[i]->Parent();
		_SetTermColors(container);
		container->Invalidate();
		views[i]->Invalidate();
	}
}


void
TermWindow::_SetTermColors(TermViewContainerView* containerView)
{
	PrefHandler* handler = PrefHandler::Default();
	rgb_color background = handler->getRGB(PREF_TEXT_BACK_COLOR);

	containerView->SetViewColor(background);

	TermView *termView = containerView->GetTermView();
	termView->SetTextColor(handler->getRGB(PREF_TEXT_FORE_COLOR), background);

	termView->SetCursorColor(handler->getRGB(PREF_CURSOR_FORE_COLOR),
		handler->getRGB(PREF_CURSOR_BACK_COLOR));
	termView->SetSelectColor(handler->getRGB(PREF_SELECT_FORE_COLOR),
		handler->getRGB(PREF_SELECT_BACK_COLOR));

	// taken from TermApp::_InitDefaultPalette()
	const char * keys[kANSIColorCount] = {
		PREF_ANSI_BLACK_COLOR,
		PREF_ANSI_RED_COLOR,
		PREF_ANSI_GREEN_COLOR,
		PREF_ANSI_YELLOW_COLOR,
		PREF_ANSI_BLUE_COLOR,
		PREF_ANSI_MAGENTA_COLOR,
		PREF_ANSI_CYAN_COLOR,
		PREF_ANSI_WHITE_COLOR,
		PREF_ANSI_BLACK_HCOLOR,
		PREF_ANSI_RED_HCOLOR,
		PREF_ANSI_GREEN_HCOLOR,
		PREF_ANSI_YELLOW_HCOLOR,
		PREF_ANSI_BLUE_HCOLOR,
		PREF_ANSI_MAGENTA_HCOLOR,
		PREF_ANSI_CYAN_HCOLOR,
		PREF_ANSI_WHITE_HCOLOR
	};

	for (uint i = 0; i < kANSIColorCount; i++)
		termView->SetTermColor(i, handler->getRGB(keys[i]), false);
}


status_t
TermWindow::_DoPageSetup()
{
	BPrintJob job("PageSetup");

	// display the page configure panel
	status_t status = job.ConfigPage();

	// save a pointer to the settings
	fPrintSettings = job.Settings();

	return status;
}


void
TermWindow::_DoPrint()
{
	BPrintJob job("Print");
	if (fPrintSettings)
		job.SetSettings(new BMessage(*fPrintSettings));

	if (job.ConfigJob() != B_OK)
		return;

	BRect pageRect = job.PrintableRect();
	BRect curPageRect = pageRect;

	int pHeight = (int)pageRect.Height();
	int pWidth = (int)pageRect.Width();
	float w, h;
	_ActiveTermView()->GetFrameSize(&w, &h);
	int xPages = (int)ceil(w / pWidth);
	int yPages = (int)ceil(h / pHeight);

	job.BeginJob();

	// loop through and draw each page, and write to spool
	for (int x = 0; x < xPages; x++) {
		for (int y = 0; y < yPages; y++) {
			curPageRect.OffsetTo(x * pWidth, y * pHeight);
			job.DrawView(_ActiveTermView(), curPageRect, B_ORIGIN);
			job.SpoolPage();

			if (!job.CanContinue()) {
				// It is likely that the only way that the job was cancelled is
				// because the user hit 'Cancel' in the page setup window, in
				// which case, the user does *not* need to be told that it was
				// cancelled.
				// He/she will simply expect that it was done.
				return;
			}
		}
	}

	job.CommitJob();
}


void
TermWindow::_NewTab()
{
	ActiveProcessInfo info;
	if (_ActiveTermView()->GetActiveProcessInfo(info))
		_AddTab(NULL, info.CurrentDirectory());
	else
		_AddTab(NULL);
}


/*!	A terminal for a pane: its view, the container that gives it a margin,
	and the scroll view around them (the pane).
*/
TermViewContainerView*
TermWindow::_CreatePane(const Arguments* args, const BString& currentDirectory,
	bool firstSession, BScrollView*& _scrollView)
{
	int argc = 0;
	const char* const* argv = NULL;
	if (args != NULL)
		args->GetShellArguments(argc, argv);
	ShellParameters shellParameters(argc, argv, currentDirectory);

	TermView* view = new TermView(
		PrefHandler::Default()->getInt32(PREF_ROWS),
		PrefHandler::Default()->getInt32(PREF_COLS),
		shellParameters,
		PrefHandler::Default()->getInt32(PREF_HISTORY_SIZE));
	view->SetListener(this);

	TermViewContainerView* containerView = new TermViewContainerView(view);
	_scrollView = new TermScrollView("scrollView", containerView, view,
		firstSession);

	BFont font;
	_GetPreferredFont(font);
	view->SetTermFont(&font);

	view->SetMouseClipboard(gMouseClipboard);

	const BCharacterSet* charset
		= BCharacterSetRoster::FindCharacterSetByName(
			PrefHandler::Default()->getString(PREF_TEXT_ENCODING));
	if (charset != NULL)
		view->SetEncoding(charset->GetConversionID());

	view->SetKeymap(fKeymap, fKeymapChars);
	view->SetUseOptionAsMetaKey(
		PrefHandler::Default()->getBool(PREF_USE_OPTION_AS_META));

	_SetTermColors(containerView);
	return containerView;
}


void
TermWindow::_AddTab(const Arguments* args, const BString& currentDirectory)
{
	try {
		bool firstSession = fSessions.IsEmpty();

		BScrollView* scrollView;
		TermViewContainerView* containerView = _CreatePane(args,
			currentDirectory, firstSession, scrollView);
		TermView* view = containerView->GetTermView();

		if (firstSession)
			fTabView->SetScrollView(scrollView);

		Session* session = new Session(_NewSessionID(), _NewSessionIndex(),
			containerView);
		session->paneView = new PaneView(scrollView);
		session->paneView->SetCornerInset(fFullScreen ? 0
			: be_control_look->GetScrollBarWidth(B_VERTICAL) - 1);
		fSessions.AddItem(session);

		float width, height;
		view->GetFontSize(&width, &height);

		float minimumHeight = -1;
		if (fMenuBar != NULL)
			minimumHeight += fMenuBar->Bounds().Height() + 1;

		if (!firstSession)
			minimumHeight += fTabView->TabHeight() + 1;

		SetSizeLimits(MIN_COLS * width - 1, MAX_COLS * width - 1,
			minimumHeight + MIN_ROWS * height - 1,
			minimumHeight + MAX_ROWS * height - 1);
			// TODO: The size limit computation is apparently broken, since
			// the terminal can be resized smaller than MIN_ROWS/MIN_COLS!

		// If it's the first time we're called, setup the window
		if (firstSession) {
			float viewWidth, viewHeight;
			containerView->GetPreferredSize(&viewWidth, &viewHeight);

			// Resize Window
			ResizeTo(viewWidth + be_control_look->GetScrollBarWidth(B_HORIZONTAL),
				viewHeight + fMenuBar->Bounds().Height() + 1);
				// NOTE: Width is one pixel too small, since the scroll view
				// is one pixel wider than its parent.
		}

		BTab* tab = new BTab;
		fTabView->AddTab(session->paneView, tab);
		view->SetScrollBar(scrollView->ScrollBar(B_VERTICAL));
		// Only sync clipboard for the initial tab.
		if (firstSession)
			view->SyncClipboard();

		int32 tabIndex = fTabView->CountTabs() - 1;
		fTabView->Select(tabIndex);

		_UpdateSessionTitle(tabIndex);
	} catch (...) {
		// most probably out of memory. That's bad.
		// TODO: Should cleanup, I guess

		// Quit the application if we don't have a shell already
		if (fTabView->CountTabs() == 0) {
			fprintf(stderr, "Terminal couldn't open a shell\n");
			PostMessage(B_QUIT_REQUESTED);
		}
	}
}


void
TermWindow::_RemoveTab(int32 index, bool ask)
{
	_FinishTitleDialog();
		// always close to avoid confusion

	if (fSessions.CountItems() > 1) {
		if (ask && !_CanClose(index))
			return;
		if (Session* session = (Session*)fSessions.RemoveItem(index)) {
			if (fSessions.CountItems() == 1) {
				fTabView->SetScrollView(dynamic_cast<BScrollView*>(
					_SessionAt(0)->containerView->Parent()));
			} else {
				// the tab view must not keep a scroll view that goes
				std::vector<TermView*> views;
				_SessionTermViews(session, views);
				for (size_t i = 0; i < views.size(); i++) {
					if (fTabView->ScrollView()
						== views[i]->Parent()->Parent()) {
						fTabView->SetScrollView(NULL);
					}
				}
			}

			delete session;
			delete fTabView->RemoveTab(index);
		}
	} else
		PostMessage(B_QUIT_REQUESTED);
}


void
TermWindow::_SetFontSize(float size)
{
	BFont font;
	_ActiveTermView()->GetTermFont(&font);

	// constrain the font size
	if (size < kMinimumFontSize)
		size = kMinimumFontSize;
	if (size > kMaximumFontSize)
		size = kMaximumFontSize;

	// mark the font size menu item
	for (int32 i = 0; i < fFontSizeMenu->CountItems(); i++) {
		BMenuItem* item = fFontSizeMenu->ItemAt(i);
		if (item == NULL)
			continue;

		item->SetMarked(false);
		if (atoi(item->Label()) == size)
			item->SetMarked(true);
	}

	font.SetSize(size);
	PrefHandler::Default()->setInt32(PREF_HALF_FONT_SIZE, (int32)size);
	_ApplyFont(font);
}


/*!	A new font for every terminal. The active tab decides the window's size:
	it fits a single terminal's rows and columns; a split tab, full screen
	or another tab keeps the size, and its terminals get other rows and
	columns.
*/
void
TermWindow::_ApplyFont(const BFont& font)
{
	TermView* activeView = _ActiveTermView();
	Session* activeSession = _SessionAt(fTabView->Selection());
	bool fitWindow = !fFullScreen && activeSession != NULL
		&& activeSession->paneView->CountPanes() == 1;

	std::vector<TermView*> views;
	_AllTermViews(views);
	for (size_t i = 0; i < views.size(); i++) {
		TermView* view = views[i];
		view->SetTermFont(&font);
		if (view == activeView && fitWindow)
			continue;
		view->SetTermSize(view->Bounds(), true);
		view->Invalidate();
	}
	if (fitWindow)
		_ResizeView(activeView);
}


void
TermWindow::_NavigateTab(int32 index, int32 direction, bool move)
{
	int32 count = fSessions.CountItems();
	if (count <= 1 || index < 0 || index >= count)
		return;

	int32 newIndex = (index + direction + count) % count;
	if (newIndex == index)
		return;

	if (move) {
		// move the given tab to the new index
		Session* session = (Session*)fSessions.RemoveItem(index);
		fSessions.AddItem(session, newIndex);
		fTabView->MoveTab(index, newIndex);
	}

	// activate the respective tab
	fTabView->Select(newIndex);
}


TermViewContainerView*
TermWindow::_ActiveTermViewContainerView() const
{
	return _TermViewContainerViewAt(fTabView->Selection());
}


TermViewContainerView*
TermWindow::_TermViewContainerViewAt(int32 index) const
{
	if (Session* session = _SessionAt(index))
		return session->containerView;
	return NULL;
}


TermView*
TermWindow::_ActiveTermView() const
{
	return _ActiveTermViewContainerView()->GetTermView();
}


TermView*
TermWindow::_TermViewAt(int32 index) const
{
	TermViewContainerView* view = _TermViewContainerViewAt(index);
	return view != NULL ? view->GetTermView() : NULL;
}


//!	The index of the tab the view is in (any of its panes).
int32
TermWindow::_IndexOfTermView(TermView* termView) const
{
	Session* session = _SessionForTermView(termView);
	return session != NULL ? _IndexOfSession(session) : -1;
}


TermWindow::Session*
TermWindow::_SessionAt(int32 index) const
{
	return (Session*)fSessions.ItemAt(index);
}


TermWindow::Session*
TermWindow::_SessionForID(const SessionID& sessionID) const
{
	for (int32 i = 0; Session* session = _SessionAt(i); i++) {
		if (session->id == sessionID)
			return session;
	}

	return NULL;
}


int32
TermWindow::_IndexOfSession(Session* session) const
{
	return fSessions.IndexOf(session);
}


void
TermWindow::_CheckChildren()
{
	std::vector<TermView*> views;
	_AllTermViews(views);
	for (size_t i = 0; i < views.size(); i++) {
		if (views[i]->CheckShellGone())
			NotifyTermViewQuit(views[i], 0);
	}
}


void
TermWindow::Zoom(BPoint leftTop, float width, float height)
{
	_ActiveTermView()->DisableResizeView();
	BWindow::Zoom(leftTop, width, height);
}


void
TermWindow::FrameResized(float newWidth, float newHeight)
{
	BWindow::FrameResized(newWidth, newHeight);

	// the size of new terminals: not that of a pane
	Session* session = _SessionAt(fTabView->Selection());
	if (session == NULL || session->paneView->CountPanes() > 1)
		return;
	TermView* view = _ActiveTermView();
	PrefHandler::Default()->setInt32(PREF_COLS, view->Columns());
	PrefHandler::Default()->setInt32(PREF_ROWS, view->Rows());
}


void
TermWindow::WorkspacesChanged(uint32 oldWorkspaces, uint32 newWorkspaces)
{
	fTerminalRoster.SetWindowInfo(IsMinimized(), Workspaces());
}


void
TermWindow::WorkspaceActivated(int32 workspace, bool state)
{
	fTerminalRoster.SetWindowInfo(IsMinimized(), Workspaces());
}


void
TermWindow::Minimize(bool minimize)
{
	BWindow::Minimize(minimize);
	fTerminalRoster.SetWindowInfo(IsMinimized(), Workspaces());
}


void
TermWindow::TabSelected(SmartTabView* tabView, int32 index)
{
	// the tab's active pane has the focus
	if (Session* session = _SessionAt(index)) {
		TermView* view = session->containerView->GetTermView();
		if (view->Window() != NULL && !view->IsFocus())
			view->MakeFocus(true);
		_UpdateDimming(session);
	}
	SessionChanged();
}


void
TermWindow::TabDoubleClicked(SmartTabView* tabView, BPoint point, int32 index)
{
	if (index >= 0) {
		// clicked on a tab -- open the title dialog
		_OpenSetTabTitleDialog(index);
	} else {
		// not clicked on a tab -- create a new one
		_NewTab();
	}
}


void
TermWindow::TabMiddleClicked(SmartTabView* tabView, BPoint point, int32 index)
{
	if (index >= 0)
		_RemoveTab(index);
}


void
TermWindow::TabRightClicked(SmartTabView* tabView, BPoint point, int32 index)
{
	if (index < 0)
		return;

	TermView* termView = _TermViewAt(index);
	if (termView == NULL)
		return;

	BMessage* closeMessage = new BMessage(kCloseView);
	_SessionAt(index)->id.AddToMessage(*closeMessage, "session");

	BMessage* closeOthersMessage = new BMessage(kCloseOtherViews);
	_SessionAt(index)->id.AddToMessage(*closeOthersMessage, "session");

	BMessage* editTitleMessage = new BMessage(kEditTabTitle);
	_SessionAt(index)->id.AddToMessage(*editTitleMessage, "session");

	BPopUpMenu* popUpMenu = new BPopUpMenu("tab menu");
	BLayoutBuilder::Menu<>(popUpMenu)
		.AddItem(B_TRANSLATE("Close tab"), closeMessage)
		.AddItem(B_TRANSLATE("Close other tabs"), closeOthersMessage)
		.AddSeparator()
		.AddItem(B_TRANSLATE("Edit tab title" B_UTF8_ELLIPSIS),
			editTitleMessage)
	;

	popUpMenu->SetAsyncAutoDestruct(true);
	popUpMenu->SetTargetForItems(BMessenger(this));

	BPoint screenWhere = tabView->ConvertToScreen(point);
	BRect mouseRect(screenWhere, screenWhere);
	mouseRect.InsetBy(-4.0, -4.0);
	popUpMenu->Go(screenWhere, true, true, mouseRect, true);
}


void
TermWindow::NotifyTermViewQuit(TermView* view, int32 reason)
{
	// Since the notification can come from the view, we send a message to
	// ourselves to avoid deleting the caller synchronously.
	if (Session* session = _SessionForTermView(view)) {
		BMessage message(kCloseView);
		session->id.AddToMessage(message, "session");
		message.AddPointer("view", view);
		message.AddInt32("reason", reason);
		PostMessage(&message);
	}
}


void
TermWindow::SetTermViewTitle(TermView* view, const char* title)
{
	Session* session = _SessionForTermView(view);
	if (session == NULL)
		return;
	// a pane's title is the tab's while the pane is active
	session->paneTitles[view] = title;
	if (session->containerView->GetTermView() != view)
		return;
	session->title.pattern = title;
	session->title.patternUserDefined = true;
	session->titleFromPane = true;
	_UpdateSessionTitle(_IndexOfSession(session));
}


void
TermWindow::TitleChanged(SetTitleDialog* dialog, const BString& title,
	bool titleUserDefined)
{
	if (dialog == fSetTabTitleDialog) {
		// tab title
		BMessage message(kTabTitleChanged);
		fSetTabTitleSession.AddToMessage(message, "session");
		if (titleUserDefined)
			message.AddString("title", title);

		PostMessage(&message);
	} else if (dialog == fSetWindowTitleDialog) {
		// window title
		BMessage message(kWindowTitleChanged);
		if (titleUserDefined)
			message.AddString("title", title);

		PostMessage(&message);
	}
}


void
TermWindow::SetTitleDialogDone(SetTitleDialog* dialog)
{
	if (dialog == fSetTabTitleDialog) {
		fSetTabTitleSession = SessionID();
		fSetTabTitleDialog = NULL;
			// assuming this is atomic
	}
}


void
TermWindow::TerminalInfosUpdated(TerminalRoster* roster)
{
	PostMessage(kUpdateSwitchTerminalsMenuItem);
}


void
TermWindow::PreviousTermView(TermView* view)
{
	_NavigateTab(_IndexOfTermView(view), -1, false);
}


void
TermWindow::NextTermView(TermView* view)
{
	_NavigateTab(_IndexOfTermView(view), 1, false);
}


// #pragma mark - split panes


void
TermWindow::TermViewFocused(TermView* view)
{
	Session* session = _SessionForTermView(view);
	if (session == NULL)
		return;
	TermViewContainerView* containerView
		= (TermViewContainerView*)view->Parent();
	if (session->containerView != containerView)
		_ActivatePane(session, containerView, false);
}


bool
TermWindow::IsActiveTermView(TermView* view)
{
	Session* session = _SessionForTermView(view);
	return session == NULL || session->containerView->GetTermView() == view;
}


void
TermWindow::ShowTermView(TermView* view)
{
	Session* session = _SessionForTermView(view);
	if (session == NULL)
		return;
	if (IsMinimized())
		Minimize(false);
	Activate();
	int32 index = _IndexOfSession(session);
	if (fTabView->Selection() != index)
		fTabView->Select(index);
	_ActivatePane(session, (TermViewContainerView*)view->Parent());
}


void
TermWindow::GetTermViewTitle(TermView* view, BString& title)
{
	Session* session = _SessionForTermView(view);
	if (session == NULL)
		return;
	title = session->title.title;
	std::map<TermView*, BString>::iterator found
		= session->paneTitles.find(view);
	if (session->containerView->GetTermView() != view
		&& found != session->paneTitles.end() && !found->second.IsEmpty()) {
		title = found->second;
	}
}


/*!	The pane of a split tab is a scroll view, with the container, with the
	terminal.
*/
/*static*/ TermViewContainerView*
TermWindow::_ContainerOfPane(BView* pane)
{
	if (pane == NULL)
		return NULL;
	for (int32 i = 0; BView* child = pane->ChildAt(i); i++) {
		if (TermViewContainerView* container
				= dynamic_cast<TermViewContainerView*>(child)) {
			return container;
		}
	}
	return NULL;
}


void
TermWindow::_SessionTermViews(Session* session,
	std::vector<TermView*>& views) const
{
	if (session == NULL)
		return;
	if (session->paneView == NULL) {
		views.push_back(session->containerView->GetTermView());
		return;
	}
	for (int32 i = 0; BView* pane = session->paneView->PaneAt(i); i++) {
		if (TermViewContainerView* container = _ContainerOfPane(pane))
			views.push_back(container->GetTermView());
	}
}


void
TermWindow::_AllTermViews(std::vector<TermView*>& views) const
{
	for (int32 i = 0; Session* session = _SessionAt(i); i++)
		_SessionTermViews(session, views);
}


TermWindow::Session*
TermWindow::_SessionForTermView(TermView* view) const
{
	if (view == NULL)
		return NULL;
	for (int32 i = 0; Session* session = _SessionAt(i); i++) {
		if (session->containerView->GetTermView() == view)
			return session;
		std::vector<TermView*> views;
		_SessionTermViews(session, views);
		if (std::find(views.begin(), views.end(), view) != views.end())
			return session;
	}
	return NULL;
}


//!	Splits the active pane of the active tab in two, as iTerm2 does.
void
TermWindow::_SplitPane(bool sideBySide)
{
	Session* session = _SessionAt(fTabView->Selection());
	if (session == NULL)
		return;

	TermView* activeView = session->containerView->GetTermView();
	BView* pane = session->containerView->Parent();

	// room for a few rows and columns in each half
	float fontWidth, fontHeight;
	activeView->GetFontSize(&fontWidth, &fontHeight);
	float minimumWidth = MIN_COLS * fontWidth + 2 * kTermViewOffset
		+ be_control_look->GetScrollBarWidth(B_VERTICAL);
	float minimumHeight = 3 * fontHeight + 2 * kTermViewOffset;
	if (!session->paneView->CanSplit(pane, sideBySide, minimumWidth,
			minimumHeight)) {
		beep();
		return;
	}

	// the new shell starts where the active one is
	BString directory;
	ActiveProcessInfo info;
	if (activeView->GetActiveProcessInfo(info))
		directory = info.CurrentDirectory();

	BScrollView* scrollView;
	TermViewContainerView* containerView;
	try {
		containerView = _CreatePane(NULL, directory, false, scrollView);
	} catch (...) {
		beep();
		return;
	}
	TermView* view = containerView->GetTermView();

	// active first, so the new pane takes the focus when it is attached
	session->containerView = containerView;
	session->paneView->Split(pane, scrollView, sideBySide);
	view->SetScrollBar(scrollView->ScrollBar(B_VERTICAL));
	_ActivatePane(session, containerView);
}


//!	Closes a pane; the last one closes the tab.
void
TermWindow::_RemovePane(Session* session, TermView* view, bool ask)
{
	if (session == NULL || view == NULL)
		return;
	if (session->paneView->CountPanes() <= 1) {
		_RemoveTab(_IndexOfSession(session), ask);
		return;
	}
	if (ask && !_CanClose(_IndexOfSession(session), view))
		return;

	_FinishTitleDialog();

	TermViewContainerView* containerView
		= (TermViewContainerView*)view->Parent();
	BView* pane = containerView->Parent();
	bool wasActive = session->containerView == containerView;

	// the pane next to it gets the focus
	BView* next = NULL;
	for (int32 direction = PaneView::RIGHT; next == NULL
			&& direction <= PaneView::DOWN; direction++) {
		next = session->paneView->Neighbor(pane, direction);
	}
	if (next == NULL)
		next = session->paneView->Neighbor(pane, PaneView::LEFT);
	if (next == NULL) {
		int32 index = session->paneView->IndexOf(pane);
		next = session->paneView->PaneAt(index > 0 ? index - 1 : 1);
	}

	if (fTabView->ScrollView() == pane)
		fTabView->SetScrollView(NULL);
	session->paneTitles.erase(view);
	session->paneView->RemovePane(pane);
	delete pane;

	if (wasActive || _ContainerOfPane(next) == NULL) {
		TermViewContainerView* nextContainer = _ContainerOfPane(next);
		if (nextContainer == NULL) {
			nextContainer = _ContainerOfPane(session->paneView->PaneAt(0));
		}
		session->containerView = nextContainer;
		_ActivatePane(session, nextContainer);
	} else
		_UpdateDimming(session);
}


void
TermWindow::_ActivatePane(Session* session,
	TermViewContainerView* containerView, bool focus)
{
	if (session == NULL || containerView == NULL)
		return;
	session->containerView = containerView;
	TermView* view = containerView->GetTermView();
	if (focus && view->Window() != NULL && !view->IsFocus())
		view->MakeFocus(true);

	// a maximized pane gives way to the one chosen
	BView* maximized = session->paneView->Maximized();
	if (maximized != NULL && maximized != containerView->Parent())
		session->paneView->SetMaximized(containerView->Parent());

	// the tab's title is that of its active pane
	std::map<TermView*, BString>::iterator found
		= session->paneTitles.find(view);
	if (found != session->paneTitles.end()) {
		session->title.pattern = found->second;
		session->title.patternUserDefined = true;
		session->titleFromPane = true;
	} else if (session->titleFromPane) {
		// back to the settings' pattern
		session->title.pattern.Truncate(0);
		session->title.patternUserDefined = false;
		session->titleFromPane = false;
	}

	_UpdateDimming(session);
	_UpdateSessionTitle(_IndexOfSession(session));
}


void
TermWindow::_NavigatePane(int32 direction)
{
	Session* session = _SessionAt(fTabView->Selection());
	if (session == NULL)
		return;
	BView* next = session->paneView->Neighbor(
		session->containerView->Parent(), direction);
	if (next == NULL)
		return;
	_ActivatePane(session, _ContainerOfPane(next));
}


void
TermWindow::_CyclePane(bool next)
{
	Session* session = _SessionAt(fTabView->Selection());
	if (session == NULL)
		return;
	int32 count = session->paneView->CountPanes();
	if (count < 2)
		return;
	int32 index = session->paneView->IndexOf(
		session->containerView->Parent());
	index = (index + (next ? 1 : count - 1)) % count;
	_ActivatePane(session,
		_ContainerOfPane(session->paneView->PaneAt(index)));
}


void
TermWindow::_UpdateDimming(Session* session)
{
	if (session == NULL)
		return;
	bool dim = PrefHandler::Default()->getBool(PREF_DIM_INACTIVE_PANES)
		&& session->paneView->CountPanes() > 1
		&& session->paneView->Maximized() == NULL;
	std::vector<TermView*> views;
	_SessionTermViews(session, views);
	TermView* active = session->containerView->GetTermView();
	for (size_t i = 0; i < views.size(); i++)
		views[i]->SetDimmed(dim && views[i] != active);
}


void
TermWindow::_ResizeView(TermView *view)
{
	float fontWidth, fontHeight;
	view->GetFontSize(&fontWidth, &fontHeight);

	// a pane of a split tab keeps its size; the window does too
	Session* session = _SessionForTermView(view);
	if (session != NULL && session->paneView->CountPanes() > 1) {
		view->SetTermSize(view->Bounds(), true);
		view->Invalidate();
		return;
	}

	float minimumHeight = -1;
	if (fMenuBar != NULL)
		minimumHeight += fMenuBar->Bounds().Height() + 1;

	if (fTabView != NULL && fTabView->CountTabs() > 1)
		minimumHeight += fTabView->TabHeight() + 1;

	SetSizeLimits(MIN_COLS * fontWidth - 1, MAX_COLS * fontWidth - 1,
		minimumHeight + MIN_ROWS * fontHeight - 1,
		minimumHeight + MAX_ROWS * fontHeight - 1);

	float width;
	float height;
	view->Parent()->GetPreferredSize(&width, &height);

	width += be_control_look->GetScrollBarWidth(B_HORIZONTAL);
		// NOTE: Width is one pixel too small, since the scroll view
		// is one pixel wider than its parent.
	if (fMenuBar != NULL)
		height += fMenuBar->Bounds().Height() + 1;
	if (fTabView != NULL && fTabView->CountTabs() > 1)
		height += fTabView->TabHeight() + 1;

	ResizeTo(width, height);
	view->Invalidate();
}


/* static */ void
TermWindow::MakeWindowSizeMenu(BMenu* menu)
{
	const int32 windowSizes[4][2] = {
		{ 80, 25 },
		{ 80, 40 },
		{ 132, 25 },
		{ 132, 40 }
	};

	const int32 sizeNum = sizeof(windowSizes) / sizeof(windowSizes[0]);
	BString label;

	for (int32 i = 0; i < sizeNum; i++) {
		int32 columns = windowSizes[i][0];
		int32 rows = windowSizes[i][1];

		label.SetToFormat("%" B_PRId32 " × %" B_PRId32, columns, rows);

		BMessage* message = new BMessage(MSG_COLS_CHANGED);
		message->AddInt32("columns", columns);
		message->AddInt32("rows", rows);

		menu->AddItem(new BMenuItem(label.String(), message));
	}
}


/*static*/ BMenu*
TermWindow::_MakeFontSizeMenu(uint32 command, uint8 defaultSize)
{
	BMenu* menu = new (std::nothrow) BMenu(B_TRANSLATE("Font size"));
	if (menu == NULL)
		return NULL;

	int32 sizes[] = {
		8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 28, 32, 36, 0
	};

	bool found = false;

	for (uint32 i = 0; sizes[i]; i++) {
		BString string;
		string << sizes[i];
		BMessage* message = new BMessage(command);
		message->AddString("font_size", string);
		BMenuItem* item = new BMenuItem(string.String(), message);
		menu->AddItem(item);
		if (sizes[i] == defaultSize) {
			item->SetMarked(true);
			found = true;
		}
	}

	if (!found) {
		for (uint32 i = 0; sizes[i]; i++) {
			if (sizes[i] > defaultSize) {
				BString string;
				string << defaultSize;
				BMessage* message = new BMessage(command);
				message->AddString("font_size", string);
				BMenuItem* item = new BMenuItem(string.String(), message);
				item->SetMarked(true);
				menu->AddItem(item, i);
				break;
			}
		}
	}

	return menu;
}


void
TermWindow::_UpdateSwitchTerminalsMenuItem()
{
	fSwitchTerminalsMenuItem->SetEnabled(_FindSwitchTerminalTarget() >= 0);
}


void
TermWindow::_TitleSettingsChanged()
{
	if (!fTitle.patternUserDefined)
		fTitle.pattern = PrefHandler::Default()->getString(PREF_WINDOW_TITLE);

	fSessionTitlePattern = PrefHandler::Default()->getString(PREF_TAB_TITLE);

	_UpdateTitles();
}


void
TermWindow::_UpdateTitles()
{
	int32 sessionCount = fSessions.CountItems();
	for (int32 i = 0; i < sessionCount; i++)
		_UpdateSessionTitle(i);
}


void
TermWindow::_UpdateSessionTitle(int32 index)
{
	Session* session = _SessionAt(index);
	if (session == NULL)
		return;

	// get the shell and active process infos
	ShellInfo shellInfo;
	ActiveProcessInfo activeProcessInfo;
	TermView* termView = _TermViewAt(index);
	if (!termView->GetShellInfo(shellInfo)
		|| !termView->GetActiveProcessInfo(activeProcessInfo)) {
		return;
	}

	// evaluate the session title pattern
	BString sessionTitlePattern = session->title.patternUserDefined
		? session->title.pattern : fSessionTitlePattern;
	TabTitlePlaceholderMapper tabMapper(shellInfo, activeProcessInfo,
		session->index);
	const BString& sessionTitle = PatternEvaluator::Evaluate(
		sessionTitlePattern, tabMapper);

	// set the tab title
	if (sessionTitle != session->title.title) {
		session->title.title = sessionTitle;
		fTabView->TabAt(index)->SetLabel(session->title.title);
		fTabView->Invalidate();
			// Invalidate the complete tab view, since other tabs might change
			// their positions.
	}

	// If this is the active tab, also recompute the window title.
	if (index != fTabView->Selection())
		return;

	// evaluate the window title pattern
	WindowTitlePlaceholderMapper windowMapper(shellInfo, activeProcessInfo,
		fTerminalRoster.CountTerminals() > 1
			? fTerminalRoster.ID() + 1 : 0, sessionTitle);
	const BString& windowTitle = PatternEvaluator::Evaluate(fTitle.pattern,
		windowMapper);

	// set the window title
	if (windowTitle != fTitle.title) {
		fTitle.title = windowTitle;
		SetTitle(fTitle.title);
	}
}


void
TermWindow::_OpenSetTabTitleDialog(int32 index)
{
	// If a dialog is active, finish it.
	_FinishTitleDialog();

	BString toolTip = BString(B_TRANSLATE(
		"The pattern specifying the current tab title. The following "
			"placeholders\n"
		"can be used:\n")) << kToolTipSetTabTitlePlaceholders << "\n"
		<< kToolTipCommonTitlePlaceholders;
	fSetTabTitleDialog = new SetTitleDialog(
		B_TRANSLATE("Set tab title"), B_TRANSLATE("Tab title:"),
		toolTip);

	Session* session = _SessionAt(index);
	bool userDefined = session->title.patternUserDefined;
	const BString& title = userDefined
		? session->title.pattern : fSessionTitlePattern;
	fSetTabTitleSession = session->id;

	// place the dialog window directly under the tab, but keep it on screen
	BPoint location = fTabView->ConvertToScreen(
		fTabView->TabFrame(index).LeftBottom() + BPoint(0, 1));
	fSetTabTitleDialog->MoveTo(location);
	_MoveWindowInScreen(fSetTabTitleDialog);

	fSetTabTitleDialog->Go(title, userDefined, this);
}


void
TermWindow::_OpenSetWindowTitleDialog()
{
	// If a dialog is active, finish it.
	_FinishTitleDialog();

	BString toolTip = BString(B_TRANSLATE(
		"The pattern specifying the window title. The following placeholders\n"
		"can be used:\n")) << kToolTipSetWindowTitlePlaceholders << "\n"
		<< kToolTipCommonTitlePlaceholders;
	fSetWindowTitleDialog = new SetTitleDialog(B_TRANSLATE("Set window title"),
		B_TRANSLATE("Window title:"), toolTip);

	// center the dialog in the window frame, but keep it on screen
	fSetWindowTitleDialog->CenterIn(Frame());
	_MoveWindowInScreen(fSetWindowTitleDialog);

	fSetWindowTitleDialog->Go(fTitle.pattern, fTitle.patternUserDefined, this);
}


void
TermWindow::_FinishTitleDialog()
{
	SetTitleDialog* oldDialog = fSetTabTitleDialog;
	if (oldDialog != NULL && oldDialog->Lock()) {
		// might have been unset in the meantime, so recheck
		if (fSetTabTitleDialog == oldDialog) {
			oldDialog->Finish();
				// this also unsets the variables
		}
		oldDialog->Unlock();
		return;
	}

	oldDialog = fSetWindowTitleDialog;
	if (oldDialog != NULL && oldDialog->Lock()) {
		// might have been unset in the meantime, so recheck
		if (fSetWindowTitleDialog == oldDialog) {
			oldDialog->Finish();
				// this also unsets the variable
		}
		oldDialog->Unlock();
		return;
	}
}


void
TermWindow::_SwitchTerminal()
{
	team_id teamID = _FindSwitchTerminalTarget();
	if (teamID < 0)
		return;

	BMessenger app(TERM_SIGNATURE, teamID);
	app.SendMessage(MSG_ACTIVATE_TERM);
}


team_id
TermWindow::_FindSwitchTerminalTarget()
{
	AutoLocker<TerminalRoster> rosterLocker(fTerminalRoster);

	team_id myTeamID = Team();

	int32 numTerms = fTerminalRoster.CountTerminals();
	if (numTerms <= 1)
		return -1;

	// Find our position in the Terminal teams.
	int32 i;

	for (i = 0; i < numTerms; i++) {
		if (myTeamID == fTerminalRoster.TerminalAt(i)->team)
			break;
	}

	if (i == numTerms) {
		// we didn't find ourselves -- that shouldn't happen
		return -1;
	}

	uint32 currentWorkspace = 1L << current_workspace();

	while (true) {
		if (--i < 0)
			i = numTerms - 1;

		const TerminalRoster::Info* info = fTerminalRoster.TerminalAt(i);
		if (info->team == myTeamID) {
			// That's ourselves again. We've run through the complete list.
			return -1;
		}

		if (!info->minimized && (info->workspaces & currentWorkspace) != 0)
			return info->team;
	}
}


TermWindow::SessionID
TermWindow::_NewSessionID()
{
	return fNextSessionID++;
}


int32
TermWindow::_NewSessionIndex()
{
	for (int32 id = 1; ; id++) {
		bool used = false;

		for (int32 i = 0;
			Session* session = _SessionAt(i); i++) {
			if (id == session->index) {
				used = true;
				break;
			}
		}

		if (!used)
			return id;
	}
}


void
TermWindow::_MoveWindowInScreen(BWindow* window)
{
	BRect frame = window->Frame();
	BSize screenSize(BScreen(window).Frame().Size());
	window->MoveTo(BLayoutUtils::MoveIntoFrame(frame, screenSize).LeftTop());
}


void
TermWindow::_UpdateKeymap()
{
	delete fKeymap;
	delete[] fKeymapChars;

	get_key_map(&fKeymap, &fKeymapChars);

	std::vector<TermView*> views;
	_AllTermViews(views);
	for (size_t i = 0; i < views.size(); i++)
		views[i]->SetKeymap(fKeymap, fKeymapChars);
}


// #pragma mark - remote control


static void
append_json_string(BString& json, const char* text)
{
	json << '"';
	for (const uint8* c = (const uint8*)text; *c != '\0'; c++) {
		switch (*c) {
			case '"':
				json << "\\\"";
				break;
			case '\\':
				json << "\\\\";
				break;
			case '\n':
				json << "\\n";
				break;
			case '\r':
				json << "\\r";
				break;
			case '\t':
				json << "\\t";
				break;
			default:
				if (*c < 0x20)
					json << BString().SetToFormat("\\u%04x", *c);
				else
					json << (char)*c;
				break;
		}
	}
	json << '"';
}


/*!	kitty's remote control (DCS @kitty-cmd): a program in one of the tabs
	asks for something done to the tabs, kitty's "windows". Off unless the
	settings have "RemoteControl" , "yes": otherwise any text printed in a
	tab could type into the others.
*/
void
TermWindow::_RemoteControl(BMessage* message)
{
	const char* json;
	TermView* sender;
	if (message->FindString("json", &json) != B_OK
		|| message->FindPointer("view", (void**)&sender) != B_OK
		|| _IndexOfTermView(sender) < 0) {
		return;
	}

	BMessage command;
	BString reply;
	BList closing;
	bool respond = true;
	bool authorized = false;
	if (BJson::Parse(json, command) != B_OK)
		reply = "{\"ok\":false,\"error\":\"The command is not valid JSON\"}";
	else if (command.HasString("encrypted")) {
		// encrypted for KITTY_PUBLIC_KEY, with a password and the time in it
		std::string plaintext;
		BMessage decrypted;
		if (!TermCrypto::Decrypt(json, plaintext)
			|| BJson::Parse(plaintext.c_str(), decrypted) != B_OK
			|| !TermCrypto::IsRecent(
				(int64)decrypted.GetDouble("timestamp", 0))) {
			reply = "{\"ok\":false,\"error\":\"The command could not be "
				"decrypted, or is too old\"}";
		} else {
			const char* password = PrefHandler::Default()->getString(
				PREF_REMOTE_CONTROL_PASSWORD);
			authorized = password != NULL && password[0] != '\0'
				&& TermCrypto::Equal(decrypted.GetString("password", ""),
					password);
			command = decrypted;
		}
	}

	if (reply.IsEmpty()) {
		respond = !command.GetBool("no_response", false);
		const char* setting = PrefHandler::Default()->getString(
			PREF_REMOTE_CONTROL);
		if (!authorized && command.HasString("password")) {
			reply = "{\"ok\":false,\"error\":\"The password is not the "
				"settings' RemoteControlPassword\"}";
		} else if (!authorized
			&& (setting == NULL || strcmp(setting, "yes") != 0)) {
			reply = "{\"ok\":false,\"error\":\"Remote control is off: add "
				"\\\"RemoteControl\\\" , \\\"yes\\\" to "
				"~/config/settings/Terminal/Default\"}";
		} else
			_RunRemoteCommand(sender, command, reply, closing);
	}

	if (respond)
		sender->RemoteControlReply(reply);

	// panes to close go after the answer, which may be theirs
	for (int32 i = 0; i < closing.CountItems(); i++) {
		TermView* view = (TermView*)closing.ItemAt(i);
		if (Session* session = _SessionForTermView(view))
			_RemovePane(session, view, false);
	}
}


bool
TermWindow::_RunRemoteCommand(TermView* sender, const BMessage& command,
	BString& _reply, BList& _closing)
{
	BString name = command.GetString("cmd", "");
	name.ReplaceAll('_', '-');
	BMessage payload;
	command.FindMessage("payload", &payload);

	BString data;
	BString error;
	bool hasData = false;
	bool ok = true;

	if (name == "ls") {
		_RemoteList(sender, data);
		hasData = true;
	} else if (name == "launch" || name == "new-window") {
		// a new tab, or a new window (another Terminal)
		std::vector<BString> arguments;
		BMessage args;
		payload.FindMessage("args", &args);
		const char* argument;
		for (int32 i = 0; args.FindString(BString() << i, &argument) == B_OK;
				i++) {
			arguments.push_back(argument);
		}
		BString cwd = payload.GetString("cwd", "");
		if (cwd == "current" || cwd == "last_reported" || cwd == "oldest") {
			ActiveProcessInfo info;
			cwd = sender->GetActiveProcessInfo(info)
				? info.CurrentDirectory() : BString();
		}
		BString title = payload.GetString("tab_title",
			payload.GetString("window_title", payload.GetString("title", "")));
		// kitty's windows are panes: "window" (the default) splits the
		// sender's tab, "tab" opens one, "os-window" another Terminal
		BString type = payload.GetString("type", "window");
		bool newWindow = type == "os-window"
			|| BString(payload.GetString("window_type", "")) == "os";
		bool newPane = type == "window" && !newWindow;

		if (newWindow) {
			std::vector<const char*> argv;
			if (!cwd.IsEmpty()) {
				argv.push_back("-w");
				argv.push_back(cwd.String());
			}
			if (!title.IsEmpty()) {
				argv.push_back("-t");
				argv.push_back(title.String());
			}
			for (size_t i = 0; i < arguments.size(); i++)
				argv.push_back(arguments[i].String());
			status_t status = be_roster->Launch(TERM_SIGNATURE, argv.size(),
				argv.data());
			if (status != B_OK) {
				ok = false;
				error.SetToFormat("Cannot start Terminal: %s", strerror(status));
			}
		} else if (newPane) {
			Session* session = _SessionForTermView(sender);
			int32 previous = fTabView->Selection();
			TermViewContainerView* previousPane = session->containerView;
			fTabView->Select(_IndexOfSession(session));
			_ActivatePane(session, (TermViewContainerView*)sender->Parent());
			BView* pane = sender->Parent()->Parent();
			BString location = payload.GetString("location", "");
			bool sideBySide = location == "vsplit" ? true
				: location == "hsplit" ? false
				: pane->Bounds().Width() >= pane->Bounds().Height() * 1.5;
			int32 before = session->paneView->CountPanes();
			_SplitPane(sideBySide);
			if (session->paneView->CountPanes() == before) {
				ok = false;
				error = "There is no room for another pane";
			} else {
				TermView* view = session->containerView->GetTermView();
				if (!arguments.empty()) {
					// the command runs in the new pane's shell
					BString text;
					for (size_t i = 0; i < arguments.size(); i++) {
						if (i > 0)
							text << ' ';
						BString argument = arguments[i];
						argument.CharacterEscape(kShellEscapeCharacters, '\\');
						text << argument;
					}
					text << '\n';
					BMessage textPayload;
					textPayload.AddString("data", BString("text:") << text);
					BString ignored;
					BString ignoredError;
					view->RemoteCommand("send-text", textPayload, ignored,
						ignoredError);
				}
				data << view->RemoteControlID();
				hasData = true;
				if (payload.GetBool("keep_focus", false)) {
					fTabView->Select(previous);
					_ActivatePane(session, previousPane);
				}
			}
		} else {
			std::vector<const char*> argv;
			argv.push_back("Terminal");
			for (size_t i = 0; i < arguments.size(); i++)
				argv.push_back(arguments[i].String());
			Arguments parsed(0, NULL);
			parsed.Parse(argv.size(), argv.data());
			int32 previous = fTabView->Selection();
			_AddTab(&parsed, cwd);
			int32 index = fSessions.CountItems() - 1;
			if (!title.IsEmpty()) {
				Session* session = _SessionAt(index);
				session->title.pattern = title;
				session->title.pattern.ReplaceAll("%", "%%");
				session->title.patternUserDefined = true;
				_UpdateSessionTitle(index);
			}
			if (payload.GetBool("keep_focus", false))
				fTabView->Select(previous);
			data << _TermViewAt(index)->RemoteControlID();
			hasData = true;
		}
	} else if (name == "set-font-size") {
		BFont font;
		_ActiveTermView()->GetTermFont(&font);
		double size = payload.GetDouble("size", 0);
		BString operation = payload.GetString("increment_op", "");
		if (operation == "+")
			size = font.Size() + size;
		else if (operation == "-")
			size = font.Size() - size;
		else if (operation == "*")
			size = font.Size() * size;
		else if (operation == "/")
			size = size != 0 ? font.Size() / size : font.Size();
		else if (size <= 0)
			size = 12;
		_SetFontSize(size);
	} else if (name == "resize-os-window") {
		BString action = payload.GetString("action", "resize");
		if (action == "toggle-fullscreen")
			PostMessage(FULLSCREEN);
		else if (action != "resize") {
			ok = false;
			error.SetToFormat("Not supported by Terminal: %s", action.String());
		} else {
			TermView* view = _ActiveTermView();
			int32 width = (int32)payload.GetDouble("width", 0);
			int32 height = (int32)payload.GetDouble("height", 0);
			int32 columns = width;
			int32 rows = height;
			if (BString(payload.GetString("unit", "cells")) == "pixels") {
				int rowsFromPixels;
				int columnsFromPixels;
				view->GetTermSizeFromRect(BRect(0, 0, width - 1, height - 1),
					&rowsFromPixels, &columnsFromPixels);
				rows = rowsFromPixels;
				columns = columnsFromPixels;
			}
			if (payload.GetBool("incremental", false)) {
				columns += view->Columns();
				rows += view->Rows();
			}
			if (columns <= 0)
				columns = view->Columns();
			if (rows <= 0)
				rows = view->Rows();
			BMessage resize(MSG_COLS_CHANGED);
			resize.AddInt32("columns", columns);
			resize.AddInt32("rows", rows);
			PostMessage(&resize);
		}
	} else {
		// commands about kitty's windows (panes) and tabs
		std::vector<TermView*> views;
		if (!_MatchRemoteTabs(payload, sender, views, error))
			ok = false;
		else if (name == "focus-window" || name == "focus-tab") {
			ShowTermView(views[0]);
		} else if (name == "close-window") {
			for (size_t i = 0; i < views.size(); i++)
				_closing.AddItem(views[i]);
		} else if (name == "close-tab") {
			// every pane of the tabs
			for (size_t i = 0; i < views.size(); i++) {
				std::vector<TermView*> panes;
				_SessionTermViews(_SessionForTermView(views[i]), panes);
				for (size_t j = 0; j < panes.size(); j++) {
					if (!_closing.HasItem(panes[j]))
						_closing.AddItem(panes[j]);
				}
			}
		} else if (name == "set-tab-title") {
			BString title = payload.GetString("title", "");
			for (size_t i = 0; i < views.size(); i++) {
				Session* session = _SessionForTermView(views[i]);
				session->title.pattern = title;
				session->title.pattern.ReplaceAll("%", "%%");
				session->title.patternUserDefined = !title.IsEmpty();
				session->titleFromPane = false;
				_UpdateSessionTitle(_IndexOfSession(session));
			}
		} else {
			// what one terminal does; text comes from the first
			for (size_t i = 0; i < views.size() && ok; i++) {
				BString text;
				ok = views[i]->RemoteCommand(name.String(),
					payload, text, error);
				if (i == 0) {
					data = text;
					hasData = name == "get-text" || name == "get-colors";
				}
			}
		}
	}

	if (!ok) {
		_reply = "{\"ok\":false,\"error\":";
		append_json_string(_reply, error.String());
		_reply << "}";
	} else if (hasData) {
		_reply = "{\"ok\":true,\"data\":";
		append_json_string(_reply, data.String());
		_reply << "}";
	} else
		_reply = "{\"ok\":true}";
	return ok;
}


/*!	The panes (kitty's windows) a command is about: those its "match" (or
	"match_window") expression names, or all of them, the sender's (self),
	or else the active one. kitty's id:, title:, pid:, num:, recent:0 and
	state: (focused, active, self) are understood; title: matches the tab's
	title, num: counts the panes of all tabs.
*/
bool
TermWindow::_MatchRemoteTabs(const BMessage& payload, TermView* sender,
	std::vector<TermView*>& _views, BString& _error)
{
	std::vector<TermView*> views;
	_AllTermViews(views);
	if (payload.GetBool("all", false)) {
		_views = views;
		return true;
	}
	if (payload.GetBool("self", false)) {
		_views.push_back(sender);
		return true;
	}

	TermView* active = _ActiveTermView();
	BString match = payload.GetString("match",
		payload.GetString("match_window", ""));
	if (match.IsEmpty()) {
		_views.push_back(active);
		return true;
	}

	int32 colon = match.FindFirst(':');
	BString field;
	BString value;
	if (colon > 0) {
		match.CopyInto(field, 0, colon);
		match.CopyInto(value, colon + 1, match.Length() - colon - 1);
	}
	for (size_t i = 0; i < views.size(); i++) {
		TermView* view = views[i];
		bool matches = false;
		if (field == "id")
			matches = view->RemoteControlID() == atoi(value.String());
		else if (field == "num")
			matches = (int32)i == atoi(value.String());
		else if (field == "recent")
			matches = atoi(value.String()) == 0 && view == active;
		else if (field == "state") {
			matches = value == "self" ? view == sender
				: (value == "focused" || value == "active")
					&& view == active;
		} else if (field == "pid") {
			ShellInfo info;
			matches = view->GetShellInfo(info)
				&& info.ProcessID() == atoi(value.String());
		} else if (field == "title") {
			try {
				std::regex pattern(value.String());
				BString title;
				GetTermViewTitle(view, title);
				matches = std::regex_search(title.String(), pattern);
			} catch (...) {
				_error.SetToFormat("Not a regular expression: %s",
					value.String());
				return false;
			}
		} else {
			_error.SetToFormat("Not supported by Terminal: %s",
				match.String());
			return false;
		}
		if (matches)
			_views.push_back(view);
	}
	if (_views.empty() && !payload.GetBool("ignore_no_match", false)) {
		_error.SetToFormat("No matching windows for expression: %s",
			match.String());
		return false;
	}
	return !_views.empty();
}


//!	kitty's ls: this window, its tabs, each with its panes as windows.
void
TermWindow::_RemoteList(TermView* sender, BString& json)
{
	int32 selection = fTabView->Selection();
	json << "[{\"id\":" << fTerminalRoster.ID() + 1
		<< ",\"is_active\":" << (IsActive() ? "true" : "false")
		<< ",\"is_focused\":" << (IsActive() ? "true" : "false")
		<< ",\"wm_class\":\"Terminal\",\"wm_name\":\"Terminal\""
		<< ",\"tabs\":[";
	for (int32 i = 0; i < fSessions.CountItems(); i++) {
		Session* session = _SessionAt(i);
		std::vector<TermView*> views;
		_SessionTermViews(session, views);
		TermView* activeView = session->containerView->GetTermView();
		bool activeTab = i == selection;
		const char* yes = activeTab ? "true" : "false";
		const char* focused = activeTab && IsActive() ? "true" : "false";

		if (i > 0)
			json << ",";
		json << "{\"id\":" << session->id.ID() + 1
			<< ",\"is_active\":" << yes
			<< ",\"is_focused\":" << focused << ",\"title\":";
		append_json_string(json, session->title.title.String());
		const char* layout = views.size() > 1 ? "splits" : "stack";
		json << ",\"layout\":\"" << layout << "\",\"enabled_layouts\":[\""
			<< layout << "\"],\"active_window_history\":["
			<< activeView->RemoteControlID() << "],\"groups\":[";
		for (size_t j = 0; j < views.size(); j++) {
			int32 id = views[j]->RemoteControlID();
			json << (j > 0 ? "," : "") << "{\"id\":" << id
				<< ",\"windows\":[" << id << "]}";
		}
		json << "],\"windows\":[";
		for (size_t j = 0; j < views.size(); j++) {
			TermView* view = views[j];
			bool active = view == activeView;
			bool viewFocused = active && activeTab && IsActive();
			ShellInfo shellInfo;
			ActiveProcessInfo processInfo;
			bool haveShell = view->GetShellInfo(shellInfo);
			bool haveProcess = view->GetActiveProcessInfo(processInfo);
			BString title;
			GetTermViewTitle(view, title);

			if (j > 0)
				json << ",";
			json << "{\"id\":" << view->RemoteControlID()
				<< ",\"is_active\":" << (active ? "true" : "false")
				<< ",\"is_focused\":" << (viewFocused ? "true" : "false")
				<< ",\"is_self\":" << (view == sender ? "true" : "false")
				<< ",\"title\":";
			append_json_string(json, title.String());
			json << ",\"pid\":"
				<< (haveShell ? (int32)shellInfo.ProcessID() : 0)
				<< ",\"cwd\":";
			append_json_string(json, haveProcess
				? processInfo.CurrentDirectory().String() : "");
			json << ",\"cmdline\":[],\"env\":{},\"foreground_processes\":[";
			if (haveProcess) {
				json << "{\"pid\":" << (int32)processInfo.ID()
					<< ",\"cmdline\":[";
				append_json_string(json, processInfo.Name().String());
				json << "],\"cwd\":";
				append_json_string(json,
					processInfo.CurrentDirectory().String());
				json << "}";
			}
			json << "],\"lines\":" << view->Rows() << ",\"columns\":"
				<< view->Columns() << ",\"user_vars\":{}}";
		}
		json << "]}";
	}
	json << "]}]";
}
