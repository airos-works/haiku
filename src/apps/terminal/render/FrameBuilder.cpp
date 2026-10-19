/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "FrameBuilder.h"

#include <ctype.h>
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
	fExtraCursorsGeneration(0),
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
		|| fColorsGeneration != terminal->ColorsGeneration()
		|| fExtraCursorsGeneration != terminal->ExtraCursorsGeneration();
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
	fExtraCursorsGeneration = terminal->ExtraCursorsGeneration();

	// rows with extra cursors blink with the cursor
	std::vector<bool> extraCursorRows(rows, false);
	const std::map<uint32, uint8>& extraCursors = terminal->ExtraCursors();
	for (std::map<uint32, uint8>::const_iterator it = extraCursors.begin();
			it != extraCursors.end(); it++) {
		int32 row = (int32)(it->first >> 16) - state.firstRow;
		if (row >= 0 && row < rows)
			extraCursorRows[row] = true;
	}

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
		if (extraCursorRows[row]) {
			signature = mix(signature, state.cursorVisible ? 0x51 : 0x52);
			signature = mix(signature, state.focused ? 0x53 : 0x54);
			signature = mix(signature, state.cursorColor);
		}
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

	_AddExtraCursors(terminal, state, frame);
}


static uint32
resolve_extra_cursor_color(Terminal* terminal, const ExtraCursorColor& color,
	uint32 unset)
{
	switch (color.space) {
		case 2:
			return color.value;
		case 5:
			return terminal->ResolveColor(palette_color(color.value), true);
		default:
			return unset;
	}
}


/*!	kitty's extra cursors, on screen cells: they blink with the cursor and
	take its colors unless told otherwise, but DECTCEM does not hide them.
	Block cursors color their cells (only rows built anew are changed), the
	others are drawn over them.
*/
void
FrameBuilder::_AddExtraCursors(Terminal* terminal, const FrameState& state,
	RenderFrame& frame)
{
	frame.extraCursors.clear();
	const std::map<uint32, uint8>& extraCursors = terminal->ExtraCursors();
	if (extraCursors.empty() || !state.cursorVisible)
		return;

	const ExtraCursorColor& textColor = terminal->ExtraCursorColors(0);
	const ExtraCursorColor& cursorColor = terminal->ExtraCursorColors(1);
	int32 mainShape = terminal->Cursor().shape;

	for (std::map<uint32, uint8>::const_iterator it = extraCursors.begin();
			it != extraCursors.end(); it++) {
		int32 x = it->first & 0xffff;
		int32 row = (int32)(it->first >> 16) - state.firstRow;
		if (row < 0 || row >= frame.rows || x >= frame.columns)
			continue;

		int32 shape = it->second;
		if (shape == EXTRA_CURSOR_FOLLOW_MAIN) {
			shape = mainShape == CURSOR_SHAPE_UNDERLINE ? EXTRA_CURSOR_UNDERLINE
				: mainShape == CURSOR_SHAPE_BEAM ? EXTRA_CURSOR_BEAM
				: EXTRA_CURSOR_BLOCK;
		}
		int32 width = x + 1 < frame.columns
			&& (frame.Cell(row, x + 1).flags & RENDER_GLYPH_RIGHT_HALF) != 0
			? 2 : 1;
		RenderCell& cell = frame.Cell(row, x);

		// "special" is reverse video: for the cursor the cell's text
		// color, for the text the cell's background
		uint32 color = cursorColor.space == 1 ? cell.foreground
			: resolve_extra_cursor_color(terminal, cursorColor,
				state.cursorColor);
		uint32 text = cursorColor.space == 1 || textColor.space == 1
			? cell.background
			: resolve_extra_cursor_color(terminal, textColor,
				state.cursorTextColor);

		if (shape == EXTRA_CURSOR_BLOCK && state.focused) {
			if (!frame.allDirty && !frame.dirtyRows[row])
				continue;
			for (int32 i = 0; i < width; i++) {
				RenderCell& covered = frame.Cell(row, x + i);
				covered.background = color;
				covered.foreground = text;
				covered.decoration = text;
				covered.foregroundAlpha = 255;
				covered.flags &= ~RENDER_DEFAULT_BACKGROUND;
			}
			continue;
		}

		RenderCursor cursor;
		cursor.x = x;
		cursor.y = row;
		cursor.width = width;
		cursor.shape = shape == EXTRA_CURSOR_BLOCK ? RENDER_CURSOR_HOLLOW
			: shape == EXTRA_CURSOR_BEAM ? RENDER_CURSOR_BEAM
			: RENDER_CURSOR_UNDERLINE;
		cursor.color = color;
		frame.extraCursors.push_back(cursor);
	}
}


