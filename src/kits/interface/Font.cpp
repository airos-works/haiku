/*
 * Copyright 2001-2015, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		DarkWyrm <bpmagic@columbus.rr.com>
 *		Jérôme Duval, jerome.duval@free.fr
 *		Axel Dörfler, axeld@pinc-software.de
 *		Stephan Aßmus <superstippi@gmx.de>
 *		Andrej Spielmann, <andrej.spielmann@seh.ox.ac.uk>
 */


#include <AppServerLink.h>
#include <ApplicationPrivate.h>
#include <EmojiTables.h>
#include <FontPrivate.h>
#include <ObjectList.h>
#include <ServerProtocol.h>
#include <ServerReadOnlyMemory.h>
#include <StackOrHeapArray.h>
#include <truncate_string.h>
#include <utf8_functions.h>

#include <Application.h>
#include <Autolock.h>
#include <Font.h>
#include <ServerProtocolStructs.h>
	// (needs Font.h)
#include <Locker.h>
#include <Message.h>
#include <PortLink.h>
#include <Rect.h>
#include <Shape.h>
#include <String.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>


using namespace std;

const float kUninitializedAscent = INFINITY;
const uint32 kUninitializedExtraFlags = 0xffffffff;

// The actual objects which the globals point to
static BFont sPlainFont;
static BFont sBoldFont;
static BFont sFixedFont;

const BFont* be_plain_font = &sPlainFont;
const BFont* be_bold_font = &sBoldFont;
const BFont* be_fixed_font = &sFixedFont;


struct style {
	BString	name;
	uint16	face;
	uint32	flags;
};

struct family {
	BString	name;
	uint32	flags;
	BObjectList<style> styles;
};

namespace {

class FontList : public BLocker {
public:
								FontList();
	virtual						~FontList();

	static	FontList*			Default();

			bool				UpdatedOnServer();

			status_t			FamilyAt(int32 index, font_family* _family,
									uint32* _flags);
			status_t			StyleAt(font_family family, int32 index,
									font_style* _style, uint16* _face,
									uint32* _flags);

			int32				CountFamilies();
			int32				CountStyles(font_family family);

private:
			status_t			_UpdateIfNecessary();
			status_t			_Update();
			uint32				_RevisionOnServer();
			family*				_FindFamily(font_family name);
	static	void				_InitSingleton();

private:
			BObjectList<family>	fFamilies;
			family*				fLastFamily;
			bigtime_t			fLastUpdate;
			uint32				fRevision;

	static	pthread_once_t		sDefaultInitOnce;
	static	FontList*			sDefaultInstance;
};

pthread_once_t FontList::sDefaultInitOnce = PTHREAD_ONCE_INIT;
FontList* FontList::sDefaultInstance = NULL;

}	// unnamed namespace


//	#pragma mark -


static int
compare_families(const family* a, const family* b)
{
	// TODO: compare font names according to the user's locale settings
	return strcmp(a->name.String(), b->name.String());
}


namespace {

FontList::FontList()
	: BLocker("font list"),
	fLastFamily(NULL),
	fLastUpdate(0),
	fRevision(0)
{
}


FontList::~FontList()
{
}


/*static*/ FontList*
FontList::Default()
{
	if (sDefaultInstance == NULL)
		pthread_once(&sDefaultInitOnce, &_InitSingleton);

	return sDefaultInstance;
}


bool
FontList::UpdatedOnServer()
{
	return _RevisionOnServer() != fRevision;
}


status_t
FontList::FamilyAt(int32 index, font_family* _family, uint32* _flags)
{
	BAutolock locker(this);

	status_t status = _UpdateIfNecessary();
	if (status < B_OK)
		return status;

	::family* family = fFamilies.ItemAt(index);
	if (family == NULL)
		return B_BAD_VALUE;

	memcpy(*_family, family->name.String(), family->name.Length() + 1);
	if (_flags)
		*_flags = family->flags;
	return B_OK;
}


status_t
FontList::StyleAt(font_family familyName, int32 index, font_style* _style,
	uint16* _face, uint32* _flags)
{
	BAutolock locker(this);

	status_t status = _UpdateIfNecessary();
	if (status < B_OK)
		return status;

	::family* family = _FindFamily(familyName);
	if (family == NULL)
		return B_BAD_VALUE;

	::style* style = family->styles.ItemAt(index);
	if (style == NULL)
		return B_BAD_VALUE;

	memcpy(*_style, style->name.String(), style->name.Length() + 1);
	if (_face)
		*_face = style->face;
	if (_flags)
		*_flags = style->flags;
	return B_OK;
}


int32
FontList::CountFamilies()
{
	BAutolock locker(this);

	_UpdateIfNecessary();
	return fFamilies.CountItems();
}


int32
FontList::CountStyles(font_family familyName)
{
	BAutolock locker(this);

	_UpdateIfNecessary();

	::family* family = _FindFamily(familyName);
	if (family == NULL)
		return 0;

	return family->styles.CountItems();
}


status_t
FontList::_Update()
{
	// check version

	uint32 revision = _RevisionOnServer();
	fLastUpdate = system_time();

	// are we up-to-date already?
	if (revision == fRevision)
		return B_OK;

	fFamilies.MakeEmpty();
	fLastFamily = NULL;

	BPrivate::AppServerLink link;

	for (int32 index = 0;; index++) {
		link.StartMessage(AS_GET_FAMILY_AND_STYLES);
		link.Attach<int32>(index);

		int32 status;
		if (link.FlushWithReply(status) != B_OK
			|| status != B_OK)
			break;

		::family* family = new (nothrow) ::family;
		if (family == NULL)
			return B_NO_MEMORY;

		link.ReadString(family->name);
		link.Read<uint32>(&family->flags);

		int32 styleCount;
		link.Read<int32>(&styleCount);

		for (int32 i = 0; i < styleCount; i++) {
			::style* style = new (nothrow) ::style;
			if (style == NULL) {
				delete family;
				return B_NO_MEMORY;
			}

			link.ReadString(style->name);
			link.Read<uint16>(&style->face);
			link.Read<uint32>(&style->flags);

			family->styles.AddItem(style);
		}

		fFamilies.BinaryInsert(family, compare_families);
	}

	fRevision = revision;

	// if the font list has been changed in the mean time, just update again
	if (UpdatedOnServer())
		_Update();

	return B_OK;
}


status_t
FontList::_UpdateIfNecessary()
{
	// an updated font list is at least valid for 1 second
	if (fLastUpdate > system_time() - 1000000)
		return B_OK;

	return _Update();
}


uint32
FontList::_RevisionOnServer()
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_FONT_LIST_REVISION);

	int32 code;
	if (link.FlushWithReply(code) != B_OK || code != B_OK) {
		// Go on as if our list is up to date. If it is a one-time thing
		// we'll try again soon. If it isn't, we have a bigger problem.
		return fRevision;
	}

	uint32 revision;
	link.Read<uint32>(&revision);

	return revision;
}


