/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef COLOR_EMOJI_H
#define COLOR_EMOJI_H


#include <unordered_map>
#include <vector>

#include <SupportDefs.h>


typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_* FT_Face;


/*!	Emoji in color, from a color font (Noto Color Emoji's bitmaps, or a font
	with color layers), drawn with FreeType: app_server draws glyphs in grey
	levels only. Flags, skin tones and families are single glyphs that the
	font's ligatures (GSUB) make of the sequence.
*/
class ColorEmoji {
public:
								ColorEmoji();
								~ColorEmoji();

			//! Whether a color emoji font was found (looked for once).
			bool				IsAvailable();

			//! The font's glyph for the code points of a grapheme cluster,
			//! or 0.
			uint32				GlyphFor(const uint32* codePoints,
									int32 count);

			//! Draws a glyph fitted and centered in a width x height box of
			//! premultiplied B, G, R, A pixels.
			bool				Draw(uint32 glyph, uint32* bits,
									int32 pixelsPerRow, int32 width,
									int32 height);

	//! Whether a grapheme cluster asks for emoji presentation.
	static	bool				IsEmojiCluster(const uint32* codePoints,
									int32 count);

private:
			struct Ligature {
				std::vector<uint16> components;	// after the first glyph
				uint16			glyph;
			};
			typedef std::unordered_map<uint16, std::vector<Ligature> >
				LigatureLookup;

			bool				_Load();
			bool				_Open(const char* path);
			void				_ReadLigatures();
			void				_ReadLigatureSubtable(const uint8* table,
									size_t length, size_t offset,
									LigatureLookup& lookup);

private:
			FT_Library			fLibrary;
			FT_Face				fFace;
			bool				fLoaded;
			std::vector<LigatureLookup> fLookups;	// in the font's order
};


#endif	// COLOR_EMOJI_H
