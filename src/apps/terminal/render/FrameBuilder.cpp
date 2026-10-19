/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "FrameBuilder.h"

#include <string.h>

#include <algorithm>
#include <string>

#include "GlyphCache.h"
#include "PlaceholderDiacritics.h"
#include "TerminalGraphics.h"


static inline uint64
mix(uint64 hash, uint64 value)
{
	hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
	return hash;
}


FrameBuilder::FrameBuilder(GlyphCache* glyphs)
	:
	fGlyphs(glyphs),
	fColumns(0),
	fRows(0),
	fWidth(0),
	fHeight(0),
	fGlyphEpoch(0),
	fColorsGeneration(0),
	fGraphicsGeneration(0),
	fFirstRow(0),
	fHadImages(false),
	fForceAll(true)
{
}


uint64
FrameBuilder::_RowSignature(const FrameState& state, int32 terminalRow,
	uint64 version, Terminal* terminal) const
{
	uint64 hash = mix(0, version);
	hash = mix(hash, (uint64)(int64)terminalRow);

	Terminal::CursorInfo cursor = terminal->Cursor();
	if (cursor.y == terminalRow) {
		hash = mix(hash, 1 + cursor.x);
		hash = mix(hash, state.cursorVisible ? cursor.shape + 1 : 0);
		hash = mix(hash, state.focused ? 1 : 2);
		hash = mix(hash, state.cursorColor);
	}

	for (size_t i = 0; i < state.highlights.size(); i++) {
		const FrameHighlight& highlight = state.highlights[i];
		if (highlight.start.y > terminalRow || highlight.end.y < terminalRow)
			continue;
		hash = mix(hash, 0x1000 + i);
		hash = mix(hash, highlight.start.y == terminalRow
			? highlight.start.x + 1 : 0);
		hash = mix(hash, highlight.end.y == terminalRow
			? highlight.end.x + 1 : 0);
		hash = mix(hash, highlight.useColors ? highlight.background : 1);
		hash = mix(hash, highlight.underline ? 7 : 8);
	}
	return hash;
}


