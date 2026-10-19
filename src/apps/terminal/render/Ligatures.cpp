/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "Ligatures.h"

#include <ctype.h>
#include <dlfcn.h>
#include <math.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <new>

#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Font.h>
#include <Path.h>

#include <ft2build.h>
#include FT_FREETYPE_H


// The part of HarfBuzz's interface used here; its ABI is stable.
typedef struct hb_blob_t hb_blob_t;
typedef struct hb_face_t hb_face_t;
typedef struct hb_font_t hb_font_t;
typedef struct hb_buffer_t hb_buffer_t;
typedef uint32 hb_codepoint_t;

struct hb_glyph_info_t {
	hb_codepoint_t	codepoint;
	uint32			mask;
	uint32			cluster;
	uint32			var1;
	uint32			var2;
};

struct hb_glyph_position_t {
	int32			x_advance;
	int32			y_advance;
	int32			x_offset;
	int32			y_offset;
	uint32			var;
};

static const int kDirectionLeftToRight = 4;			// HB_DIRECTION_LTR
static const uint32 kScriptLatin = 0x4c61746e;		// HB_SCRIPT_LATIN


struct HarfBuzz {
	void*			image;
	hb_blob_t*		(*blob_create_from_file)(const char* path);
	void			(*blob_destroy)(hb_blob_t* blob);
	hb_face_t*		(*face_create)(hb_blob_t* blob, unsigned int index);
	void			(*face_destroy)(hb_face_t* face);
	hb_font_t*		(*font_create)(hb_face_t* face);
	void			(*font_destroy)(hb_font_t* font);
	void			(*font_set_scale)(hb_font_t* font, int x, int y);
	int				(*font_get_nominal_glyph)(hb_font_t* font,
						hb_codepoint_t unicode, hb_codepoint_t* glyph);
	hb_buffer_t*	(*buffer_create)();
	void			(*buffer_destroy)(hb_buffer_t* buffer);
	void			(*buffer_clear_contents)(hb_buffer_t* buffer);
	void			(*buffer_add_codepoints)(hb_buffer_t* buffer,
						const hb_codepoint_t* text, int length,
						unsigned int offset, int itemLength);
	void			(*buffer_set_direction)(hb_buffer_t* buffer,
						int direction);
	void			(*buffer_set_script)(hb_buffer_t* buffer, uint32 script);
	void			(*shape)(hb_font_t* font, hb_buffer_t* buffer,
						const void* features, unsigned int count);
	hb_glyph_info_t* (*buffer_get_glyph_infos)(hb_buffer_t* buffer,
						unsigned int* length);
	hb_glyph_position_t* (*buffer_get_glyph_positions)(hb_buffer_t* buffer,
						unsigned int* length);

	bool Load()
	{
		image = dlopen("libharfbuzz.so.0", RTLD_LAZY | RTLD_LOCAL);
		if (image == NULL)
			return false;
		const struct {
			void**		function;
			const char*	name;
		} kFunctions[] = {
			{ (void**)&blob_create_from_file, "hb_blob_create_from_file" },
			{ (void**)&blob_destroy, "hb_blob_destroy" },
			{ (void**)&face_create, "hb_face_create" },
			{ (void**)&face_destroy, "hb_face_destroy" },
			{ (void**)&font_create, "hb_font_create" },
			{ (void**)&font_destroy, "hb_font_destroy" },
			{ (void**)&font_set_scale, "hb_font_set_scale" },
			{ (void**)&font_get_nominal_glyph, "hb_font_get_nominal_glyph" },
			{ (void**)&buffer_create, "hb_buffer_create" },
			{ (void**)&buffer_destroy, "hb_buffer_destroy" },
			{ (void**)&buffer_clear_contents, "hb_buffer_clear_contents" },
			{ (void**)&buffer_add_codepoints, "hb_buffer_add_codepoints" },
			{ (void**)&buffer_set_direction, "hb_buffer_set_direction" },
			{ (void**)&buffer_set_script, "hb_buffer_set_script" },
			{ (void**)&shape, "hb_shape" },
			{ (void**)&buffer_get_glyph_infos, "hb_buffer_get_glyph_infos" },
			{ (void**)&buffer_get_glyph_positions,
				"hb_buffer_get_glyph_positions" },
		};
		for (size_t i = 0; i < sizeof(kFunctions) / sizeof(kFunctions[0]);
				i++) {
			*kFunctions[i].function = dlsym(image, kFunctions[i].name);
			if (*kFunctions[i].function == NULL) {
				dlclose(image);
				image = NULL;
				return false;
			}
		}
		return true;
	}
};


