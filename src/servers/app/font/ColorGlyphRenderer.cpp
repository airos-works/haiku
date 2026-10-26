/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "ColorGlyphRenderer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include FT_OUTLINE_H

#include <agg_conv_curve.h>
#include <agg_conv_transform.h>
#include <agg_rasterizer_scanline_aa.h>
#include <agg_scanline_u.h>


static const int32 kMaxDepth = 64;
static const int32 kMaxOperations = 50000;
	// a malformed or hostile font cannot keep the server busy for long
static const int32 kMaxCanvasSize = 1024;
static const int32 kMaxCompositeDepth = 8;
	// each composite needs two more canvases
static const int32 kColorLineSize = 256;


static inline double
fixed_to_double(FT_Fixed value)
{
	return value / 65536.0;
}


static inline uint8
to_byte(double value)
{
	if (value <= 0)
		return 0;
	if (value >= 1)
		return 255;
	return uint8(value * 255 + 0.5);
}


// #pragma mark - ColorGlyphBitmap


ColorGlyphBitmap::ColorGlyphBitmap()
	:
	left(0),
	top(0),
	width(0),
	height(0),
	data(NULL)
{
}


ColorGlyphBitmap::~ColorGlyphBitmap()
{
	free(data);
}


bool
ColorGlyphBitmap::Allocate(int32 newWidth, int32 newHeight)
{
	free(data);
	width = newWidth;
	height = newHeight;
	data = (uint8*)calloc((size_t)width * height, 4);
	return data != NULL;
}


// #pragma mark - private types


//! Premultiplied RGBA pixels.
struct ColorGlyphRenderer::Canvas {
	Canvas(int32 width, int32 height)
		:
		width(width),
		height(height),
		pixels((uint8*)calloc((size_t)width * height, 4))
	{
	}

	~Canvas()
	{
		free(pixels);
	}

	int32	width;
	int32	height;
	uint8*	pixels;
};


//! Coverage of the glyph clips that apply, and the rectangle it covers.
struct ColorGlyphRenderer::Clip {
	Clip(int32 width, int32 height)
		:
		coverage((uint8*)calloc((size_t)width * height, 1)),
		left(width),
		top(height),
		right(-1),
		bottom(-1)
	{
	}

	~Clip()
	{
		free(coverage);
	}

	uint8*	coverage;
	int32	left;
	int32	top;
	int32	right;
	int32	bottom;
};


//! A color line, sampled over the range of its stops.
struct ColorGlyphRenderer::ColorLine {
	FT_PaintExtend	extend;
	uint8			colors[kColorLineSize][4];

	inline const uint8* At(double t) const
	{
		switch (extend) {
			case FT_COLR_PAINT_EXTEND_REPEAT:
				t -= floor(t);
				break;
			case FT_COLR_PAINT_EXTEND_REFLECT:
				t = fabs(t - 2 * floor(t / 2 + 0.5));
				break;
			case FT_COLR_PAINT_EXTEND_PAD:
			default:
				break;
		}
		int32 index = int32(t * (kColorLineSize - 1) + 0.5);
		if (index < 0)
			index = 0;
		else if (index >= kColorLineSize)
			index = kColorLineSize - 1;
		return colors[index];
	}
};


struct LinearGradient {
	double	x0, y0, dx, dy;
		// t = ((p - p0) . d) with d scaled by 1 / |d|^2

	inline bool At(double x, double y, double& t) const
	{
		t = (x - x0) * dx + (y - y0) * dy;
		return true;
	}
};


struct RadialGradient {
	// two circles: c(t) = c0 + t * cd, r(t) = r0 + t * dr
	double	cx, cy, r0;
	double	cdx, cdy, dr;
	double	a;

	inline bool At(double x, double y, double& t) const
	{
		double px = x - cx;
		double py = y - cy;
		double b = px * cdx + py * cdy + r0 * dr;
		double c = px * px + py * py - r0 * r0;
		if (fabs(a) < 1e-9) {
			if (fabs(b) < 1e-9)
				return false;
			t = c / (2 * b);
			return r0 + t * dr >= 0;
		}
		double discriminant = b * b - a * c;
		if (discriminant < 0)
			return false;
		double root = sqrt(discriminant);
		double t1 = (b + root) / a;
		double t2 = (b - root) / a;
		if (t1 < t2) {
			double swap = t1;
			t1 = t2;
			t2 = swap;
		}
		if (r0 + t1 * dr >= 0) {
			t = t1;
			return true;
		}
		if (r0 + t2 * dr >= 0) {
			t = t2;
			return true;
		}
		return false;
	}
};


struct SweepGradient {
	double	cx, cy;
	double	start, range;
		// in degrees, counter-clockwise from the x axis