family*
FontList::_FindFamily(font_family name)
{
	if (fLastFamily != NULL && fLastFamily->name == name)
		return fLastFamily;

	::family family;
	family.name = name;
	fLastFamily = const_cast< ::family*>(fFamilies.BinarySearch(family,
		compare_families));
	return fLastFamily;
}


/*static*/ void
FontList::_InitSingleton()
{
	sDefaultInstance = new FontList;
}

}	// unnamed namespace


//	#pragma mark -


void
_init_global_fonts_()
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_SYSTEM_FONTS);

	int32 code;
	if (link.FlushWithReply(code) != B_OK
		|| code != B_OK) {
		printf("DEBUG: Couldn't initialize global fonts!\n");
		return;
	}

	char type[B_OS_NAME_LENGTH];

	while (link.ReadString(type, sizeof(type)) >= B_OK && type[0]) {
		BFont dummy;
		BFont* font = &dummy;

		if (!strcmp(type, "plain"))
			font = &sPlainFont;
		else if (!strcmp(type, "bold"))
			font = &sBoldFont;
		else if (!strcmp(type, "fixed"))
			font = &sFixedFont;

		link.Read<uint16>(&font->fFamilyID);
		link.Read<uint16>(&font->fStyleID);
		link.Read<float>(&font->fSize);
		link.Read<uint16>(&font->fFace);
		link.Read<uint32>(&font->fFlags);

		font->fHeight.ascent = kUninitializedAscent;
		font->fExtraFlags = kUninitializedExtraFlags;
	}
}


void
_font_control_(BFont* font, int32 cmd, void* data)
{
}


status_t
get_font_cache_info(uint32 id, void* set)
{
	return B_ERROR;
}


status_t
set_font_cache_info(uint32 id, void* set)
{
	return B_ERROR;
}


// Private function used to replace the R5 hack which sets a system font
void
_set_system_font_(const char* which, font_family family, font_style style,
	float size)
{
	// R5 used a global area offset table to set the system fonts in the Font
	// preferences panel. Bleah.
	BPrivate::AppServerLink link;

	link.StartMessage(AS_SET_SYSTEM_FONT);
	link.AttachString(which, B_OS_NAME_LENGTH);
	link.AttachString(family, sizeof(font_family));
	link.AttachString(style, sizeof(font_style));
	link.Attach<float>(size);
	link.Flush();
}


status_t
_get_system_default_font_(const char* which, font_family family,
	font_style style, float* _size)
{
	BPrivate::AppServerLink link;

	link.StartMessage(AS_GET_SYSTEM_DEFAULT_FONT);
	link.AttachString(which, B_OS_NAME_LENGTH);

	int32 status = B_ERROR;
	if (link.FlushWithReply(status) != B_OK
		|| status < B_OK)
		return status;

	link.ReadString(family, sizeof(font_family));
	link.ReadString(style, sizeof(font_style));
	link.Read<float>(_size);
	return B_OK;
}


// Returns the number of installed font families
int32
count_font_families()
{
	return FontList::Default()->CountFamilies();
}


// Returns the number of styles available for a font family
int32
count_font_styles(font_family family)
{
	return FontList::Default()->CountStyles(family);
}


// Retrieves the family name at the specified index
status_t
get_font_family(int32 index, font_family* _name, uint32* _flags)
{
	if (_name == NULL)
		return B_BAD_VALUE;

	return FontList::Default()->FamilyAt(index, _name, _flags);
}


// Retrieves the family name at the specified index
status_t
get_font_style(font_family family, int32 index, font_style* _name,
	uint32* _flags)
{
	return get_font_style(family, index, _name, NULL, _flags);
}


// Retrieves the family name at the specified index
status_t
get_font_style(font_family family, int32 index, font_style* _name,
	uint16* _face, uint32* _flags)
{
	// The face value returned by this function is not very reliable. At the
	// same time, the value returned should be fairly reliable, returning the
	// proper flag for 90%-99% of font names.

	if (_name == NULL)
		return B_BAD_VALUE;

	return FontList::Default()->StyleAt(family, index, _name, _face, _flags);
}


// Updates the font family list
bool
update_font_families(bool /*checkOnly*/)
{
	return FontList::Default()->UpdatedOnServer();
}


//	#pragma mark -


BFont::BFont()
	:
	// initialise for be_plain_font (avoid circular definition)
	fFamilyID(0),
	fStyleID(0),
	fSize(10.0),
	fShear(90.0),
	fRotation(0.0),
	fFalseBoldWidth(0.0),
	fSpacing(B_BITMAP_SPACING),
	fEncoding(B_UNICODE_UTF8),
	fFace(0),
	fFlags(0),
	fExtraFlags(kUninitializedExtraFlags)
{
	if (be_plain_font != NULL && this != &sPlainFont)
		*this = *be_plain_font;
	else {
		fHeight.ascent = 7.0;
		fHeight.descent = 2.0;
		fHeight.leading = 13.0;
	}
}


BFont::BFont(const BFont& font)
{
	*this = font;
}


BFont::BFont(const BFont* font)
{
	if (font != NULL)
		*this = *font;
	else
		*this = *be_plain_font;
}


// Sets the font's family and style all at once
status_t
BFont::SetFamilyAndStyle(const font_family family, const font_style style)
{
	if (family == NULL && style == NULL)
		return B_BAD_VALUE;

	BPrivate::AppServerLink link;

	link.StartMessage(AS_GET_FAMILY_AND_STYLE_IDS);
	link.AttachString(family, sizeof(font_family));
	link.AttachString(style, sizeof(font_style));
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(0xffff);
	link.Attach<uint16>(fFace);

	int32 status = B_ERROR;
	if (link.FlushWithReply(status) != B_OK || status != B_OK)
		return status;

	link.Read<uint16>(&fFamilyID);
	link.Read<uint16>(&fStyleID);
	link.Read<uint16>(&fFace);

	fHeight.ascent = kUninitializedAscent;
	fExtraFlags = kUninitializedExtraFlags;

	return B_OK;
}


// Sets the font's family and style all at once
void
BFont::SetFamilyAndStyle(uint32 code)
{
	// R5 has a bug here: the face is not updated even though the IDs are set.
	// This is a problem because the face flag includes Regular/Bold/Italic
	// information in addition to stuff like underlining and strikethrough.
	// As a result, this will need a trip to the server and, thus, be slower
	// than R5's in order to be correct

	uint16 family, style;
	style = code & 0xFFFF;
	family = (code & 0xFFFF0000) >> 16;

	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_FAMILY_AND_STYLE_IDS);
	link.AttachString(NULL);	// no family and style name
	link.AttachString(NULL);
	link.Attach<uint16>(family);
	link.Attach<uint16>(style);
	link.Attach<uint16>(fFace);

	int32 fontcode;
	if (link.FlushWithReply(fontcode) != B_OK || fontcode != B_OK)
		return;

	link.Read<uint16>(&fFamilyID);
	link.Read<uint16>(&fStyleID);
	link.Read<uint16>(&fFace);
	fHeight.ascent = kUninitializedAscent;
	fExtraFlags = kUninitializedExtraFlags;
}


