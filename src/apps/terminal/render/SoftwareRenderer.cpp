/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "SoftwareRenderer.h"

#include <limits.h>
#include <math.h>
#include <string.h>

#include <algorithm>


static inline uint32
opaque(uint32 rgb)
{
	return 0xff000000 | rgb;
}


static inline uint32
blend(uint32 destination, uint32 color, uint32 alpha)
{
	if (alpha >= 255)
		return opaque(color);
	uint32 inverse = 255 - alpha;
	uint32 red = (((color >> 16) & 0xff) * alpha
		+ ((destination >> 16) & 0xff) * inverse + 127) / 255;
	uint32 green = (((color >> 8) & 0xff) * alpha
		+ ((destination >> 8) & 0xff) * inverse + 127) / 255;
	uint32 blue = ((color & 0xff) * alpha + (destination & 0xff) * inverse
		+ 127) / 255;
	return 0xff000000 | (red << 16) | (green << 8) | blue;
}


//!	A glyph with its own colors (premultiplied) over what is there.
static void
render_color_glyph(const uint32* source, int32 sourcePixelsPerRow,
	uint32* bits, int32 pixelsPerRow, int32 width, int32 height,
	uint32 alpha)
{
	for (int32 y = 0; y < height; y++) {
		const uint32* from = source + (size_t)y * sourcePixelsPerRow;
		uint32* to = bits + (size_t)y * pixelsPerRow;
		for (int32 x = 0; x < width; x++) {
			uint32 pixel = from[x];
			uint32 a = pixel >> 24;
			if (a == 0)
				continue;
			// faint text makes the glyph translucent
			if (alpha < 255)
				a = a * alpha / 255;
			uint32 channels[3];
			for (int32 c = 0; c < 3; c++) {
				uint32 value = (pixel >> (8 * c)) & 0xff;
				if (alpha < 255)
					value = value * alpha / 255;
				uint32 below = (to[x] >> (8 * c)) & 0xff;
				channels[c] = std::min((uint32)255,
					value + (below * (255 - a) + 127) / 255);
			}
			to[x] = 0xff000000 | channels[0] | (channels[1] << 8)
				| (channels[2] << 16);
		}
	}
}


static void
fill_rect(uint32* bits, int32 pixelsPerRow, int32 left, int32 top,
	int32 right, int32 bottom, uint32 color)
{
	uint32 value = opaque(color);
	for (int32 y = top; y < bottom; y++) {
		uint32* row = bits + (size_t)y * pixelsPerRow;
		for (int32 x = left; x < right; x++)
			row[x] = value;
	}
}


static void
blend_rect(uint32* bits, int32 pixelsPerRow, int32 left, int32 top,
	int32 right, int32 bottom, int32 clipTop, int32 clipBottom, uint32 color,
	uint32 alpha)
{
	top = std::max(top, clipTop);
	bottom = std::min(bottom, clipBottom);
	for (int32 y = top; y < bottom; y++) {
		uint32* row = bits + (size_t)y * pixelsPerRow;
		for (int32 x = left; x < right; x++)
			row[x] = blend(row[x], color, alpha);
	}
}


