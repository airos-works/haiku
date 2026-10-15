/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "TablePreview.h"

#include <algorithm>
#include <stdio.h>

#include <Catalog.h>
#include <ColumnListView.h>
#include <ColumnTypes.h>
#include <Font.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "QuickLook"


static const int32 kMaxRows = 20000;
static const int32 kMaxColumns = 64;
static const float kMinColumnWidth = 48;
static const float kMaxColumnWidth = 360;


TablePreview::TablePreview(const BString& text, bool truncated)
	:
	BQuickLookPreview("table preview"),
	fColumnCount(0),
	fTruncated(truncated),
	fDelimiter(','),
	fList(NULL)
{
	fDelimiter = GuessDelimiter(text);
	_Parse(text, fDelimiter);

	fList = new BColumnListView(Bounds(), "table", B_FOLLOW_ALL,
		B_WILL_DRAW | B_NAVIGABLE, B_FANCY_BORDER, true);
	fList->SetSortingEnabled(false);
	fList->SetSelectionMode(B_SINGLE_SELECTION_LIST);
	AddChild(fList);
	_Fill();
}


TablePreview::~TablePreview()
{
}


BSize
TablePreview::PreferredContentSize()
{
	float width = 24;
	for (size_t i = 0; i < fWidths.size(); i++)
		width += fWidths[i] + 8;
	font_height height;
	be_plain_font->GetHeight(&height);
	float rowHeight = ceilf(height.ascent + height.descent + height.leading)
		+ 4;
	float rows = std::max((size_t)3, fRows.size());
	return BSize(std::max(420.0f, std::min(width, 1100.0f)),
		std::max(200.0f, std::min(rows * rowHeight + 60, 700.0f)));
}


BString
TablePreview::Summary()
{
	BString summary;
	int32 rows = fRows.size() > 0 ? fRows.size() - 1 : 0;
	summary.SetToFormat(B_TRANSLATE("%d rows \xC3\x97 %d columns"),
		(int)rows, (int)fColumnCount);
	if (fDelimiter == '\t')
		summary << " \xE2\x80\xA2 " << B_TRANSLATE("tab separated");
	else if (fDelimiter == ';')
		summary << " \xE2\x80\xA2 " << B_TRANSLATE("semicolon separated");
	if (fTruncated || (int32)fRows.size() > kMaxRows)
		summary << " \xE2\x80\xA2 " << B_TRANSLATE("beginning only");
	return summary;
}


void
TablePreview::AttachedToWindow()
{
	BQuickLookPreview::AttachedToWindow();
	fList->ResizeTo(Bounds().Width(), Bounds().Height());
}


void
TablePreview::FrameResized(float width, float height)
{
	BQuickLookPreview::FrameResized(width, height);
	fList->ResizeTo(width, height);
}


char
TablePreview::GuessDelimiter(const BString& text)
{
	int32 end = text.FindFirst('\n');
	if (end < 0)
		end = text.Length();
	BString line;
	text.CopyInto(line, 0, end);
	int32 commas = 0, semicolons = 0, tabs = 0;
	bool quoted = false;
	for (int32 i = 0; i < line.Length(); i++) {
		char c = line[i];
		if (c == '"')
			quoted = !quoted;
		else if (!quoted) {
			if (c == ',')
				commas++;
			else if (c == ';')
				semicolons++;
			else if (c == '\t')
				tabs++;
		}
	}
	if (tabs > commas && tabs > semicolons)
		return '\t';
	if (semicolons > commas)
		return ';';
	return ',';
}


void
TablePreview::_Parse(const BString& text, char delimiter)
{
	std::vector<BString> row;
	BString field;
	bool quoted = false;
	const char* s = text.String();
	int32 length = text.Length();
	for (int32 i = 0; i < length; i++) {
		char c = s[i];
		if (quoted) {
			if (c == '"') {
				if (i + 1 < length && s[i + 1] == '"') {
					field += '"';
					i++;
				} else
					quoted = false;
			} else
				field += c;
			continue;
		}
		if (c == '"' && field.Length() == 0)
			quoted = true;
		else if (c == delimiter) {
			row.push_back(field);
			field.Truncate(0);
		} else if (c == '\n' || c == '\r') {
			if (c == '\r' && i + 1 < length && s[i + 1] == '\n')
				i++;
			row.push_back(field);
			field.Truncate(0);
			if (row.size() > 1 || row[0].Length() > 0) {
				fRows.push_back(row);
				if ((int32)fRows.size() > kMaxRows)
					break;
			}
			row.clear();
		} else
			field += c;
	}
	if (field.Length() > 0 || !row.empty()) {
		row.push_back(field);
		fRows.push_back(row);
	}

	for (size_t i = 0; i < fRows.size(); i++)
		fColumnCount = std::max(fColumnCount, (int32)fRows[i].size());
	fColumnCount = std::min(fColumnCount, kMaxColumns);
}


void
TablePreview::_Fill()
{
	// Column widths from the first rows.
	fWidths.assign(fColumnCount, kMinColumnWidth);
	size_t sample = std::min(fRows.size(), (size_t)200);
	for (size_t r = 0; r < sample; r++) {
		for (int32 c = 0; c < fColumnCount && c < (int32)fRows[r].size();
				c++) {
			float width = be_plain_font->StringWidth(fRows[r][c]) + 16;
			fWidths[c] = std::min(kMaxColumnWidth,
				std::max(fWidths[c], width));
		}
	}

	for (int32 c = 0; c < fColumnCount; c++) {
		BString title;
		if (!fRows.empty() && c < (int32)fRows[0].size())
			title = fRows[0][c];
		if (title.Length() == 0)
			title.SetToFormat("%d", (int)c + 1);
		fList->AddColumn(new BStringColumn(title, fWidths[c], kMinColumnWidth,
			2000, B_TRUNCATE_END), c);
	}

	for (size_t r = 1; r < fRows.size() && (int32)r <= kMaxRows; r++) {
		BRow* row = new BRow();
		for (int32 c = 0; c < fColumnCount; c++) {
			const char* value = c < (int32)fRows[r].size()
				? fRows[r][c].String() : "";
			row->SetField(new BStringField(value), c);
		}
		fList->AddRow(row);
	}
}