// Sets the font's family and face all at once
status_t
BFont::SetFamilyAndFace(const font_family family, uint16 face)
{
	// To comply with the BeBook, this function will only set valid values
	// i.e. passing a nonexistent family will cause only the face to be set.
	// Additionally, if a particular  face does not exist in a family, the
	// closest match will be chosen.

	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_FAMILY_AND_STYLE_IDS);
	link.AttachString(family, sizeof(font_family));
	link.AttachString(NULL);	// no style given
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(0xffff);
	link.Attach<uint16>(face);

	int32 status = B_ERROR;
	if (link.FlushWithReply(status) != B_OK || status != B_OK)
		return status;

	link.Read<uint16>(&fFamilyID);
	link.Read<uint16>(&fStyleID);
	link.Read<uint16>(&fFace);
	fHeight.ascent = kUninitializedAscent;
	fExtraFlags = kUninitializedExtraFlags;

	return B_OK;
}


void
BFont::SetSize(float size)
{
	fSize = size;
	fHeight.ascent = kUninitializedAscent;
}


void
BFont::SetShear(float shear)
{
	fShear = shear;
	fHeight.ascent = kUninitializedAscent;
}


void
BFont::SetRotation(float rotation)
{
	fRotation = rotation;
	fHeight.ascent = kUninitializedAscent;
}


void
BFont::SetFalseBoldWidth(float width)
{
	fFalseBoldWidth = width;
}


void
BFont::SetSpacing(uint8 spacing)
{
	fSpacing = spacing;
}


void
BFont::SetEncoding(uint8 encoding)
{
	fEncoding = encoding;
}


void
BFont::SetFace(uint16 face)
{
	if (face == fFace)
		return;

	SetFamilyAndFace(NULL, face);
}


void
BFont::SetFlags(uint32 flags)
{
	fFlags = flags;
}


void
BFont::GetFamilyAndStyle(font_family* family, font_style* style) const
{
	if (family == NULL && style == NULL)
		return;

	// it's okay to call this function with either family or style set to NULL

	font_family familyBuffer;
	font_style styleBuffer;

	if (family == NULL)
		family = &familyBuffer;
	if (style == NULL)
		style = &styleBuffer;

	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_FAMILY_AND_STYLE);
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(fStyleID);

	int32 code;
	if (link.FlushWithReply(code) != B_OK || code != B_OK) {
		// the least we can do is to clear the buffers
		memset(*family, 0, sizeof(font_family));
		memset(*style, 0, sizeof(font_style));
		return;
	}

	link.ReadString(*family, sizeof(font_family));
	link.ReadString(*style, sizeof(font_style));
}


uint32
BFont::FamilyAndStyle() const
{
	return (fFamilyID << 16UL) | fStyleID;
}


float
BFont::Size() const
{
	return fSize;
}


float
BFont::Shear() const
{
	return fShear;
}


float
BFont::Rotation() const
{
	return fRotation;
}


float
BFont::FalseBoldWidth() const
{
	return fFalseBoldWidth;
}


uint8
BFont::Spacing() const
{
	return fSpacing;
}


uint8
BFont::Encoding() const
{
	return fEncoding;
}


uint16
BFont::Face() const
{
	return fFace;
}


uint32
BFont::Flags() const
{
	return fFlags;
}


font_direction
BFont::Direction() const
{
	_GetExtraFlags();
	return (font_direction)(fExtraFlags >> B_PRIVATE_FONT_DIRECTION_SHIFT);
}


bool
BFont::IsFixed() const
{
	_GetExtraFlags();
	return (fExtraFlags & B_IS_FIXED) != 0;
}


// Returns whether or not the font is fixed-width and contains both
// full and half-width characters.
bool
BFont::IsFullAndHalfFixed() const
{
	// This was left unimplemented as of R5. It is a way to work with both
	// Kanji and Roman characters in the same fixed-width font.

	_GetExtraFlags();
	return (fExtraFlags & B_PRIVATE_FONT_IS_FULL_AND_HALF_FIXED) != 0;
}


BRect
BFont::BoundingBox() const
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_FONT_BOUNDING_BOX);
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(fStyleID);
	link.Attach<float>(fSize);

	int32 code;
	if (link.FlushWithReply(code) != B_OK
		|| code != B_OK)
		return BRect(0, 0, 0 ,0);

	BRect box;
	link.Read<BRect>(&box);
	return box;
}


unicode_block
BFont::Blocks() const
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_UNICODE_BLOCKS);
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(fStyleID);

	int32 status;
	if (link.FlushWithReply(status) != B_OK
		|| status != B_OK) {
		return unicode_block(~0LL, ~0LL);
	}

	unicode_block blocksForFont;
	link.Read<unicode_block>(&blocksForFont);

	return blocksForFont;
}

bool
BFont::IncludesBlock(uint32 start, uint32 end) const
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_HAS_UNICODE_BLOCK);
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(fStyleID);
	link.Attach<uint32>(start);
	link.Attach<uint32>(end);

	int32 status;
	if (link.FlushWithReply(status) != B_OK
		|| status != B_OK) {
		return false;
	}

	bool hasBlock;
	link.Read<bool>(&hasBlock);

	return hasBlock;
}


font_file_format
BFont::FileFormat() const
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_FONT_FILE_FORMAT);
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(fStyleID);

	int32 status;
	if (link.FlushWithReply(status) != B_OK
		|| status != B_OK) {
		// just take a safe bet...
		return B_TRUETYPE_WINDOWS;
	}

	uint16 format;
	link.Read<uint16>(&format);

	return (font_file_format)format;
}


int32
BFont::CountTuned() const
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_TUNED_COUNT);
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(fStyleID);

	int32 code;
	if (link.FlushWithReply(code) != B_OK
		|| code != B_OK)
		return -1;

	int32 count;
	link.Read<int32>(&count);
	return count;
}


void
BFont::GetTunedInfo(int32 index, tuned_font_info* info) const
{
	if (info == NULL)
		return;

	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_TUNED_INFO);
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(fStyleID);
	link.Attach<uint32>(index);

	int32 code;
	if (link.FlushWithReply(code) != B_OK || code != B_OK)
		return;

	link.Read<tuned_font_info>(info);
}


// Truncates a string to a given _pixel_ width based on the font and size
void
BFont::TruncateString(BString* inOut, uint32 mode, float width) const
{
	if (mode == B_NO_TRUNCATION)
		return;

	// NOTE: Careful, we cannot directly use "inOut->String()" as result
	// array, because the string length increases by 3 bytes in the worst
	// case scenario.
	const char* string = inOut->String();
	GetTruncatedStrings(&string, 1, mode, width, inOut);
}


