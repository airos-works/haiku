/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "ColorEmoji.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>

#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Path.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H

#include "UnicodeTables.h"


static inline uint16
read16(const uint8* table, size_t length, size_t offset)
{
	if (offset + 2 > length)
		return 0;
	return (uint16)((table[offset] << 8) | table[offset + 1]);
}


static inline uint32
read32(const uint8* table, size_t length, size_t offset)
{
	if (offset + 4 > length)
		return 0;
	return ((uint32)table[offset] << 24) | ((uint32)table[offset + 1] << 16)
		| ((uint32)table[offset + 2] << 8) | table[offset + 3];
}


static bool
is_color_font_name(const char* name)
{
	std::string lower(name);
	for (size_t i = 0; i < lower.size(); i++)
		lower[i] = tolower((unsigned char)lower[i]);
	if (lower.find(".ttf") == std::string::npos
		&& lower.find(".otf") == std::string::npos) {
		return false;
	}
	return lower.find("coloremoji") != std::string::npos
		|| lower.find("color-emoji") != std::string::npos
		|| lower.find("twemoji") != std::string::npos;
}


//!	A font file in a font directory, at most two levels down.
static bool
find_color_font(BDirectory& directory, int32 depth, BPath& _path)
{
	BEntry entry;
	while (directory.GetNextEntry(&entry) == B_OK) {
		char name[B_FILE_NAME_LENGTH];
		if (entry.GetName(name) != B_OK)
			continue;
		if (entry.IsDirectory()) {
			if (depth > 0) {
				BDirectory subdirectory(&entry);
				if (find_color_font(subdirectory, depth - 1, _path))
					return true;
			}
		} else if (is_color_font_name(name) && entry.GetPath(&_path) == B_OK)
			return true;
	}
	return false;
}


/*!	Resamples premultiplied pixels (four floats each) along one axis by
	the area each target pixel covers.
*/
static void
resample(const float* source, int32 sourceCount, int32 sourceStep,
	float* target, int32 targetCount, int32 targetStep)
{
	float ratio = (float)sourceCount / targetCount;
	for (int32 i = 0; i < targetCount; i++) {
		float start = i * ratio;
		float end = start + ratio;
		float sum[4] = { 0, 0, 0, 0 };
		for (int32 j = (int32)start; j < sourceCount && j < end; j++) {
			float weight = std::min(end, (float)(j + 1))
				- std::max(start, (float)j);
			if (weight <= 0)
				continue;
			const float* pixel = source + (size_t)j * sourceStep;
			for (int32 c = 0; c < 4; c++)
				sum[c] += pixel[c] * weight;
		}
		float* pixel = target + (size_t)i * targetStep;
		for (int32 c = 0; c < 4; c++)
			pixel[c] = sum[c] / ratio;
	}
}


ColorEmoji::ColorEmoji()
	:
	fLibrary(NULL),
	fFace(NULL),
	fLoaded(false)
{
}


ColorEmoji::~ColorEmoji()
{
	if (fFace != NULL)
		FT_Done_Face(fFace);
	if (fLibrary != NULL)
		FT_Done_FreeType(fLibrary);
}


bool
ColorEmoji::IsAvailable()
{
	return _Load();
}


/*static*/ bool
ColorEmoji::IsEmojiCluster(const uint32* codePoints, int32 count)
{
	if (count <= 0)
		return false;
	for (int32 i = 0; i < count; i++) {
		if (codePoints[i] == 0xfe0e)
			return false;
	}
	for (int32 i = 0; i < count; i++) {
		// emoji presentation, a keycap
		if (codePoints[i] == 0xfe0f || codePoints[i] == 0x20e3)
			return true;
	}
	return unicode_has_emoji_presentation(codePoints[0]);
}


uint32
ColorEmoji::GlyphFor(const uint32* codePoints, int32 count)
{
	if (!_Load())
		return 0;

	// The font's sequences leave out the variation selectors.
	std::vector<uint16> glyphs;
	for (int32 i = 0; i < count; i++) {
		if (codePoints[i] == 0xfe0f || codePoints[i] == 0xfe0e)
			continue;
		glyphs.push_back((uint16)FT_Get_Char_Index(fFace, codePoints[i]));
	}
	if (glyphs.empty() || glyphs[0] == 0)
		return 0;

	for (size_t l = 0; l < fLookups.size(); l++) {
		const LigatureLookup& lookup = fLookups[l];
		for (size_t i = 0; i < glyphs.size(); i++) {
			LigatureLookup::const_iterator found = lookup.find(glyphs[i]);
			if (found == lookup.end())
				continue;
			const std::vector<Ligature>& ligatures = found->second;
			for (size_t k = 0; k < ligatures.size(); k++) {
				const std::vector<uint16>& components
					= ligatures[k].components;
				if (i + 1 + components.size() > glyphs.size())
					continue;
				if (!std::equal(components.begin(), components.end(),
						glyphs.begin() + i + 1)) {
					continue;
				}
				glyphs[i] = ligatures[k].glyph;
				glyphs.erase(glyphs.begin() + i + 1,
					glyphs.begin() + i + 1 + components.size());
				break;
			}
		}
	}

	// a sequence the font has no glyph for shows its first emoji
	return glyphs[0];
}