//	#pragma mark - font files


struct FontFile {
	std::string		path;
	int32			index;
};

static std::map<std::string, FontFile> sFontFiles;
	// by family and style; an empty path when there is none


static std::string
simplified(const char* name)
{
	std::string text;
	for (; *name != '\0'; name++) {
		if (isalnum((unsigned char)*name))
			text += tolower((unsigned char)*name);
	}
	return text;
}


static bool
is_font_file(const char* name)
{
	const char* dot = strrchr(name, '.');
	return dot != NULL && (strcasecmp(dot, ".ttf") == 0
		|| strcasecmp(dot, ".otf") == 0 || strcasecmp(dot, ".ttc") == 0
		|| strcasecmp(dot, ".otc") == 0);
}


static bool
match_font_file(FT_Library library, const char* path, const std::string& family,
	const std::string& style, FontFile& _file)
{
	FT_Face face;
	if (FT_New_Face(library, path, -1, &face) != 0)
		return false;
	FT_Long count = face->num_faces;
	FT_Done_Face(face);
	for (FT_Long i = 0; i < count; i++) {
		if (FT_New_Face(library, path, i, &face) != 0)
			continue;
		bool matches = face->family_name != NULL && face->style_name != NULL
			&& family == face->family_name && style == face->style_name;
		FT_Done_Face(face);
		if (matches) {
			_file.path = path;
			_file.index = i;
			return true;
		}
	}
	return false;
}


/*!	Looks through the font directories for the font's file: files named
	like the family first, then all of them.
*/
static bool
search_font_file(FT_Library library, BDirectory& directory, int32 depth,
	const std::string& family, const std::string& style, bool byName,
	FontFile& _file)
{
	std::string prefix = simplified(family.c_str());
	BEntry entry;
	directory.Rewind();
	while (directory.GetNextEntry(&entry, true) == B_OK) {
		char name[B_FILE_NAME_LENGTH];
		entry.GetName(name);
		if (entry.IsDirectory()) {
			BDirectory subdirectory(&entry);
			if (depth > 0 && search_font_file(library, subdirectory,
					depth - 1, family, style, byName, _file)) {
				return true;
			}
			continue;
		}
		if (!is_font_file(name)
			|| (byName && simplified(name).compare(0, prefix.size(), prefix)
				!= 0)) {
			continue;
		}
		BPath path;
		if (entry.GetPath(&path) == B_OK
			&& match_font_file(library, path.Path(), family, style, _file)) {
			return true;
		}
	}
	return false;
}


static bool
find_font_file(FT_Library library, const std::string& family,
	const std::string& style, FontFile& _file)
{
	std::string key = family + "\n" + style;
	std::map<std::string, FontFile>::iterator found = sFontFiles.find(key);
	if (found != sFontFiles.end()) {
		_file = found->second;
		return !_file.path.empty();
	}

	const directory_which kDirectories[] = {
		B_USER_NONPACKAGED_FONTS_DIRECTORY,
		B_USER_FONTS_DIRECTORY,
		B_SYSTEM_NONPACKAGED_FONTS_DIRECTORY,
		B_SYSTEM_FONTS_DIRECTORY
	};
	FontFile file;
	file.index = 0;
	bool done = false;
	for (int pass = 0; pass < 2 && !done; pass++) {
		for (size_t i = 0; i < sizeof(kDirectories) / sizeof(kDirectories[0])
				&& !done; i++) {
			BPath path;
			if (find_directory(kDirectories[i], &path) != B_OK)
				continue;
			BDirectory directory(path.Path());
			done = directory.InitCheck() == B_OK
				&& search_font_file(library, directory, 3, family, style,
					pass == 0, file);
		}
	}
	if (!done)
		file.path.clear();
	sFontFiles[key] = file;
	_file = file;
	return done;
}


//	#pragma mark - Ligatures


Ligatures::Font::Font()
	:
	size(0),
	usable(false),
	looked(false),
	face(NULL),
	hbFont(NULL)
{
}


Ligatures::Ligatures()
	:
	fHarfBuzz(NULL),
	fLibrary(NULL),
	fBuffer(NULL)
{
}


