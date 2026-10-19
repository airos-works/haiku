/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef TERMINAL_CELL_H
#define TERMINAL_CELL_H


#include <string.h>

#include <SupportDefs.h>


/*!	A color as the emulator keeps it: the default foreground or background
	(0), one of the 256 palette entries, or a direct (true) color.
*/
typedef uint32 TermColor;

static const TermColor kDefaultColor = 0;
static const uint32 kColorTypeIndexed = 0x01000000;
static const uint32 kColorTypeDirect = 0x02000000;


static inline TermColor
palette_color(uint32 index)
{
	return kColorTypeIndexed | (index & 0xff);
}


static inline TermColor
rgb_color_value(uint8 red, uint8 green, uint8 blue)
{
	return kColorTypeDirect | ((uint32)red << 16) | ((uint32)green << 8)
		| blue;
}


static inline bool
is_indexed_color(TermColor color)
{
	return (color & 0xff000000) == kColorTypeIndexed;
}


static inline bool
is_direct_color(TermColor color)
{
	return (color & 0xff000000) == kColorTypeDirect;
}


// cell style flags
enum {
	CELL_BOLD			= 0x0001,
	CELL_FAINT			= 0x0002,
	CELL_ITALIC			= 0x0004,
	CELL_BLINK			= 0x0008,
	CELL_INVERSE		= 0x0010,
	CELL_INVISIBLE		= 0x0020,
	CELL_STRIKE			= 0x0040,
	CELL_OVERLINE		= 0x0080,
	CELL_PROTECTED		= 0x0100,	// DECSCA: not erased by DECSED/DECSEL
	CELL_WIDE			= 0x0200,	// first cell of a double width character
	CELL_WIDE_TAIL		= 0x0400,	// second cell of a double width character

	// what SGR 0 resets
	CELL_SGR_FLAGS		= CELL_BOLD | CELL_FAINT | CELL_ITALIC | CELL_BLINK
		| CELL_INVERSE | CELL_INVISIBLE | CELL_STRIKE | CELL_OVERLINE
};


// underline styles (SGR 4:n)
enum {
	UNDERLINE_NONE = 0,
	UNDERLINE_SINGLE,
	UNDERLINE_DOUBLE,
	UNDERLINE_CURLY,
	UNDERLINE_DOTTED,
	UNDERLINE_DASHED
};


//!	What SGR sets: colors and attributes of the text written next.
struct CellStyle {
	TermColor	foreground;
	TermColor	background;
	TermColor	underlineColor;
	uint16		flags;
	uint8		underline;
	uint8		reserved;

	CellStyle()
		:
		foreground(kDefaultColor),
		background(kDefaultColor),
		underlineColor(kDefaultColor),
		flags(0),
		underline(UNDERLINE_NONE),
		reserved(0)
	{
	}

	inline bool operator==(const CellStyle& other) const
	{
		return foreground == other.foreground
			&& background == other.background
			&& underlineColor == other.underlineColor
			&& flags == other.flags && underline == other.underline;
	}

	inline bool operator!=(const CellStyle& other) const
	{
		return !(*this == other);
	}
};


/*!	The character of a cell: 0 for a cell nothing was written to (erased
	cells are empty too, with the background color they were erased with),
	a code point, or the index of a grapheme cluster of several code points
	with kClusterBit set.
*/
static const uint32 kClusterBit = 0x80000000;


struct TerminalCell {
	uint32		character;
	uint32		hyperlink;		// 0 or the id of an OSC 8 link
	CellStyle	style;

	inline bool IsEmpty() const
	{
		return character == 0;
	}

	inline bool IsWideTail() const
	{
		return (style.flags & CELL_WIDE_TAIL) != 0;
	}

	inline bool IsWide() const
	{
		return (style.flags & CELL_WIDE) != 0;
	}
};


// line flags
enum {
	LINE_WRAPPED		= 0x0001,	// continues on the next line (soft wrap)
	LINE_PROMPT			= 0x0002,	// a shell prompt starts here (OSC 133;A)
	LINE_OUTPUT			= 0x0004,	// command output starts here (OSC 133;C)
	LINE_PLACEHOLDERS	= 0x0008,	// has kitty image placeholders
	LINE_SECONDARY_PROMPT = 0x0010	// a continuation prompt (k=s)
};


/*!	A row of cells. \c version changes with every change of the row's
	content, so a renderer can tell which rows to draw again.
*/
struct TerminalLine {
	TerminalCell*	cells;
	uint64			version;
	uint16			flags;
};


#endif	// TERMINAL_CELL_H