	inline bool At(double x, double y, double& t) const
	{
		double angle = atan2(y - cy, x - cx) * 180 / M_PI;
		if (angle < 0)
			angle += 360;
		t = (angle - start) / range;
		return true;
	}
};


// #pragma mark - outline decomposition


struct OutlineSink {
	agg::path_storage*	path;
	bool				open;
};


static int
outline_move_to(const FT_Vector* to, void* user)
{
	OutlineSink* sink = (OutlineSink*)user;
	if (sink->open)
		sink->path->close_polygon();
	sink->path->move_to(to->x, to->y);
	sink->open = true;
	return 0;
}


static int
outline_line_to(const FT_Vector* to, void* user)
{
	((OutlineSink*)user)->path->line_to(to->x, to->y);
	return 0;
}


static int
outline_conic_to(const FT_Vector* control, const FT_Vector* to, void* user)
{
	((OutlineSink*)user)->path->curve3(control->x, control->y, to->x, to->y);
	return 0;
}


static int
outline_cubic_to(const FT_Vector* control1, const FT_Vector* control2,
	const FT_Vector* to, void* user)
{
	((OutlineSink*)user)->path->curve4(control1->x, control1->y,
		control2->x, control2->y, to->x, to->y);
	return 0;
}


// #pragma mark - blending


static inline void
blend_over(uint8* destination, const uint8* source, uint32 coverage)
{
	// source is premultiplied; coverage is 0..255
	uint32 alpha = (source[3] * coverage + 127) / 255;
	if (alpha == 0)
		return;
	uint32 inverse = 255 - alpha;
	for (int i = 0; i < 3; i++) {
		destination[i] = (source[i] * coverage + 127) / 255
			+ (destination[i] * inverse + 127) / 255;
	}
	destination[3] = alpha + (destination[3] * inverse + 127) / 255;
}


static double
blend_channel(FT_Composite_Mode mode, double s, double d)
{
	// the separable blend modes of Compositing and Blending Level 1, with
	// colors that are not premultiplied
	switch (mode) {
		case FT_COLR_COMPOSITE_SCREEN:
			return s + d - s * d;
		case FT_COLR_COMPOSITE_OVERLAY:
			return blend_channel(FT_COLR_COMPOSITE_HARD_LIGHT, d, s);
		case FT_COLR_COMPOSITE_DARKEN:
			return s < d ? s : d;
		case FT_COLR_COMPOSITE_LIGHTEN:
			return s > d ? s : d;
		case FT_COLR_COMPOSITE_COLOR_DODGE:
			if (d == 0)
				return 0;
			if (s >= 1)
				return 1;
			return d / (1 - s) < 1 ? d / (1 - s) : 1;
		case FT_COLR_COMPOSITE_COLOR_BURN:
			if (d >= 1)
				return 1;
			if (s <= 0)
				return 0;
			return 1 - ((1 - d) / s < 1 ? (1 - d) / s : 1);
		case FT_COLR_COMPOSITE_HARD_LIGHT:
			if (s <= 0.5)
				return d * 2 * s;
			return blend_channel(FT_COLR_COMPOSITE_SCREEN, 2 * s - 1, d);
		case FT_COLR_COMPOSITE_SOFT_LIGHT:
		{
			if (s <= 0.5)
				return d - (1 - 2 * s) * d * (1 - d);
			double e = d <= 0.25 ? ((16 * d - 12) * d + 4) * d : sqrt(d);
			return d + (2 * s - 1) * (e - d);
		}
		case FT_COLR_COMPOSITE_DIFFERENCE:
			return fabs(s - d);
		case FT_COLR_COMPOSITE_EXCLUSION:
			return s + d - 2 * s * d;
		case FT_COLR_COMPOSITE_MULTIPLY:
			return s * d;
		default:
			return s;
	}
}


static inline double
luminosity(const double c[3])
{
	return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
}


static void
set_luminosity(double c[3], double l)
{
	double d = l - luminosity(c);
	for (int i = 0; i < 3; i++)
		c[i] += d;

	l = luminosity(c);
	double n = fmin(c[0], fmin(c[1], c[2]));
	double x = fmax(c[0], fmax(c[1], c[2]));
	for (int i = 0; i < 3; i++) {
		if (n < 0)
			c[i] = l + (c[i] - l) * l / (l - n);
		if (x > 1)
			c[i] = l + (c[i] - l) * (1 - l) / (x - l);
	}
}


static inline double
saturation(const double c[3])
{
	return fmax(c[0], fmax(c[1], c[2])) - fmin(c[0], fmin(c[1], c[2]));
}


