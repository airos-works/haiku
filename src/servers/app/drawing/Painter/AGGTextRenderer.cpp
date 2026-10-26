/*
 * Copyright 2005-2009, Stephan Aßmus <superstippi@gmx.de>.
 * Copyright 2008, Andrej Spielmann <andrej.spielmann@seh.ox.ac.uk>.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "AGGTextRenderer.h"

#include <agg_basics.h>
#include <agg_bounding_rect.h>
#include <agg_conv_segmentator.h>
#include <agg_conv_stroke.h>
#include <agg_conv_transform.h>
#include <agg_path_storage.h>
#include <agg_pixfmt_rgba.h>
#include <agg_scanline_boolean_algebra.h>
#include <agg_span_allocator.h>
#include <agg_span_image_filter_rgba.h>
#include <agg_span_interpolator_linear.h>
#include <agg_trans_affine.h>

#include <math.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#define SHOW_GLYPH_BOUNDS 0

#include "GlobalSubpixelSettings.h"
#include "GlyphLayoutEngine.h"
#include "IntRect.h"


AGGTextRenderer::AGGTextRenderer(renderer_base& baseRenderer,
		renderer_subpix_type& subpixRenderer,
		renderer_type& solidRenderer, renderer_bin_type& binRenderer,
		scanline_unpacked_type& scanline,
		scanline_unpacked_subpix_type& subpixScanline,
		rasterizer_subpix_type& subpixRasterizer,
		scanline_unpacked_masked_type*& maskedScanline,
		agg::clipped_alpha_mask*& alphaMask,
		agg::trans_affine& viewTransformation)
	:
	fPathAdaptor(),
	fGray8Adaptor(),
	fGray8Scanline(),
	fMonoAdaptor(),
	fMonoScanline(),

	fCurves(fPathAdaptor),
	fContour(fCurves),

	fBaseRenderer(baseRenderer),
	fSolidRenderer(solidRenderer),
	fBinRenderer(binRenderer),
	fSubpixRenderer(subpixRenderer),
	fScanline(scanline),
	fSubpixScanline(subpixScanline),
	fSubpixRasterizer(subpixRasterizer),
	fMaskedScanline(maskedScanline),
	fAlphaMask(alphaMask),

	fRasterizer(),

	fHinted(true),
	fAntialias(true),
	fColorGlyphAlpha(255),
	fColorGlyphShapes(false),
	fEmbeddedTransformation(),
	fViewTransformation(viewTransformation)
{
	fCurves.approximation_scale(2.0);
	fContour.auto_detect_orientation(false);
}


AGGTextRenderer::~AGGTextRenderer()
{
}


void
AGGTextRenderer::SetFont(const ServerFont& font)
{
	fFont = font;

	// construct an embedded transformation (rotate & shear)
	fEmbeddedTransformation.Reset();
	fEmbeddedTransformation.ShearBy(B_ORIGIN,
		(90.0 - font.Shear()) * M_PI / 180.0, 0.0);
	fEmbeddedTransformation.RotateBy(B_ORIGIN,
		-font.Rotation() * M_PI / 180.0);

	fContour.width(font.FalseBoldWidth() * 2.0);

	// Halos, outlines and shadows of text: see B_COLOR_GLYPHS_AS_SHAPES
	fColorGlyphShapes = font.FalseBoldWidth() != 0.0
		|| (font.Flags() & B_COLOR_GLYPHS_AS_SHAPES) != 0;
}


void
AGGTextRenderer::SetHinting(bool hinting)
{
	fHinted = hinting;
}


void
AGGTextRenderer::SetAntialiasing(bool antialiasing)
{
	if (fAntialias != antialiasing) {
		fAntialias = antialiasing;
		// NOTE: The fSubpixRasterizer is not used when anti-aliasing is
		// disabled.
		if (!fAntialias)
			fRasterizer.gamma(agg::gamma_threshold(0.5));
		else
			fRasterizer.gamma(agg::gamma_power(1.0));
	}
}


typedef agg::conv_transform<FontCacheEntry::CurveConverter, Transformable>
	conv_font_trans_type;

typedef agg::conv_transform<FontCacheEntry::ContourConverter, Transformable>
	conv_font_contour_trans_type;



class AGGTextRenderer::StringRenderer {
public:
	StringRenderer(const IntRect& clippingFrame, bool dryRun,
			FontCacheEntry::TransformedOutline& transformedGlyph,
			FontCacheEntry::TransformedContourOutline& transformedContour,
			const Transformable& transform,
			const BPoint& transformOffset,
			BPoint* nextCharPos,
			AGGTextRenderer& renderer)
		:
		fTransform(transform),
		fTransformOffset(transformOffset),
		fClippingFrame(clippingFrame),
		fDryRun(dryRun),
		fVector(false),
		fBounds(INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN),
		fNextCharPos(nextCharPos),

		fTransformedGlyph(transformedGlyph),
		fTransformedContour(transformedContour),

		fRenderer(renderer)
	{
		fSubpixelAntiAliased = subpixel_antialiasing_active()
			&& fRenderer.Antialiasing();
	}

	bool NeedsVector()
	{
		return !fTransform.IsTranslationOnly()
			|| (fSubpixelAntiAliased && fRenderer.fMaskedScanline != NULL);
	}

	void Start()
	{
		fRenderer.fRasterizer.reset();
		fRenderer.fSubpixRasterizer.reset();
	}

	void Finish(double x, double y)
	{
		if (fVector) {
			if (fRenderer.fMaskedScanline != NULL) {
				agg::render_scanlines(fRenderer.fRasterizer,
					*fRenderer.fMaskedScanline, fRenderer.fSolidRenderer);
			} else if (fSubpixelAntiAliased) {
				agg::render_scanlines(fRenderer.fSubpixRasterizer,
					fRenderer.fSubpixScanline, fRenderer.fSubpixRenderer);
			} else {
				agg::render_scanlines(fRenderer.fRasterizer,
					fRenderer.fScanline, fRenderer.fSolidRenderer);
			}
		}

		if (!fDryRun) {
			if ((fRenderer.fFont.Face() & B_UNDERSCORE_FACE) != 0)
				_DrawHorizontalLine(y + 2);

			if ((fRenderer.fFont.Face() & B_STRIKEOUT_FACE) != 0) {
				font_height fontHeight;
				fRenderer.fFont.GetHeight(fontHeight);
				_DrawHorizontalLine(y - (fontHeight.ascent + fontHeight.descent) / 4);
			}
		}

		if (fNextCharPos) {
			fNextCharPos->x = x;
			fNextCharPos->y = y;
			fTransform.Transform(fNextCharPos);
		}
	}

	void ConsumeEmptyGlyph(int32 index, uint32 charCode, double x, double y)
	{
	}

	bool ConsumeGlyph(int32 index, uint32 charCode, const GlyphCache* glyph,
		FontCacheEntry* entry, double x, double y, double advanceX,
			double advanceY)
	{
		// "glyphBounds" is the bounds of the glyph transformed
		// by the x y location of the glyph along the base line,
		// it is therefore yet "untransformed" in case there is an
		// embedded transformation.
		const agg::rect_i& r = glyph->bounds;
		if (!r.is_valid())
			return true;
		IntRect glyphBounds(int32(r.x1 + x), int32(r.y1 + y - 1),
			int32(r.x2 + x + 1), int32(r.y2 + y + 1));
			// NOTE: "-1"/"+1" converts the glyph bounding box from pixel
			// indices to pixel area coordinates

		// track bounding box
		fBounds = fBounds | glyphBounds;

		if (glyph->data_type == glyph_data_color) {
			if (!fDryRun) {
				fRenderer._DrawColorGlyph(glyph, x, y, fTransform,
					fClippingFrame);
			}
			return true;
		}

		// render the glyph if this is not a dry run
		if (!fDryRun) {
			// init the fontmanager's embedded adaptors
			// NOTE: The initialization for the "location" of
			// the glyph is different depending on whether we
			// deal with non-(rotated/sheared) text, in which
			// case we have a native FT bitmap. For rotated or
			// sheared text, we use AGG vector outlines and
			// a transformation pipeline, which will be applied
			// _after_ we retrieve the outline, and that's why
			// we simply pass x and y, which are untransformed.

			// "glyphBounds" is now transformed into screen coords
			// in order to stop drawing when we are already outside
			// of the clipping frame
			if (glyph->data_type != glyph_data_outline) {
				// we cannot use the transformation pipeline
				double transformedX = x + fTransformOffset.x;
				double transformedY = y + fTransformOffset.y;
				entry->InitAdaptors(glyph, transformedX, transformedY,
					fRenderer.fMonoAdaptor,
					fRenderer.fGray8Adaptor,
					fRenderer.fPathAdaptor);

				glyphBounds.OffsetBy(fTransformOffset);
			} else {
				entry->InitAdaptors(glyph, x, y,
					fRenderer.fMonoAdaptor,
					fRenderer.fGray8Adaptor,
					fRenderer.fPathAdaptor);

				int32 falseBoldWidth = (int32)fRenderer.fContour.width();
				if (falseBoldWidth != 0)
					glyphBounds.InsetBy(-falseBoldWidth, -falseBoldWidth);
				// TODO: not correct! this is later used for clipping,
				// but it doesn't get the rect right
				glyphBounds = fTransform.TransformBounds(glyphBounds);
			}

			if (fClippingFrame.Intersects(glyphBounds)) {
				switch (glyph->data_type) {
					case glyph_data_mono:
						agg::render_scanlines(fRenderer.fMonoAdaptor,
							fRenderer.fMonoScanline, fRenderer.fBinRenderer);
						break;

					case glyph_data_gray8:
						if (fRenderer.fMaskedScanline != NULL) {
							agg::render_scanlines(fRenderer.fGray8Adaptor,
								*fRenderer.fMaskedScanline,
								fRenderer.fSolidRenderer);
						} else {
							agg::render_scanlines(fRenderer.fGray8Adaptor,
								fRenderer.fGray8Scanline,
								fRenderer.fSolidRenderer);
						}
						break;

					case glyph_data_subpix:
						// TODO: Handle alpha mask (fRenderer.fMaskedScanline)
						//       and remove the grayscale workaround for that.
						agg::render_scanlines(fRenderer.fGray8Adaptor,
							fRenderer.fGray8Scanline,
							fRenderer.fSubpixRenderer);
						break;

					case glyph_data_outline: {
						fVector = true;
						if (fSubpixelAntiAliased && fRenderer.fMaskedScanline == NULL) {
							if (fRenderer.fContour.width() == 0.0) {
								fRenderer.fSubpixRasterizer.add_path(
									fTransformedGlyph);
							} else {
								fRenderer.fSubpixRasterizer.add_path(
									fTransformedContour);
							}
						} else {
							if (fRenderer.fContour.width() == 0.0) {
								fRenderer.fRasterizer.add_path(
									fTransformedGlyph);
							} else {
								fRenderer.fRasterizer.add_path(
									fTransformedContour);
							}
						}
#if SHOW_GLYPH_BOUNDS
	agg::path_storage p;
	p.move_to(glyphBounds.left + 0.5, glyphBounds.top + 0.5);
	p.line_to(glyphBounds.right + 0.5, glyphBounds.top + 0.5);
	p.line_to(glyphBounds.right + 0.5, glyphBounds.bottom + 0.5);
	p.line_to(glyphBounds.left + 0.5, glyphBounds.bottom + 0.5);
	p.close_polygon();
	agg::conv_stroke<agg::path_storage> ps(p);
	ps.width(1.0);
	if (fSubpixelAntiAliased && fRenderer.fMaskedScanline != NULL)
		fRenderer.fSubpixRasterizer.add_path(ps);
	else
		fRenderer.fRasterizer.add_path(ps);
#endif

						break;
					}
					default:
						break;
				}
			}
		}
		return true;
	}

	IntRect Bounds() const
	{
		return fBounds;
	}

private:
	void _DrawHorizontalLine(float y)
	{
		agg::path_storage path;
		IntRect bounds = fBounds;
		BPoint left(bounds.left, y);
		BPoint right(bounds.right, y);
		fTransform.Transform(&left);
		fTransform.Transform(&right);
		path.move_to(left.x + 0.5, left.y + 0.5);
		path.line_to(right.x + 0.5, right.y + 0.5);
		agg::conv_stroke<agg::path_storage> pathStorage(path);
		pathStorage.width(fRenderer.fFont.Size() / 12.0f);
		if (fRenderer.fMaskedScanline != NULL) {
			fRenderer.fRasterizer.add_path(pathStorage);
			agg::render_scanlines(fRenderer.fRasterizer,
				*fRenderer.fMaskedScanline, fRenderer.fSolidRenderer);
		} else if (fSubpixelAntiAliased) {
			fRenderer.fSubpixRasterizer.add_path(pathStorage);
			agg::render_scanlines(fRenderer.fSubpixRasterizer,
				fRenderer.fSubpixScanline, fRenderer.fSubpixRenderer);
		} else {
			fRenderer.fRasterizer.add_path(pathStorage);
			agg::render_scanlines(fRenderer.fRasterizer,
				fRenderer.fScanline, fRenderer.fSolidRenderer);
		}
	}

private:
	const Transformable& fTransform;
	const BPoint&		fTransformOffset;
	const IntRect&		fClippingFrame;
	bool				fDryRun;
	bool				fSubpixelAntiAliased;
	bool				fVector;
	IntRect				fBounds;
	BPoint*				fNextCharPos;

	FontCacheEntry::TransformedOutline& fTransformedGlyph;
	FontCacheEntry::TransformedContourOutline& fTransformedContour;
	AGGTextRenderer&	fRenderer;
};


//! Lets AGG's span generators draw through _BlendColorSpan()
struct AGGTextRenderer::ColorSpanRenderer {
	typedef agg::rgba8 color_type;

	ColorSpanRenderer(AGGTextRenderer& renderer)
		:
		fRenderer(renderer)
	{
	}

	void prepare() {}

	void blend_color_hspan(int x, int y, unsigned length,
		const color_type* colors, const uint8* covers, uint8 cover)
	{
		fRenderer._BlendColorSpan(x, y, length, colors, covers, cover);
	}

	AGGTextRenderer& fRenderer;
};


/*!	Draws the pixels of a color glyph. They have their own alpha, which
	becomes the coverage; the drawing mode applies fColorGlyphAlpha.
*/
void
AGGTextRenderer::_BlendColorSpan(int x, int y, unsigned length,
	const agg::rgba8* colors, const uint8* covers, uint8 cover)
{
	static const unsigned kChunk = 256;
	agg::rgba8 spanColors[kChunk];
	uint8 spanCovers[kChunk];

	while (length > 0) {
		unsigned count = length < kChunk ? length : kChunk;
		bool visible = false;
		for (unsigned i = 0; i < count; i++) {
			uint32 alpha = colors[i].a * (covers != NULL ? covers[i] : cover);
			spanCovers[i] = (alpha + 127) / 255;
			spanColors[i] = agg::rgba8(colors[i].r, colors[i].g, colors[i].b,
				fColorGlyphAlpha);
			visible |= spanCovers[i] != 0;
		}
		if (visible && fMaskedScanline != NULL && fAlphaMask != NULL)
			fAlphaMask->combine_hspan(x, y, spanCovers, count);
		if (visible && fColorGlyphShapes) {
			fBaseRenderer.blend_solid_hspan(x, y, count,
				fSolidRenderer.color(), spanCovers);
		} else if (visible)
			fBaseRenderer.blend_color_hspan(x, y, count, spanColors, spanCovers, 0);

		x += count;
		colors += count;
		if (covers != NULL)
			covers += count;
		length -= count;
	}
}