void
BFont::GetTruncatedStrings(const char* stringArray[], int32 numStrings,
	uint32 mode, float width, BString resultArray[]) const
{
	if (stringArray != NULL && numStrings > 0) {
		// the width of the "…" glyph
		float ellipsisWidth = StringWidth(B_UTF8_ELLIPSIS);

		for (int32 i = 0; i < numStrings; i++) {
			resultArray[i] = stringArray[i];
			int32 numChars = resultArray[i].CountChars();

			// get the escapement of each glyph in font units
			float* escapementArray = new float[numChars];
			GetEscapements(stringArray[i], numChars, NULL, escapementArray);

			truncate_string(resultArray[i], mode, width, escapementArray,
				fSize, ellipsisWidth, numChars);

			delete[] escapementArray;
		}
	}
}


void
BFont::GetTruncatedStrings(const char* stringArray[], int32 numStrings,
	uint32 mode, float width, char* resultArray[]) const
{
	if (stringArray != NULL && numStrings > 0) {
		for (int32 i = 0; i < numStrings; i++) {
			BString* strings = new BString[numStrings];
			GetTruncatedStrings(stringArray, numStrings, mode, width, strings);

			for (int32 i = 0; i < numStrings; i++)
				strcpy(resultArray[i], strings[i].String());

			delete[] strings;
		}
	}
}


float
BFont::StringWidth(const char* string) const
{
	if (string == NULL)
		return 0.0;

	int32 length = strlen(string);
	float width;
	GetStringWidths(&string, &length, 1, &width);

	return width;
}


float
BFont::StringWidth(const char* string, int32 length) const
{
	if (!string || length < 1)
		return 0.0f;

	float width = 0.0f;
	GetStringWidths(&string, &length, 1, &width);

	return width;
}


//	#pragma mark - string widths


/*	The width of a string is app_server's to say: it has the fonts. Asking it
	is a synchronous round trip on the application's one link to it, for
	every label that is laid out or drawn. What it answered is remembered
	here, per process, in two ways:

	- The advance of single characters, per font (family and style ID, size,
	  spacing mode: what a width request carries). app_server adds up the
	  advances of a string's characters in doubles and returns the sum as a
	  float (GlyphLayoutEngine::LayoutGlyphs() with a StringWidthConsumer);
	  width_cache_sum() adds the same doubles in the same order, so the
	  result is the same number, not merely a close one. Only characters that
	  were not seen before are asked for (AS_GET_GLYPH_ADVANCES), all those of
	  a call in one request. A missing glyph that a fallback font supplies
	  has that font's advance, like in the server.
	- That only holds when a character's advance does not depend on its
	  neighbours. It does with kerning (B_STRING_SPACING) and where an emoji
	  sequence may begin (emoji_cluster_may_start_at(), the same test the
	  server's layout makes). Such strings are asked for whole, as before,
	  and the answer is remembered by the string.

	Both are dropped when server_read_only_memory::font_generation changes:
	app_server changes it when fonts are installed or removed (also an
	application's own, whose IDs can be used again), and when the hinting or
	anti-aliasing settings or the render scale change. Both are bounded:
	kWidthCacheFonts fonts with at most kWidthCacheCharLimit characters
	besides ASCII each (about 160 KiB when all are in use), and
	kWidthCacheStringSlots strings of at most kWidthCacheMaxStringLength
	bytes. Longer strings are not remembered at all.
*/