bool
ColorEmoji::Draw(uint32 glyph, uint32* bits, int32 pixelsPerRow,
	int32 width, int32 height)
{
	if (!_Load() || width <= 0 || height <= 0)
		return false;

	for (int32 y = 0; y < height; y++)
		memset(bits + (size_t)y * pixelsPerRow, 0, width * 4);

	if (FT_HAS_FIXED_SIZES(fFace)) {
		// bitmaps: the smallest strike that is not smaller than the box
		int best = 0;
		for (int i = 1; i < fFace->num_fixed_sizes; i++) {
			int size = fFace->available_sizes[i].height;
			int bestSize = fFace->available_sizes[best].height;
			if ((bestSize < height && size > bestSize)
				|| (size >= height && size < bestSize)) {
				best = i;
			}
		}
		if (FT_Select_Size(fFace, best) != 0)
			return false;
	} else if (FT_Set_Pixel_Sizes(fFace, 0, height) != 0)
		return false;

	if (FT_Load_Glyph(fFace, glyph, FT_LOAD_COLOR) != 0)
		return false;
	FT_GlyphSlot slot = fFace->glyph;
	if (slot->format != FT_GLYPH_FORMAT_BITMAP
		&& FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0) {
		return false;
	}

	const FT_Bitmap& bitmap = slot->bitmap;
	int32 sourceWidth = bitmap.width;
	int32 sourceHeight = bitmap.rows;
	if (sourceWidth <= 0 || sourceHeight <= 0
		|| (bitmap.pixel_mode != FT_PIXEL_MODE_BGRA
			&& bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)) {
		return false;
	}

	std::vector<float> source((size_t)sourceWidth * sourceHeight * 4);
	for (int32 y = 0; y < sourceHeight; y++) {
		const uint8* row = bitmap.buffer + (ptrdiff_t)y * bitmap.pitch;
		float* pixel = source.data() + (size_t)y * sourceWidth * 4;
		for (int32 x = 0; x < sourceWidth; x++, pixel += 4) {
			if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
				for (int32 c = 0; c < 4; c++)
					pixel[c] = row[x * 4 + c];
			} else {
				// coverage only: black
				pixel[0] = pixel[1] = pixel[2] = 0;
				pixel[3] = row[x];
			}
		}
	}

	// fitted into the box, keeping its proportions
	float scale = std::min((float)width / sourceWidth,
		(float)height / sourceHeight);
	int32 targetWidth = std::max((int32)1,
		std::min(width, (int32)roundf(sourceWidth * scale)));
	int32 targetHeight = std::max((int32)1,
		std::min(height, (int32)roundf(sourceHeight * scale)));

	std::vector<float> narrow((size_t)targetWidth * sourceHeight * 4);
	for (int32 y = 0; y < sourceHeight; y++) {
		resample(source.data() + (size_t)y * sourceWidth * 4, sourceWidth, 4,
			narrow.data() + (size_t)y * targetWidth * 4, targetWidth, 4);
	}
	std::vector<float> target((size_t)targetWidth * targetHeight * 4);
	for (int32 x = 0; x < targetWidth; x++) {
		resample(narrow.data() + x * 4, sourceHeight, targetWidth * 4,
			target.data() + x * 4, targetHeight, targetWidth * 4);
	}

	int32 left = (width - targetWidth) / 2;
	int32 top = (height - targetHeight) / 2;
	for (int32 y = 0; y < targetHeight; y++) {
		uint32* row = bits + (size_t)(top + y) * pixelsPerRow + left;
		const float* pixel = target.data() + (size_t)y * targetWidth * 4;
		for (int32 x = 0; x < targetWidth; x++, pixel += 4) {
			uint32 value[4];
			for (int32 c = 0; c < 4; c++) {
				value[c] = (uint32)std::min(255.0f,
					std::max(0.0f, pixel[c] + 0.5f));
			}
			// premultiplied: no channel above the alpha
			for (int32 c = 0; c < 3; c++)
				value[c] = std::min(value[c], value[3]);
			row[x] = value[0] | (value[1] << 8) | (value[2] << 16)
				| (value[3] << 24);
		}
	}
	return true;
}