void
render_decorations(const RenderFrame& frame, const RenderCell& cell,
	uint32* pixels, int32 pixelsPerRow, int32 clipTop, int32 clipBottom)
{
	// pixels points to the cell's top left
	const RenderMetrics& metrics = frame.metrics;
	int32 width = metrics.cellWidth;
	int32 height = metrics.cellHeight;
	int32 thickness = metrics.underlineThickness;
	uint32 color = cell.decoration;
	uint32 alpha = cell.foregroundAlpha;
	int32 offset = 0;
	(void)offset;

	int32 underline = cell.flags & RENDER_UNDERLINE_MASK;
	int32 top = metrics.underlinePosition;
	switch (underline) {
		case UNDERLINE_SINGLE:
			blend_rect(pixels, pixelsPerRow, 0, top, width, top + thickness,
				clipTop, clipBottom, color, alpha);
			break;
		case UNDERLINE_DOUBLE:
		{
			int32 second = top + thickness * 2;
			if (second + thickness > height) {
				second = height - thickness;
				top = std::max((int32)0, second - thickness * 2);
			}
			blend_rect(pixels, pixelsPerRow, 0, top, width, top + thickness,
				clipTop, clipBottom, color, alpha);
			blend_rect(pixels, pixelsPerRow, 0, second, width,
				second + thickness, clipTop, clipBottom, color, alpha);
			break;
		}
		case UNDERLINE_CURLY:
		{
			// one wave per cell
			float amplitude = std::max(1.0f, thickness * 1.25f);
			float center = std::min((float)top + amplitude,
				height - amplitude - thickness / 2.0f);
			float half = thickness / 2.0f + 0.25f;
			int32 first = std::max(clipTop,
				(int32)floorf(center - amplitude - half - 1));
			int32 last = std::min(clipBottom,
				(int32)ceilf(center + amplitude + half + 1));
			for (int32 x = 0; x < width; x++) {
				float wave = center + amplitude
					* sinf((x + 0.5f) * 2 * (float)M_PI / width);
				for (int32 y = first; y < last; y++) {
					float distance = fabsf(y + 0.5f - wave);
					float coverage = half + 0.5f - distance;
					if (coverage <= 0)
						continue;
					uint32 a = (uint32)(std::min(coverage, 1.0f) * alpha);
					uint32* pixel = pixels + (size_t)y * pixelsPerRow + x;
					*pixel = blend(*pixel, color, a);
				}
			}
			break;
		}
		case UNDERLINE_DOTTED:
		{
			int32 dot = std::max((int32)1, thickness);
			for (int32 x = 0; x < width; x += dot * 2) {
				blend_rect(pixels, pixelsPerRow, x,
					top, std::min(width, x + dot), top + thickness, clipTop,
					clipBottom, color, alpha);
			}
			break;
		}
		case UNDERLINE_DASHED:
		{
			int32 dash = std::max((int32)2, width / 3);
			blend_rect(pixels, pixelsPerRow, 0, top, dash, top + thickness,
				clipTop, clipBottom, color, alpha);
			blend_rect(pixels, pixelsPerRow, std::min(width, dash * 2), top,
				width, top + thickness, clipTop, clipBottom, color, alpha);
			break;
		}
	}

	if ((cell.flags & RENDER_STRIKE) != 0) {
		int32 strike = metrics.strikePosition;
		blend_rect(pixels, pixelsPerRow, 0, strike, width,
			strike + metrics.lineThickness, clipTop, clipBottom,
			cell.foreground, alpha);
	}
	if ((cell.flags & RENDER_OVERLINE) != 0) {
		blend_rect(pixels, pixelsPerRow, 0, 0, width,
			metrics.lineThickness, clipTop, clipBottom, cell.foreground,
			alpha);
	}
}


//!	A cursor that leaves its cell's colors alone: bar, underline, outline.
void
render_cursor(const RenderFrame& frame, const RenderCursor& cursor,
	uint32* bits, int32 pixelsPerRow)
{
	if (cursor.shape == RENDER_CURSOR_NONE
		|| cursor.shape == RENDER_CURSOR_BLOCK) {
		return;
	}

	const RenderMetrics& metrics = frame.metrics;
	int32 left = cursor.x * metrics.cellWidth;
	int32 top = cursor.y * metrics.cellHeight;
	int32 right = std::min(frame.width,
		left + metrics.cellWidth * cursor.width);
	int32 bottom = std::min(frame.height, top + metrics.cellHeight);
	int32 thickness = std::max((int32)2, metrics.lineThickness * 2);
	uint32 color = cursor.color;

	switch (cursor.shape) {
		case RENDER_CURSOR_UNDERLINE:
			fill_rect(bits, pixelsPerRow, left, bottom - thickness, right,
				bottom, color);
			break;
		case RENDER_CURSOR_BEAM:
			fill_rect(bits, pixelsPerRow, left, top,
				std::min(right, left + thickness), bottom, color);
			break;
		case RENDER_CURSOR_HOLLOW:
		{
			int32 line = metrics.lineThickness;
			fill_rect(bits, pixelsPerRow, left, top, right, top + line,
				color);
			fill_rect(bits, pixelsPerRow, left, bottom - line, right, bottom,
				color);
			fill_rect(bits, pixelsPerRow, left, top, left + line, bottom,
				color);
			fill_rect(bits, pixelsPerRow, right - line, top, right, bottom,
				color);
			break;
		}
	}
}


