/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


//!	Box drawing and block characters, drawn to fit the cells exactly.


#include "BoxDrawing.h"

#include <math.h>
#include <string.h>

#include <algorithm>


// Box drawing U+2500 - U+257F: the weight of the arm going up, right, down
// and left: 0 none, 1 light, 2 heavy, 3 double. Dashed lines, arcs and
// diagonals are marked separately.
static const char* const kBoxArms[] = {
	"0101", "0202", "1010", "2020", "0101", "0202", "1010", "2020",	// 2500
	"0101", "0202", "1010", "2020", "0110", "0210", "0120", "0220",	// 2508
	"0011", "0012", "0021", "0022", "1100", "1200", "2100", "2200",	// 2510
	"1001", "1002", "2001", "2002", "1110", "1210", "2110", "1120",	// 2518
	"2120", "2210", "1220", "2220", "1011", "1012", "2011", "1021",	// 2520
	"2021", "2012", "1022", "2022", "0111", "0112", "0211", "0212",	// 2528
	"0121", "0122", "0221", "0222", "1101", "1102", "1201", "1202",	// 2530
	"2101", "2102", "2201", "2202", "1111", "1112", "1211", "1212",	// 2538
	"2111", "1121", "2121", "2112", "2211", "1122", "1221", "2212",	// 2540
	"1222", "2122", "2221", "2222", "0101", "0202", "1010", "2020",	// 2548
	"0303", "3030", "0310", "0130", "0330", "0013", "0031", "0033",	// 2550
	"1300", "3100", "3300", "1003", "3001", "3003", "1310", "3130",	// 2558
	"3330", "1013", "3031", "3033", "0313", "0131", "0333", "1303",	// 2560
	"3101", "3303", "1313", "3131", "3333", "0110", "0011", "1001",	// 2568
	"1100", "0000", "0000", "0000", "0001", "1000", "0100", "0010",	// 2570
	"0002", "2000", "0200", "0020", "0201", "1020", "0102", "2010"	// 2578
};


static inline void
fill(uint8* bits, int32 bytesPerRow, int32 width, int32 height, int32 left,
	int32 top, int32 right, int32 bottom, uint8 value = 255)
{
	left = std::max(left, (int32)0);
	top = std::max(top, (int32)0);
	right = std::min(right, width);
	bottom = std::min(bottom, height);
	for (int32 y = top; y < bottom; y++) {
		uint8* row = bits + y * bytesPerRow;
		for (int32 x = left; x < right; x++)
			row[x] = std::max(row[x], value);
	}
}


/*!	Fills a rectangle given in fractions of pixels, with partial coverage
	at its edges.
*/
static void
fill_smooth(uint8* bits, int32 bytesPerRow, int32 width, int32 height,
	float left, float top, float right, float bottom, float strength = 1.0f)
{
	int32 firstX = std::max((int32)floorf(left), (int32)0);
	int32 lastX = std::min((int32)ceilf(right), width);
	int32 firstY = std::max((int32)floorf(top), (int32)0);
	int32 lastY = std::min((int32)ceilf(bottom), height);
	for (int32 y = firstY; y < lastY; y++) {
		float coverY = std::min(bottom, y + 1.0f) - std::max(top, (float)y);
		if (coverY <= 0)
			continue;
		uint8* row = bits + y * bytesPerRow;
		for (int32 x = firstX; x < lastX; x++) {
			float coverX = std::min(right, x + 1.0f)
				- std::max(left, (float)x);
			if (coverX <= 0)
				continue;
			int32 value = (int32)(coverX * coverY * strength * 255 + 0.5f);
			row[x] = std::max((int32)row[x], std::min(value, (int32)255));
		}
	}
}


/*!	Draws a shape given as an inside test, sampled 4 x 4 times per pixel.
*/
template<typename Inside>
static void
fill_shape(uint8* bits, int32 bytesPerRow, int32 width, int32 height,
	Inside inside)
{
	for (int32 y = 0; y < height; y++) {
		uint8* row = bits + y * bytesPerRow;
		for (int32 x = 0; x < width; x++) {
			int32 count = 0;
			for (int32 sy = 0; sy < 4; sy++) {
				for (int32 sx = 0; sx < 4; sx++) {
					if (inside(x + (sx + 0.5f) / 4, y + (sy + 0.5f) / 4))
						count++;
				}
			}
			if (count > 0)
				row[x] = std::max((int32)row[x], count * 255 / 16);
		}
	}
}