void
AGGTextRenderer::_DrawColorGlyph(const GlyphCache* glyph, double x, double y,
	const Transformable& transform, const IntRect& clippingFrame)
{
	const agg::rect_i& bounds = glyph->bounds;
	int32 width = bounds.x2 - bounds.x1 + 1;
	int32 height = bounds.y2 - bounds.y1 + 1;
	if (width <= 0 || height <= 0
		|| glyph->data_size < uint32(width * height * 4)) {
		return;
	}
	const agg::rgba8* pixels = (const agg::rgba8*)glyph->data;

	if (transform.IsTranslationOnly()) {
		BPoint origin(x, y);
		transform.Transform(&origin);
		int32 left = (int32)floor(origin.x + 0.5) + bounds.x1;
		int32 top = (int32)floor(origin.y + 0.5) + bounds.y1;

		if (fColorGlyphShapes && fFont.FalseBoldWidth() != 0.0) {
			_DrawColorGlyphShape(glyph, left, top, clippingFrame);
			return;
		}

		IntRect glyphFrame(left, top, left + width - 1, top + height - 1);
		if (!clippingFrame.Intersects(glyphFrame))
			return;
		for (int32 row = 0; row < height; row++)
			_BlendColorSpan(left, top + row, width, pixels + row * width, NULL,
				255);
		return;
	}

	// Rotated, sheared or scaled: sample the glyph through the transform
	typedef agg::pixfmt_rgba32 glyph_pixfmt;
	typedef agg::span_interpolator_linear<> interpolator_type;
	typedef agg::span_image_filter_rgba_bilinear_clip<glyph_pixfmt,
		interpolator_type> span_generator_type;

	agg::rendering_buffer buffer(glyph->data, width, height, width * 4);
	glyph_pixfmt glyphPixels(buffer);

	agg::trans_affine glyphToView = agg::trans_affine_translation(
		x + bounds.x1, y + bounds.y1);
	glyphToView *= transform;
	agg::trans_affine viewToGlyph = glyphToView;
	viewToGlyph.invert();

	interpolator_type interpolator(viewToGlyph);
	span_generator_type generator(glyphPixels, agg::rgba8(0, 0, 0, 0),
		interpolator);
	agg::span_allocator<agg::rgba8> allocator;

	agg::path_storage rectangle;
	rectangle.move_to(0, 0);
	rectangle.line_to(width, 0);
	rectangle.line_to(width, height);
	rectangle.line_to(0, height);
	rectangle.close_polygon();
	agg::conv_transform<agg::path_storage> transformed(rectangle,
		glyphToView);

	agg::rasterizer_scanline_aa<> rasterizer;
	rasterizer.clip_box(clippingFrame.left, clippingFrame.top,
		clippingFrame.right + 1, clippingFrame.bottom + 1);
	rasterizer.add_path(transformed);

	agg::scanline_u8 scanline;
	ColorSpanRenderer spanRenderer(*this);
	agg::render_scanlines_aa(rasterizer, scanline, spanRenderer, allocator,
		generator);
}