/*!	Draws the images with a z-index from \a minimumZ to \a maximumZ
	(inclusive) into the rows of pixels from \a top to \a bottom.
*/
void
render_images(const RenderFrame& frame, uint32* bits, int32 pixelsPerRow,
	int32 top, int32 bottom, int32 minimumZ, int32 maximumZ)
{
	// lower z first; the same z: lower id first
	std::vector<const ImageDraw*> draws;
	for (size_t i = 0; i < frame.images.size(); i++) {
		const ImageDraw& draw = frame.images[i];
		if (draw.z < minimumZ || draw.z > maximumZ || draw.frame == NULL)
			continue;
		if (draw.y >= bottom || draw.y + draw.height <= top)
			continue;
		draws.push_back(&draw);
	}
	std::stable_sort(draws.begin(), draws.end(),
		[](const ImageDraw* a, const ImageDraw* b) {
			return a->z < b->z || (a->z == b->z && a->imageId < b->imageId);
		});

	for (size_t i = 0; i < draws.size(); i++) {
		const ImageDraw& draw = *draws[i];
		const uint8* pixels = draw.frame->pixels.data();
		int32 imageWidth = draw.imageWidth;
		int32 imageHeight = draw.imageHeight;
		if (draw.width <= 0 || draw.height <= 0)
			continue;

		int32 firstY = std::max(top, draw.y);
		int32 lastY = std::min(bottom, draw.y + draw.height);
		int32 firstX = std::max((int32)0, draw.x);
		int32 lastX = std::min(frame.width, draw.x + draw.width);
		float scaleX = draw.sourceWidth / draw.width;
		float scaleY = draw.sourceHeight / draw.height;
		bool oneToOne = fabsf(scaleX - 1) < 0.001f
			&& fabsf(scaleY - 1) < 0.001f;

		for (int32 y = firstY; y < lastY; y++) {
			uint32* row = bits + (size_t)y * pixelsPerRow;
			float sourceY = draw.sourceY + (y - draw.y + 0.5f) * scaleY - 0.5f;
			for (int32 x = firstX; x < lastX; x++) {
				float sourceX = draw.sourceX + (x - draw.x + 0.5f) * scaleX
					- 0.5f;
				uint32 red, green, blue, alpha;
				if (oneToOne || scaleX > 1.5f || scaleY > 1.5f) {
					int32 sx = std::max((int32)0, std::min(imageWidth - 1,
						(int32)(sourceX + 0.5f)));
					int32 sy = std::max((int32)0, std::min(imageHeight - 1,
						(int32)(sourceY + 0.5f)));
					const uint8* p = pixels
						+ ((size_t)sy * imageWidth + sx) * 4;
					red = p[0];
					green = p[1];
					blue = p[2];
					alpha = p[3];
				} else {
					// bilinear
					float fx = std::max(0.0f, sourceX);
					float fy = std::max(0.0f, sourceY);
					int32 x0 = std::min((int32)fx, imageWidth - 1);
					int32 y0 = std::min((int32)fy, imageHeight - 1);
					int32 x1 = std::min(x0 + 1, imageWidth - 1);
					int32 y1 = std::min(y0 + 1, imageHeight - 1);
					float ax = fx - x0;
					float ay = fy - y0;
					const uint8* p00 = pixels
						+ ((size_t)y0 * imageWidth + x0) * 4;
					const uint8* p10 = pixels
						+ ((size_t)y0 * imageWidth + x1) * 4;
					const uint8* p01 = pixels
						+ ((size_t)y1 * imageWidth + x0) * 4;
					const uint8* p11 = pixels
						+ ((size_t)y1 * imageWidth + x1) * 4;
					float result[4];
					for (int c = 0; c < 4; c++) {
						float a = p00[c] + (p10[c] - p00[c]) * ax;
						float b = p01[c] + (p11[c] - p01[c]) * ax;
						result[c] = a + (b - a) * ay;
					}
					red = (uint32)(result[0] + 0.5f);
					green = (uint32)(result[1] + 0.5f);
					blue = (uint32)(result[2] + 0.5f);
					alpha = (uint32)(result[3] + 0.5f);
				}
				if (alpha == 0)
					continue;
				row[x] = blend(row[x], (red << 16) | (green << 8) | blue,
					alpha);
			}
		}
	}
}