/*!	Draws lines from the cell edges to its middle. Double lines are drawn
	as thick bands with their middle channel taken out again, which gives
	the right inner and outer corners where they meet; single lines are
	drawn on top, so they cross double ones.
*/
static void
draw_lines(uint8* bits, int32 bytesPerRow, int32 width, int32 height,
	int32 lineWidth, const char* arms, int32 dashes)
{
	int32 light = lineWidth;
	int32 heavy = lineWidth * 2 + (lineWidth == 1 ? 1 : 0);
	int32 gap = std::max((int32)1, lineWidth);
	int32 doubleWidth = light * 2 + gap;

	int32 weights[4];	// up, right, down, left
	for (int32 i = 0; i < 4; i++)
		weights[i] = arms[i] - '0';

	int32 centerX = width / 2;
	int32 centerY = height / 2;

	struct Thickness {
		static int32 Of(int32 weight, int32 light, int32 heavy,
			int32 doubleWidth)
		{
			return weight == 1 ? light : weight == 2 ? heavy
				: weight == 3 ? doubleWidth : 0;
		}
	};

	int32 horizontal = std::max(
		Thickness::Of(weights[1], light, heavy, doubleWidth),
		Thickness::Of(weights[3], light, heavy, doubleWidth));
	int32 vertical = std::max(
		Thickness::Of(weights[0], light, heavy, doubleWidth),
		Thickness::Of(weights[2], light, heavy, doubleWidth));

	// a temporary cell for the double bands, so their channels can be
	// cleared without touching anything else
	uint8 bands[width * height];
	memset(bands, 0, sizeof(bands));
	bool hasDouble = false;

	for (int32 pass = 0; pass < 2; pass++) {
		for (int32 i = 0; i < 4; i++) {
			int32 weight = weights[i];
			if (weight == 0 || (weight == 3) != (pass == 0))
				continue;

			bool isVertical = i == 0 || i == 2;
			int32 thickness = Thickness::Of(weight, light, heavy,
				doubleWidth);
			// how far into the middle the arm reaches: across the
			// perpendicular lines, or to the middle if there are none
			int32 across = isVertical ? horizontal : vertical;
			int32 reachStart = (isVertical ? centerY : centerX) - across / 2;
			int32 reachEnd = reachStart + across;
			if (across == 0) {
				reachStart = (isVertical ? centerY : centerX)
					- thickness / 2;
				reachEnd = reachStart + thickness;
			}

			int32 lineStart = (isVertical ? centerX : centerY)
				- thickness / 2;
			int32 lineEnd = lineStart + thickness;
			int32 from = (i == 0 || i == 3) ? 0 : reachStart;
			int32 to = (i == 0 || i == 3) ? reachEnd
				: (isVertical ? height : width);

			uint8* target = weight == 3 ? bands : bits;
			int32 stride = weight == 3 ? width : bytesPerRow;
			if (weight == 3)
				hasDouble = true;

			int32 length = isVertical ? height : width;
			int32 segments = dashes > 0 ? dashes : 1;
			int32 segment = length / segments;
			for (int32 d = 0; d < segments; d++) {
				int32 a = from;
				int32 b = to;
				if (dashes > 0) {
					a = std::max(from, d * segment);
					b = std::min(to, d * segment + segment * 2 / 3);
				}
				if (isVertical) {
					fill(target, stride, width, height, lineStart, a,
						lineEnd, b);
				} else {
					fill(target, stride, width, height, a, lineStart, b,
						lineEnd);
				}
			}
		}

		if (pass == 0 && hasDouble) {
			// take out the channels in the middle of the double lines
			for (int32 i = 0; i < 4; i++) {
				if (weights[i] != 3)
					continue;
				bool isVertical = i == 0 || i == 2;
				int32 channelStart = (isVertical ? centerX : centerY)
					- doubleWidth / 2 + light;
				int32 channelEnd = channelStart + gap;
				int32 middle = (isVertical ? centerY : centerX)
					- doubleWidth / 2 + light;
				int32 from = (i == 0 || i == 3) ? 0 : middle;
				int32 to = (i == 0 || i == 3) ? middle + gap
					: (isVertical ? height : width);
				bool straight = weights[(i + 2) % 4] == 3
					&& weights[(i + 1) % 4] == 0
					&& weights[(i + 3) % 4] == 0;
				if (straight) {
					from = 0;
					to = isVertical ? height : width;
				}
				for (int32 y = isVertical ? from : channelStart;
						y < (isVertical ? to : channelEnd); y++) {
					if (y < 0 || y >= height)
						continue;
					for (int32 x = isVertical ? channelStart : from;
							x < (isVertical ? channelEnd : to); x++) {
						if (x >= 0 && x < width)
							bands[y * width + x] = 0;
					}
				}
			}
			for (int32 y = 0; y < height; y++) {
				uint8* row = bits + y * bytesPerRow;
				for (int32 x = 0; x < width; x++)
					row[x] = std::max(row[x], bands[y * width + x]);
			}
		}
	}
}