//!	The style of a cell's glyph, if the cell can be part of a ligature: a
//!	character of its own, one cell wide, shown.
static bool
ligature_style(const TerminalCell& cell, uint32& _style)
{
	uint32 character = cell.character;
	if (character == 0 || character == ' ' || character >= 0x110000
		|| (character & kClusterBit) != 0 || is_multicell(character)
		|| character == kPlaceholderCharacter || cell.IsWide()
		|| cell.IsWideTail() || (cell.style.flags & CELL_INVISIBLE) != 0) {
		return false;
	}
	_style = 0;
	if ((cell.style.flags & CELL_BOLD) != 0)
		_style |= GLYPH_BOLD;
	if ((cell.style.flags & CELL_ITALIC) != 0)
		_style |= GLYPH_ITALIC;
	return true;
}


static inline bool
is_symbol(uint32 c)
{
	return c > 0x20 && c < 0x7f && !isalnum((int)c);
}


/*!	Ligatures: words (runs of characters between spaces, in one style)
	with two symbols next to each other are shaped as a whole, as that is
	where coding fonts have them (->, !=, ===, //, ...).
*/
void
FrameBuilder::_FindLigatures(Terminal* terminal, const TerminalCell* cells,
	int32 terminalRow, int32 columns)
{
	fLigatureSlots.assign(columns, 0);
	int32 mode = fGlyphs->LigatureMode();
	if (mode == LIGATURES_NEVER)
		return;
	Terminal::CursorInfo cursor = terminal->Cursor();

	for (int32 x = 0; x < columns;) {
		uint32 style;
		if (!ligature_style(cells[x], style)) {
			x++;
			continue;
		}
		int32 start = x;
		bool symbols = false;
		fRunCodePoints.clear();
		uint32 cellStyle;
		while (x < columns && ligature_style(cells[x], cellStyle)
			&& cellStyle == style) {
			uint32 c = cells[x].character;
			if (x > start && is_symbol(c) && is_symbol(fRunCodePoints.back()))
				symbols = true;
			fRunCodePoints.push_back(c);
			x++;
		}
		if (!symbols)
			continue;
		if (mode == LIGATURES_NOT_AT_CURSOR && cursor.y == terminalRow
			&& cursor.x >= start && cursor.x <= x) {
			continue;
		}
		fGlyphs->LookupRun(fRunCodePoints.data(), fRunCodePoints.size(),
			style, &fLigatureSlots[start]);
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

	_FindLigatures(terminal, cells, terminalRow, columns);

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
		TermMulticell multicell;
		if (is_multicell(source.character)
			&& terminal->GetMulticell(multicell_index(source.character),
				multicell)) {
			// a cell of a text sizing block: its piece of the text, and the
			// lines along the block's edges only
			int32 blockY = multicell_y(source.character);
			if (blockY != multicell.scale - 1)
				flags &= ~RENDER_UNDERLINE_MASK;
			if (blockY != 0)
				flags &= ~RENDER_OVERLINE;
			if (multicell.scale > 1)
				flags &= ~RENDER_STRIKE;
			if ((style.flags & CELL_INVISIBLE) == 0) {
				text.clear();
				terminal->AppendCharacterUTF8(multicell.text, text);
				uint32 glyphStyle = 0;
				if ((style.flags & CELL_BOLD) != 0)
					glyphStyle |= GLYPH_BOLD;
				if ((style.flags & CELL_ITALIC) != 0)
					glyphStyle |= GLYPH_ITALIC;
				MulticellShape shape;
				shape.columns = multicell.columns;
				shape.rows = multicell.scale;
				shape.numerator = multicell.numerator;
				shape.denominator = multicell.denominator;
				shape.verticalAlignment = multicell.verticalAlignment;
				shape.horizontalAlignment = multicell.horizontalAlignment;
				glyph = fGlyphs->LookupMulticell(
					(uint64)source.character | (1ULL << 41), text.c_str(),
					shape, multicell_x(source.character), blockY, glyphStyle);
				if ((glyph & kColorGlyphSlot) != 0) {
					glyph &= ~kColorGlyphSlot;
					flags |= RENDER_COLOR_GLYPH;
				}
			}
		} else if (fLigatureSlots[x] != 0)
			glyph = fLigatureSlots[x];
		else if (source.character != 0 && source.character != ' '
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