void
FrameBuilder::Build(Terminal* terminal, const FrameState& state, int32 width,
	int32 height, RenderFrame& frame)
{
	int32 columns = terminal->Columns();
	int32 rows = terminal->Rows();
	const RenderMetrics& metrics = fGlyphs->Metrics();

	bool all = fForceAll || columns != fColumns || rows != fRows
		|| width != fWidth || height != fHeight
		|| fGlyphEpoch != fGlyphs->Epoch()
		|| fColorsGeneration != terminal->ColorsGeneration();
	fForceAll = false;

	if (columns != fColumns || rows != fRows) {
		fColumns = columns;
		fRows = rows;
		fRowSignatures.assign(rows, 0);
		fCells.resize(columns);
	}
	fWidth = width;
	fHeight = height;
	fGlyphEpoch = fGlyphs->Epoch();
	fColorsGeneration = terminal->ColorsGeneration();

	frame.columns = columns;
	frame.rows = rows;
	frame.width = width;
	frame.height = height;
	frame.metrics = metrics;
	frame.cells.resize((size_t)columns * rows);
	frame.dirtyRows.assign(rows, false);
	frame.defaultBackground = terminal->ResolveColor(kDefaultColor, false);
	if (terminal->IsReverseVideo())
		frame.defaultBackground = terminal->ResolveColor(kDefaultColor, true);

	// images: everything is drawn again when they change
	TerminalGraphics* graphics = terminal->Graphics();
	frame.images.clear();
	bool hasPlaceholders = false;
	if (graphics != NULL) {
		if (graphics->HasPlacements())
			graphics->CollectDraws(state.firstRow, rows, frame.images);
	}
	for (int32 row = 0; row < rows && !hasPlaceholders; row++) {
		if ((terminal->LineFlags(state.firstRow + row) & LINE_PLACEHOLDERS)
				!= 0) {
			hasPlaceholders = true;
		}
	}
	bool hasImages = !frame.images.empty() || hasPlaceholders;
	uint64 graphicsGeneration = graphics != NULL ? graphics->Generation() : 0;
	frame.imagesChanged = hasImages != fHadImages
		|| (hasImages && (graphicsGeneration != fGraphicsGeneration
			|| state.firstRow != fFirstRow));
	if (frame.imagesChanged)
		all = true;
	fHadImages = hasImages;
	fGraphicsGeneration = graphicsGeneration;
	fFirstRow = state.firstRow;
	frame.allDirty = all;

	for (int32 row = 0; row < rows; row++) {
		int32 terminalRow = state.firstRow + row;
		uint16 flags = 0;
		uint64 version = 0;
		const TerminalLine* line = terminal->ScreenLine(terminalRow);
		if (line != NULL) {
			version = line->version;
			flags = line->flags;
		} else if (!terminal->GetLine(terminalRow, fCells.data(), &flags,
				&version)) {
			version = 0;
		}

		uint64 signature = _RowSignature(state, terminalRow, version,
			terminal);
		if (!all && signature == fRowSignatures[row])
			continue;
		fRowSignatures[row] = signature;
		frame.dirtyRows[row] = true;

		const TerminalCell* cells;
		if (line != NULL)
			cells = line->cells;
		else {
			if (version == 0) {
				for (int32 x = 0; x < columns; x++) {
					fCells[x].character = 0;
					fCells[x].hyperlink = 0;
					fCells[x].style = CellStyle();
				}
			}
			cells = fCells.data();
		}
		_BuildRow(terminal, state, cells, row, terminalRow, frame);

		if ((flags & LINE_PLACEHOLDERS) != 0 && graphics != NULL)
			graphics->CollectPlaceholderDraws(cells, columns, row,
				frame.images);
	}

	if (hasPlaceholders && !all) {
		// placeholder images of rows that did not change
		for (int32 row = 0; row < rows; row++) {
			int32 terminalRow = state.firstRow + row;
			if (frame.dirtyRows[row]
				|| (terminal->LineFlags(terminalRow) & LINE_PLACEHOLDERS)
					== 0) {
				continue;
			}
			terminal->GetLine(terminalRow, fCells.data());
			graphics->CollectPlaceholderDraws(fCells.data(), columns, row,
				frame.images);
		}
	}

	// the cursor
	Terminal::CursorInfo cursor = terminal->Cursor();
	frame.cursorShape = RENDER_CURSOR_NONE;
	int32 cursorRow = cursor.y - state.firstRow;
	if (state.cursorVisible && cursor.visible && cursorRow >= 0
		&& cursorRow < rows && cursor.x < columns) {
		frame.cursorX = cursor.x;
		frame.cursorY = cursorRow;
		frame.cursorWidth = 1;
		if (cursor.x + 1 < columns
			&& (frame.Cell(cursorRow, cursor.x + 1).flags
				& RENDER_GLYPH_RIGHT_HALF) != 0) {
			frame.cursorWidth = 2;
		}
		frame.cursorColor = state.cursorColor;
		frame.cursorTextColor = state.cursorTextColor;
		if (!state.focused)
			frame.cursorShape = RENDER_CURSOR_HOLLOW;
		else if (cursor.shape == CURSOR_SHAPE_UNDERLINE)
			frame.cursorShape = RENDER_CURSOR_UNDERLINE;
		else if (cursor.shape == CURSOR_SHAPE_BEAM)
			frame.cursorShape = RENDER_CURSOR_BEAM;
		else {
			frame.cursorShape = RENDER_CURSOR_BLOCK;
			// the cell under a block cursor takes its colors
			for (int32 i = 0; i < frame.cursorWidth; i++) {
				RenderCell& cell = frame.Cell(cursorRow, cursor.x + i);
				cell.background = state.cursorColor;
				cell.foreground = state.cursorTextColor;
				cell.decoration = state.cursorTextColor;
				cell.foregroundAlpha = 255;
				cell.flags &= ~RENDER_DEFAULT_BACKGROUND;
			}
		}
	}
}


