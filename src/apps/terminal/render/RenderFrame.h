/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RENDER_FRAME_H
#define RENDER_FRAME_H


#include <vector>

#include <SupportDefs.h>

#include "TerminalGraphics.h"


/*!	What a renderer needs to draw the terminal: cells with their colors
	resolved and their glyphs in the atlas, the cursor and the images. It is
	shared with the OpenGL renderer add-on, so it holds plain data only.
*/


// RenderCell flags
enum {
	RENDER_UNDERLINE_MASK	= 0x0007,	// UNDERLINE_* style
	RENDER_STRIKE			= 0x0008,
	RENDER_OVERLINE			= 0x0010,
	RENDER_GLYPH_RIGHT_HALF	= 0x0020,	// draw the right cell of a wide glyph
	RENDER_COLOR_GLYPH		= 0x0040,	// the glyph has its own colors
	RENDER_DEFAULT_BACKGROUND = 0x0080,	// background shows images below
	RENDER_HYPERLINK_HOVER	= 0x0100
};


struct RenderCell {
	uint32	glyph;			// atlas slot, 0 for none
	uint32	foreground;		// 0x00RRGGBB
	uint32	background;		// 0x00RRGGBB
	uint32	decoration;		// underline, strike and overline color
	uint16	flags;
	uint8	foregroundAlpha;	// 255, or less for faint text
	uint8	reserved;
};


enum {
	RENDER_CURSOR_NONE = 0,
	RENDER_CURSOR_BLOCK,
	RENDER_CURSOR_UNDERLINE,
	RENDER_CURSOR_BEAM,
	RENDER_CURSOR_HOLLOW
};


/*!	The glyph atlas: pages of coverage (8 bits per pixel) divided into
	slots of one cell. A wide glyph takes two slots in a row. Slot 0 is
	never used.
*/
struct GlyphAtlasInfo {
	int32		pageSize;		// pages are pageSize x pageSize pixels
	int32		cellWidth;
	int32		cellHeight;
	int32		slotsPerRow;
	int32		slotsPerPage;
	int32		pageCount;
	const uint8* const* pages;	// pageCount pointers, 1 byte per pixel
	const uint32* const* colorPages;	// RGBA pages for color glyphs or NULL
	uint64		generation;		// changes whenever pixels change
	// the area changed since the renderer last took it, per page
	struct Dirty {
		int32	top;
		int32	bottom;		// exclusive; top >= bottom: clean
	};
	Dirty*		dirty;

	inline void SlotLocation(uint32 slot, int32& page, int32& x,
		int32& y) const
	{
		page = slot / slotsPerPage;
		int32 local = slot % slotsPerPage;
		x = (local % slotsPerRow) * cellWidth;
		y = (local / slotsPerRow) * cellHeight;
	}
};


//! A cursor drawn over its cells: a bar, an underline or an outline.
struct RenderCursor {
	int32		x;
	int32		y;
	int32		width;			// in cells
	int32		shape;			// RENDER_CURSOR_*
	uint32		color;
};


struct RenderMetrics {
	int32		cellWidth;		// in frame buffer pixels
	int32		cellHeight;
	int32		baseline;		// from the top of the cell
	int32		underlinePosition;	// top of the underline, from the top
	int32		underlineThickness;
	int32		strikePosition;
	int32		lineThickness;	// for strike, overline, cursor
};


struct RenderFrame {
	int32		columns;
	int32		rows;
	int32		width;			// of the target, in pixels
	int32		height;
	RenderMetrics metrics;

	std::vector<RenderCell> cells;		// rows * columns
	std::vector<bool>	dirtyRows;

	uint32		defaultBackground;	// 0x00RRGGBB
	bool		allDirty;

	// the cursor
	int32		cursorX;
	int32		cursorY;
	int32		cursorShape;	// RENDER_CURSOR_*
	int32		cursorWidth;	// in cells
	uint32		cursorColor;
	uint32		cursorTextColor;
	uint32		cursorTextGlyph;
	uint16		cursorTextFlags;

	// kitty's extra cursors that are not blocks (those color their cells)
	std::vector<RenderCursor> extraCursors;

	std::vector<ImageDraw> images;		// relative to the top left
	bool		imagesChanged;

	inline RenderCell& Cell(int32 row, int32 column)
	{
		return cells[(size_t)row * columns + column];
	}

	inline const RenderCell& Cell(int32 row, int32 column) const
	{
		return cells[(size_t)row * columns + column];
	}
};


#endif	// RENDER_FRAME_H
