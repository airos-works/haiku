/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */


#include "EmojiPanel.h"

#include "EmojiData.h"
#include "Theme.h"

#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Region.h>
#include <View.h>

#include <algorithm>
#include <math.h>
#include <string.h>


using Theme::S;
using Theme::F;


// at least this many columns, more on a wide keyboard
static const int kMinColumns = 10;
#define kColumnWidth S(72)
#define kMinRowHeight S(56)
#define kEmojiSize F(34)
static const int kMaxRecent = 40;


EmojiPanel::EmojiPanel()
	:
	fRows(1),
	fColumns(kMinColumns),
	fColumnWidth(1),
	fScroll(0)
{
	_Load();
}


void
EmojiPanel::SetFrame(BRect frame)
{
	bool first = !fFrame.IsValid();
	fFrame = frame;
	fRows = std::max(1, (int)((frame.Height() + 1) / kMinRowHeight));
	fColumns = std::max(kMinColumns, (int)((frame.Width() + 1) / kColumnWidth));
	fColumnWidth = (frame.Width() + 1) / fColumns;
	// It opens on the smileys.
	if (first)
		JumpTo(0);
	else
		SetScroll(fScroll);
}


void
EmojiPanel::Draw(BView* view, const char* pressed)
{
	BRegion clip(fFrame);
	view->ConstrainClippingRegion(&clip);

	BFont font(be_plain_font);
	font.SetSize(kEmojiSize);
	view->SetFont(&font);
	font_height height;
	font.GetHeight(&height);
	float rowHeight = (fFrame.Height() + 1) / fRows;

	int firstVisible = (int)floorf(fScroll / fColumnWidth);
	for (int section = kRecent; section < kEmojiCategoryCount; section++) {
		int first = _FirstColumn(section);
		int columns = _Columns(section);
		if (first + columns <= firstVisible || first > firstVisible + fColumns)
			continue;

		for (int i = 0; i < _Count(section); i++) {
			BRect cell;
			cell.left = fFrame.left + (first + i / fRows) * fColumnWidth
				- fScroll;
			cell.top = fFrame.top + (i % fRows) * rowHeight;
			cell.right = cell.left + fColumnWidth - 1;
			cell.bottom = cell.top + rowHeight - 1;
			if (cell.right < fFrame.left || cell.left > fFrame.right)
				continue;

			const char* emoji = _Emoji(section, i);
			if (emoji == pressed) {
				view->SetHighColor(Theme::kAccent);
				view->FillRoundRect(cell.InsetByCopy(3, 3), S(8), S(8));
			}
			view->SetLowColor(emoji == pressed ? Theme::kAccent : Theme::kPanel);
			view->SetHighColor(Theme::kText);
			view->DrawString(emoji, BPoint(
				cell.left + (cell.Width() - view->StringWidth(emoji)) / 2,
				cell.top + (cell.Height() + height.ascent - height.descent)
					/ 2));
		}
	}

	view->ConstrainClippingRegion(NULL);
}


void
EmojiPanel::JumpTo(int section)
{
	SetScroll(_FirstColumn(section) * fColumnWidth);
}


/*!	The section at the left edge, or the last one when scrolled to the end. */
int
EmojiPanel::Section() const
{
	if (fScroll >= _MaxScroll() - 1)
		return kEmojiCategoryCount - 1;

	int column = (int)roundf(fScroll / fColumnWidth);
	int section = kRecent;
	for (int next = kRecent; next < kEmojiCategoryCount; next++) {
		if (_Columns(next) > 0 && _FirstColumn(next) <= column)
			section = next;
	}
	return section;
}


void
EmojiPanel::SetScroll(float scroll)
{
	fScroll = std::clamp(scroll, 0.0f, _MaxScroll());
}


void
EmojiPanel::Snap()
{
	SetScroll(roundf(fScroll / fColumnWidth) * fColumnWidth);
}


const char*
EmojiPanel::EmojiAt(BPoint where) const
{
	if (!fFrame.Contains(where))
		return NULL;

	int column = (int)floorf((where.x - fFrame.left + fScroll) / fColumnWidth);
	int row = (int)((where.y - fFrame.top) / ((fFrame.Height() + 1) / fRows));
	for (int section = kRecent; section < kEmojiCategoryCount; section++) {
		int index = (column - _FirstColumn(section)) * fRows + row;
		if (column >= _FirstColumn(section) && index < _Count(section))
			return _Emoji(section, index);
	}
	return NULL;
}


void
EmojiPanel::AddRecent(const char* emoji)
{
	int section = Section();
	int columns = _Columns(kRecent);

	BString picked(emoji);
	fRecent.erase(std::remove(fRecent.begin(), fRecent.end(), picked),
		fRecent.end());
	fRecent.insert(fRecent.begin(), picked);
	if ((int)fRecent.size() > kMaxRecent)
		fRecent.resize(kMaxRecent);
	_Save();

	// A longer recent section pushes the rest along: keep them in view.
	if (section != kRecent)
		SetScroll(fScroll + (_Columns(kRecent) - columns) * fColumnWidth);
}


int
EmojiPanel::_Count(int section) const
{
	return section == kRecent ? (int)fRecent.size()
		: kEmojiCategories[section].count;
}


const char*
EmojiPanel::_Emoji(int section, int index) const
{
	return section == kRecent ? fRecent[index].String()
		: kEmojiCategories[section].emoji[index];
}


int
EmojiPanel::_Columns(int section) const
{
	return (_Count(section) + fRows - 1) / fRows;
}


/*!	Each section starts a column of its own. */
int
EmojiPanel::_FirstColumn(int section) const
{
	int column = 0;
	for (int before = kRecent; before < section; before++)
		column += _Columns(before);
	return column;
}


float
EmojiPanel::_MaxScroll() const
{
	return std::max(0, _FirstColumn(kEmojiCategoryCount) - fColumns)
		* fColumnWidth;
}


static status_t
recent_path(BPath& path)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path, true);
	if (status == B_OK)
		status = path.Append("MobileShell/recent emoji");
	return status;
}


void
EmojiPanel::_Load()
{
	BPath path;
	BFile file;
	if (recent_path(path) != B_OK || file.SetTo(path.Path(), B_READ_ONLY)
			!= B_OK) {
		return;
	}

	char buffer[kMaxRecent * 8];
	ssize_t size = file.Read(buffer, sizeof(buffer) - 1);
	if (size <= 0)
		return;
	buffer[size] = '\0';

	// one emoji a line
	for (char* line = strtok(buffer, "\n"); line != NULL
			&& (int)fRecent.size() < kMaxRecent; line = strtok(NULL, "\n")) {
		if (strlen(line) < 8)
			fRecent.push_back(line);
	}
}


void
EmojiPanel::_Save() const
{
	BPath path;
	if (recent_path(path) != B_OK)
		return;
	BPath folder;
	if (path.GetParent(&folder) == B_OK)
		create_directory(folder.Path(), 0755);

	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	for (const BString& emoji : fRecent) {
		file.Write(emoji.String(), emoji.Length());
		file.Write("\n", 1);
	}
}