void
FrameBuilder::_BuildRow(Terminal* terminal, const FrameState& state,
	const TerminalCell* cells, int32 row, int32 terminalRow,
	RenderFrame& frame)
{
	int32 columns = frame.columns;
	bool reverse = terminal->IsReverseVideo();
	uint32 defaultForeground = terminal->ResolveColor(kDefaultColor,
		!reverse);
	uint32 defaultBackground = terminal->ResolveColor(kDefaultColor,
		reverse);

	std::string text;
	uint32 codePoints[4];

	for (int32 x = 0; x < columns; x++) {
		const TerminalCell& source = cells[x];
		RenderCell& cell = frame.Cell(row, x);

		// the right half of a wide character draws from its left half
		if (source.IsWideTail() && x > 0
			&& (frame.Cell(row, x - 1).flags & RENDER_GLYPH_RIGHT_HALF) == 0
			&& cells[x - 1].IsWide()) {
			cell = frame.Cell(row, x - 1);
			cell.flags |= RENDER_GLYPH_RIGHT_HALF;
			continue;
		}

		const CellStyle& style = source.style;
		uint32 foreground = style.foreground == kDefaultColor
			? defaultForeground : terminal->ResolveColor(style.foreground,
				true);
		uint32 background = style.background == kDefaultColor
			? defaultBackground : terminal->ResolveColor(style.background,
				false);
		bool defaultBack = style.background == kDefaultColor;
		if ((style.flags & CELL_INVERSE) != 0) {
			std::swap(foreground, background);
			defaultBack = false;
		}

		uint16 flags = style.underline & RENDER_UNDERLINE_MASK;
		if ((style.flags & CELL_STRIKE) != 0)
			flags |= RENDER_STRIKE;
		if ((style.flags & CELL_OVERLINE) != 0)
			flags |= RENDER_OVERLINE;

		uint32 decoration = style.underlineColor == kDefaultColor
			? foreground : terminal->ResolveColor(style.underlineColor, true);

		// highlights: the selection, links, matches
		TermPoint position(x, terminalRow);
		for (size_t i = 0; i < state.highlights.size(); i++) {
			const FrameHighlight& highlight = state.highlights[i];
			if (position < highlight.start || !(position < highlight.end))
				continue;
			if (highlight.useColors) {
				foreground = highlight.foreground;
				background = highlight.background;
				decoration = foreground;
				defaultBack = false;
			}
			if (highlight.underline) {
				flags = (flags & ~RENDER_UNDERLINE_MASK) | UNDERLINE_SINGLE;
				decoration = foreground;
			}
		}

		if (defaultBack)
			flags |= RENDER_DEFAULT_BACKGROUND;

		uint32 glyph = 0;
		if (source.character != 0 && source.character != ' '
			&& (style.flags & CELL_INVISIBLE) == 0) {
			int32 count = terminal->GetCharacter(source.character,
				codePoints, 4);
			if (count > 0 && codePoints[0] != kPlaceholderCharacter) {
				text.clear();
				terminal->AppendCharacterUTF8(source.character, text);
				uint32 glyphStyle = 0;
				if ((style.flags & CELL_BOLD) != 0)
					glyphStyle |= GLYPH_BOLD;
				if ((style.flags & CELL_ITALIC) != 0)
					glyphStyle |= GLYPH_ITALIC;
				uint64 key = (source.character & kClusterBit) != 0
					? (uint64)(source.character & ~kClusterBit) | (1ULL << 40)
					: source.character;
				glyph = fGlyphs->Lookup(key, text.c_str(), codePoints[0],
					source.IsWide() ? 2 : 1, glyphStyle);
				if ((glyph & kColorGlyphSlot) != 0) {
					glyph &= ~kColorGlyphSlot;
					flags |= RENDER_COLOR_GLYPH;
				}
			}
		}

		cell.glyph = glyph;
		cell.foreground = foreground;
		cell.background = background;
		cell.decoration = decoration;
		cell.flags = flags;
		cell.foregroundAlpha = (style.flags & CELL_FAINT) != 0 ? 128 : 255;
		cell.reserved = 0;
	}
}