//!	Draws one row of cells: backgrounds, images, text, decorations, cursor.
static void
render_row(const RenderFrame& frame, const GlyphAtlasInfo& atlas,
	uint32* bits, int32 pixelsPerRow, int32 row)
{
	const RenderMetrics& metrics = frame.metrics;
	int32 cellWidth = metrics.cellWidth;
	int32 cellHeight = metrics.cellHeight;
	int32 gridWidth = std::min(frame.width, frame.columns * cellWidth);
	int32 top = row * cellHeight;
	int32 bottom = std::min(frame.height, top + cellHeight);
	bool haveImages = !frame.images.empty();

	bool rowImages = false;
	if (haveImages) {
		for (size_t i = 0; i < frame.images.size(); i++) {
			const ImageDraw& draw = frame.images[i];
			if (draw.y < bottom && draw.y + draw.height > top) {
				rowImages = true;
				break;
			}
		}
	}

	// backgrounds
	if (rowImages) {
		fill_rect(bits, pixelsPerRow, 0, top, gridWidth, bottom,
			frame.defaultBackground);
		render_images(frame, bits, pixelsPerRow, top, bottom, INT32_MIN,
			kBelowBackgroundsZ - 1);
	}
	for (int32 column = 0; column < frame.columns; column++) {
		const RenderCell& cell = frame.Cell(row, column);
		if (rowImages && (cell.flags & RENDER_DEFAULT_BACKGROUND) != 0)
			continue;
		int32 left = column * cellWidth;
		fill_rect(bits, pixelsPerRow, left, top,
			std::min(left + cellWidth, frame.width), bottom,
			cell.background);
	}
	if (rowImages) {
		render_images(frame, bits, pixelsPerRow, top, bottom,
			kBelowBackgroundsZ, -1);
	}

	// text
	for (int32 column = 0; column < frame.columns; column++) {
		const RenderCell& cell = frame.Cell(row, column);
		int32 left = column * cellWidth;
		if (left + cellWidth > frame.width)
			break;
		uint32* cellBits = bits + (size_t)top * pixelsPerRow + left;

		if (cell.glyph != 0) {
			int32 page, slotX, slotY;
			atlas.SlotLocation(cell.glyph, page, slotX, slotY);
			if ((cell.flags & RENDER_GLYPH_RIGHT_HALF) != 0)
				slotX += cellWidth;
			if (page < atlas.pageCount
				&& (cell.flags & RENDER_COLOR_GLYPH) != 0) {
				if (atlas.colorPages != NULL
					&& atlas.colorPages[page] != NULL) {
					render_color_glyph(atlas.colorPages[page]
							+ (size_t)slotY * atlas.pageSize + slotX,
						atlas.pageSize, cellBits, pixelsPerRow, cellWidth,
						bottom - top, cell.foregroundAlpha);
				}
			} else if (page < atlas.pageCount) {
				const uint8* coverage = atlas.pages[page]
					+ (size_t)slotY * atlas.pageSize + slotX;
				uint32 color = cell.foreground;
				uint32 alpha = cell.foregroundAlpha;
				for (int32 y = 0; y < bottom - top; y++) {
					const uint8* source = coverage
						+ (size_t)y * atlas.pageSize;
					uint32* target = cellBits + (size_t)y * pixelsPerRow;
					for (int32 x = 0; x < cellWidth; x++) {
						uint32 a = source[x];
						if (a == 0)
							continue;
						target[x] = blend(target[x], color,
							alpha == 255 ? a : a * alpha / 255);
					}
				}
			}
		}

		if ((cell.flags & (RENDER_UNDERLINE_MASK | RENDER_STRIKE
				| RENDER_OVERLINE)) != 0) {
			render_decorations(frame, cell, cellBits, pixelsPerRow, 0,
				bottom - top);
		}
	}

	if (rowImages) {
		render_images(frame, bits, pixelsPerRow, top, bottom, 0,
			INT32_MAX);
	}

	if (frame.cursorY == row) {
		RenderCursor cursor = { frame.cursorX, frame.cursorY,
			frame.cursorWidth, frame.cursorShape, frame.cursorColor };
		render_cursor(frame, cursor, bits, pixelsPerRow);
	}
	for (size_t i = 0; i < frame.extraCursors.size(); i++) {
		if (frame.extraCursors[i].y == row)
			render_cursor(frame, frame.extraCursors[i], bits, pixelsPerRow);
	}
}