Ligatures::~Ligatures()
{
	for (int32 i = 0; i < 4; i++)
		_Close(fFonts[i]);
	if (fHarfBuzz != NULL) {
		if (fBuffer != NULL)
			fHarfBuzz->buffer_destroy((hb_buffer_t*)fBuffer);
		dlclose(fHarfBuzz->image);
		delete fHarfBuzz;
	}
	if (fLibrary != NULL)
		FT_Done_FreeType(fLibrary);
}


void
Ligatures::SetFonts(const BFont* fonts, int32 count, const bool* usable)
{
	for (int32 i = 0; i < 4 && i < count; i++) {
		Font& font = fFonts[i];
		font_family family;
		font_style style;
		fonts[i].GetFamilyAndStyle(&family, &style);
		if (font.family == family && font.style == style
			&& font.size == fonts[i].Size() && font.usable == usable[i]) {
			continue;
		}
		_Close(font);
		font.family = family;
		font.style = style;
		font.size = fonts[i].Size();
		font.usable = usable[i];
		font.looked = false;
	}
}


bool
Ligatures::IsAvailable(uint32 style)
{
	return style < 4 && _Open(fFonts[style]);
}


void
Ligatures::Shape(uint32 style, const uint32* codePoints, int32 count,
	std::vector<LigatureSegment>& segments)
{
	segments.clear();
	if (style >= 4 || count < 2)
		return;
	Font& font = fFonts[style];
	std::vector<Glyph> glyphs;
	if (!_ShapeGlyphs(font, codePoints, count, glyphs))
		return;

	// cells whose glyphs are not their characters' own, or not one each
	std::vector<int32> glyphCount(count, 0);
	std::vector<bool> changed(count, false);
	hb_font_t* hbFont = (hb_font_t*)font.hbFont;
	for (size_t i = 0; i < glyphs.size(); i++) {
		const Glyph& glyph = glyphs[i];
		if (glyph.cluster < 0 || glyph.cluster >= count)
			continue;
		glyphCount[glyph.cluster]++;
		hb_codepoint_t nominal = 0;
		fHarfBuzz->font_get_nominal_glyph(hbFont,
			codePoints[glyph.cluster], &nominal);
		if (glyph.id != nominal || glyph.xOffset != 0 || glyph.yOffset != 0)
			changed[glyph.cluster] = true;
	}
	for (int32 i = 0; i < count; i++) {
		if (glyphCount[i] == 1)
			continue;
		// none: part of a glyph made of more characters before it
		changed[i] = true;
		if (glyphCount[i] == 0 && i > 0)
			changed[i - 1] = true;
	}

	for (int32 i = 0; i < count;) {
		if (!changed[i]) {
			i++;
			continue;
		}
		LigatureSegment segment;
		segment.start = i;
		while (i < count && (changed[i] || glyphCount[i] == 0))
			i++;
		segment.length = i - segment.start;
		segments.push_back(segment);
	}
}


void
Ligatures::Draw(uint32 style, const uint32* codePoints, int32 count,
	const LigatureSegment& segment, uint8* bits, int32 bytesPerRow,
	int32 cellWidth, int32 cellHeight, int32 baseline)
{
	int32 width = segment.length * cellWidth;
	for (int32 row = 0; row < cellHeight; row++)
		memset(bits + (size_t)row * bytesPerRow, 0, width);

	if (style >= 4)
		return;
	Font& font = fFonts[style];
	std::vector<Glyph> glyphs;
	if (!_ShapeGlyphs(font, codePoints, count, glyphs))
		return;

	for (size_t i = 0; i < glyphs.size(); i++) {
		const Glyph& glyph = glyphs[i];
		if (glyph.cluster < segment.start
			|| glyph.cluster >= segment.start + segment.length) {
			continue;
		}
		if (FT_Load_Glyph(font.face, glyph.id,
				FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT) != 0
			|| FT_Render_Glyph(font.face->glyph, FT_RENDER_MODE_NORMAL)
				!= 0) {
			continue;
		}
		const FT_GlyphSlot slot = font.face->glyph;
		const FT_Bitmap& bitmap = slot->bitmap;

		// each glyph from its cell, as the font's own advances may not be
		// the cells'
		int32 left = (glyph.cluster - segment.start) * cellWidth
			+ (int32)roundf(glyph.xOffset / 64.0f) + slot->bitmap_left;
		int32 top = baseline - (int32)roundf(glyph.yOffset / 64.0f)
			- slot->bitmap_top;
		for (int32 y = 0; y < (int32)bitmap.rows; y++) {
			int32 targetY = top + y;
			if (targetY < 0 || targetY >= cellHeight)
				continue;
			const uint8* source = bitmap.buffer + (size_t)y * bitmap.pitch;
			uint8* target = bits + (size_t)targetY * bytesPerRow;
			for (int32 x = 0; x < (int32)bitmap.width; x++) {
				int32 targetX = left + x;
				if (targetX < 0 || targetX >= width)
					continue;
				uint8 value = bitmap.pixel_mode == FT_PIXEL_MODE_MONO
					? ((source[x / 8] >> (7 - x % 8)) & 1) * 255
					: source[x];
				target[targetX] = std::max(target[targetX], value);
			}
		}
	}
}