static void
set_saturation(double c[3], double s)
{
	int max = 0, min = 0;
	for (int i = 1; i < 3; i++) {
		if (c[i] > c[max])
			max = i;
		if (c[i] < c[min])
			min = i;
	}
	if (max == min) {
		c[0] = c[1] = c[2] = 0;
		return;
	}
	int mid = 3 - max - min;
	c[mid] = (c[mid] - c[min]) * s / (c[max] - c[min]);
	c[max] = s;
	c[min] = 0;
}


static void
composite_pixel(FT_Composite_Mode mode, const uint8* sourcePixel,
	uint8* destinationPixel)
{
	double s[4], d[4];
	for (int i = 0; i < 4; i++) {
		s[i] = sourcePixel[i] / 255.0;
		d[i] = destinationPixel[i] / 255.0;
	}
	double sa = s[3];
	double da = d[3];
	double r[4];

	double fs = 0, fd = 0;
		// Porter-Duff factors for the source and the destination
	bool porterDuff = true;
	switch (mode) {
		case FT_COLR_COMPOSITE_CLEAR:		fs = 0;			fd = 0;			break;
		case FT_COLR_COMPOSITE_SRC:			fs = 1;			fd = 0;			break;
		case FT_COLR_COMPOSITE_DEST:		fs = 0;			fd = 1;			break;
		case FT_COLR_COMPOSITE_SRC_OVER:	fs = 1;			fd = 1 - sa;	break;
		case FT_COLR_COMPOSITE_DEST_OVER:	fs = 1 - da;	fd = 1;			break;
		case FT_COLR_COMPOSITE_SRC_IN:		fs = da;		fd = 0;			break;
		case FT_COLR_COMPOSITE_DEST_IN:		fs = 0;			fd = sa;		break;
		case FT_COLR_COMPOSITE_SRC_OUT:		fs = 1 - da;	fd = 0;			break;
		case FT_COLR_COMPOSITE_DEST_OUT:	fs = 0;			fd = 1 - sa;	break;
		case FT_COLR_COMPOSITE_SRC_ATOP:	fs = da;		fd = 1 - sa;	break;
		case FT_COLR_COMPOSITE_DEST_ATOP:	fs = 1 - da;	fd = sa;		break;
		case FT_COLR_COMPOSITE_XOR:			fs = 1 - da;	fd = 1 - sa;	break;
		case FT_COLR_COMPOSITE_PLUS:		fs = 1;			fd = 1;			break;
		default:
			porterDuff = false;
			break;
	}

	if (porterDuff) {
		for (int i = 0; i < 4; i++)
			r[i] = fmin(1, s[i] * fs + d[i] * fd);
	} else {
		double cs[3], cd[3], b[3];
		for (int i = 0; i < 3; i++) {
			cs[i] = sa > 0 ? s[i] / sa : 0;
			cd[i] = da > 0 ? d[i] / da : 0;
		}
		switch (mode) {
			case FT_COLR_COMPOSITE_HSL_HUE:
				memcpy(b, cs, sizeof(b));
				set_saturation(b, saturation(cd));
				set_luminosity(b, luminosity(cd));
				break;
			case FT_COLR_COMPOSITE_HSL_SATURATION:
				memcpy(b, cd, sizeof(b));
				set_saturation(b, saturation(cs));
				set_luminosity(b, luminosity(cd));
				break;
			case FT_COLR_COMPOSITE_HSL_COLOR:
				memcpy(b, cs, sizeof(b));
				set_luminosity(b, luminosity(cd));
				break;
			case FT_COLR_COMPOSITE_HSL_LUMINOSITY:
				memcpy(b, cd, sizeof(b));
				set_luminosity(b, luminosity(cs));
				break;
			default:
				for (int i = 0; i < 3; i++)
					b[i] = blend_channel(mode, cs[i], cd[i]);
				break;
		}
		for (int i = 0; i < 3; i++)
			r[i] = (1 - da) * s[i] + (1 - sa) * d[i] + sa * da * b[i];
		r[3] = sa + da - sa * da;
	}

	for (int i = 0; i < 4; i++)
		destinationPixel[i] = to_byte(r[i]);
	if (destinationPixel[3] < destinationPixel[0])
		destinationPixel[0] = destinationPixel[3];
	if (destinationPixel[3] < destinationPixel[1])
		destinationPixel[1] = destinationPixel[3];
	if (destinationPixel[3] < destinationPixel[2])
		destinationPixel[2] = destinationPixel[3];
}


// #pragma mark - ColorGlyphRenderer


ColorGlyphRenderer::ColorGlyphRenderer(FT_Face face)
	:
	fFace(face),
	fPalette(NULL),
	fPaletteSize(0),
	fScale(0),
	fMeasuring(false),
	fOperations(0),
	fCompositeDepth(0),
	fCanvas(NULL),
	fClip(NULL)
{
	FT_Palette_Data paletteData;
	if (FT_Palette_Data_Get(face, &paletteData) == 0
		&& FT_Palette_Select(face, 0, &fPalette) == 0) {
		fPaletteSize = paletteData.num_palette_entries;
	} else
		fPalette = NULL;
}


