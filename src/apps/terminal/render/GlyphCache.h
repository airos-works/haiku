/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef GLYPH_CACHE_H
#define GLYPH_CACHE_H


#include <string>
#include <unordered_map>
#include <vector>

#include <Font.h>

#include "ColorEmoji.h"
#include "RenderFrame.h"


class BBitmap;
class BView;


// glyph styles
enum {
	GLYPH_BOLD		= 0x1,
	GLYPH_ITALIC	= 0x2
};

//! Set in a slot Lookup() returns when the glyph is in color (an emoji).
static const uint32 kColorGlyphSlot = 0x80000000;


//! How the text of a text sizing block is laid out (see TermMulticell).
struct MulticellShape {
	int32		columns;
	int32		rows;		// the scale
	int32		numerator;
	int32		denominator;
	int32		verticalAlignment;
	int32		horizontalAlignment;
};


/*!	Glyphs drawn once by app_server (so they look like all other text) at
	the frame buffer's density, kept in an atlas of cell sized slots that
	the renderers draw from. Box drawing and block characters are drawn
	here instead, to meet exactly from cell to cell, and so are emoji when a
	color emoji font is installed: into color pages beside the coverage
	ones.
*/
class GlyphCache {
public:
								GlyphCache();
								~GlyphCache();

			//! Sets the font, at its size in view coordinates, and how
			//! many frame buffer pixels a view coordinate has.
			void				SetFont(const BFont& font, float scale,
									bool allowBold, bool emulateBold);
			const RenderMetrics& Metrics() const	{ return fMetrics; }
			//! The cell size in view coordinates.
			float				CellWidth() const;
			float				CellHeight() const;

			/*!	The slot of a glyph, given as UTF-8, that is \a cells wide.
				New glyphs get a slot at once and are drawn by
				DrawPending(). Returns 0 when there is no room, and has
				kColorGlyphSlot set for a glyph in the color pages.
			*/
			uint32				Lookup(uint64 key, const char* utf8,
									uint32 firstCodePoint, int32 cells,
									uint32 style);
			/*!	The slot of one cell of a text sizing block: the piece of
				the block's text, drawn in its size, that falls into the
				cell at \a x, \a y of the block.
			*/
			uint32				LookupMulticell(uint64 key, const char* utf8,
									const MulticellShape& shape, int32 x,
									int32 y, uint32 style);
			bool				HasPending() const
									{ return !fPending.empty(); }
			//! Draws the glyphs looked up since the last call.
			void				DrawPending();

			//! Changes when every slot was given up (the atlas was full or
			//! the font changed): renderers must look up all glyphs again.
			uint32				Epoch() const		{ return fEpoch; }

			const GlyphAtlasInfo& Atlas() const		{ return fAtlas; }

private:
			struct Pending {
				uint32			slot;
				std::string		text;
				uint32			codePoint;
				int32			cells;
				uint32			style;
				uint32			emoji;	// the color font's glyph, or 0
				bool			multicell;
				MulticellShape	shape;	// of the multicell
				int32			x;		// the cell in it
				int32			y;
			};

			void				_Reset();
			uint32				_AllocateSlot(int32 cells);
			bool				_AddPage();
			uint32*				_ColorPage(int32 page);
			void				_MarkDirty(int32 page, int32 top,
									int32 bottom);
			bool				_EnsureStaging(int32 width, int32 height);
			void				_EmboldenSlot(uint32 slot, int32 cells);
			void				_DrawCluster(const Pending& pending,
									const BFont& font, BPoint origin);
			void				_DrawMulticells(std::vector<Pending>& pending);

private:
			BFont				fFonts[4];		// by style
			float				fScale;
			bool				fAllowBold;
			bool				fEmulateBold;
			RenderMetrics		fMetrics;
			float				fFontAscent;

			std::unordered_map<uint64, uint32> fSlots;
			std::vector<Pending> fPending;
			uint32				fNextSlot;
			uint32				fEpoch;

			std::vector<uint8*>	fPages;
			std::vector<uint32*> fColorPages;	// NULL until a page has one
			ColorEmoji			fEmoji;
			std::vector<GlyphAtlasInfo::Dirty> fDirty;
			GlyphAtlasInfo		fAtlas;

			BBitmap*			fStaging;
			BView*				fStagingView;
};


#endif	// GLYPH_CACHE_H
