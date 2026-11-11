/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "CopyConflicts.h"

#include <Button.h>
#include <Catalog.h>
#include <ControlLook.h>
#include <DateTimeFormat.h>
#include <LayoutBuilder.h>
#include <ListView.h>
#include <Locale.h>
#include <Path.h>
#include <ScrollView.h>
#include <StringFormat.h>
#include <StringForSize.h>
#include <StringView.h>
#include <TextView.h>
#include <Window.h>

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "CopyConflicts"


namespace BPrivate {


static const int32 kMaxConflicts = 5000;
static const int32 kMaxDepth = 64;

static const uint32 kMsgDecide = 'ccdc';
static const uint32 kMsgReplaceOlder = 'ccro';
static const uint32 kMsgContinue = 'ccok';
static const uint32 kMsgSelection = 'ccsl';


void
MakeKeepBothName(char* name, const BDirectory* dir)
{
	if (!dir->Contains(name))
		return;

	BString base(name);
	BString extension;
	int32 dot = base.FindLast('.');
	if (dot > 0) {
		base.CopyInto(extension, dot, base.Length() - dot);
		base.Truncate(dot);
	}

	for (int32 number = 2; number < 10000; number++) {
		BString candidate;
		candidate << base << ' ' << number << extension;
		if (candidate.Length() >= B_FILE_NAME_LENGTH)
			break;
		if (!dir->Contains(candidate.String())) {
			strlcpy(name, candidate.String(), B_FILE_NAME_LENGTH);
			return;
		}
	}
}


CopyConflicts::CopyConflicts()
	:
	fConflicts(20),
	fTruncated(false)
{
}


void
CopyConflicts::_Add(const BEntry& source, const BEntry& existing,
	const BString& displayName)
{
	if (fConflicts.CountItems() >= kMaxConflicts) {
		fTruncated = true;
		return;
	}

	struct stat sourceStat;
	struct stat existingStat;
	if (source.GetStat(&sourceStat) != B_OK
		|| existing.GetStat(&existingStat) != B_OK) {
		return;
	}

	CopyConflict* conflict = new CopyConflict;
	source.GetRef(&conflict->source);
	BPath existingPath;
	existing.GetPath(&existingPath);
	conflict->existingPath = existingPath.Path();
	conflict->displayName = displayName;
	conflict->sourceSize = sourceStat.st_size;
	conflict->existingSize = existingStat.st_size;
	conflict->sourceTime = sourceStat.st_mtime;
	conflict->existingTime = existingStat.st_mtime;
	// newer items replace older ones unless the user says otherwise
	conflict->decision = sourceStat.st_mtime > existingStat.st_mtime
		? kConflictReplace : kConflictSkip;

	fByPath[conflict->existingPath.String()] = fConflicts.CountItems();
	fConflicts.AddItem(conflict);
}


void
CopyConflicts::_FindIn(const BDirectory& sourceDir, const BDirectory& destDir,
	const BString& prefix, int32 depth)
{
	if (depth > kMaxDepth)
		return;

	BDirectory source(sourceDir);
	source.Rewind();
	BEntry entry;
	while (source.GetNextEntry(&entry) == B_OK) {
		if (fConflicts.CountItems() >= kMaxConflicts) {
			fTruncated = true;
			return;
		}

		BEntry existing;
		if (destDir.FindEntry(entry.Name(), &existing) != B_OK)
			continue;

		BString displayName(prefix);
		displayName << entry.Name();

		bool sourceIsDir = entry.IsDirectory();
		bool existingIsDir = existing.IsDirectory();
		if (sourceIsDir && existingIsDir) {
			// merged: look at what is inside
			BDirectory subSource(&entry);
			BDirectory subDest(&existing);
			BString subPrefix(displayName);
			subPrefix << '/';
			_FindIn(subSource, subDest, subPrefix, depth + 1);
		} else if (!sourceIsDir && !existingIsDir)
			_Add(entry, existing, displayName);
	}
}


bool
CopyConflicts::Find(const BObjectList<entry_ref, true>* srcList,
	const BDirectory& destDir)
{
	for (int32 index = 0; index < srcList->CountItems(); index++) {
		entry_ref* ref = srcList->ItemAt(index);
		BEntry entry(ref);
		BDirectory parent;
		if (entry.GetParent(&parent) == B_OK && parent == destDir)
			continue;

		BEntry existing;
		if (destDir.FindEntry(ref->name, &existing) != B_OK)
			continue;

		bool sourceIsDir = entry.IsDirectory();
		bool existingIsDir = existing.IsDirectory();
		if (sourceIsDir && existingIsDir) {
			BDirectory subSource(&entry);
			BDirectory subDest(&existing);
			BString prefix(ref->name);
			prefix << '/';
			_FindIn(subSource, subDest, prefix, 1);
		} else if (!sourceIsDir && !existingIsDir)
			_Add(entry, existing, BString(ref->name));
		// a file meeting a folder is refused later, with its own message
	}

	return !fConflicts.IsEmpty();
}


int32
CopyConflicts::DecisionFor(const BDirectory& dir, const char* name) const
{
	BPath path(&dir, name);
	if (path.InitCheck() != B_OK)
		return -1;

	std::map<std::string, int32>::const_iterator found
		= fByPath.find(path.Path());
	if (found == fByPath.end())
		return -1;

	return fConflicts.ItemAt(found->second)->decision;
}


//	#pragma mark - the window


class ConflictItem : public BListItem {
public:
	ConflictItem(CopyConflict* conflict)
		:
		fConflict(conflict)
	{
	}

