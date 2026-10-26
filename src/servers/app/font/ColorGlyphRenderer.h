/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef COLOR_GLYPH_RENDERER_H
#define COLOR_GLYPH_RENDERER_H


#include <SupportDefs.h>

#include <ft2build.h>
#include FT_FREETYPE_H

// FreeType reads 'COLR' version 1 since 2.13
#if FREETYPE_MAJOR > 2 || (FREETYPE_MAJOR == 2 && FREETYPE_MINOR >= 13)
#	define COLOR_GLYPH_RENDERER_COLR_V1 1
#	include FT_COLOR_H
#endif

#include <agg_path_storage.h>
#include <agg_trans_affine.h>


//! A color glyph image: premultiplied RGBA rows, top to bottom.
struct ColorGlyphBitmap {
								ColorGlyphBitmap();
								~ColorGlyphBitmap();

			bool				Allocate(int32 width, int32 height);

			int32				left;
				// from the pen position to the first column, in pixels
			int32				top;
				// from the base line to the first row, downwards
			int32				width;
			int32				height;
			uint8*				data;
};


/*!	Paints the glyphs of an OpenType 'COLR' version 1 table (the paint
	graphs of https://learn.microsoft.com/typography/opentype/spec/colr):
	layers, solid colors, linear, radial and sweep gradients, glyph clips,
	transforms and the composite modes. FreeType reads the graph; it does
	not paint it.
*/
class ColorGlyphRenderer {
public:
								ColorGlyphRenderer(FT_Face face);
								~ColorGlyphRenderer();

	static	bool				HasPaint(FT_Face face, uint32 glyphIndex);

			// Paints the glyph at the face's current size.
			bool				Render(uint32 glyphIndex,
									ColorGlyphBitmap& bitmap);

private:
			FT_Face				fFace;

#ifdef COLOR_GLYPH_RENDERER_COLR_V1
	struct Canvas;
	struct Clip;
	struct ColorLine;

			bool				_Paint(const FT_OpaquePaint& opaquePaint,
									int32 depth);
			bool				_PaintTransformed(
									const FT_OpaquePaint& child,
									const agg::trans_affine& transform,
									int32 depth);
			bool				_PaintGlyph(const FT_PaintGlyph& paint,
									int32 depth);
			bool				_PaintComposite(
									const FT_PaintComposite& paint,
									int32 depth);
			bool				_LoadOutline(uint32 glyphIndex);
			void				_AddOutlineBounds();

			void				_FillSolid(const FT_ColorIndex& color);
			void				_FillLinear(
									const FT_PaintLinearGradient& paint);
			void				_FillRadial(
									const FT_PaintRadialGradient& paint);
			void				_FillSweep(
									const FT_PaintSweepGradient& paint);
			template<class Gradient>
			void				_FillGradient(const ColorLine& line,
									const Gradient& gradient);
			bool				_ReadColorLine(FT_ColorLine& colorLine,
									ColorLine& line, double& first,
									double& last);
			void				_Color(const FT_ColorIndex& color,
									double rgba[4]) const;

			FT_Color*			fPalette;
			uint16				fPaletteSize;
			double				fScale;
				// pixels per font unit

			bool				fMeasuring;
			double				fBounds[4];
			int32				fOperations;
			int32				fCompositeDepth;

			agg::trans_affine	fTransform;
				// paint coordinates (font units) to the canvas
			Canvas*				fCanvas;
			Clip*				fClip;
			agg::path_storage	fPath;
				// the outline of the current glyph clip, in font units
#endif
};


#endif	// COLOR_GLYPH_RENDERER_H
