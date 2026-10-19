/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "GlyphCache.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include <Bitmap.h>
#include <View.h>

#include "BoxDrawing.h"


static const int32 kPageSize = 2048;
static const int32 kMaxPages = 4;
static const int32 kStagingColumns = 16;
static const int32 kStagingRows = 8;


GlyphCache::GlyphCache()
	:
	fScale(1),
	fAllowBold(true),
	fEmulateBold(false),
	fFontAscent(0),
	fNextSlot(1),
	fEpoch(0),
	fStaging(NULL),
	fStagingView(NULL)
{
	memset(&fMetrics, 0, sizeof(fMetrics));
	memset(&fAtlas, 0, sizeof(fAtlas));
	fMetrics.cellWidth = 8;
	fMetrics.cellHeight = 16;
}


GlyphCache::~GlyphCache()
{
	delete fStaging;
	for (size_t i = 0; i < fPages.size(); i++)
		free(fPages[i]);
}


void
GlyphCache::SetFont(const BFont& font, float scale, bool allowBold,
	bool emulateBold)
{
	if (scale < 1)
		scale = 1;
	fScale = scale;
	fAllowBold = allowBold;
	fEmulateBold = emulateBold;

	// The cell as Terminal always had it, in view coordinates: the widest
	// ASCII character, and the font's height with at least one pixel of
	// leading plus one.
	BFont regular(font);
	regular.SetSpacing(B_FIXED_SPACING);
	float width = 0;
	for (int c = 0x20; c <= 0x7e; c++) {
		char text[2] = { (char)c, '\0' };
		width = std::max(width, regular.StringWidth(text));
	}
	font_height height;
	regular.GetHeight(&height);
	int32 ascent = (int32)height.ascent;
	int32 descent = (int32)height.descent;
	int32 leading = std::max((int32)height.leading, (int32)1);

	fMetrics.cellWidth = std::max((int32)1, (int32)roundf(width * scale));
	fMetrics.cellHeight = std::max((int32)1,
		(int32)roundf((ascent + descent + leading + 1) * scale));
	fMetrics.baseline = (int32)roundf(ascent * scale);
	fMetrics.lineThickness = std::max((int32)1, (int32)roundf(scale));
	fMetrics.underlineThickness = fMetrics.lineThickness;
	fMetrics.underlinePosition = std::min(fMetrics.cellHeight
			- fMetrics.underlineThickness,
		fMetrics.baseline + (int32)roundf(scale));
	fMetrics.strikePosition = fMetrics.baseline
		- (int32)roundf(ascent * scale * 0.3f);

	// Is there an italic style in the family? Without one, italics are
	// slanted.
	font_family family;
	font_style regularStyle;
	regular.GetFamilyAndStyle(&family, &regularStyle);
	bool hasItalic = false;
	int32 styleCount = count_font_styles(family);
	for (int32 i = 0; i < styleCount; i++) {
		font_style style;
		uint16 face;
		uint32 flags;
		if (get_font_style(family, i, &style, &face, &flags) == B_OK
			&& (face & B_ITALIC_FACE) != 0) {
			hasItalic = true;
			break;
		}
	}

	// The fonts glyphs are drawn with: the frame buffer's size, and the
	// characters' own spacing, so that combining marks stay over the
	// character before them.
	for (int32 style = 0; style < 4; style++) {
		BFont& styled = fFonts[style];
		styled = regular;
		styled.SetSize(regular.Size() * scale);
		styled.SetSpacing(B_CHAR_SPACING);
		uint16 face = regular.Face() & ~B_REGULAR_FACE;
		if ((style & GLYPH_BOLD) != 0)
			face |= B_BOLD_FACE;
		if ((style & GLYPH_ITALIC) != 0) {
			if (hasItalic)
				face |= B_ITALIC_FACE;
			else
				styled.SetShear(102);
		}
		if (face == 0)
			face = B_REGULAR_FACE;
		styled.SetFace(face);
	}
	fFontAscent = ascent * scale;

	_Reset();
}


float
GlyphCache::CellWidth() const
{
	return fMetrics.cellWidth / fScale;
}


float
GlyphCache::CellHeight() const
{
	return fMetrics.cellHeight / fScale;
}