bool
ColorEmoji::_Load()
{
	if (fLoaded)
		return fFace != NULL;
	fLoaded = true;

	if (FT_Init_FreeType(&fLibrary) != 0) {
		fLibrary = NULL;
		return false;
	}

	const char* path = getenv("TERMINAL_EMOJI_FONT");
	if (path != NULL && path[0] != '\0')
		return _Open(path);

	const directory_which kDirectories[] = {
		B_USER_NONPACKAGED_FONTS_DIRECTORY,
		B_USER_FONTS_DIRECTORY,
		B_SYSTEM_NONPACKAGED_FONTS_DIRECTORY,
		B_SYSTEM_FONTS_DIRECTORY
	};
	for (size_t i = 0; i < sizeof(kDirectories) / sizeof(kDirectories[0]);
			i++) {
		BPath directoryPath;
		if (find_directory(kDirectories[i], &directoryPath) != B_OK)
			continue;
		BDirectory directory(directoryPath.Path());
		BPath fontPath;
		if (directory.InitCheck() == B_OK
			&& find_color_font(directory, 2, fontPath)
			&& _Open(fontPath.Path())) {
			return true;
		}
	}
	return false;
}


bool
ColorEmoji::_Open(const char* path)
{
	FT_Face face;
	if (FT_New_Face(fLibrary, path, 0, &face) != 0)
		return false;
	if (!FT_HAS_COLOR(face)) {
		FT_Done_Face(face);
		return false;
	}
	fFace = face;
	_ReadLigatures();
	return true;
}


//!	The ligatures (GSUB lookup type 4, also behind extensions) in order.
void
ColorEmoji::_ReadLigatures()
{
	FT_ULong length = 0;
	if (FT_Load_Sfnt_Table(fFace, TTAG_GSUB, 0, NULL, &length) != 0
		|| length < 10) {
		return;
	}
	std::vector<uint8> buffer(length);
	if (FT_Load_Sfnt_Table(fFace, TTAG_GSUB, 0, buffer.data(), &length) != 0)
		return;
	const uint8* table = buffer.data();

	size_t lookupList = read16(table, length, 8);
	uint16 lookupCount = read16(table, length, lookupList);
	for (uint16 i = 0; i < lookupCount; i++) {
		size_t lookup = lookupList
			+ read16(table, length, lookupList + 2 + 2 * i);
		uint16 type = read16(table, length, lookup);
		uint16 subtableCount = read16(table, length, lookup + 4);
		LigatureLookup ligatures;
		for (uint16 j = 0; j < subtableCount; j++) {
			size_t subtable = lookup
				+ read16(table, length, lookup + 6 + 2 * j);
			uint16 subtableType = type;
			if (type == 7) {
				// an extension: the real subtable is further away
				subtableType = read16(table, length, subtable + 2);
				subtable += read32(table, length, subtable + 4);
			}
			if (subtableType == 4)
				_ReadLigatureSubtable(table, length, subtable, ligatures);
		}
		if (!ligatures.empty())
			fLookups.push_back(ligatures);
	}
}


void
ColorEmoji::_ReadLigatureSubtable(const uint8* table, size_t length,
	size_t offset, LigatureLookup& lookup)
{
	if (read16(table, length, offset) != 1)
		return;

	// the coverage: the first glyph of each ligature set, in order
	std::vector<uint16> firstGlyphs;
	size_t coverage = offset + read16(table, length, offset + 2);
	uint16 format = read16(table, length, coverage);
	uint16 count = read16(table, length, coverage + 2);
	if (format == 1) {
		for (uint16 i = 0; i < count; i++)
			firstGlyphs.push_back(read16(table, length, coverage + 4 + 2 * i));
	} else if (format == 2) {
		for (uint16 i = 0; i < count; i++) {
			size_t range = coverage + 4 + 6 * i;
			uint16 start = read16(table, length, range);
			uint16 end = read16(table, length, range + 2);
			uint16 index = read16(table, length, range + 4);
			for (uint32 glyph = start; glyph <= end; glyph++) {
				size_t position = index + (glyph - start);
				if (position >= 65536)
					break;
				if (firstGlyphs.size() <= position)
					firstGlyphs.resize(position + 1);
				firstGlyphs[position] = glyph;
			}
		}
	} else
		return;

	uint16 setCount = read16(table, length, offset + 4);
	for (uint16 i = 0; i < setCount && i < firstGlyphs.size(); i++) {
		size_t set = offset + read16(table, length, offset + 6 + 2 * i);
		uint16 ligatureCount = read16(table, length, set);
		std::vector<Ligature>& ligatures = lookup[firstGlyphs[i]];
		for (uint16 j = 0; j < ligatureCount; j++) {
			size_t entry = set + read16(table, length, set + 2 + 2 * j);
			Ligature ligature;
			ligature.glyph = read16(table, length, entry);
			uint16 componentCount = read16(table, length, entry + 2);
			for (uint16 k = 1; k < componentCount; k++) {
				ligature.components.push_back(
					read16(table, length, entry + 4 + 2 * (k - 1)));
			}
			ligatures.push_back(ligature);
		}
	}
}