	CopyConflict* Conflict() const { return fConflict; }

	virtual void Update(BView* owner, const BFont* font)
	{
		font_height fontHeight;
		font->GetHeight(&fontHeight);
		fLineHeight = ceilf(fontHeight.ascent + fontHeight.descent
			+ fontHeight.leading);
		SetHeight(fLineHeight * 2 + 8);
		SetWidth(owner->Bounds().Width());
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

		font_height fontHeight;
		owner->GetFontHeight(&fontHeight);
		float spacing = be_control_look->DefaultLabelSpacing();
		float left = frame.left + spacing;
		float firstBaseline = frame.top + 4 + ceilf(fontHeight.ascent);
		float secondBaseline = firstBaseline + fLineHeight;

		// what happens to it, on the right
		const char* decision;
		switch (fConflict->decision) {
			case kConflictSkip:
				decision = B_TRANSLATE("Skip");
				break;
			case kConflictKeepBoth:
				decision = B_TRANSLATE("Keep both");
				break;
			default:
				decision = B_TRANSLATE("Replace");
				break;
		}
		float decisionWidth = owner->StringWidth(decision);
		owner->SetHighColor(text);
		owner->SetFont(be_bold_font);
		decisionWidth = owner->StringWidth(decision);
		owner->DrawString(decision, BPoint(frame.right - spacing
			- decisionWidth, firstBaseline));
		owner->SetFont(be_plain_font);

		BString name(fConflict->displayName);
		owner->TruncateString(&name, B_TRUNCATE_MIDDLE,
			frame.right - left - decisionWidth - 3 * spacing);
		owner->DrawString(name.String(), BPoint(left, firstBaseline));

		BString details;
		_Describe(details);
		owner->SetHighColor(tint_color(text, text.IsDark()
			? B_LIGHTEN_1_TINT : B_DARKEN_1_TINT));
		owner->TruncateString(&details, B_TRUNCATE_END,
			frame.right - left - spacing);
		owner->DrawString(details.String(), BPoint(left, secondBaseline));
	}

private:
	void _Describe(BString& details)
	{
		BDateTimeFormat format;
		char size[64];
		BString when;

		BString existing(B_TRANSLATE_COMMENT("Existing: %size, %date",
			"Details of an item that is already in the destination"));
		string_for_size(fConflict->existingSize, size, sizeof(size));
		format.Format(when, fConflict->existingTime, B_SHORT_DATE_FORMAT,
			B_SHORT_TIME_FORMAT);
		existing.ReplaceFirst("%size", size);
		existing.ReplaceFirst("%date", when);
		if (fConflict->existingTime > fConflict->sourceTime)
			existing << " " << B_TRANSLATE("(newer)");

		BString incoming(B_TRANSLATE_COMMENT("Incoming: %size, %date",
			"Details of an item being copied"));
		string_for_size(fConflict->sourceSize, size, sizeof(size));
		when = "";
		format.Format(when, fConflict->sourceTime, B_SHORT_DATE_FORMAT,
			B_SHORT_TIME_FORMAT);
		incoming.ReplaceFirst("%size", size);
		incoming.ReplaceFirst("%date", when);
		if (fConflict->sourceTime > fConflict->existingTime)
			incoming << " " << B_TRANSLATE("(newer)");

		details << existing << "   ·   " << incoming;
	}