void
GlyphCache::_Reset()
{
	fSlots.clear();
	fPending.clear();
	fNextSlot = 1;
	fEpoch++;

	for (size_t i = 0; i < fPages.size(); i++)
		free(fPages[i]);
	fPages.clear();
	fDirty.clear();

	fAtlas.pageSize = kPageSize;
	fAtlas.cellWidth = fMetrics.cellWidth;
	fAtlas.cellHeight = fMetrics.cellHeight;
	fAtlas.slotsPerRow = std::max((int32)1, kPageSize / fMetrics.cellWidth);
	fAtlas.slotsPerPage = fAtlas.slotsPerRow
		* std::max((int32)1, kPageSize / fMetrics.cellHeight);
	fAtlas.pageCount = 0;
	fAtlas.pages = NULL;
	fAtlas.colorPages = NULL;
	fAtlas.dirty = NULL;
	fAtlas.generation++;

	_AddPage();

	delete fStaging;
	fStaging = NULL;
	fStagingView = NULL;
}


bool
GlyphCache::_AddPage()
{
	if ((int32)fPages.size() >= kMaxPages)
		return false;
	uint8* page = (uint8*)calloc(kPageSize, kPageSize);
	if (page == NULL)
		return false;
	fPages.push_back(page);
	GlyphAtlasInfo::Dirty dirty = { 0, 0 };
	fDirty.push_back(dirty);
	fAtlas.pageCount = fPages.size();
	fAtlas.pages = fPages.data();
	fAtlas.dirty = fDirty.data();
	fAtlas.generation++;
	return true;
}


uint32
GlyphCache::_AllocateSlot(int32 cells)
{
	uint32 slot = fNextSlot;
	if (cells > 1) {
		// keep the cells of a wide glyph in one row
		int32 column = slot % fAtlas.slotsPerRow;
		if (column + cells > fAtlas.slotsPerRow)
			slot += fAtlas.slotsPerRow - column;
	}

	int32 page = (slot + cells - 1) / fAtlas.slotsPerPage;
	if (page >= (int32)fPages.size()) {
		if (!_AddPage())
			return 0;
		if (page >= (int32)fPages.size())
			return 0;
	}

	fNextSlot = slot + cells;
	return slot;
}


void
GlyphCache::_MarkDirty(int32 page, int32 top, int32 bottom)
{
	if (page < 0 || page >= (int32)fDirty.size())
		return;
	GlyphAtlasInfo::Dirty& dirty = fDirty[page];
	if (dirty.top >= dirty.bottom) {
		dirty.top = top;
		dirty.bottom = bottom;
	} else {
		dirty.top = std::min(dirty.top, top);
		dirty.bottom = std::max(dirty.bottom, bottom);
	}
	fAtlas.generation++;
}


uint32
GlyphCache::Lookup(uint64 key, const char* utf8, uint32 firstCodePoint,
	int32 cells, uint32 style)
{
	if (!fAllowBold)
		style &= ~GLYPH_BOLD;
	key = (key & 0xffffffffffffULL) | ((uint64)style << 56)
		| ((uint64)cells << 60);

	std::unordered_map<uint64, uint32>::iterator found = fSlots.find(key);
	if (found != fSlots.end())
		return found->second;

	uint32 slot = _AllocateSlot(cells);
	if (slot == 0) {
		// full: start over (callers see the new epoch)
		_Reset();
		slot = _AllocateSlot(cells);
		if (slot == 0)
			return 0;
	}

	fSlots[key] = slot;
	Pending pending;
	pending.slot = slot;
	pending.text = utf8;
	pending.codePoint = firstCodePoint;
	pending.cells = cells;
	pending.style = style;
	fPending.push_back(pending);
	return slot;
}


bool
GlyphCache::_EnsureStaging(int32 width, int32 height)
{
	if (fStaging != NULL && fStaging->Bounds().IntegerWidth() + 1 >= width
		&& fStaging->Bounds().IntegerHeight() + 1 >= height) {
		return true;
	}

	delete fStaging;
	fStaging = new(std::nothrow) BBitmap(BRect(0, 0, width - 1, height - 1),
		B_BITMAP_ACCEPTS_VIEWS, B_RGB32);
	if (fStaging == NULL || fStaging->InitCheck() != B_OK) {
		delete fStaging;
		fStaging = NULL;
		fStagingView = NULL;
		return false;
	}
	fStagingView = new BView(fStaging->Bounds(), "glyphs", B_FOLLOW_NONE, 0);
	fStaging->AddChild(fStagingView);
	return true;
}