/*!	Draws the shape of a color glyph grown by the false bold width, as the
	outlines of text are.
*/
void
AGGTextRenderer::_DrawColorGlyphShape(const GlyphCache* glyph, int32 left,
	int32 top, const IntRect& clippingFrame)
{
	const agg::rect_i& bounds = glyph->bounds;
	int32 width = bounds.x2 - bounds.x1 + 1;
	int32 height = bounds.y2 - bounds.y1 + 1;
	int32 radius = (int32)ceilf(fabsf(fFont.FalseBoldWidth()));
	if (radius > 8)
		radius = 8;

	int32 grownWidth = width + 2 * radius;
	int32 grownHeight = height + 2 * radius;
	IntRect frame(left - radius, top - radius, left - radius + grownWidth - 1,
		top - radius + grownHeight - 1);
	if (!clippingFrame.Intersects(frame))
		return;

	uint8* alpha = (uint8*)calloc((size_t)grownWidth * grownHeight, 1);
	agg::rgba8* row = (agg::rgba8*)malloc(sizeof(agg::rgba8) * grownWidth);
	if (alpha == NULL || row == NULL) {
		free(alpha);
		free(row);
		return;
	}

	// the largest alpha within the radius of each pixel
	const agg::rgba8* pixels = (const agg::rgba8*)glyph->data;
	for (int32 y = 0; y < height; y++) {
		for (int32 x = 0; x < width; x++) {
			uint8 value = pixels[y * width + x].a;
			if (value == 0)
				continue;
			for (int32 dy = -radius; dy <= radius; dy++) {
				uint8* target = alpha + (y + radius + dy) * grownWidth
					+ x + radius;
				for (int32 dx = -radius; dx <= radius; dx++) {
					if (dx * dx + dy * dy <= radius * radius
						&& target[dx] < value) {
						target[dx] = value;
					}
				}
			}
		}
	}

	for (int32 y = 0; y < grownHeight; y++) {
		for (int32 x = 0; x < grownWidth; x++)
			row[x] = agg::rgba8(0, 0, 0, alpha[y * grownWidth + x]);
		_BlendColorSpan(frame.left, frame.top + y, grownWidth, row, NULL, 255);
	}

	free(alpha);
	free(row);
}