	CopyConflict* fConflict;
	float fLineHeight;
};


class ConflictWindow : public BWindow {
public:
	ConflictWindow(CopyConflicts* conflicts, bool moving, const char* destName,
		sem_id done, bool* result)
		:
		BWindow(BRect(0, 0, 640, 440), B_TRANSLATE("Items with the same name"),
			B_TITLED_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS | B_NOT_ZOOMABLE
				| B_CLOSE_ON_ESCAPE),
		fConflicts(conflicts),
		fDone(done),
		fResult(result)
	{
		static BStringFormat format(B_TRANSLATE_COMMENT(
			"{0, plural,"
			" one{An item in \"%folder\" has the same name as one being "
				"%verb.}"
			" other{# items in \"%folder\" have the same names as ones being "
				"%verb.}}",
			"%verb is copied or moved"));
		BString text;
		format.Format(text, conflicts->CountConflicts());
		text.ReplaceFirst("%folder", destName);
		text.ReplaceFirst("%verb", moving ? B_TRANSLATE("moved")
			: B_TRANSLATE("copied"));
		text << "\n" << B_TRANSLATE("Newer items replace older ones unless "
			"you choose otherwise. The buttons change the selected items, or "
			"all of them when none is selected.");
		if (conflicts->Truncated()) {
			text << "\n" << B_TRANSLATE("Only the first ones are listed; the "
				"others are replaced.");
		}

		BTextView* message = new BTextView("message");
		message->SetText(text.String());
		message->MakeEditable(false);
		message->MakeSelectable(false);
		message->SetWordWrap(true);
		message->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		message->SetExplicitMinSize(BSize(500, B_SIZE_UNSET));

		fList = new BListView("conflicts", B_MULTIPLE_SELECTION_LIST);
		for (int32 index = 0; index < conflicts->CountConflicts(); index++)
			fList->AddItem(new ConflictItem(conflicts->ConflictAt(index)));
		BScrollView* scrollView = new BScrollView("scroll", fList, 0, false,
			true);
		scrollView->SetExplicitMinSize(BSize(B_SIZE_UNSET, 200));

		BButton* replace = _DecisionButton(B_TRANSLATE("Replace"),
			kConflictReplace);
		BButton* skip = _DecisionButton(B_TRANSLATE("Skip"), kConflictSkip);
		BButton* keepBoth = _DecisionButton(B_TRANSLATE("Keep both"),
			kConflictKeepBoth);
		BButton* replaceOlder = new BButton(B_TRANSLATE("Replace older"),
			new BMessage(kMsgReplaceOlder));

		BButton* cancel = new BButton(B_TRANSLATE("Cancel"),
			new BMessage(B_QUIT_REQUESTED));
		BButton* ok = new BButton(B_TRANSLATE("Continue"),
			new BMessage(kMsgContinue));
		SetDefaultButton(ok);

		BLayoutBuilder::Group<>(this, B_VERTICAL)
			.SetInsets(B_USE_WINDOW_SPACING)
			.Add(message)
			.Add(scrollView, 1.0f)
			.AddGroup(B_HORIZONTAL)
				.Add(replace)
				.Add(skip)
				.Add(keepBoth)
				.Add(replaceOlder)
				.AddGlue()
			.End()
			.AddGroup(B_HORIZONTAL)
				.AddGlue()
				.Add(cancel)
				.Add(ok)
			.End();

		CenterOnScreen();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgDecide:
			{
				int32 decision = message->GetInt32("decision", kConflictReplace);
				bool selectedOnly = fList->CurrentSelection() >= 0;
				for (int32 index = 0; index < fList->CountItems(); index++) {
					ConflictItem* item = (ConflictItem*)fList->ItemAt(index);
					if (!selectedOnly || item->IsSelected())
						item->Conflict()->decision = decision;
				}
				fList->Invalidate();
				break;
			}

			case kMsgReplaceOlder:
			{
				bool selectedOnly = fList->CurrentSelection() >= 0;
				for (int32 index = 0; index < fList->CountItems(); index++) {
					ConflictItem* item = (ConflictItem*)fList->ItemAt(index);
					if (selectedOnly && !item->IsSelected())
						continue;
					CopyConflict* conflict = item->Conflict();
					conflict->decision
						= conflict->sourceTime > conflict->existingTime
							? kConflictReplace : kConflictSkip;
				}
				fList->Invalidate();
				break;
			}

			case kMsgContinue:
				*fResult = true;
				Quit();
				break;

			default:
				BWindow::MessageReceived(message);
				break;
		}
	}

	virtual void Quit()
	{
		release_sem(fDone);
		BWindow::Quit();
	}

private:
	BButton* _DecisionButton(const char* label, int32 decision)
	{
		BMessage* message = new BMessage(kMsgDecide);
		message->AddInt32("decision", decision);
		return new BButton(label, message);
	}

	CopyConflicts* fConflicts;
	BListView* fList;
	sem_id fDone;
	bool* fResult;
};


bool
CopyConflicts::Ask(bool moving, const char* destName)
{
	// called from the copy thread, which waits for the answer
	sem_id done = create_sem(0, "copy conflicts");
	if (done < 0)
		return false;

	bool result = false;
	ConflictWindow* window = new ConflictWindow(this, moving, destName, done,
		&result);
	window->Show();

	while (acquire_sem(done) == B_INTERRUPTED)
		;
	delete_sem(done);

	return result;
}


} // namespace BPrivate