ColorGlyphRenderer::~ColorGlyphRenderer()
{
}


/*static*/ bool
ColorGlyphRenderer::HasPaint(FT_Face face, uint32 glyphIndex)
{
	FT_OpaquePaint paint;
	paint.p = NULL;
	return FT_Get_Color_Glyph_Paint(face, glyphIndex,
		FT_COLOR_NO_ROOT_TRANSFORM, &paint) != 0;
}


bool
ColorGlyphRenderer::Render(uint32 glyphIndex, ColorGlyphBitmap& bitmap)
{
	if (fFace->size == NULL || fFace->units_per_EM == 0)
		return false;

	FT_OpaquePaint root;
	root.p = NULL;
	if (!FT_Get_Color_Glyph_Paint(fFace, glyphIndex,
			FT_COLOR_NO_ROOT_TRANSFORM, &root)) {
		return false;
	}

	// x_scale converts font units to 26.6 pixels
	fScale = fixed_to_double(fFace->size->metrics.x_scale) / 64.0;

	// The bounds, in pixels and with y going up
	FT_ClipBox clipBox;
	if (FT_Get_Color_Glyph_ClipBox(fFace, glyphIndex, &clipBox)) {
		const FT_Vector* corners = &clipBox.bottom_left;
		fBounds[0] = fBounds[2] = corners[0].x / 64.0;
		fBounds[1] = fBounds[3] = corners[0].y / 64.0;
		for (int i = 1; i < 4; i++) {
			fBounds[0] = fmin(fBounds[0], corners[i].x / 64.0);
			fBounds[1] = fmin(fBounds[1], corners[i].y / 64.0);
			fBounds[2] = fmax(fBounds[2], corners[i].x / 64.0);
			fBounds[3] = fmax(fBounds[3], corners[i].y / 64.0);
		}
	} else {
		fBounds[0] = fBounds[1] = HUGE_VAL;
		fBounds[2] = fBounds[3] = -HUGE_VAL;
		fMeasuring = true;
		fOperations = 0;
		fTransform = agg::trans_affine_scaling(fScale);
		bool measured = _Paint(root, 0);
		fMeasuring = false;
		if (!measured || fBounds[0] > fBounds[2])
			return false;
	}

	int32 left = (int32)floor(fBounds[0]);
	int32 right = (int32)ceil(fBounds[2]);
	int32 top = (int32)floor(-fBounds[3]);
	int32 bottom = (int32)ceil(-fBounds[1]);
	int32 width = right - left;
	int32 height = bottom - top;
	if (width <= 0 || height <= 0)
		return false;
	if (width > kMaxCanvasSize || height > kMaxCanvasSize)
		return false;

	Canvas canvas(width, height);
	if (canvas.pixels == NULL)
		return false;

	// font units (y up) to the canvas (y down)
	fTransform = agg::trans_affine(fScale, 0, 0, -fScale, -left, -top);
	fCanvas = &canvas;
	fClip = NULL;
	fOperations = 0;
	bool painted = _Paint(root, 0);
	fCanvas = NULL;
	if (!painted)
		return false;

	bitmap.left = left;
	bitmap.top = top;
	bitmap.width = width;
	bitmap.height = height;
	free(bitmap.data);
	bitmap.data = canvas.pixels;
	canvas.pixels = NULL;
	return true;
}