namespace {

const int32 kWidthCacheFonts = 12;
const uint32 kWidthCacheCharSlots = 1024;
	// a power of two
const uint32 kWidthCacheCharLimit = 768;
	// then the font's table starts over
const uint32 kWidthCacheStringSlots = 256;
const int32 kWidthCacheMaxStringLength = 256;

struct width_cache_font {
	uint32	familyAndStyle;
	float	size;
	uint8	spacing;
	uint64	lastUsed;
	uint32	charCount;
	bool	asciiKnown[128];
	double	ascii[128];
	uint32	codes[kWidthCacheCharSlots];
		// 0: free
	double	advances[kWidthCacheCharSlots];
};

struct width_cache_string {
	char*	string;
		// NULL: free
	uint32	hash;
	int32	length;
	uint32	familyAndStyle;
	float	size;
	uint8	spacing;
	float	width;
};

enum {
	kWidthKnown,
	kWidthNeedsAdvances,
	kWidthNeedsServer
};

pthread_mutex_t sWidthCacheLock = PTHREAD_MUTEX_INITIALIZER;
int32 sWidthCacheGeneration = 0;
uint64 sWidthCacheClock = 0;
width_cache_font* sWidthCacheFonts[kWidthCacheFonts];
width_cache_string sWidthCacheStrings[kWidthCacheStringSlots];


//!	app_server's font generation; 0 when nothing may be remembered.
int32
width_cache_generation()
{
	if (be_app == NULL)
		return 0;
	server_read_only_memory* shared
		= BApplication::Private::ServerReadOnlyMemory();
	if (shared == NULL)
		return 0;
	return atomic_get(&shared->font_generation);
}


/*!	With the lock held: whether the cache is the one of \a generation
	(emptied, if it was an older one's). False when the generation moved on
	meanwhile: what was asked under \a generation is not to be put in.
*/
bool
width_cache_validate(int32 generation)
{
	if (generation == 0 || width_cache_generation() != generation)
		return false;
	if (sWidthCacheGeneration == generation)
		return true;

	for (int32 i = 0; i < kWidthCacheFonts; i++) {
		free(sWidthCacheFonts[i]);
		sWidthCacheFonts[i] = NULL;
	}
	for (uint32 i = 0; i < kWidthCacheStringSlots; i++) {
		free(sWidthCacheStrings[i].string);
		sWidthCacheStrings[i].string = NULL;
	}
	sWidthCacheGeneration = generation;
	return true;
}


void
width_cache_reset_font(width_cache_font* font)
{
	font->charCount = 0;
	memset(font->asciiKnown, 0, sizeof(font->asciiKnown));
	memset(font->codes, 0, sizeof(font->codes));
}


width_cache_font*
width_cache_font_for(uint32 familyAndStyle, float size, uint8 spacing,
	bool create)
{
	int32 slot = -1;
	for (int32 i = 0; i < kWidthCacheFonts; i++) {
		width_cache_font* font = sWidthCacheFonts[i];
		if (font == NULL) {
			if (slot < 0 || sWidthCacheFonts[slot] != NULL)
				slot = i;
			continue;
		}
		if (font->familyAndStyle == familyAndStyle && font->size == size
			&& font->spacing == spacing) {
			font->lastUsed = ++sWidthCacheClock;
			return font;
		}
		if (slot < 0 || (sWidthCacheFonts[slot] != NULL
				&& font->lastUsed < sWidthCacheFonts[slot]->lastUsed)) {
			slot = i;
		}
	}
	if (!create)
		return NULL;

	// a free slot, or the one of the font that was used least recently
	width_cache_font* font = sWidthCacheFonts[slot];
	if (font == NULL) {
		font = (width_cache_font*)malloc(sizeof(width_cache_font));
		if (font == NULL)
			return NULL;
		sWidthCacheFonts[slot] = font;
	}
	font->familyAndStyle = familyAndStyle;
	font->size = size;
	font->spacing = spacing;
	font->lastUsed = ++sWidthCacheClock;
	width_cache_reset_font(font);
	return font;
}


inline uint32
width_cache_char_slot(uint32 code)
{
	return (code * 2654435761u) >> 22;
		// 10 bits: kWidthCacheCharSlots
}


inline bool
width_cache_lookup(const width_cache_font* font, uint32 code, double& advance)
{
	if (code < 128) {
		advance = font->ascii[code];
		return font->asciiKnown[code];
	}
	uint32 slot = width_cache_char_slot(code);
	while (font->codes[slot] != 0) {
		if (font->codes[slot] == code) {
			advance = font->advances[slot];
			return true;
		}
		slot = (slot + 1) & (kWidthCacheCharSlots - 1);
	}
	return false;
}


void
width_cache_insert(width_cache_font* font, uint32 code, double advance)
{
	if (code < 128) {
		font->ascii[code] = advance;
		font->asciiKnown[code] = true;
		return;
	}
	if (font->charCount >= kWidthCacheCharLimit) {
		// full: start over (ASCII stays)
		font->charCount = 0;
		memset(font->codes, 0, sizeof(font->codes));
	}
	uint32 slot = width_cache_char_slot(code);
	while (font->codes[slot] != 0) {
		if (font->codes[slot] == code) {
			font->advances[slot] = advance;
			return;
		}
		slot = (slot + 1) & (kWidthCacheCharSlots - 1);
	}
	font->codes[slot] = code;
	font->advances[slot] = advance;
	font->charCount++;
}


uint32
width_cache_string_hash(const char* string, int32 length,
	uint32 familyAndStyle, float size, uint8 spacing)
{
	uint32 hash = 2166136261u ^ familyAndStyle;
	hash = (hash * 16777619u) ^ spacing;
	uint32 sizeBits;
	memcpy(&sizeBits, &size, sizeof(sizeBits));
	hash = (hash * 16777619u) ^ sizeBits;
	for (int32 i = 0; i < length; i++)
		hash = (hash * 16777619u) ^ (uint8)string[i];
	return hash;
}


/*!	The characters of one call whose advances are not known yet: their codes,
	and the bytes of each in a slot of kGlyphAdvanceSlotSize, the way
	AS_GET_GLYPH_ADVANCES takes them.
*/
struct missing_characters {
	int32	count;
	uint32	codes[kMaxGlyphAdvanceCount];
	char	bytes[kMaxGlyphAdvanceCount * kGlyphAdvanceSlotSize];
};


/*!	The width of the null terminated \a text of \a length bytes from the
	advances of its characters, if they are all known: the same loop as
	GlyphLayoutEngine::LayoutGlyphs() runs for a StringWidthConsumer, with
	the same doubles. Characters that are not known are added to \a missing
	(kWidthNeedsAdvances). kWidthNeedsServer: the server has to lay out the
	string as a whole.
*/
int
width_cache_sum(const width_cache_font* font, const char* text, int32 length,
	float& width, missing_characters* missing)
{
	const char* position = text;
	double x = 0.0;
	double advance = 0.0;
	bool complete = true;

	while (true) {
		const char* start = position;
		uint32 code = UTF8ToCharCode(&position);
		if (code == 0)
			break;
		if (emoji_cluster_may_start_at(code, position,
				position - text < length)) {
			return kWidthNeedsServer;
		}

		x += advance;
		if (font == NULL || !width_cache_lookup(font, code, advance)) {
			advance = 0.0;
			complete = false;
			if (missing == NULL)
				return kWidthNeedsServer;

			int32 bytes = position - start;
			if (bytes >= kGlyphAdvanceSlotSize)
				return kWidthNeedsServer;
			int32 index = 0;
			while (index < missing->count && missing->codes[index] != code)
				index++;
			if (index == missing->count) {
				if (index == kMaxGlyphAdvanceCount)
					return kWidthNeedsServer;
				missing->codes[index] = code;
				char* slot = missing->bytes + index * kGlyphAdvanceSlotSize;
				memset(slot, 0, kGlyphAdvanceSlotSize);
				memcpy(slot, start, bytes);
				missing->count++;
			}
		}

		if (position - text + 1 > length)
			break;
	}
	x += advance;

	if (!complete)
		return kWidthNeedsAdvances;
	width = (float)x;
	return kWidthKnown;
}


status_t
get_string_widths_from_server(uint16 familyID, uint16 styleID, float size,
	uint8 spacing, const char* stringArray[], const int32 lengthArray[],
	int32 numStrings, float widthArray[])
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_STRING_WIDTHS);
	link.Attach<uint16>(familyID);
	link.Attach<uint16>(styleID);
	link.Attach<float>(size);
	link.Attach<uint8>(spacing);
	link.Attach<int32>(numStrings);

	// TODO: all strings into a single array???
	// we do have a maximum message length, and it could be easily touched
	// here...
	for (int32 i = 0; i < numStrings; i++)
		link.AttachString(stringArray[i], lengthArray[i]);

	status_t status;
	if (link.FlushWithReply(status) != B_OK)
		return B_ERROR;
	if (status != B_OK)
		return status;

	return link.Read(widthArray, sizeof(float) * numStrings);
}


status_t
get_glyph_advances_from_server(uint16 familyID, uint16 styleID, float size,
	uint8 spacing, const missing_characters& missing, double* advances)
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_GLYPH_ADVANCES);
	link.Attach<uint16>(familyID);
	link.Attach<uint16>(styleID);
	link.Attach<float>(size);
	link.Attach<uint8>(spacing);
	link.Attach<int32>(missing.count);
	link.Attach(missing.bytes, missing.count * kGlyphAdvanceSlotSize);

	status_t status;
	if (link.FlushWithReply(status) != B_OK)
		return B_ERROR;
	if (status != B_OK)
		return status;

	return link.Read(advances, missing.count * sizeof(double));
}

}	// unnamed namespace


