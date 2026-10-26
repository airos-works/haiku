/*
 * Copyright 2005-2009, Stephan Aßmus <superstippi@gmx.de>.
 * Copyright 2008, Andrej Spielmann <andrej.spielmann@seh.ox.ac.uk>.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef AGG_TEXT_RENDERER_H
#define AGG_TEXT_RENDERER_H


#include "defines.h"

#include "FontCacheEntry.h"
#include "ServerFont.h"
#include "Transformable.h"

#include <agg_conv_curve.h>
#include <agg_conv_contour.h>
#include <agg_scanline_u.h>


class FontCacheReference;
class IntRect;

class AGGTextRenderer {
public:
								AGGTextRenderer(
									renderer_base& baseRenderer,
									renderer_subpix_type& subpixRenderer,
									renderer_type& solidRenderer,
									renderer_bin_type& binRenderer,
									scanline_unpacked_type& scanline,
									scanline_unpacked_subpix_type&
										subpixScanline,
									rasterizer_subpix_type& subpixRasterizer,
									scanline_unpacked_masked_type*&
										maskedScanline,
									agg::clipped_alpha_mask*& alphaMask,
									agg::trans_affine& viewTransformation);
	virtual						~AGGTextRenderer();

			void				SetFont(const ServerFont &font);
	inline	const ServerFont&	Font() const
									{ return fFont; }

			void				SetHinting(bool hinting);
			bool				Hinting() const
									{ return fHinted; }

			void				SetAntialiasing(bool antialiasing);
			bool				Antialiasing() const
									{ return fAntialias; }

			// The opacity of color glyphs (emoji), whose pixels have their
			// own colors; the drawing mode applies it.
			void				SetColorGlyphAlpha(uint8 alpha)
									{ fColorGlyphAlpha = alpha; }

			BRect				RenderString(const char* utf8String,
									uint32 length, const BPoint& baseLine,
									const BRect& clippingFrame, bool dryRun,
									BPoint* nextCharPos,
									const escapement_delta* delta,
									FontCacheReference* cacheReference);

			BRect				RenderString(const char* utf8String,
									uint32 length, const BPoint* offsets,
									const BRect& clippingFrame, bool dryRun,
									BPoint* nextCharPos,
									FontCacheReference* cacheReference);

private:

	class StringRenderer;
	friend class StringRenderer;
	struct ColorSpanRenderer;
	friend struct ColorSpanRenderer;

			void				_DrawColorGlyph(const GlyphCache* glyph,
									double x, double y,
									const Transformable& transform,
									const IntRect& clippingFrame);
			void				_DrawColorGlyphShape(const GlyphCache* glyph,
									int32 left, int32 top,
									const IntRect& clippingFrame);
			void				_BlendColorSpan(int x, int y, unsigned length,
									const agg::rgba8* colors,
									const uint8* covers, uint8 cover);

	// Pipeline to process the vectors glyph paths (curves + contour)
	FontCacheEntry::GlyphPathAdapter	fPathAdaptor;
	FontCacheEntry::GlyphGray8Adapter	fGray8Adaptor;
	FontCacheEntry::GlyphGray8Scanline	fGray8Scanline;
	FontCacheEntry::GlyphMonoAdapter	fMonoAdaptor;
	FontCacheEntry::GlyphMonoScanline	fMonoScanline;

	FontCacheEntry::CurveConverter		fCurves;
	FontCacheEntry::ContourConverter	fContour;

	renderer_base&				fBaseRenderer;
	renderer_type&				fSolidRenderer;
	renderer_bin_type&			fBinRenderer;
	renderer_subpix_type&		fSubpixRenderer;
	scanline_unpacked_type&		fScanline;
	scanline_unpacked_subpix_type& fSubpixScanline;
	rasterizer_subpix_type&		fSubpixRasterizer;
	scanline_unpacked_masked_type*& fMaskedScanline;
	agg::clipped_alpha_mask*&	fAlphaMask;

	rasterizer_type				fRasterizer;
		// NOTE: the object has it's own rasterizer object
		// since it might be using a different gamma setting
		// to support non-anti-aliased text rendering

	ServerFont					fFont;
	bool						fHinted;
									// is glyph hinting active?
	bool						fAntialias;
	uint8						fColorGlyphAlpha;
	bool						fColorGlyphShapes;
	Transformable				fEmbeddedTransformation;
									// rotated or sheared font?
	agg::trans_affine&			fViewTransformation;
};

#endif // AGG_TEXT_RENDERER_H