bool
is_box_character(uint32 c)
{
	return (c >= 0x2500 && c <= 0x259f)
		|| (c >= 0x2800 && c <= 0x28ff)
		|| (c >= 0xe0b0 && c <= 0xe0bf)
		|| (c >= 0x1fb00 && c <= 0x1fb3b);
}


bool
draw_box_character(uint32 c, uint8* bits, int32 bytesPerRow, int32 width,
	int32 height, int32 lineWidth)
{
	if (!is_box_character(c))
		return false;

	lineWidth = std::max((int32)1, lineWidth);
	float w = width;
	float h = height;

	// lines and junctions
	if (c >= 0x2500 && c <= 0x257f) {
		int32 index = c - 0x2500;
		int32 dashes = 0;
		switch (c) {
			case 0x2504: case 0x2505: case 0x2506: case 0x2507:
				dashes = 3;
				break;
			case 0x2508: case 0x2509: case 0x250a: case 0x250b:
				dashes = 4;
				break;
			case 0x254c: case 0x254d: case 0x254e: case 0x254f:
				dashes = 2;
				break;
		}

		if (c >= 0x256d && c <= 0x2570) {
			// rounded corners: a quarter circle joining two arms
			float cx = (int32)(width / 2) + (lineWidth % 2) * 0.5f;
			float cy = (int32)(height / 2) + (lineWidth % 2) * 0.5f;
			float half = lineWidth / 2.0f;
			float radius = std::min(w, h) / 2;
			bool right = c == 0x256d || c == 0x2570;
			bool down = c == 0x256d || c == 0x256e;
			float centerX = right ? cx + radius : cx - radius;
			float centerY = down ? cy + radius : cy - radius;
			fill_shape(bits, bytesPerRow, width, height,
				[=](float x, float y) {
					bool inQuadrant = (right ? x <= centerX : x >= centerX)
						&& (down ? y <= centerY : y >= centerY);
					if (inQuadrant) {
						float d = hypotf(x - centerX, y - centerY);
						return fabsf(d - radius) <= half;
					}
					// the straight continuations to the cell edges
					if (right && x >= centerX && fabsf(y - cy) <= half)
						return true;
					if (!right && x <= centerX && fabsf(y - cy) <= half)
						return true;
					if (down && y >= centerY && fabsf(x - cx) <= half)
						return true;
					if (!down && y <= centerY && fabsf(x - cx) <= half)
						return true;
					return false;
				});
			return true;
		}

		if (c >= 0x2571 && c <= 0x2573) {
			float half = lineWidth * 0.6f;
			bool rising = c == 0x2571 || c == 0x2573;
			bool falling = c == 0x2572 || c == 0x2573;
			float length = hypotf(w, h);
			fill_shape(bits, bytesPerRow, width, height,
				[=](float x, float y) {
					if (rising && fabsf(x * h + y * w - w * h) / length <= half)
						return true;
					if (falling && fabsf(x * h - y * w) / length <= half)
						return true;
					return false;
				});
			return true;
		}

		draw_lines(bits, bytesPerRow, width, height, lineWidth,
			kBoxArms[index], dashes);
		return true;
	}

	// block elements
	if (c >= 0x2580 && c <= 0x259f) {
		switch (c) {
			case 0x2580:
				fill_smooth(bits, bytesPerRow, width, height, 0, 0, w, h / 2);
				return true;
			case 0x2581: case 0x2582: case 0x2583: case 0x2584:
			case 0x2585: case 0x2586: case 0x2587: case 0x2588:
				fill_smooth(bits, bytesPerRow, width, height, 0,
					h - h * (c - 0x2580) / 8, w, h);
				return true;
			case 0x2589: case 0x258a: case 0x258b: case 0x258c:
			case 0x258d: case 0x258e: case 0x258f:
				fill_smooth(bits, bytesPerRow, width, height, 0, 0,
					w * (0x2590 - c) / 8, h);
				return true;
			case 0x2590:
				fill_smooth(bits, bytesPerRow, width, height, w / 2, 0, w, h);
				return true;
			case 0x2591: case 0x2592: case 0x2593:
				fill(bits, bytesPerRow, width, height, 0, 0, width, height,
					(c - 0x2590) * 64);
				return true;
			case 0x2594:
				fill_smooth(bits, bytesPerRow, width, height, 0, 0, w, h / 8);
				return true;
			case 0x2595:
				fill_smooth(bits, bytesPerRow, width, height, w * 7 / 8, 0, w,
					h);
				return true;
		}
		// quadrants: bit 0 upper left, 1 upper right, 2 lower left,
		// 3 lower right
		static const uint8 kQuadrants[] = {
			0x4, 0x8, 0x1, 0xd, 0x9, 0x7, 0xb, 0x2, 0x6, 0xe
		};
		uint8 quadrants = kQuadrants[c - 0x2596];
		if (quadrants & 1)
			fill_smooth(bits, bytesPerRow, width, height, 0, 0, w / 2, h / 2);
		if (quadrants & 2)
			fill_smooth(bits, bytesPerRow, width, height, w / 2, 0, w, h / 2);
		if (quadrants & 4)
			fill_smooth(bits, bytesPerRow, width, height, 0, h / 2, w / 2, h);
		if (quadrants & 8)
			fill_smooth(bits, bytesPerRow, width, height, w / 2, h / 2, w, h);
		return true;
	}

	// sextants: 2 x 3 blocks, all combinations but those of other blocks
	if (c >= 0x1fb00 && c <= 0x1fb3b) {
		int32 value = c - 0x1fb00 + 1;
		if (value >= 21)
			value++;
		if (value >= 42)
			value++;
		for (int32 bit = 0; bit < 6; bit++) {
			if ((value & (1 << bit)) == 0)
				continue;
			float left = (bit % 2) * w / 2;
			float top = (bit / 2) * h / 3;
			fill_smooth(bits, bytesPerRow, width, height, left, top,
				left + w / 2, top + h / 3);
		}
		return true;
	}

	// braille: 2 x 4 dots
	if (c >= 0x2800 && c <= 0x28ff) {
		uint32 dots = c - 0x2800;
		static const int32 kColumn[] = { 0, 0, 0, 1, 1, 1, 0, 1 };
		static const int32 kRow[] = { 0, 1, 2, 0, 1, 2, 3, 3 };
		float cellW = w / 2;
		float cellH = h / 4;
		float radius = std::max(1.0f, std::min(cellW, cellH) * 0.3f);
		for (int32 dot = 0; dot < 8; dot++) {
			if ((dots & (1 << dot)) == 0)
				continue;
			float centerX = (kColumn[dot] + 0.5f) * cellW;
			float centerY = (kRow[dot] + 0.5f) * cellH;
			fill_shape(bits, bytesPerRow, width, height,
				[=](float x, float y) {
					return hypotf(x - centerX, y - centerY) <= radius;
				});
		}
		return true;
	}

	// powerline symbols
	if (c >= 0xe0b0 && c <= 0xe0bf) {
		float half = lineWidth * 0.6f;
		switch (c) {
			case 0xe0b0:	// solid triangle pointing right
				fill_shape(bits, bytesPerRow, width, height,
					[=](float x, float y) {
						return x <= w * (1 - fabsf(y - h / 2) / (h / 2));
					});
				return true;
			case 0xe0b2:	// solid triangle pointing left
				fill_shape(bits, bytesPerRow, width, height,
					[=](float x, float y) {
						return x >= w * fabsf(y - h / 2) / (h / 2);
					});
				return true;
			case 0xe0b1:	// chevron right
			case 0xe0b3:	// chevron left
			{
				bool right = c == 0xe0b1;
				float length = hypotf(w, h / 2);
				fill_shape(bits, bytesPerRow, width, height,
					[=](float x, float y) {
						float px = right ? x : w - x;
						float py = y < h / 2 ? y : h - y;
						// the line from (0, 0) to (w, h / 2)
						float distance = fabsf(px * (h / 2) - py * w) / length;
						return distance <= half;
					});
				return true;
			}
			case 0xe0b4:	// solid half circle, right
			case 0xe0b6:	// solid half circle, left
			{
				bool right = c == 0xe0b4;
				fill_shape(bits, bytesPerRow, width, height,
					[=](float x, float y) {
						float dx = (right ? x : w - x) / w;
						float dy = (y - h / 2) / (h / 2);
						return dx * dx + dy * dy <= 1;
					});
				return true;
			}
			case 0xe0b5:
			case 0xe0b7:
			{
				bool right = c == 0xe0b5;
				fill_shape(bits, bytesPerRow, width, height,
					[=](float x, float y) {
						float dx = (right ? x : w - x);
						float dy = (y - h / 2) * w / (h / 2);
						float d = hypotf(dx, dy);
						return fabsf(d - (w - half)) <= half;
					});
				return true;
			}
			case 0xe0b8:	// lower left triangle
				fill_shape(bits, bytesPerRow, width, height,
					[=](float x, float y) { return x / w <= y / h; });
				return true;
			case 0xe0ba:	// lower right triangle
				fill_shape(bits, bytesPerRow, width, height,
					[=](float x, float y) { return (w - x) / w <= y / h; });
				return true;
			case 0xe0bc:	// upper left triangle
				fill_shape(bits, bytesPerRow, width, height,
					[=](float x, float y) { return x / w + y / h <= 1; });
				return true;
			case 0xe0be:	// upper right triangle
				fill_shape(bits, bytesPerRow, width, height,
					[=](float x, float y) { return y / h <= x / w; });
				return true;
		}
		return false;
	}

	return false;
}