void
BFont::GetStringWidths(const char* stringArray[], const int32 lengthArray[],
	int32 numStrings, float widthArray[]) const
{
	if (stringArray == NULL || lengthArray == NULL || numStrings < 1
		|| widthArray == NULL) {
		return;
	}

	// (see the comment above for what is remembered and why it is exact)
	const int32 generation = width_cache_generation();
	BStackOrHeapArray<uint8, 64> states(numStrings);
	if (generation == 0 || !(fSize > 0) || !states.IsValid()) {
		get_string_widths_from_server(fFamilyID, fStyleID, fSize, fSpacing,
			stringArray, lengthArray, numStrings, widthArray);
		return;
	}

	const uint32 familyAndStyle = ((uint32)fFamilyID << 16) | fStyleID;
	const bool summable = fSpacing != B_STRING_SPACING;
	missing_characters missing;
	missing.count = 0;
	int32 needAdvances = 0;
	int32 needServer = 0;
	char text[kWidthCacheMaxStringLength + 1];

	pthread_mutex_lock(&sWidthCacheLock);
	bool valid = width_cache_validate(generation);
	const width_cache_font* font = valid
		? width_cache_font_for(familyAndStyle, fSize, fSpacing, false) : NULL;

	for (int32 i = 0; i < numStrings; i++) {
		// what the server would be sent: no more than the string has
		const char* string = stringArray[i];
		int32 length = lengthArray[i];
		states[i] = kWidthNeedsServer;
		if (string != NULL && length >= 0) {
			length = strnlen(string, length);
			if (length == 0) {
				widthArray[i] = 0.0f;
				states[i] = kWidthKnown;
			} else if (valid && length <= kWidthCacheMaxStringLength) {
				memcpy(text, string, length);
				text[length] = '\0';
				if (summable) {
					states[i] = width_cache_sum(font, text, length,
						widthArray[i], &missing);
				}
				if (states[i] == kWidthNeedsServer) {
					// one that was asked for whole before?
					uint32 hash = width_cache_string_hash(text, length,
						familyAndStyle, fSize, fSpacing);
					const width_cache_string& entry = sWidthCacheStrings[
						hash % kWidthCacheStringSlots];
					if (entry.string != NULL && entry.hash == hash
						&& entry.length == length
						&& entry.familyAndStyle == familyAndStyle
						&& entry.size == fSize && entry.spacing == fSpacing
						&& memcmp(entry.string, text, length) == 0) {
						widthArray[i] = entry.width;
						states[i] = kWidthKnown;
					}
				}
			}
		}
		if (states[i] == kWidthNeedsAdvances)
			needAdvances++;
		else if (states[i] == kWidthNeedsServer)
			needServer++;
	}
	pthread_mutex_unlock(&sWidthCacheLock);

	if (needAdvances > 0) {
		// the characters that were not seen before, in one request
		double advances[kMaxGlyphAdvanceCount];
		bool received = get_glyph_advances_from_server(fFamilyID, fStyleID,
			fSize, fSpacing, missing, advances) == B_OK;

		pthread_mutex_lock(&sWidthCacheLock);
		width_cache_font* font = NULL;
		if (received && width_cache_validate(generation)) {
			font = width_cache_font_for(familyAndStyle, fSize, fSpacing,
				true);
		}
		if (font != NULL) {
			for (int32 i = 0; i < missing.count; i++)
				width_cache_insert(font, missing.codes[i], advances[i]);
		}
		for (int32 i = 0; i < numStrings; i++) {
			if (states[i] != kWidthNeedsAdvances)
				continue;
			// (a string with more new characters than fit one request,
			// or one whose characters did not stay: asked for whole)
			int32 length = strnlen(stringArray[i], lengthArray[i]);
			memcpy(text, stringArray[i], length);
			text[length] = '\0';
			states[i] = kWidthNeedsServer;
			if (font != NULL) {
				states[i] = width_cache_sum(font, text, length, widthArray[i],
					NULL);
			}
			if (states[i] != kWidthKnown) {
				states[i] = kWidthNeedsServer;
				needServer++;
			}
		}
		pthread_mutex_unlock(&sWidthCacheLock);
	}

	if (needServer == 0)
		return;

	if (needServer == numStrings) {
		if (get_string_widths_from_server(fFamilyID, fStyleID, fSize,
				fSpacing, stringArray, lengthArray, numStrings, widthArray)
				!= B_OK) {
			return;
		}
	} else {
		// only those that are left
		BStackOrHeapArray<const char*, 64> strings(needServer);
		BStackOrHeapArray<int32, 64> lengths(needServer);
		BStackOrHeapArray<float, 64> widths(needServer);
		if (!strings.IsValid() || !lengths.IsValid() || !widths.IsValid())
			return;
		int32 count = 0;
		for (int32 i = 0; i < numStrings; i++) {
			if (states[i] == kWidthNeedsServer) {
				strings[count] = stringArray[i];
				lengths[count++] = lengthArray[i];
			}
		}
		if (get_string_widths_from_server(fFamilyID, fStyleID, fSize,
				fSpacing, strings, lengths, count, widths) != B_OK) {
			return;
		}
		count = 0;
		for (int32 i = 0; i < numStrings; i++) {
			if (states[i] == kWidthNeedsServer)
				widthArray[i] = widths[count++];
		}
	}

	// remember the short ones by the string
	pthread_mutex_lock(&sWidthCacheLock);
	if (width_cache_validate(generation)) {
		for (int32 i = 0; i < numStrings; i++) {
			if (states[i] != kWidthNeedsServer || stringArray[i] == NULL
				|| lengthArray[i] < 0) {
				continue;
			}
			int32 length = strnlen(stringArray[i], lengthArray[i]);
			if (length == 0 || length > kWidthCacheMaxStringLength)
				continue;
			uint32 hash = width_cache_string_hash(stringArray[i], length,
				familyAndStyle, fSize, fSpacing);
			width_cache_string& entry = sWidthCacheStrings[
				hash % kWidthCacheStringSlots];
			char* copy = (char*)realloc(entry.string, length);
			if (copy == NULL)
				continue;
			memcpy(copy, stringArray[i], length);
			entry.string = copy;
			entry.hash = hash;
			entry.length = length;
			entry.familyAndStyle = familyAndStyle;
			entry.size = fSize;
			entry.spacing = fSpacing;
			entry.width = widthArray[i];
		}
	}
	pthread_mutex_unlock(&sWidthCacheLock);
}


void
BFont::GetEscapements(const char charArray[], int32 numChars,
	float escapementArray[]) const
{
	GetEscapements(charArray, numChars, NULL, escapementArray);
}


void
BFont::GetEscapements(const char charArray[], int32 numChars,
	escapement_delta* delta, float escapementArray[]) const
{
	if (charArray == NULL || numChars < 1 || escapementArray == NULL)
		return;

	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_ESCAPEMENTS_AS_FLOATS);
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(fStyleID);
	link.Attach<float>(fSize);
	link.Attach<uint8>(fSpacing);
	link.Attach<float>(fRotation);
	link.Attach<uint32>(fFlags);

	link.Attach<float>(delta ? delta->nonspace : 0.0f);
	link.Attach<float>(delta ? delta->space : 0.0f);
	link.Attach<int32>(numChars);

	// TODO: Should we not worry about the port capacity here?!?
	uint32 bytesInBuffer = UTF8CountBytes(charArray, numChars);
	link.Attach<int32>(bytesInBuffer);
	link.Attach(charArray, bytesInBuffer);

	int32 code;
	if (link.FlushWithReply(code) != B_OK || code != B_OK)
		return;

	link.Read(escapementArray, numChars * sizeof(float));
}