bool
ColorGlyphRenderer::_Paint(const FT_OpaquePaint& opaquePaint, int32 depth)
{
	if (depth > kMaxDepth || ++fOperations > kMaxOperations)
		return false;

	FT_COLR_Paint paint;
	if (!FT_Get_Paint(fFace, opaquePaint, &paint))
		return false;

	switch (paint.format) {
		case FT_COLR_PAINTFORMAT_COLR_LAYERS:
		{
			FT_LayerIterator iterator = paint.u.colr_layers.layer_iterator;
			FT_OpaquePaint layer;
			layer.p = NULL;
			while (FT_Get_Paint_Layers(fFace, &iterator, &layer)) {
				if (!_Paint(layer, depth + 1))
					return false;
			}
			return true;
		}

		case FT_COLR_PAINTFORMAT_SOLID:
			if (!fMeasuring)
				_FillSolid(paint.u.solid.color);
			return true;

		case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT:
			if (!fMeasuring)
				_FillLinear(paint.u.linear_gradient);
			return true;

		case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT:
			if (!fMeasuring)
				_FillRadial(paint.u.radial_gradient);
			return true;

		case FT_COLR_PAINTFORMAT_SWEEP_GRADIENT:
			if (!fMeasuring)
				_FillSweep(paint.u.sweep_gradient);
			return true;

		case FT_COLR_PAINTFORMAT_GLYPH:
			return _PaintGlyph(paint.u.glyph, depth);

		case FT_COLR_PAINTFORMAT_COLR_GLYPH:
		{
			FT_OpaquePaint child;
			child.p = NULL;
			if (!FT_Get_Color_Glyph_Paint(fFace, paint.u.colr_glyph.glyphID,
					FT_COLOR_NO_ROOT_TRANSFORM, &child)) {
				return true;
			}
			return _Paint(child, depth + 1);
		}

		case FT_COLR_PAINTFORMAT_TRANSFORM:
		{
			const FT_Affine23& affine = paint.u.transform.affine;
			agg::trans_affine transform(fixed_to_double(affine.xx),
				fixed_to_double(affine.yx), fixed_to_double(affine.xy),
				fixed_to_double(affine.yy), fixed_to_double(affine.dx),
				fixed_to_double(affine.dy));
			return _PaintTransformed(paint.u.transform.paint, transform,
				depth);
		}

		case FT_COLR_PAINTFORMAT_TRANSLATE:
			return _PaintTransformed(paint.u.translate.paint,
				agg::trans_affine_translation(
					fixed_to_double(paint.u.translate.dx),
					fixed_to_double(paint.u.translate.dy)), depth);

		case FT_COLR_PAINTFORMAT_SCALE:
		{
			const FT_PaintScale& scale = paint.u.scale;
			double cx = fixed_to_double(scale.center_x);
			double cy = fixed_to_double(scale.center_y);
			agg::trans_affine transform = agg::trans_affine_translation(-cx,
				-cy);
			transform *= agg::trans_affine_scaling(
				fixed_to_double(scale.scale_x),
				fixed_to_double(scale.scale_y));
			transform *= agg::trans_affine_translation(cx, cy);
			return _PaintTransformed(scale.paint, transform, depth);
		}

		case FT_COLR_PAINTFORMAT_ROTATE:
		{
			const FT_PaintRotate& rotate = paint.u.rotate;
			double cx = fixed_to_double(rotate.center_x);
			double cy = fixed_to_double(rotate.center_y);
			agg::trans_affine transform = agg::trans_affine_translation(-cx,
				-cy);
			transform *= agg::trans_affine_rotation(
				fixed_to_double(rotate.angle) * M_PI);
			transform *= agg::trans_affine_translation(cx, cy);
			return _PaintTransformed(rotate.paint, transform, depth);
		}

		case FT_COLR_PAINTFORMAT_SKEW:
		{
			const FT_PaintSkew& skew = paint.u.skew;
			double cx = fixed_to_double(skew.center_x);
			double cy = fixed_to_double(skew.center_y);
			agg::trans_affine transform = agg::trans_affine_translation(-cx,
				-cy);
			// x' = x - tan(x angle) * y, y' = y + tan(y angle) * x
			transform *= agg::trans_affine(1,
				tan(fixed_to_double(skew.y_skew_angle) * M_PI),
				-tan(fixed_to_double(skew.x_skew_angle) * M_PI), 1, 0, 0);
			transform *= agg::trans_affine_translation(cx, cy);
			return _PaintTransformed(skew.paint, transform, depth);
		}

		case FT_COLR_PAINTFORMAT_COMPOSITE:
			return _PaintComposite(paint.u.composite, depth);

		default:
			// unknown paints draw nothing
			return true;
	}
}


bool
ColorGlyphRenderer::_PaintTransformed(const FT_OpaquePaint& child,
	const agg::trans_affine& transform, int32 depth)
{
	agg::trans_affine saved = fTransform;
	// the child's coordinates go through the transform first
	agg::trans_affine combined = transform;
	combined *= fTransform;
	fTransform = combined;
	bool result = _Paint(child, depth + 1);
	fTransform = saved;
	return result;
}


bool
ColorGlyphRenderer::_LoadOutline(uint32 glyphIndex)
{
	fPath.remove_all();
	if (FT_Load_Glyph(fFace, glyphIndex, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING
			| FT_LOAD_NO_BITMAP | FT_LOAD_IGNORE_TRANSFORM) != 0
		|| fFace->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
		return false;
	}

	FT_Outline_Funcs functions;
	functions.move_to = outline_move_to;
	functions.line_to = outline_line_to;
	functions.conic_to = outline_conic_to;
	functions.cubic_to = outline_cubic_to;
	functions.shift = 0;
	functions.delta = 0;

	OutlineSink sink;
	sink.path = &fPath;
	sink.open = false;
	if (FT_Outline_Decompose(&fFace->glyph->outline, &functions, &sink) != 0)
		return false;
	if (sink.open)
		fPath.close_polygon();
	return true;
}


