/*
 * Copyright 2007-2009, Haiku. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Maxim Shemanarev <mcseemagg@yahoo.com>
 *		Stephan Aßmus <superstippi@gmx.de>
 *		Andrej Spielmann, <andrej.spielmann@seh.ox.ac.uk>
 */

//----------------------------------------------------------------------------
// Anti-Grain Geometry - Version 2.4
// Copyright (C) 2002-2005 Maxim Shemanarev (http://www.antigrain.com)
//
// Permission to copy, use, modify, sell and distribute this software
// is granted provided this copyright notice appears in all copies.
// This software is provided "as is" without express or implied
// warranty, and with no claim as to its suitability for any purpose.
//
//----------------------------------------------------------------------------
// Contact: mcseem@antigrain.com
//			mcseemagg@yahoo.com
//			http://www.antigrain.com
//----------------------------------------------------------------------------

#ifndef FONT_CACHE_ENTRY_H
#define FONT_CACHE_ENTRY_H


#include <AutoDeleter.h>
#include <Locker.h>

#include <agg_conv_curve.h>
#include <agg_conv_contour.h>
#include <agg_conv_transform.h>

#include "ServerFont.h"
#include "FontEngine.h"
#include "MultiLocker.h"
#include "Referenceable.h"
#include "Transformable.h"


struct GlyphCache {
	GlyphCache(uint32 glyphIndex, uint32 dataSize, glyph_data_type dataType,
			const agg::rect_i& bounds, float advanceX, float advanceY,
			float preciseAdvanceX, float preciseAdvanceY,
			float insetLeft, float insetRight)
		:
		glyph_index(glyphIndex),
		data((uint8*)malloc(dataSize)),
		data_size(dataSize),
		data_type(dataType),
		bounds(bounds),
		advance_x(advanceX),
		advance_y(advanceY),
		precise_advance_x(preciseAdvanceX),
		precise_advance_y(preciseAdvanceY),
		inset_left(insetLeft),
		inset_right(insetRight),
		hash_link(NULL)
	{
	}

	~GlyphCache()
	{
		free(data);
	}

	uint32			glyph_index;
	uint8*			data;
	uint32			data_size;
	glyph_data_type	data_type;
	agg::rect_i		bounds;
	float			advance_x;
	float			advance_y;
	float			precise_advance_x;
	float			precise_advance_y;
	float			inset_left;
	float			inset_right;

	GlyphCache*		hash_link;
};

//! Glyph cache keys of glyphs from a color font, by their glyph index.
static const uint32 kColorGlyphKey = 0x80000000;

//! An emoji sequence and the glyph a color font joined (part of) it into.
struct EmojiSequence {
	static const int32 kMaxCodes = 16;

	uint32			codes[kMaxCodes];
	int32			count;
	uint32			glyph_key;
		// 0 when the color font has no glyph for the sequence
	int32			used_codes;
		// the length of the prefix the glyph is for

	EmojiSequence*	hash_link;
};

class FontCache;

class FontCacheEntry : public MultiLocker, public BReferenceable {
 public:
	typedef FontEngine::PathAdapter					GlyphPathAdapter;
	typedef FontEngine::Gray8Adapter				GlyphGray8Adapter;
	typedef GlyphGray8Adapter::embedded_scanline	GlyphGray8Scanline;
	typedef FontEngine::MonoAdapter					GlyphMonoAdapter;
	typedef GlyphMonoAdapter::embedded_scanline		GlyphMonoScanline;
	typedef FontEngine::SubpixAdapter				SubpixAdapter;
	typedef agg::conv_curve<GlyphPathAdapter>		CurveConverter;
	typedef agg::conv_contour<CurveConverter>		ContourConverter;

	typedef agg::conv_transform<CurveConverter, Transformable>
													TransformedOutline;

	typedef agg::conv_transform<ContourConverter, Transformable>
													TransformedContourOutline;


								FontCacheEntry();
	virtual						~FontCacheEntry();

			bool				Init(const ServerFont& font, bool forceVector);

			bool				HasGlyphs(const char* utf8String,
									ssize_t glyphCount) const;

			const GlyphCache*	CachedGlyph(uint32 glyphCode);
			const GlyphCache*	CreateGlyph(uint32 glyphCode,
									FontCacheEntry* fallbackEntry = NULL);
			bool				CanCreateGlyph(uint32 glyphCode);

			bool				HasColorGlyphs() const
									{ return fEngine.HasColorGlyphs(); }
			// Needs a write lock; see FontEngine::ShapeSequence().
			bool				ShapeSequence(const uint32* codes,
									int32 count, uint32& glyphIndex,
									int32& usedCodes)
									{ return fEngine.ShapeSequence(codes,
										count, glyphIndex, usedCodes); }
			// Caches the glyph of sourceEntry's font (or of this one) with
			// that glyph index, under kColorGlyphKey | glyphIndex.
			const GlyphCache*	CreateColorGlyph(uint32 glyphIndex,
									FontCacheEntry* sourceEntry);

			const EmojiSequence* CachedSequence(const uint32* codes,
									int32 count) const;
			const EmojiSequence* CacheSequence(const uint32* codes,
									int32 count, uint32 glyphKey,
									int32 usedCodes);

			void				InitAdaptors(const GlyphCache* glyph,
									double x, double y,
									GlyphMonoAdapter& monoAdapter,
									GlyphGray8Adapter& gray8Adapter,
									GlyphPathAdapter& pathAdapter,
									double scale = 1.0);

			bool				GetKerning(uint32 glyphCode1,
									uint32 glyphCode2, double* x, double* y);

	static	void				GenerateSignature(char* signature,
									size_t signatureSize,
									const ServerFont& font, bool forceVector);

	// private to FontCache class:
			void				UpdateUsage();
			bigtime_t			LastUsed() const
									{ return fLastUsedTime; }
			uint64				UsedCount() const
									{ return fUseCounter; }

 private:
								FontCacheEntry(const FontCacheEntry&);
			const FontCacheEntry& operator=(const FontCacheEntry&);

	static	glyph_rendering		_RenderTypeFor(const ServerFont& font,
									bool forceVector);

			class GlyphCachePool;
			class SequenceCachePool;

			ObjectDeleter<GlyphCachePool>
								fGlyphCache;
			ObjectDeleter<SequenceCachePool>
								fSequenceCache;
			// FreeType's memory face borrows the style's font data. Retain it
			// until after fEngine is destroyed, including after its app exits.
			BReference<FontStyle>
								fFontStyle;
			FontEngine			fEngine;

	static	BLocker				sUsageUpdateLock;
			bigtime_t			fLastUsedTime;
			uint64				fUseCounter;
};

#endif // FONT_CACHE_ENTRY_H