void
BFont::GetEscapements(const char charArray[], int32 numChars,
	escapement_delta* delta, BPoint escapementArray[]) const
{
	GetEscapements(charArray, numChars, delta, escapementArray, NULL);
}


void
BFont::GetEscapements(const char charArray[], int32 numChars,
	escapement_delta* delta, BPoint escapementArray[],
	BPoint offsetArray[]) const
{
	if (charArray == NULL || numChars < 1 || escapementArray == NULL)
		return;

	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_ESCAPEMENTS);
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(fStyleID);
	link.Attach<float>(fSize);
	link.Attach<uint8>(fSpacing);
	link.Attach<float>(fRotation);
	link.Attach<uint32>(fFlags);

	link.Attach<float>(delta ? delta->nonspace : 0.0);
	link.Attach<float>(delta ? delta->space : 0.0);
	link.Attach<bool>(offsetArray != NULL);
	link.Attach<int32>(numChars);

	// TODO: Should we not worry about the port capacity here?!?
	uint32 bytesInBuffer = UTF8CountBytes(charArray, numChars);
	link.Attach<int32>(bytesInBuffer);
	link.Attach(charArray, bytesInBuffer);

	int32 code;
	if (link.FlushWithReply(code) != B_OK || code != B_OK)
		return;

	link.Read(escapementArray, sizeof(BPoint) * numChars);
	if (offsetArray)
		link.Read(offsetArray, sizeof(BPoint) * numChars);
}


void
BFont::GetEdges(const char charArray[], int32 numChars,
	edge_info edgeArray[]) const
{
	if (!charArray || numChars < 1 || !edgeArray)
		return;

	int32 code;
	BPrivate::AppServerLink link;

	link.StartMessage(AS_GET_EDGES);
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(fStyleID);
	link.Attach<int32>(numChars);

	uint32 bytesInBuffer = UTF8CountBytes(charArray, numChars);
	link.Attach<int32>(bytesInBuffer);
	link.Attach(charArray, bytesInBuffer);

	if (link.FlushWithReply(code) != B_OK || code != B_OK)
		return;

	link.Read(edgeArray, sizeof(edge_info) * numChars);
}


void
BFont::GetHeight(font_height* _height) const
{
	if (_height == NULL)
		return;

	if (fHeight.ascent == kUninitializedAscent) {
		// we don't have the font height cached yet
		BPrivate::AppServerLink link;

		link.StartMessage(AS_GET_FONT_HEIGHT);
		link.Attach<uint16>(fFamilyID);
		link.Attach<uint16>(fStyleID);
		link.Attach<float>(fSize);

		int32 code;
		if (link.FlushWithReply(code) != B_OK || code != B_OK)
			return;

		// Who put that "const" to this method? :-)
		// We made fHeight mutable for this, but we should drop the "const"
		// when we can
		link.Read<font_height>(&fHeight);
	}

	*_height = fHeight;
}


void
BFont::GetBoundingBoxesAsGlyphs(const char charArray[], int32 numChars,
	font_metric_mode mode, BRect boundingBoxArray[]) const
{
	_GetBoundingBoxes(charArray, numChars, mode, false, NULL,
		boundingBoxArray, false);
}


void
BFont::GetBoundingBoxesAsString(const char charArray[], int32 numChars,
	font_metric_mode mode, escapement_delta* delta,
	BRect boundingBoxArray[]) const
{
	_GetBoundingBoxes(charArray, numChars, mode, true, delta,
		boundingBoxArray, true);
}


void
BFont::_GetBoundingBoxes(const char charArray[], int32 numChars,
	font_metric_mode mode, bool string_escapement, escapement_delta* delta,
	BRect boundingBoxArray[], bool asString) const
{
	if (charArray == NULL || numChars < 1 || boundingBoxArray == NULL)
		return;

	int32 code;
	BPrivate::AppServerLink link;

	link.StartMessage(asString
		? AS_GET_BOUNDINGBOXES_STRING : AS_GET_BOUNDINGBOXES_CHARS);
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(fStyleID);
	link.Attach<float>(fSize);
	link.Attach<float>(fRotation);
	link.Attach<float>(fShear);
	link.Attach<float>(fFalseBoldWidth);
	link.Attach<uint8>(fSpacing);

	link.Attach<uint32>(fFlags);
	link.Attach<font_metric_mode>(mode);
	link.Attach<bool>(string_escapement);

	if (delta != NULL) {
		link.Attach<escapement_delta>(*delta);
	} else {
		escapement_delta emptyDelta = {0, 0};
		link.Attach<escapement_delta>(emptyDelta);
	}

	link.Attach<int32>(numChars);
	uint32 bytesInBuffer = UTF8CountBytes(charArray, numChars);
	link.Attach<int32>(bytesInBuffer);
	link.Attach(charArray, bytesInBuffer);

	if (link.FlushWithReply(code) != B_OK || code != B_OK)
		return;

	link.Read(boundingBoxArray, sizeof(BRect) * numChars);
}


void
BFont::GetBoundingBoxesForStrings(const char* stringArray[], int32 numStrings,
	font_metric_mode mode, escapement_delta deltas[],
	BRect boundingBoxArray[]) const
{
	if (!stringArray || numStrings < 1 || !boundingBoxArray)
		return;

	int32 code;
	BPrivate::AppServerLink link;

	link.StartMessage(AS_GET_BOUNDINGBOXES_STRINGS);
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(fStyleID);
	link.Attach<float>(fSize);
	link.Attach<float>(fRotation);
	link.Attach<float>(fShear);
	link.Attach<float>(fFalseBoldWidth);
	link.Attach<uint8>(fSpacing);
	link.Attach<uint32>(fFlags);
	link.Attach<font_metric_mode>(mode);
	link.Attach<int32>(numStrings);

	for (int32 i = 0; i < numStrings; i++)
		link.AttachString(stringArray[i]);

	if (deltas) {
		for (int32 i = 0; i < numStrings; i++)
			link.Attach<escapement_delta>(deltas[i]);
	} else {
		escapement_delta emptyDelta = {0, 0};
		for (int32 i = 0; i < numStrings; i++)
			link.Attach<escapement_delta>(emptyDelta);
	}

	if (link.FlushWithReply(code) != B_OK || code != B_OK)
		return;

	link.Read(boundingBoxArray, sizeof(BRect) * numStrings);
}


