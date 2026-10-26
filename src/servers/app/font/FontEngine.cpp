/*
 * Copyright 2007, Haiku. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Maxim Shemanarev <mcseemagg@yahoo.com>
 *		Stephan Aßmus <superstippi@gmx.de>
 *		Anthony Lee <don.anthony.lee@gmail.com>
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


#include "FontEngine.h"

#include FT_GLYPH_H
#include FT_OUTLINE_H
#include FT_LCD_FILTER_H

#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <agg_bitset_iterator.h>
#include <agg_renderer_scanline.h>

#include "ColorGlyphRenderer.h"
#include "GlobalSubpixelSettings.h"
#include "OpenTypeLigatures.h"


static const bool kFlipY = true;


static inline double
int26p6_to_dbl(int p)
{
	return double(p) / 64.0;
}


static inline int
dbl_to_int26p6(double p)
{
	return int(p * 64.0 + 0.5);
}


template<class PathStorage>
bool
decompose_ft_outline(const FT_Outline& outline, bool flip_y, PathStorage& path)
{
	typedef typename PathStorage::value_type value_type;

	FT_Vector v_last;
	FT_Vector v_control;
	FT_Vector v_start;
	double x1, y1, x2, y2, x3, y3;

	FT_Vector* point;
	FT_Vector* limit;
#if FREETYPE_MAJOR > 2 || (FREETYPE_MAJOR == 2 && (FREETYPE_MINOR > 13 \
			|| (FREETYPE_MINOR == 13 && FREETYPE_PATCH >= 3)))
	unsigned char* tags;
#else
	char* tags;
#endif

	int   n;		 // index of contour in outline
	int   first;	 // index of first point in contour
	char  tag;	   // current point's state

	first = 0;

	for (n = 0; n < outline.n_contours; n++) {
		int  last;  // index of last point in contour

		last  = outline.contours[n];
		limit = outline.points + last;

		v_start = outline.points[first];
		v_last  = outline.points[last];

		v_control = v_start;

		point = outline.points + first;
		tags  = outline.tags  + first;
		tag   = FT_CURVE_TAG(tags[0]);

		// A contour cannot start with a cubic control point!
		if (tag == FT_CURVE_TAG_CUBIC)
			return false;

		// check first point to determine origin
		if ( tag == FT_CURVE_TAG_CONIC) {
			// first point is conic control.  Yes, this happens.
			if (FT_CURVE_TAG(outline.tags[last]) == FT_CURVE_TAG_ON) {
				// start at last point if it is on the curve
				v_start = v_last;
				limit--;
			} else {
				// if both first and last points are conic,
				// start at their middle and record its position
				// for closure
				v_start.x = (v_start.x + v_last.x) / 2;
				v_start.y = (v_start.y + v_last.y) / 2;

				v_last = v_start;
			}
			point--;
			tags--;
		}

		x1 = int26p6_to_dbl(v_start.x);
		y1 = int26p6_to_dbl(v_start.y);
		if (flip_y) y1 = -y1;
		path.move_to(value_type(dbl_to_int26p6(x1)),
					 value_type(dbl_to_int26p6(y1)));

		while(point < limit) {
			point++;
			tags++;

			tag = FT_CURVE_TAG(tags[0]);
			switch(tag) {
				case FT_CURVE_TAG_ON: { // emit a single line_to
					x1 = int26p6_to_dbl(point->x);
					y1 = int26p6_to_dbl(point->y);
					if (flip_y) y1 = -y1;
					path.line_to(value_type(dbl_to_int26p6(x1)),
								 value_type(dbl_to_int26p6(y1)));
					//path.line_to(conv(point->x), flip_y ? -conv(point->y) : conv(point->y));
					continue;
				}

				case FT_CURVE_TAG_CONIC: { // consume conic arcs
					v_control.x = point->x;
					v_control.y = point->y;

				Do_Conic:
					if (point < limit) {
						FT_Vector vec;
						FT_Vector v_middle;

						point++;
						tags++;
						tag = FT_CURVE_TAG(tags[0]);

						vec.x = point->x;
						vec.y = point->y;

						if (tag == FT_CURVE_TAG_ON) {
							x1 = int26p6_to_dbl(v_control.x);
							y1 = int26p6_to_dbl(v_control.y);
							x2 = int26p6_to_dbl(vec.x);
							y2 = int26p6_to_dbl(vec.y);
							if (flip_y) { y1 = -y1; y2 = -y2; }
							path.curve3(value_type(dbl_to_int26p6(x1)),
										value_type(dbl_to_int26p6(y1)),
										value_type(dbl_to_int26p6(x2)),
										value_type(dbl_to_int26p6(y2)));
							continue;
						}

						if (tag != FT_CURVE_TAG_CONIC)
							return false;

						v_middle.x = (v_control.x + vec.x) / 2;
						v_middle.y = (v_control.y + vec.y) / 2;

						x1 = int26p6_to_dbl(v_control.x);
						y1 = int26p6_to_dbl(v_control.y);
						x2 = int26p6_to_dbl(v_middle.x);
						y2 = int26p6_to_dbl(v_middle.y);
						if (flip_y) { y1 = -y1; y2 = -y2; }
						path.curve3(value_type(dbl_to_int26p6(x1)),
									value_type(dbl_to_int26p6(y1)),
									value_type(dbl_to_int26p6(x2)),
									value_type(dbl_to_int26p6(y2)));

						//path.curve3(conv(v_control.x),
						//			flip_y ? -conv(v_control.y) : conv(v_control.y),
						//			conv(v_middle.x),
						//			flip_y ? -conv(v_middle.y) : conv(v_middle.y));

						v_control = vec;
						goto Do_Conic;
					}

					x1 = int26p6_to_dbl(v_control.x);
					y1 = int26p6_to_dbl(v_control.y);
					x2 = int26p6_to_dbl(v_start.x);
					y2 = int26p6_to_dbl(v_start.y);
					if (flip_y) { y1 = -y1; y2 = -y2; }
					path.curve3(value_type(dbl_to_int26p6(x1)),
								value_type(dbl_to_int26p6(y1)),
								value_type(dbl_to_int26p6(x2)),
								value_type(dbl_to_int26p6(y2)));

					//path.curve3(conv(v_control.x),
					//			flip_y ? -conv(v_control.y) : conv(v_control.y),
					//			conv(v_start.x),
					//			flip_y ? -conv(v_start.y) : conv(v_start.y));
					goto Close;
				}

				default: { // FT_CURVE_TAG_CUBIC
					FT_Vector vec1, vec2;

					if (point + 1 > limit || FT_CURVE_TAG(tags[1]) != FT_CURVE_TAG_CUBIC)
						return false;

					vec1.x = point[0].x;
					vec1.y = point[0].y;
					vec2.x = point[1].x;
					vec2.y = point[1].y;

					point += 2;
					tags  += 2;

					if (point <= limit) {
						FT_Vector vec;

						vec.x = point->x;
						vec.y = point->y;

						x1 = int26p6_to_dbl(vec1.x);
						y1 = int26p6_to_dbl(vec1.y);
						x2 = int26p6_to_dbl(vec2.x);
						y2 = int26p6_to_dbl(vec2.y);
						x3 = int26p6_to_dbl(vec.x);
						y3 = int26p6_to_dbl(vec.y);
						if (flip_y) { y1 = -y1; y2 = -y2; y3 = -y3; }
						path.curve4(value_type(dbl_to_int26p6(x1)),
									value_type(dbl_to_int26p6(y1)),
									value_type(dbl_to_int26p6(x2)),
									value_type(dbl_to_int26p6(y2)),
									value_type(dbl_to_int26p6(x3)),
									value_type(dbl_to_int26p6(y3)));

						//path.curve4(conv(vec1.x),
						//			flip_y ? -conv(vec1.y) : conv(vec1.y),
						//			conv(vec2.x),
						//			flip_y ? -conv(vec2.y) : conv(vec2.y),
						//			conv(vec.x),
						//			flip_y ? -conv(vec.y) : conv(vec.y));
						continue;
					}

					x1 = int26p6_to_dbl(vec1.x);
					y1 = int26p6_to_dbl(vec1.y);
					x2 = int26p6_to_dbl(vec2.x);
					y2 = int26p6_to_dbl(vec2.y);
					x3 = int26p6_to_dbl(v_start.x);
					y3 = int26p6_to_dbl(v_start.y);
					if (flip_y) { y1 = -y1; y2 = -y2; y3 = -y3; }
					path.curve4(value_type(dbl_to_int26p6(x1)),
								value_type(dbl_to_int26p6(y1)),
								value_type(dbl_to_int26p6(x2)),
								value_type(dbl_to_int26p6(y2)),
								value_type(dbl_to_int26p6(x3)),
								value_type(dbl_to_int26p6(y3)));

					//path.curve4(conv(vec1.x),
					//			flip_y ? -conv(vec1.y) : conv(vec1.y),
					//			conv(vec2.x),
					//			flip_y ? -conv(vec2.y) : conv(vec2.y),
					//			conv(v_start.x),
					//			flip_y ? -conv(v_start.y) : conv(v_start.y));
					goto Close;
				}
			}
		}

		path.close_polygon();

   Close:
		first = last + 1;
	}

	return true;
}


template<class Scanline, class ScanlineStorage>
void
decompose_ft_bitmap_mono(const FT_Bitmap& bitmap, int x, int y,
	bool flip_y, Scanline& sl, ScanlineStorage& storage)
{
	const uint8* buf = (const uint8*)bitmap.buffer;
	int pitch = bitmap.pitch;
	sl.reset(x, x + bitmap.width);
	storage.prepare();
	if (flip_y) {
		buf += bitmap.pitch * (bitmap.rows - 1);
		y += bitmap.rows;
		pitch = -pitch;
	}
	for (unsigned int i = 0; i < bitmap.rows; i++) {
		sl.reset_spans();
		agg::bitset_iterator bits(buf, 0);
		for (unsigned int j = 0; j < bitmap.width; j++) {
			if (bits.bit())
				sl.add_cell(x + j, agg::cover_full);
			++bits;
		}
		buf += pitch;
		if (sl.num_spans()) {
			sl.finalize(y - i - 1);
			storage.render(sl);
		}
	}
}


template<class Scanline, class ScanlineStorage>
void
decompose_ft_bitmap_gray8(const FT_Bitmap& bitmap, int x, int y,
	bool flip_y, Scanline& sl, ScanlineStorage& storage)
{
	const uint8* buf = (const uint8*)bitmap.buffer;
	int pitch = bitmap.pitch;
	sl.reset(x, x + bitmap.width);
	storage.prepare();
	if (flip_y) {
		buf += bitmap.pitch * (bitmap.rows - 1);
		y += bitmap.rows;
		pitch = -pitch;
	}
	for (unsigned int i = 0; i < bitmap.rows; i++) {
		sl.reset_spans();

		if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
			// font has built-in mono bitmap
			agg::bitset_iterator bits(buf, 0);
			for (unsigned int j = 0; j < bitmap.width; j++) {
				if (bits.bit())
					sl.add_cell(x + j, agg::cover_full);
				++bits;
			}
		} else {
			const uint8* p = buf;
			for (unsigned int j = 0; j < bitmap.width; j++) {
				if (*p)
					sl.add_cell(x + j, *p);
				++p;
			}
		}

		buf += pitch;
		if (sl.num_spans()) {
			sl.finalize(y - i - 1);
			storage.render(sl);
		}
	}
}


template<class Scanline, class ScanlineStorage>
void
decompose_ft_bitmap_subpix(const FT_Bitmap& bitmap, int x, int y,
	bool flip_y, Scanline& sl, ScanlineStorage& storage)
{
	const uint8* buf = (const uint8*)bitmap.buffer;
	int pitch = bitmap.pitch;
	if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
		sl.reset(x, x + bitmap.width);
	else
		sl.reset(x, x + bitmap.width / 3);
	storage.prepare();

	if (flip_y) {
		buf += bitmap.pitch * (bitmap.rows - 1);
		y += bitmap.rows;
		pitch = -pitch;
	}

	for (unsigned int i = 0; i < bitmap.rows; i++) {
		sl.reset_spans();

		if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
			// font has built-in mono bitmap
			agg::bitset_iterator bits(buf, 0);
			for (unsigned int j = 0; j < bitmap.width; j++) {
				if (bits.bit()) {
					sl.add_cell(x + j,
						agg::cover_full, agg::cover_full, agg::cover_full);
				}
				++bits;
			}
		} else {
			const uint8* p = buf;
			int w = bitmap.width / 3;

			for (int j = 0; j < w; j++) {
				if (p[0] || p[1] || p[2])
					sl.add_cell(x + j, p[0], p[1], p[2]);
				p += 3;
			}
		}

		buf += pitch;
		if (sl.num_spans()) {
			sl.finalize(y - i - 1);
			storage.render(sl);
		}
	}
}


// #pragma mark -


FontEngine::FontEngine()
	:
	fLastError(0),
	fLibraryInitialized(false),
	fLibrary(0),
	fFace(NULL),

	fGlyphRendering(glyph_ren_native_gray8),
	fHinting(true),
	fDeviceScaled(false),
	fPixelSize(0),

	fColorGlyphs(false),
	fBitmapScale(1.0),
	fColorRenderer(NULL),
	fLigatures(NULL),
	fLigaturesLoaded(false),
	fColorData(NULL),

	fDataSize(0),
	fDataType(glyph_data_invalid),
	fBounds(1, 1, 0, 0),
	fAdvanceX(0.0),
	fAdvanceY(0.0),
	fInsetLeft(0.0),
	fInsetRight(0.0),

	fPath(),
	fCurves(fPath),
	fScanlineAA(),
	fScanlineBin(),
	fScanlineSubpix(),
	fScanlineStorageAA(),
	fScanlineStorageBin(),
	fScanlineStorageSubpix()
{
	fCurves.approximation_scale(4.0);

	fLastError = FT_Init_FreeType(&fLibrary);
	if (fLastError == 0)
		fLibraryInitialized = true;
}


FontEngine::~FontEngine()
{
	delete fColorRenderer;
	delete fLigatures;
	free(fColorData);
	FT_Done_Face(fFace);

	if (fLibraryInitialized)
		FT_Done_FreeType(fLibrary);
}


unsigned
FontEngine::CountFaces() const
{
	if (fFace)
		return fFace->num_faces;

	return 0;
}


uint32
FontEngine::GlyphIndexForGlyphCode(uint32 glyphCode) const
{
	return FT_Get_Char_Index(fFace, glyphCode);
}


bool
FontEngine::PrepareGlyph(uint32 glyphIndex)
{
	if (fColorGlyphs && _PrepareColorGlyph(glyphIndex))
		return true;

	FT_Int32 loadFlags = fHinting ? FT_LOAD_DEFAULT : FT_LOAD_NO_HINTING;
	if (fDeviceScaled) {
		// Only the vertical metrics (baseline, x-height, stems of horizontal
		// strokes) snap to the pixel grid; nothing moves sideways, so the
		// advances stay those of the logical size times the scale.
		if (fHinting)
			loadFlags |= FT_LOAD_TARGET_LIGHT;
	} else {
		loadFlags |= fGlyphRendering == glyph_ren_subpix ?
			FT_LOAD_TARGET_LCD : FT_LOAD_TARGET_NORMAL;
	}

	// Load unscaled and without hinting to get precise advance values
	// for B_CHAR_SPACING
	fLastError = FT_Load_Glyph(fFace, glyphIndex, loadFlags
		| FT_LOAD_NO_HINTING | FT_LOAD_NO_SCALE);

	FT_UShort units_per_EM = fFace->units_per_EM;
	if (!FT_IS_SCALABLE(fFace))
		units_per_EM = 1;
	fPreciseAdvanceX = (double)fFace->glyph->advance.x / units_per_EM;
	fPreciseAdvanceY = (double)fFace->glyph->advance.y / units_per_EM;

	// Need to load again with hinting.
	fLastError = FT_Load_Glyph(fFace, glyphIndex, loadFlags);

	if (fLastError != 0)
		return false;

	fAdvanceX = int26p6_to_dbl(fFace->glyph->advance.x);
	fAdvanceY = int26p6_to_dbl(fFace->glyph->advance.y);
	if (fDeviceScaled && FT_IS_SCALABLE(fFace)) {
		// light hinting rounds the advance to whole pixels
		fAdvanceX = fFace->glyph->linearHoriAdvance / 65536.0;
	}

	fInsetLeft = int26p6_to_dbl(fFace->glyph->metrics.horiBearingX);
	fInsetRight = int26p6_to_dbl(fFace->glyph->metrics.horiBearingX
		+ fFace->glyph->metrics.width - fFace->glyph->metrics.horiAdvance);

	switch(fGlyphRendering) {
		case glyph_ren_native_mono:
			fLastError = FT_Render_Glyph(fFace->glyph, FT_RENDER_MODE_MONO);
			if (fLastError == 0) {
				decompose_ft_bitmap_mono(fFace->glyph->bitmap,
					fFace->glyph->bitmap_left, kFlipY ?
					-fFace->glyph->bitmap_top : fFace->glyph->bitmap_top,
					kFlipY, fScanlineBin, fScanlineStorageBin);
				fBounds.x1 = fScanlineStorageBin.min_x();
				fBounds.y1 = fScanlineStorageBin.min_y();
				fBounds.x2 = fScanlineStorageBin.max_x();
				fBounds.y2 = fScanlineStorageBin.max_y();
				fDataSize = fScanlineStorageBin.byte_size();
				fDataType = glyph_data_mono;
				return true;
			}
			break;


		case glyph_ren_native_gray8:
			fLastError = FT_Render_Glyph(fFace->glyph, FT_RENDER_MODE_NORMAL);
			if (fLastError == 0) {
				decompose_ft_bitmap_gray8(fFace->glyph->bitmap,
					fFace->glyph->bitmap_left, kFlipY ?
					-fFace->glyph->bitmap_top : fFace->glyph->bitmap_top,
					kFlipY, fScanlineAA, fScanlineStorageAA);
				fBounds.x1 = fScanlineStorageAA.min_x();
				fBounds.y1 = fScanlineStorageAA.min_y();
				fBounds.x2 = fScanlineStorageAA.max_x();
				fBounds.y2 = fScanlineStorageAA.max_y();
				fDataSize = fScanlineStorageAA.byte_size();
				fDataType = glyph_data_gray8;
				return true;
			}
			break;


		case glyph_ren_subpix:
			fLastError = FT_Render_Glyph(fFace->glyph, FT_RENDER_MODE_LCD);
			if (fLastError == 0) {
				decompose_ft_bitmap_subpix(fFace->glyph->bitmap,
					fFace->glyph->bitmap_left, kFlipY ?
					-fFace->glyph->bitmap_top : fFace->glyph->bitmap_top,
					kFlipY, fScanlineSubpix, fScanlineStorageSubpix);
				fBounds.x1 = fScanlineStorageSubpix.min_x();
				fBounds.y1 = fScanlineStorageSubpix.min_y();
				fBounds.x2 = fScanlineStorageSubpix.max_x();
				fBounds.y2 = fScanlineStorageSubpix.max_y();
				fDataSize = fScanlineStorageSubpix.byte_size();
				fDataType = glyph_data_subpix;
				return true;
			}
			break;


		case glyph_ren_outline:
			fPath.remove_all();
			if (decompose_ft_outline(fFace->glyph->outline, kFlipY, fPath)) {
				agg::rect_d bounds = fPath.bounding_rect();
				fBounds.x1 = int(floor(bounds.x1));
				fBounds.y1 = int(floor(bounds.y1));
				fBounds.x2 = int(ceil(bounds.x2));
				fBounds.y2 = int(ceil(bounds.y2));
				fDataSize = fPath.byte_size();
				fDataType = glyph_data_outline;
				return true;
			}
			break;
	}
	return false;
}

// #pragma mark -

// WriteGlyphTo
void
FontEngine::WriteGlyphTo(uint8* data) const
{
	if (data && fDataSize) {
		switch(fDataType) {
			case glyph_data_mono:
				fScanlineStorageBin.serialize(data);
				break;

			case glyph_data_gray8:
				fScanlineStorageAA.serialize(data);
				break;

			case glyph_data_subpix:
				fScanlineStorageSubpix.serialize(data);
				break;

			case glyph_data_outline:
				fPath.serialize(data);
				break;

			case glyph_data_color:
				memcpy(data, fColorData, fDataSize);
				break;

			case glyph_data_invalid:
			default:
				break;
		}
	}
}


// GetKerning
bool
FontEngine::GetKerning(uint32 first, uint32 second, double* x, double* y)
{
	if (fFace && first && second && FT_HAS_KERNING(fFace)) {
		FT_Vector delta;
		FT_Get_Kerning(fFace, first, second, FT_KERNING_DEFAULT, &delta);

		double dx = int26p6_to_dbl(delta.x);
		double dy = int26p6_to_dbl(delta.y);

		*x += dx;
		*y += dy;

		return true;
	}
	return false;
}


// #pragma mark -


bool
FontEngine::Init(const char* fontFilePath, unsigned faceIndex, double size,
	FT_Encoding charMap, glyph_rendering ren_type, bool hinting,
	const void* fontFileBuffer, const long fontFileBufferSize)
{
	if (!fLibraryInitialized)
		return false;

	fHinting = hinting;
	fPixelSize = size;

	fLastError = 0;

	delete fColorRenderer;
	fColorRenderer = NULL;
	delete fLigatures;
	fLigatures = NULL;
	fLigaturesLoaded = false;

	FT_Done_Face(fFace);
	if (fontFileBuffer && fontFileBufferSize) {
		fLastError = FT_New_Memory_Face(fLibrary,
			(const FT_Byte*)fontFileBuffer, fontFileBufferSize,
			faceIndex, &fFace);
	} else {
		fLastError = FT_New_Face(fLibrary, fontFilePath, faceIndex, &fFace);
	}

	if (fLastError != 0)
		return false;

	switch(ren_type) {
		case glyph_ren_native_mono:
			fGlyphRendering = glyph_ren_native_mono;
			break;

		case glyph_ren_native_gray8:
			fGlyphRendering = glyph_ren_native_gray8;
			break;

		case glyph_ren_subpix:
			fGlyphRendering = glyph_ren_subpix;
			break;

		case glyph_ren_outline:
			if (FT_IS_SCALABLE(fFace))
				fGlyphRendering = glyph_ren_outline;
			else
				fGlyphRendering = glyph_ren_native_gray8;
			break;
	}

	fColorGlyphs = FT_HAS_COLOR(fFace);
	fBitmapScale = 1.0;

	if (fColorGlyphs && !FT_IS_SCALABLE(fFace) && fFace->num_fixed_sizes > 0) {
		// Color bitmap fonts (CBDT, sbix) have strikes of a few sizes: use
		// the smallest one that is not smaller than the size, and scale it.
		int best = 0;
		for (int i = 1; i < fFace->num_fixed_sizes; i++) {
			FT_Pos ppem = fFace->available_sizes[i].y_ppem;
			FT_Pos bestPpem = fFace->available_sizes[best].y_ppem;
			if ((bestPpem < size * 64 && ppem > bestPpem)
				|| (ppem >= size * 64 && ppem < bestPpem)) {
				best = i;
			}
		}
		FT_Select_Size(fFace, best);
		FT_Pos ppem = fFace->available_sizes[best].y_ppem;
		if (ppem > 0)
			fBitmapScale = size * 64.0 / ppem;
	} else if (fDeviceScaled && FT_IS_SCALABLE(fFace)) {
		// A logical size times the scale is rarely a whole number of
		// pixels; rounding it would make the glyphs narrower or wider than
		// their measured advances.
		FT_Set_Char_Size(fFace, 0, (FT_F26Dot6)(size * 64.0 + 0.5), 72, 72);
	} else {
		FT_Set_Pixel_Sizes(fFace,
			unsigned(size * 64.0) >> 6,		// pixel_width
			unsigned(size * 64.0) >> 6);	// pixel_height
	}

	if (charMap != FT_ENCODING_NONE) {
		fLastError = FT_Select_Charmap(fFace, charMap);
	} else {
		if (FT_Select_Charmap(fFace, FT_ENCODING_UNICODE) != 0)
			fLastError = FT_Select_Charmap(fFace, FT_ENCODING_NONE);
	}

	return fLastError == 0;
}



// #pragma mark - color glyphs


bool
FontEngine::ShapeSequence(const uint32* codes, int32 count,
	uint32& glyphIndex, int32& usedCodes)
{
	static const int32 kMaxSequence = 32;
	if (fFace == NULL || count <= 0)
		return false;
	if (count > kMaxSequence)
		count = kMaxSequence;

	uint32 glyphs[kMaxSequence];
	int32 mapped = 0;
	while (mapped < count) {
		glyphs[mapped] = FT_Get_Char_Index(fFace, codes[mapped]);
		if (glyphs[mapped] == 0)
			break;
		mapped++;
	}
	if (mapped == 0)
		return false;

	glyphIndex = glyphs[0];
	usedCodes = 1;
	if (mapped == 1)
		return true;

	if (!fLigaturesLoaded) {
		fLigaturesLoaded = true;
		fLigatures = new(std::nothrow) OpenTypeLigatures;
		if (fLigatures != NULL && !fLigatures->Init(fFace)) {
			delete fLigatures;
			fLigatures = NULL;
		}
	}
	if (fLigatures == NULL || !fLigatures->HasLigatures())
		return true;

	// the longest prefix that becomes one glyph
	for (int32 length = mapped; length > 1; length--) {
		uint32 shaped[kMaxSequence];
		memcpy(shaped, glyphs, sizeof(uint32) * length);
		if (fLigatures->Apply(shaped, length) == 1) {
			glyphIndex = shaped[0];
			usedCodes = length;
			return true;
		}
	}
	return true;
}


bool
FontEngine::_PrepareColorGlyph(uint32 glyphIndex)
{
	if (ColorGlyphRenderer::HasPaint(fFace, glyphIndex)) {
		// 'COLR' version 1: the metrics of the (unhinted) base glyph, then
		// its paint graph
		if (FT_Load_Glyph(fFace, glyphIndex, FT_LOAD_NO_HINTING
				| FT_LOAD_NO_BITMAP) != 0) {
			return false;
		}
		FT_GlyphSlot slot = fFace->glyph;
		double advance = slot->linearHoriAdvance / 65536.0;

		if (fColorRenderer == NULL)
			fColorRenderer = new(std::nothrow) ColorGlyphRenderer(fFace);
		ColorGlyphBitmap bitmap;
		if (fColorRenderer == NULL
			|| !fColorRenderer->Render(glyphIndex, bitmap)) {
			return false;
		}

		fAdvanceX = advance;
		fAdvanceY = 0;
		fInsetLeft = bitmap.left;
		fInsetRight = bitmap.left + bitmap.width - advance;
		if (!_SetColorBitmap(bitmap.data, bitmap.width, bitmap.height,
				bitmap.width * 4, false, 1.0, bitmap.left, bitmap.top)) {
			return false;
		}
	} else {
		// Bitmaps (CBDT, sbix) come as BGRA from loading them, 'COLR'
		// version 0 layers from rendering them.
		if (FT_Load_Glyph(fFace, glyphIndex, FT_LOAD_COLOR) != 0)
			return false;
		FT_GlyphSlot slot = fFace->glyph;
		if (slot->format != FT_GLYPH_FORMAT_BITMAP
			&& FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) {
			return false;
		}
		const FT_Bitmap& bitmap = slot->bitmap;
		if (bitmap.pixel_mode != FT_PIXEL_MODE_BGRA)
			return false;

		double scale = slot->format == FT_GLYPH_FORMAT_BITMAP
			&& !FT_IS_SCALABLE(fFace) ? fBitmapScale : 1.0;
		fAdvanceX = int26p6_to_dbl(slot->advance.x) * scale;
		fAdvanceY = 0;
		fInsetLeft = int26p6_to_dbl(slot->metrics.horiBearingX) * scale;
		fInsetRight = int26p6_to_dbl(slot->metrics.horiBearingX
			+ slot->metrics.width - slot->metrics.horiAdvance) * scale;
		if (!_SetColorBitmap(bitmap.buffer, bitmap.width, bitmap.rows,
				bitmap.pitch, true, scale, slot->bitmap_left,
				-slot->bitmap_top)) {
			return false;
		}
	}

	fPreciseAdvanceX = fPixelSize > 0 ? fAdvanceX / fPixelSize : 0;
	fPreciseAdvanceY = 0;
	return true;
}


/*!	Stores a premultiplied BGRA or RGBA image, scaled by \a scale, as the
	glyph's RGBA pixels that are not premultiplied. \a left and \a top
	place the unscaled image relative to the pen, with y going down.
*/
bool
FontEngine::_SetColorBitmap(const uint8* pixels, int32 width, int32 height,
	int32 stride, bool bgra, double scale, int32 left, int32 top)
{
	if (pixels == NULL || width <= 0 || height <= 0 || scale <= 0)
		return false;

	int32 targetLeft = (int32)floor(left * scale + 0.5);
	int32 targetTop = (int32)floor(top * scale + 0.5);
	int32 targetWidth = (int32)ceil(width * scale - 0.01);
	int32 targetHeight = (int32)ceil(height * scale - 0.01);
	if (targetWidth < 1)
		targetWidth = 1;
	if (targetHeight < 1)
		targetHeight = 1;
	if (targetWidth > 4096 || targetHeight > 4096)
		return false;

	free(fColorData);
	fDataSize = targetWidth * targetHeight * 4;
	fColorData = (uint8*)malloc(fDataSize);
	if (fColorData == NULL) {
		fDataSize = 0;
		return false;
	}

	int red = bgra ? 2 : 0;
	int blue = bgra ? 0 : 2;
	// the source pixels each target pixel covers, in both directions
	double sx = double(width) / targetWidth;
	double sy = double(height) / targetHeight;

	uint8* target = fColorData;
	for (int32 y = 0; y < targetHeight; y++) {
		double y0 = y * sy;
		double y1 = y0 + sy;
		for (int32 x = 0; x < targetWidth; x++) {
			double x0 = x * sx;
			double x1 = x0 + sx;
			double sum[4] = { 0, 0, 0, 0 };
			double area = 0;

			if (sx <= 1 && sy <= 1) {
				// enlarging: bilinear between the nearest source pixels
				double fx = (x + 0.5) * sx - 0.5;
				double fy = (y + 0.5) * sy - 0.5;
				int32 ix = (int32)floor(fx);
				int32 iy = (int32)floor(fy);
				fx -= ix;
				fy -= iy;
				for (int32 j = 0; j < 2; j++) {
					int32 row = iy + j < 0 ? 0
						: (iy + j >= height ? height - 1 : iy + j);
					for (int32 i = 0; i < 2; i++) {
						int32 column = ix + i < 0 ? 0
							: (ix + i >= width ? width - 1 : ix + i);
						double weight = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
						const uint8* p = pixels + row * stride + column * 4;
						for (int k = 0; k < 4; k++)
							sum[k] += p[k] * weight;
						area += weight;
					}
				}
			} else {
				// shrinking: the average of the source area
				for (int32 row = (int32)y0; row < height && row < y1; row++) {
					double h = fmin(y1, row + 1) - fmax(y0, row);
					if (h <= 0)
						continue;
					for (int32 column = (int32)x0; column < width
							&& column < x1; column++) {
						double w = fmin(x1, column + 1) - fmax(x0, column);
						if (w <= 0)
							continue;
						const uint8* p = pixels + row * stride + column * 4;
						for (int k = 0; k < 4; k++)
							sum[k] += p[k] * w * h;
						area += w * h;
					}
				}
			}

			double alpha = area > 0 ? sum[3] / area : 0;
			if (alpha < 0.5) {
				target[0] = target[1] = target[2] = target[3] = 0;
			} else {
				// not premultiplied any longer
				target[0] = (uint8)fmin(255, sum[red] / area * 255 / alpha
					+ 0.5);
				target[1] = (uint8)fmin(255, sum[1] / area * 255 / alpha
					+ 0.5);
				target[2] = (uint8)fmin(255, sum[blue] / area * 255 / alpha
					+ 0.5);
				target[3] = (uint8)fmin(255, alpha + 0.5);
			}
			target += 4;
		}
	}

	fBounds.x1 = targetLeft;
	fBounds.y1 = targetTop;
	fBounds.x2 = targetLeft + targetWidth - 1;
	fBounds.y2 = targetTop + targetHeight - 1;
	fDataType = glyph_data_color;
	return true;
}