bool
Ligatures::_Open(Font& font)
{
	if (font.looked)
		return font.face != NULL;
	font.looked = true;
	if (!font.usable || font.family.empty())
		return false;

	if (fHarfBuzz == NULL) {
		fHarfBuzz = new(std::nothrow) HarfBuzz;
		if (fHarfBuzz == NULL)
			return false;
		if (!fHarfBuzz->Load()) {
			fHarfBuzz->image = NULL;
			return false;
		}
		fBuffer = fHarfBuzz->buffer_create();
	}
	if (fHarfBuzz->image == NULL || fBuffer == NULL)
		return false;
	if (fLibrary == NULL && FT_Init_FreeType(&fLibrary) != 0) {
		fLibrary = NULL;
		return false;
	}

	FontFile file;
	if (!find_font_file(fLibrary, font.family, font.style, file))
		return false;
	if (FT_New_Face(fLibrary, file.path.c_str(), file.index, &font.face)
			!= 0) {
		font.face = NULL;
		return false;
	}
	FT_Set_Char_Size(font.face, 0, (FT_F26Dot6)(font.size * 64 + 0.5f), 72,
		72);

	hb_blob_t* blob = fHarfBuzz->blob_create_from_file(file.path.c_str());
	hb_face_t* face = fHarfBuzz->face_create(blob, file.index);
	hb_font_t* hbFont = fHarfBuzz->font_create(face);
	fHarfBuzz->face_destroy(face);
	fHarfBuzz->blob_destroy(blob);
	int scale = (int)(font.size * 64 + 0.5f);
	fHarfBuzz->font_set_scale(hbFont, scale, scale);
	font.hbFont = hbFont;
	return true;
}


bool
Ligatures::_ShapeGlyphs(Font& font, const uint32* codePoints, int32 count,
	std::vector<Glyph>& glyphs)
{
	if (!_Open(font))
		return false;
	hb_buffer_t* buffer = (hb_buffer_t*)fBuffer;
	fHarfBuzz->buffer_clear_contents(buffer);
	// one character a cell: its index is its cluster
	fHarfBuzz->buffer_add_codepoints(buffer, codePoints, count, 0, count);
	fHarfBuzz->buffer_set_direction(buffer, kDirectionLeftToRight);
	fHarfBuzz->buffer_set_script(buffer, kScriptLatin);
	fHarfBuzz->shape((hb_font_t*)font.hbFont, buffer, NULL, 0);

	unsigned int length = 0;
	hb_glyph_info_t* infos = fHarfBuzz->buffer_get_glyph_infos(buffer,
		&length);
	unsigned int positionCount = 0;
	hb_glyph_position_t* positions = fHarfBuzz->buffer_get_glyph_positions(
		buffer, &positionCount);
	if (infos == NULL || positions == NULL || positionCount != length)
		return false;
	glyphs.resize(length);
	for (unsigned int i = 0; i < length; i++) {
		glyphs[i].id = infos[i].codepoint;
		glyphs[i].cluster = infos[i].cluster;
		glyphs[i].xOffset = positions[i].x_offset;
		glyphs[i].yOffset = positions[i].y_offset;
	}
	return true;
}


void
Ligatures::_Close(Font& font)
{
	if (font.hbFont != NULL && fHarfBuzz != NULL)
		fHarfBuzz->font_destroy((hb_font_t*)font.hbFont);
	font.hbFont = NULL;
	if (font.face != NULL)
		FT_Done_Face(font.face);
	font.face = NULL;
	font.looked = false;
}