void
ColorGlyphRenderer::_AddOutlineBounds()
{
	agg::conv_curve<agg::path_storage> curves(fPath);
	agg::conv_transform<agg::conv_curve<agg::path_storage> > transformed(
		curves, fTransform);
	transformed.rewind(0);
	double x, y;
	unsigned command;
	while (!agg::is_stop(command = transformed.vertex(&x, &y))) {
		if (!agg::is_vertex(command))
			continue;
		fBounds[0] = fmin(fBounds[0], x);
		fBounds[1] = fmin(fBounds[1], y);
		fBounds[2] = fmax(fBounds[2], x);
		fBounds[3] = fmax(fBounds[3], y);
	}
}


bool
ColorGlyphRenderer::_PaintGlyph(const FT_PaintGlyph& paint, int32 depth)
{
	if (!_LoadOutline(paint.glyphID))
		return true;

	if (fMeasuring) {
		// the fill cannot go beyond the glyph; the transform of the
		// measuring pass has y going up
		_AddOutlineBounds();
		return true;
	}

	Clip clip(fCanvas->width, fCanvas->height);
	if (clip.coverage == NULL)
		return false;

	agg::conv_curve<agg::path_storage> curves(fPath);
	agg::conv_transform<agg::conv_curve<agg::path_storage> > transformed(
		curves, fTransform);
	curves.approximation_scale(fTransform.scale() > 0
		? fTransform.scale() : 1);

	agg::rasterizer_scanline_aa<> rasterizer;
	rasterizer.clip_box(0, 0, fCanvas->width, fCanvas->height);
	rasterizer.filling_rule(agg::fill_non_zero);
	rasterizer.add_path(transformed);

	agg::scanline_u8 scanline;
	if (rasterizer.rewind_scanlines()) {
		scanline.reset(rasterizer.min_x(), rasterizer.max_x());
		while (rasterizer.sweep_scanline(scanline)) {
			int32 y = scanline.y();
			if (y < 0 || y >= fCanvas->height)
				continue;
			unsigned spanCount = scanline.num_spans();
			agg::scanline_u8::const_iterator span = scanline.begin();
			for (; spanCount > 0; spanCount--, ++span) {
				int32 x = span->x;
				int32 length = span->len;
				const uint8* covers = span->covers;
				if (x < 0) {
					covers -= x;
					length += x;
					x = 0;
				}
				if (x + length > fCanvas->width)
					length = fCanvas->width - x;
				if (length <= 0)
					continue;

				uint8* row = clip.coverage + (size_t)y * fCanvas->width;
				const uint8* outer = fClip != NULL
					? fClip->coverage + (size_t)y * fCanvas->width : NULL;
				for (int32 i = 0; i < length; i++) {
					uint32 coverage = covers[i];
					if (outer != NULL)
						coverage = (coverage * outer[x + i] + 127) / 255;
					row[x + i] = coverage;
				}
				if (clip.left > x)
					clip.left = x;
				if (clip.right < x + length - 1)
					clip.right = x + length - 1;
				if (clip.top > y)
					clip.top = y;
				if (clip.bottom < y)
					clip.bottom = y;
			}
		}
	}

	if (clip.right < clip.left)
		return true;

	Clip* saved = fClip;
	fClip = &clip;
	bool result = _Paint(paint.paint, depth + 1);
	fClip = saved;
	return result;
}


bool
ColorGlyphRenderer::_PaintComposite(const FT_PaintComposite& paint,
	int32 depth)
{
	if (fMeasuring) {
		return _Paint(paint.backdrop_paint, depth + 1)
			&& _Paint(paint.source_paint, depth + 1);
	}

	if (fCompositeDepth >= kMaxCompositeDepth)
		return false;

	Canvas backdrop(fCanvas->width, fCanvas->height);
	Canvas source(fCanvas->width, fCanvas->height);
	if (backdrop.pixels == NULL || source.pixels == NULL)
		return false;

	Canvas* saved = fCanvas;
	fCompositeDepth++;
	fCanvas = &backdrop;
	bool result = _Paint(paint.backdrop_paint, depth + 1);
	fCanvas = &source;
	result = result && _Paint(paint.source_paint, depth + 1);
	fCanvas = saved;
	fCompositeDepth--;
	if (!result)
		return false;

	size_t count = (size_t)fCanvas->width * fCanvas->height;
	for (size_t i = 0; i < count; i++) {
		uint8* backdropPixel = backdrop.pixels + i * 4;
		const uint8* sourcePixel = source.pixels + i * 4;
		if (paint.composite_mode == FT_COLR_COMPOSITE_SRC_OVER)
			blend_over(backdropPixel, sourcePixel, 255);
		else
			composite_pixel(paint.composite_mode, sourcePixel, backdropPixel);
		blend_over(fCanvas->pixels + i * 4, backdropPixel, 255);
	}
	return true;
}


