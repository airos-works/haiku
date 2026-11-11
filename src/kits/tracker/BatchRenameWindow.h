/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TRACKER_BATCH_RENAME_WINDOW_H
#define _TRACKER_BATCH_RENAME_WINDOW_H


#include <Entry.h>
#include <ObjectList.h>
#include <String.h>
#include <Window.h>


class BButton;
class BCardLayout;
class BCheckBox;
class BListView;
class BMenuField;
class BStringView;
class BTextControl;


namespace BPrivate {


class BContainerWindow;


// Renames several items at once: replace text (plain or a regular
// expression), add text before or after the name, or number them. The new
// names are previewed and clashes block the renaming; it is undone in one
// step.
class BatchRenameWindow : public BWindow {
public:
	BatchRenameWindow(BContainerWindow* parent,
		const BObjectList<entry_ref, true>& refs);

	virtual void MessageReceived(BMessage* message);

private:
	void _BuildNewNames();
	BString _NewName(const char* name, int32 index, bool* error);
	void _UpdatePreview();
	void _Rename();

	BContainerWindow* fParent;
	BObjectList<entry_ref, true> fRefs;
	BObjectList<BString, true> fNewNames;

	BMenuField* fModeField;
	BCardLayout* fCards;
	BTextControl* fFindText;
	BTextControl* fReplaceText;
	BCheckBox* fRegexCheckBox;
	BCheckBox* fCaseCheckBox;
	BTextControl* fAddText;
	BMenuField* fWhereField;
	BTextControl* fBaseName;
	BTextControl* fStartAt;
	BTextControl* fDigits;
	BListView* fPreview;
	BStringView* fStatus;
	BButton* fRenameButton;

	int32 fMode;
	bool fAddBefore;
};


} // namespace BPrivate

using namespace BPrivate;


#endif	// _TRACKER_BATCH_RENAME_WINDOW_H