void
GlyphCache::DrawPending()
{
	if (fPending.empty())
		return;

	int32 cellWidth = fMetrics.cellWidth;
	int32 cellHeight = fMetrics.cellHeight;

	// box drawing characters are drawn here; the rest by app_server, in
	// batches: each glyph in a box with a cell of room around it, so that
	// what reaches out of its cells is cut off rather than smeared into
	// the next glyph
	std::vector<Pending> drawn;
	for (size_t i = 0; i < fPending.size(); i++) {
		Pending& pending = fPending[i];
		int32 page, x, y;
		fAtlas.SlotLocation(pending.slot, page, x, y);
		if (page >= (int32)fPages.size())
			continue;

		if (is_box_character(pending.codePoint)) {
			uint8* bits = fPages[page] + (size_t)y * kPageSize + x;
			for (int32 row = 0; row < cellHeight; row++)
				memset(bits + (size_t)row * kPageSize, 0, cellWidth);
			int32 lineWidth = fMetrics.lineThickness;
			if ((pending.style & GLYPH_BOLD) != 0)
				lineWidth = lineWidth * 3 / 2 + 1;
			draw_box_character(pending.codePoint, bits, kPageSize,
				cellWidth * pending.cells, cellHeight, lineWidth);
			_MarkDirty(page, y, y + cellHeight);
			continue;
		}
		drawn.push_back(pending);
	}
	fPending.clear();

	int32 boxWidth = cellWidth * 4;
	int32 boxHeight = cellHeight * 2;
	if (!drawn.empty() && !_EnsureStaging(boxWidth * kStagingColumns,
			boxHeight * kStagingRows)) {
		return;
	}

	size_t perRound = kStagingColumns * kStagingRows;
	for (size_t start = 0; start < drawn.size(); start += perRound) {
		size_t end = std::min(drawn.size(), start + perRound);

		if (!fStaging->Lock())
			return;
		fStagingView->SetDrawingMode(B_OP_COPY);
		fStagingView->SetHighColor(0, 0, 0);
		fStagingView->FillRect(fStagingView->Bounds());
		fStagingView->SetHighColor(255, 255, 255);
		fStagingView->SetLowColor(0, 0, 0);
		for (size_t i = start; i < end; i++) {
			const Pending& pending = drawn[i];
			int32 index = i - start;
			float left = (index % kStagingColumns) * boxWidth + cellWidth;
			float top = (index / kStagingColumns) * boxHeight
				+ cellHeight / 2;
			uint32 style = pending.style;
			if (fEmulateBold)
				style &= ~GLYPH_BOLD;
			fStagingView->SetFont(&fFonts[style]);
			fStagingView->DrawString(pending.text.c_str(),
				BPoint(left, top + fMetrics.baseline));
		}
		fStagingView->Sync();

		const uint8* bits = (const uint8*)fStaging->Bits();
		int32 bytesPerRow = fStaging->BytesPerRow();
		for (size_t i = start; i < end; i++) {
			const Pending& pending = drawn[i];
			int32 index = i - start;
			int32 left = (index % kStagingColumns) * boxWidth + cellWidth;
			int32 top = (index / kStagingColumns) * boxHeight
				+ cellHeight / 2;
			int32 page, x, y;
			fAtlas.SlotLocation(pending.slot, page, x, y);
			int32 width = cellWidth * pending.cells;
			for (int32 row = 0; row < cellHeight; row++) {
				const uint8* source = bits + (size_t)(top + row) * bytesPerRow
					+ left * 4;
				uint8* target = fPages[page] + (size_t)(y + row) * kPageSize
					+ x;
				for (int32 column = 0; column < width; column++) {
					const uint8* pixel = source + column * 4;
					target[column] = (pixel[0] + pixel[1] + pixel[2]) / 3;
				}
			}
			if (fEmulateBold && (pending.style & GLYPH_BOLD) != 0)
				_EmboldenSlot(pending.slot, pending.cells);
			_MarkDirty(page, y, y + cellHeight);
		}
		fStaging->Unlock();
	}
}


//!	"Emulate bold": the glyph drawn again one pixel to the right.
void
GlyphCache::_EmboldenSlot(uint32 slot, int32 cells)
{
	int32 page, x, y;
	fAtlas.SlotLocation(slot, page, x, y);
	int32 width = fMetrics.cellWidth * cells;
	int32 shift = std::max((int32)1, (int32)roundf(fScale));
	for (int32 row = 0; row < fMetrics.cellHeight; row++) {
		uint8* bits = fPages[page] + (size_t)(y + row) * kPageSize + x;
		for (int32 column = width - 1; column >= shift; column--)
			bits[column] = std::max(bits[column], bits[column - shift]);
	}
}