BRect
AGGTextRenderer::RenderString(const char* string, uint32 length,
	const BPoint& baseLine, const BRect& clippingFrame, bool dryRun,
	BPoint* nextCharPos, const escapement_delta* delta,
	FontCacheReference* cacheReference)
{
//printf("RenderString(\"%s\", length: %ld, dry: %d)\n", string, length, dryRun);

	Transformable transform(fEmbeddedTransformation);
	transform.TranslateBy(baseLine);
	transform *= fViewTransformation;

	fCurves.approximation_scale(transform.scale());

	// use a transformation behind the curves
	// (only if glyph->data_type == agg::glyph_data_outline)
	// in the pipeline for the rasterizer
	FontCacheEntry::TransformedOutline
		transformedOutline(fCurves, transform);
	FontCacheEntry::TransformedContourOutline
		transformedContourOutline(fContour, transform);

	// for when we bypass the transformation pipeline
	BPoint transformOffset(0.0, 0.0);
	transform.Transform(&transformOffset);
	IntRect clippingIntFrame(clippingFrame);

	StringRenderer renderer(clippingIntFrame, dryRun, transformedOutline, transformedContourOutline,
		transform, transformOffset, nextCharPos, *this);

	GlyphLayoutEngine::LayoutGlyphs(renderer, fFont, string, length, INT32_MAX,
		delta, fFont.Spacing(), NULL, cacheReference);

	return transform.TransformBounds(renderer.Bounds());
}


