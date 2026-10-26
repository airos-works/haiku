/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef OPEN_TYPE_LIGATURES_H
#define OPEN_TYPE_LIGATURES_H


#include <SupportDefs.h>

#include <ft2build.h>
#include FT_FREETYPE_H


/*!	The ligature substitutions (GSUB lookup type 4) of a font's 'ccmp',
	'liga', 'clig' and 'rlig' features, applied in lookup order. That is
	enough to shape emoji sequences: color emoji fonts join the flags, skin
	tones, keycaps, tag and ZWJ sequences with ligatures. Contextual lookups
	and script or language systems are not supported.
*/
class OpenTypeLigatures {
public:
								OpenTypeLigatures();
								~OpenTypeLigatures();

			bool				Init(FT_Face face);
			bool				HasLigatures() const
									{ return fLookupCount > 0; }

			// Substitutes the ligatures in glyphs, returns the new count.
			int32				Apply(uint32* glyphs, int32 count) const;

private:
			bool				_U16(uint32 offset, uint16& value) const;
			bool				_U32(uint32 offset, uint32& value) const;
			bool				_AddLookups(uint32 lookupList,
									const uint16* indices, int32 count);
			bool				_AddSubtable(uint32 subtable);
			int32				_CoverageIndex(uint32 coverage,
									uint32 glyph) const;
			int32				_ApplySubtable(uint32 subtable,
									uint32* glyphs, int32 count,
									int32 position) const;

private:
			uint8*				fTable;
			uint32				fTableSize;

			// Each lookup is a range of fSubtables.
	struct Lookup {
			int32				firstSubtable;
			int32				subtableCount;
	};
			Lookup*				fLookups;
			int32				fLookupCount;
			uint32*				fSubtables;
			int32				fSubtableCount;
			int32				fSubtableCapacity;
};


#endif	// OPEN_TYPE_LIGATURES_H