void
ColorGlyphRenderer::_Color(const FT_ColorIndex& color, double rgba[4]) const
{
	double alpha = color.alpha / 16384.0;
	if (color.palette_index < fPaletteSize && fPalette != NULL) {
		const FT_Color& entry = fPalette[color.palette_index];
		rgba[0] = entry.red / 255.0;
		rgba[1] = entry.green / 255.0;
		rgba[2] = entry.blue / 255.0;
		alpha *= entry.alpha / 255.0;
	} else {
		// 0xffff is the text color, which a cached glyph cannot know
		rgba[0] = rgba[1] = rgba[2] = 0;
	}
	rgba[3] = alpha < 0 ? 0 : (alpha > 1 ? 1 : alpha);
}


void
ColorGlyphRenderer::_FillSolid(const FT_ColorIndex& color)
{
	double rgba[4];
	_Color(color, rgba);
	uint8 pixel[4];
	for (int i = 0; i < 3; i++)
		pixel[i] = to_byte(rgba[i] * rgba[3]);
	pixel[3] = to_byte(rgba[3]);
	if (pixel[3] == 0)
		return;

	int32 left = 0, top = 0;
	int32 right = fCanvas->width - 1, bottom = fCanvas->height - 1;
	if (fClip != NULL) {
		left = fClip->left;
		top = fClip->top;
		right = fClip->right;
		bottom = fClip->bottom;
	}
	for (int32 y = top; y <= bottom; y++) {
		uint8* row = fCanvas->pixels + (size_t)y * fCanvas->width * 4;
		const uint8* coverage = fClip != NULL
			? fClip->coverage + (size_t)y * fCanvas->width : NULL;
		for (int32 x = left; x <= right; x++) {
			uint32 cover = coverage != NULL ? coverage[x] : 255;
			if (cover != 0)
				blend_over(row + x * 4, pixel, cover);
		}
	}
}


/*!	Reads the stops of a color line, and samples them over the range from
	the first to the last stop; \a first and \a last return that range, on
	which the gradient's own geometry must be rescaled.
*/
bool
ColorGlyphRenderer::_ReadColorLine(FT_ColorLine& colorLine, ColorLine& line,
	double& first, double& last)
{
	static const int32 kMaxStops = 64;
	double offsets[kMaxStops];
	double colors[kMaxStops][4];
	int32 count = 0;

	FT_ColorStop stop;
	while (count < kMaxStops && FT_Get_Colorline_Stops(fFace, &stop,
			&colorLine.color_stop_iterator)) {
		double offset = fixed_to_double(stop.stop_offset);
		double rgba[4];
		_Color(stop.color, rgba);

		// keep the stops sorted; equal offsets stay in their order
		int32 index = count;
		while (index > 0 && offsets[index - 1] > offset) {
			offsets[index] = offsets[index - 1];
			memcpy(colors[index], colors[index - 1], sizeof(colors[0]));
			index--;
		}
		offsets[index] = offset;
		// interpolate premultiplied colors
		for (int i = 0; i < 3; i++)
			colors[index][i] = rgba[i] * rgba[3];
		colors[index][3] = rgba[3];
		count++;
	}
	if (count == 0)
		return false;

	line.extend = colorLine.extend;
	first = offsets[0];
	last = offsets[count - 1];
	double range = last - first;
	if (range < 1e-6) {
		// all stops at one offset: the last color, everywhere
		for (int32 i = 0; i < kColorLineSize; i++) {
			for (int j = 0; j < 4; j++)
				line.colors[i][j] = to_byte(colors[count - 1][j]);
		}
		last = first + 1;
		return true;
	}

	int32 stop1 = 0;
	for (int32 i = 0; i < kColorLineSize; i++) {
		double t = first + range * i / (kColorLineSize - 1);
		while (stop1 < count - 2 && offsets[stop1 + 1] < t)
			stop1++;
		int32 stop2 = count > 1 ? stop1 + 1 : stop1;
		double span = offsets[stop2] - offsets[stop1];
		double f = span > 1e-9 ? (t - offsets[stop1]) / span : 1;
		if (f < 0)
			f = 0;
		else if (f > 1)
			f = 1;
		for (int j = 0; j < 4; j++) {
			line.colors[i][j] = to_byte(colors[stop1][j]
				+ (colors[stop2][j] - colors[stop1][j]) * f);
		}
	}
	return true;
}