BRect
AGGTextRenderer::RenderString(const char* string, uint32 length,
	const BPoint* offsets, const BRect& clippingFrame, bool dryRun,
	BPoint* nextCharPos, FontCacheReference* cacheReference)
{
//printf("RenderString(\"%s\", length: %ld, dry: %d)\n", string, length, dryRun);

	Transformable transform(fEmbeddedTransformation);
	transform *= fViewTransformation;

	fCurves.approximation_scale(transform.scale());

	// use a transformation behind the curves
	// (only if glyph->data_type == agg::glyph_data_outline)
	// in the pipeline for the rasterizer
	FontCacheEntry::TransformedOutline
		transformedOutline(fCurves, transform);
	FontCacheEntry::TransformedContourOutline
		transformedContourOutline(fContour, transform);

	// for when we bypass the transformation pipeline
	BPoint transformOffset(0.0, 0.0);
	transform.Transform(&transformOffset);
	IntRect clippingIntFrame(clippingFrame);

	StringRenderer renderer(clippingIntFrame, dryRun, transformedOutline, transformedContourOutline,
		transform, transformOffset, nextCharPos, *this);

	GlyphLayoutEngine::LayoutGlyphs(renderer, fFont, string, length, INT32_MAX,
		NULL, fFont.Spacing(), offsets, cacheReference);

	return transform.TransformBounds(renderer.Bounds());
}