void
BFont::GetGlyphShapes(const char charArray[], int32 numChars,
	BShape* glyphShapeArray[]) const
{
	// TODO: implement code specifically for passing BShapes to and
	// from the server
	if (!charArray || numChars < 1 || !glyphShapeArray)
		return;

	int32 code;
	BPrivate::AppServerLink link;

	link.StartMessage(AS_GET_GLYPH_SHAPES);
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(fStyleID);
	link.Attach<float>(fSize);
	link.Attach<float>(fShear);
	link.Attach<float>(fRotation);
	link.Attach<float>(fFalseBoldWidth);
	link.Attach<uint32>(fFlags);
	link.Attach<int32>(numChars);

	uint32 bytesInBuffer = UTF8CountBytes(charArray, numChars);
	link.Attach<int32>(bytesInBuffer);
	link.Attach(charArray, bytesInBuffer);

	if (link.FlushWithReply(code) != B_OK || code != B_OK)
		return;

	for (int32 i = 0; i < numChars; i++)
		link.ReadShape(glyphShapeArray[i]);
}


void
BFont::GetHasGlyphs(const char charArray[], int32 numChars,
	bool hasArray[]) const
{
	GetHasGlyphs(charArray, numChars, hasArray, true);
}


void
BFont::GetHasGlyphs(const char charArray[], int32 numChars, bool hasArray[],
	bool useFallbacks) const
{
	if (!charArray || numChars < 1 || !hasArray)
		return;

	int32 code;
	BPrivate::AppServerLink link;

	link.StartMessage(AS_GET_HAS_GLYPHS);
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(fStyleID);
	link.Attach<int32>(numChars);

	uint32 bytesInBuffer = UTF8CountBytes(charArray, numChars);
	link.Attach<int32>(bytesInBuffer);
	link.Attach(charArray, bytesInBuffer);

	link.Attach<bool>(useFallbacks);

	if (link.FlushWithReply(code) != B_OK || code != B_OK)
		return;

	link.Read(hasArray, sizeof(bool) * numChars);
}


BFont&
BFont::operator=(const BFont& font)
{
	fFamilyID = font.fFamilyID;
	fStyleID = font.fStyleID;
	fSize = font.fSize;
	fShear = font.fShear;
	fRotation = font.fRotation;
	fFalseBoldWidth = font.fFalseBoldWidth;
	fSpacing = font.fSpacing;
	fEncoding = font.fEncoding;
	fFace = font.fFace;
	fHeight = font.fHeight;
	fFlags = font.fFlags;
	fExtraFlags = font.fExtraFlags;

	return *this;
}


bool
BFont::operator==(const BFont& font) const
{
	return fFamilyID == font.fFamilyID
		&& fStyleID == font.fStyleID
		&& fSize == font.fSize
		&& fShear == font.fShear
		&& fRotation == font.fRotation
		&& fFalseBoldWidth == font.fFalseBoldWidth
		&& fSpacing == font.fSpacing
		&& fEncoding == font.fEncoding
		&& fFace == font.fFace;
}


bool
BFont::operator!=(const BFont& font) const
{
	return fFamilyID != font.fFamilyID
		|| fStyleID != font.fStyleID
		|| fSize != font.fSize
		|| fShear != font.fShear
		|| fRotation != font.fRotation
		|| fFalseBoldWidth != font.fFalseBoldWidth
		|| fSpacing != font.fSpacing
		|| fEncoding != font.fEncoding
		|| fFace != font.fFace;
}


void
BFont::PrintToStream() const
{
	font_family family;
	font_style style;
	GetFamilyAndStyle(&family, &style);

	printf("BFont { %s (%d), %s (%d) 0x%x %f/%f %fpt (%f %f %f), %d }\n",
		family, fFamilyID, style, fStyleID, fFace, fShear, fRotation, fSize,
		fHeight.ascent, fHeight.descent, fHeight.leading, fEncoding);
}


void
BFont::_GetExtraFlags() const
{
	// TODO: this has to be const in order to allow other font getters to
	// stay const as well
	if (fExtraFlags != kUninitializedExtraFlags)
		return;

	BPrivate::AppServerLink link;
	link.StartMessage(AS_GET_EXTRA_FONT_FLAGS);
	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(fStyleID);

	status_t status = B_ERROR;
	if (link.FlushWithReply(status) != B_OK || status != B_OK) {
		// use defaut values for the flags
		fExtraFlags = (uint32)B_FONT_LEFT_TO_RIGHT
			<< B_PRIVATE_FONT_DIRECTION_SHIFT;
		return;
	}

	link.Read<uint32>(&fExtraFlags);
}


status_t
BFont::LoadFont(const char* path)
{
	return LoadFont(path, 0, 0);
}


status_t
BFont::LoadFont(const char* path, uint16 index, uint16 instance)
{
	BPrivate::AppServerLink link;
	link.StartMessage(AS_ADD_FONT_FILE);
	link.AttachString(path);
	link.Attach<uint16>(index);
	link.Attach<uint16>(instance);
	status_t status = B_ERROR;
	if (link.FlushWithReply(status) != B_OK || status != B_OK) {
		return status;
	}

	link.Read<uint16>(&fFamilyID);
	link.Read<uint16>(&fStyleID);
	link.Read<uint16>(&fFace);
	fHeight.ascent = kUninitializedAscent;
	fExtraFlags = kUninitializedExtraFlags;

	return B_OK;
}


status_t
BFont::LoadFont(const area_id fontAreaID, size_t size, size_t offset)
{
	return LoadFont(fontAreaID, size, offset, 0, 0);
}


status_t
BFont::LoadFont(const area_id fontAreaID, size_t size, size_t offset, uint16 index, uint16 instance)
{
	BPrivate::AppServerLink link;

	link.StartMessage(AS_ADD_FONT_MEMORY);

	link.Attach<int32>(fontAreaID);
	link.Attach<size_t>(size);
	link.Attach<size_t>(offset);
	link.Attach<uint16>(index);
	link.Attach<uint16>(instance);

	status_t status = B_ERROR;
	if (link.FlushWithReply(status) != B_OK || status != B_OK) {
		return status;
	}

	link.Read<uint16>(&fFamilyID);
	link.Read<uint16>(&fStyleID);
	link.Read<uint16>(&fFace);
	fHeight.ascent = kUninitializedAscent;
	fExtraFlags = kUninitializedExtraFlags;

	return B_OK;
}


status_t
BFont::UnloadFont()
{
	BPrivate::AppServerLink link;

	link.StartMessage(AS_REMOVE_FONT);

	link.Attach<uint16>(fFamilyID);
	link.Attach<uint16>(fStyleID);

	status_t status = B_ERROR;
	if (link.FlushWithReply(status) != B_OK || status != B_OK) {
		return status;
	}

	// reset to plain font
	fFamilyID = sPlainFont.fFamilyID;
	fStyleID = sPlainFont.fStyleID;
	fFace = sPlainFont.fFace;
	fExtraFlags = sPlainFont.fExtraFlags;

	fHeight.ascent = kUninitializedAscent;

	return B_OK;
}
