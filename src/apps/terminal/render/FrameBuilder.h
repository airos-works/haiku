/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef FRAME_BUILDER_H
#define FRAME_BUILDER_H


#include <vector>

#include "RenderFrame.h"
#include "Terminal.h"


class GlyphCache;


/*!	A range of text drawn differently: the selection, a link under the
	pointer, a search match.
*/
struct FrameHighlight {
	TermPoint	start;
	TermPoint	end;			// exclusive
	bool		useColors;
	uint32		foreground;
	uint32		background;
	bool		underline;
};


struct FrameState {
	int32		firstRow;		// the terminal row at the top of the view
	bool		cursorVisible;	// blinking and focus taken into account
	bool		focused;
	uint32		cursorColor;
	uint32		cursorTextColor;
	std::vector<FrameHighlight> highlights;
};


/*!	Turns what the terminal shows into a RenderFrame, keeping track of the
	rows that changed since the last frame.
*/
class FrameBuilder {
public:
								FrameBuilder(GlyphCache* glyphs);

			void				Invalidate()	{ fForceAll = true; }

			//! Builds the frame. The terminal must be locked. Glyphs that
			//! are new are drawn afterwards with GlyphCache::DrawPending().
			void				Build(Terminal* terminal,
									const FrameState& state,
									int32 width, int32 height,
									RenderFrame& frame);

private:
			void				_BuildRow(Terminal* terminal,
									const FrameState& state,
									const TerminalCell* cells, int32 row,
									int32 terminalRow, RenderFrame& frame);
			uint64				_RowSignature(const FrameState& state,
									int32 terminalRow, uint64 version,
									Terminal* terminal) const;
			void				_AddExtraCursors(Terminal* terminal,
									const FrameState& state,
									RenderFrame& frame);

private:
			GlyphCache*			fGlyphs;
			std::vector<uint64>	fRowSignatures;
			std::vector<TerminalCell> fCells;
			int32				fColumns;
			int32				fRows;
			int32				fWidth;
			int32				fHeight;
			uint32				fGlyphEpoch;
			uint32				fColorsGeneration;
			uint32				fExtraCursorsGeneration;
			uint64				fGraphicsGeneration;
			int32				fFirstRow;
			bool				fHadImages;
			bool				fForceAll;
};


#endif	// FRAME_BUILDER_H