template<class Gradient>
void
ColorGlyphRenderer::_FillGradient(const ColorLine& line,
	const Gradient& gradient)
{
	agg::trans_affine inverse = fTransform;
	inverse.invert();

	int32 left = 0, top = 0;
	int32 right = fCanvas->width - 1, bottom = fCanvas->height - 1;
	if (fClip != NULL) {
		left = fClip->left;
		top = fClip->top;
		right = fClip->right;
		bottom = fClip->bottom;
	}
	for (int32 y = top; y <= bottom; y++) {
		uint8* row = fCanvas->pixels + (size_t)y * fCanvas->width * 4;
		const uint8* coverage = fClip != NULL
			? fClip->coverage + (size_t)y * fCanvas->width : NULL;
		for (int32 x = left; x <= right; x++) {
			uint32 cover = coverage != NULL ? coverage[x] : 255;
			if (cover == 0)
				continue;
			double px = x + 0.5;
			double py = y + 0.5;
			inverse.transform(&px, &py);
			double t;
			if (!gradient.At(px, py, t))
				continue;
			blend_over(row + x * 4, line.At(t), cover);
		}
	}
}


void
ColorGlyphRenderer::_FillLinear(const FT_PaintLinearGradient& paint)
{
	FT_ColorLine colorLine = paint.colorline;
	ColorLine line;
	double first, last;
	if (!_ReadColorLine(colorLine, line, first, last))
		return;

	double x0 = fixed_to_double(paint.p0.x);
	double y0 = fixed_to_double(paint.p0.y);
	double x1 = fixed_to_double(paint.p1.x);
	double y1 = fixed_to_double(paint.p1.y);
	double x2 = fixed_to_double(paint.p2.x);
	double y2 = fixed_to_double(paint.p2.y);

	// The color line runs from p0 to p1 projected onto the line through p0
	// that is perpendicular to p0-p2.
	double nx = y2 - y0;
	double ny = -(x2 - x0);
	double length = nx * nx + ny * ny;
	double x3 = x1, y3 = y1;
	if (length > 1e-9) {
		double projection = ((x1 - x0) * nx + (y1 - y0) * ny) / length;
		x3 = x0 + nx * projection;
		y3 = y0 + ny * projection;
	}

	// rescale onto the range of the stops
	double dx = x3 - x0;
	double dy = y3 - y0;
	LinearGradient gradient;
	gradient.x0 = x0 + dx * first;
	gradient.y0 = y0 + dy * first;
	dx *= last - first;
	dy *= last - first;
	length = dx * dx + dy * dy;
	if (length < 1e-9)
		return;
	gradient.dx = dx / length;
	gradient.dy = dy / length;
	_FillGradient(line, gradient);
}


void
ColorGlyphRenderer::_FillRadial(const FT_PaintRadialGradient& paint)
{
	FT_ColorLine colorLine = paint.colorline;
	ColorLine line;
	double first, last;
	if (!_ReadColorLine(colorLine, line, first, last))
		return;

	double cx0 = fixed_to_double(paint.c0.x);
	double cy0 = fixed_to_double(paint.c0.y);
	double r0 = fixed_to_double(paint.r0);
	double cx1 = fixed_to_double(paint.c1.x);
	double cy1 = fixed_to_double(paint.c1.y);
	double r1 = fixed_to_double(paint.r1);

	// rescale both circles onto the range of the stops
	RadialGradient gradient;
	gradient.cx = cx0 + (cx1 - cx0) * first;
	gradient.cy = cy0 + (cy1 - cy0) * first;
	gradient.r0 = r0 + (r1 - r0) * first;
	gradient.cdx = (cx1 - cx0) * (last - first);
	gradient.cdy = (cy1 - cy0) * (last - first);
	gradient.dr = (r1 - r0) * (last - first);
	gradient.a = gradient.cdx * gradient.cdx + gradient.cdy * gradient.cdy
		- gradient.dr * gradient.dr;
	_FillGradient(line, gradient);
}


void
ColorGlyphRenderer::_FillSweep(const FT_PaintSweepGradient& paint)
{
	FT_ColorLine colorLine = paint.colorline;
	ColorLine line;
	double first, last;
	if (!_ReadColorLine(colorLine, line, first, last))
		return;

	// The table stores the angles less 180 degrees so that they can reach
	// 360; FreeType returns them as stored.
	double start = (fixed_to_double(paint.start_angle) + 1) * 180;
	double end = (fixed_to_double(paint.end_angle) + 1) * 180;

	SweepGradient gradient;
	gradient.cx = fixed_to_double(paint.center.x);
	gradient.cy = fixed_to_double(paint.center.y);
	gradient.start = start + (end - start) * first;
	gradient.range = (end - start) * (last - first);
	if (fabs(gradient.range) < 1e-9)
		return;
	_FillGradient(line, gradient);
}