SoftwareRenderer::SoftwareRenderer()
	:
	fStart(-1),
	fDone(-1),
	fQuit(false),
	fFrame(NULL),
	fAtlas(NULL),
	fBits(NULL),
	fPixelsPerRow(0),
	fNextRow(0)
{
	// up to three helpers, for the frames large enough to share
	system_info info;
	int32 helpers = 0;
	if (get_system_info(&info) == B_OK)
		helpers = std::min((int32)info.cpu_count - 1, (int32)3);
	if (helpers <= 0)
		return;

	fStart = create_sem(0, "software renderer start");
	fDone = create_sem(0, "software renderer done");
	if (fStart < 0 || fDone < 0)
		return;
	for (int32 i = 0; i < helpers; i++) {
		thread_id thread = spawn_thread(&_HelperEntry, "software renderer",
			B_DISPLAY_PRIORITY, this);
		if (thread < 0)
			break;
		fHelpers.push_back(thread);
		resume_thread(thread);
	}
}


SoftwareRenderer::~SoftwareRenderer()
{
	fQuit = true;
	if (!fHelpers.empty())
		release_sem_etc(fStart, fHelpers.size(), 0);
	for (size_t i = 0; i < fHelpers.size(); i++) {
		status_t result;
		wait_for_thread(fHelpers[i], &result);
	}
	if (fStart >= 0)
		delete_sem(fStart);
	if (fDone >= 0)
		delete_sem(fDone);
}


bool
SoftwareRenderer::Render(const RenderFrame& frame,
	const GlyphAtlasInfo& atlas, uint8* bitsBytes, int32 bytesPerRow,
	int32& _firstRow, int32& _lastRow)
{
	uint32* bits = (uint32*)bitsBytes;
	int32 pixelsPerRow = bytesPerRow / 4;
	const RenderMetrics& metrics = frame.metrics;
	int32 cellWidth = metrics.cellWidth;
	int32 cellHeight = metrics.cellHeight;
	int32 gridWidth = std::min(frame.width, frame.columns * cellWidth);

	if (frame.allDirty) {
		// the margins right of and below the cells
		fill_rect(bits, pixelsPerRow, gridWidth, 0, frame.width, frame.height,
			frame.defaultBackground);
		int32 gridBottom = std::min(frame.height, frame.rows * cellHeight);
		fill_rect(bits, pixelsPerRow, 0, gridBottom, gridWidth, frame.height,
			frame.defaultBackground);
	}

	fRows.clear();
	for (int32 row = 0; row < frame.rows; row++) {
		if (row * cellHeight >= frame.height)
			break;
		if (frame.allDirty || frame.dirtyRows[row])
			fRows.push_back(row);
	}

	// Rows only draw into their own band, so helpers can take some.
	if (!fHelpers.empty()
		&& (int64)fRows.size() * cellHeight * frame.width >= kShareMinimum) {
		fFrame = &frame;
		fAtlas = &atlas;
		fBits = bits;
		fPixelsPerRow = pixelsPerRow;
		fNextRow = 0;
		release_sem_etc(fStart, fHelpers.size(), 0);
		_DrawRows();
		for (size_t i = 0; i < fHelpers.size(); i++) {
			while (acquire_sem(fDone) == B_INTERRUPTED)
				;
		}
	} else {
		for (size_t i = 0; i < fRows.size(); i++)
			render_row(frame, atlas, bits, pixelsPerRow, fRows[i]);
	}

	if (fRows.empty() && !frame.allDirty)
		return false;
	if (fRows.empty()) {
		_firstRow = 0;
		_lastRow = std::max((int32)0, frame.rows - 1);
	} else {
		_firstRow = fRows.front();
		_lastRow = fRows.back();
	}
	return true;
}


//!	Draws rows of the current frame, a few at a time, until none is left.
void
SoftwareRenderer::_DrawRows()
{
	while (true) {
		int32 index = fNextRow.fetch_add(kRowsAtOnce);
		if (index >= (int32)fRows.size())
			break;
		int32 end = std::min(index + kRowsAtOnce, (int32)fRows.size());
		for (int32 i = index; i < end; i++)
			render_row(*fFrame, *fAtlas, fBits, fPixelsPerRow, fRows[i]);
	}
}


/*static*/ status_t
SoftwareRenderer::_HelperEntry(void* data)
{
	SoftwareRenderer* renderer = (SoftwareRenderer*)data;
	while (true) {
		if (acquire_sem(renderer->fStart) == B_INTERRUPTED)
			continue;
		if (renderer->fQuit)
			return B_OK;
		renderer->_DrawRows();
		release_sem(renderer->fDone);
	}
}
