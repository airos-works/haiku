/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef LIGATURES_H
#define LIGATURES_H


#include <string>
#include <vector>

#include <SupportDefs.h>


class BFont;
struct HarfBuzz;
typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;


//! A run of cells the font draws differently together (a ligature).
struct LigatureSegment {
	int32		start;		// the first cell of the run
	int32		length;		// in cells
};


/*!	Ligatures of coding fonts (Fira Code, JetBrains Mono, Cascadia Code):
	runs of characters are shaped with HarfBuzz, loaded when it is there,
	and where the font puts other glyphs than the characters' own, those
	cells are drawn together with FreeType from the glyphs (app_server draws
	characters, not glyphs). The fonts' files are found by their family and
	style names.
*/
class Ligatures {
public:
								Ligatures();
								~Ligatures();

			//! The fonts by glyph style, at their size in pixels; none for
			//! a style that is slanted rather than italic.
			void				SetFonts(const BFont* fonts, int32 count,
									const bool* usable);
			bool				IsAvailable(uint32 style);

			/*!	Shapes the characters of a run of cells, one per cell, and
				returns the runs of cells that are ligatures.
			*/
			void				Shape(uint32 style, const uint32* codePoints,
									int32 count,
									std::vector<LigatureSegment>& segments);

			/*!	Draws the cells \a segment of a run in grey levels into
				\a bits (as wide as the segment), the baseline at
				\a baseline.
			*/
			void				Draw(uint32 style, const uint32* codePoints,
									int32 count, const LigatureSegment& segment,
									uint8* bits, int32 bytesPerRow,
									int32 cellWidth, int32 cellHeight,
									int32 baseline);

private:
			struct Font {
								Font();

				std::string		family;
				std::string		style;
				float			size;
				bool			usable;
				bool			looked;		// for the file
				FT_Face			face;
				void*			hbFont;
			};

			struct Glyph {
				uint32			id;
				int32			cluster;
				int32			xOffset;	// 26.6
				int32			yOffset;
			};

			bool				_Open(Font& font);
			bool				_ShapeGlyphs(Font& font,
									const uint32* codePoints, int32 count,
									std::vector<Glyph>& glyphs);
			void				_Close(Font& font);

private:
			HarfBuzz*			fHarfBuzz;
			FT_Library			fLibrary;
			Font				fFonts[4];
			void*				fBuffer;
};


#endif	// LIGATURES_H
