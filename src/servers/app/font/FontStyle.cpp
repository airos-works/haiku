/*
 * Copyright 2001-2008, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		DarkWyrm <bpmagic@columbus.rr.com>
 *		Axel Dörfler, axeld@pinc-software.de
 */

/**	Classes to represent font styles and families */


#include "FontFamily.h"
#include "FontManager.h"
#include "ServerFont.h"

#include <FontPrivate.h>

#include <Autolock.h>
#include <Entry.h>


static BLocker sFontLock("font lock");

/*!	The most faces of styles opened on demand that stay open while nothing
	uses them. Such a face is only needed for the less common queries (glyph
	shapes, Unicode blocks, the bounding box, the fallback font coverage);
	this is more than the fallback fonts and the default fonts take.
*/
static const int32 kMaxOpenFaces = 32;

FontStyle::OpenFaceList FontStyle::sOpenFaces;
int32 FontStyle::sOpenFaceCount = 0;
FT_Library FontStyle::sFaceLibrary = NULL;


/*!	Whether the printable ASCII characters of \a face all have the same
	advance.
*/
static bool
is_full_and_half_fixed(FT_Face face)
{
	FT_Int32 loadFlags = FT_LOAD_NO_SCALE | FT_LOAD_TARGET_NORMAL;
	if (FT_Load_Char(face, (uint32)' ', loadFlags) != 0)
		return false;

	int firstWidth = face->glyph->advance.x;
	for (uint32 c = ' ' + 1; c <= 0x7e; c++) {
		if (FT_Load_Char(face, c, loadFlags) != 0)
			return false;

		if (face->glyph->advance.x != firstWidth)
			return false;
	}

	return true;
}


/*!
	\brief Constructor
	\param filepath path to a font file
	\param face FreeType handle for the font file after it is loaded
	\param openOnDemand if false, the style keeps \a face open until it is
		destroyed. If true, it takes what it needs from \a face and closes it;
		FreeTypeFace() opens the file again when it is asked for the face.
		A font manager that knows every installed font uses this, as an open
		face maps its whole file and keeps the pages it read.
*/
FontStyle::FontStyle(node_ref& nodeRef, const char* path, FT_Face face,
	FontManager* fontManager, bool openOnDemand)
	:
	fFreeTypeFace(face),
	fFaceIndex(face->face_index),
	fOpenOnDemand(openOnDemand),
	fIsFixedWidth(FT_IS_FIXED_WIDTH(face)),
	fIsScalable(FT_IS_SCALABLE(face)),
	fHasKerning(FT_HAS_KERNING(face)),
	fTunedCount(face->num_fixed_sizes),
	fGlyphCount(face->num_glyphs),
	fCharMapCount(face->num_charmaps),
	fName(face->style_name),
	fPath(path),
	fNodeRef(nodeRef),
	fFamily(NULL),
	fID(0),
	fBounds(0, 0, 0, 0),
	fFace(_TranslateStyleToFace(face->style_name)),
	fFullAndHalfFixed(false),
	fFontData(NULL),
	fFontManager(fontManager)
{
	fName.Truncate(B_FONT_STYLE_LENGTH);
		// make sure this style can be found using the Be API

	if (IsScalable()) {
		fHeight.ascent = (double)face->ascender / face->units_per_EM;
		fHeight.descent = (double)-face->descender / face->units_per_EM;
			// FT2's descent numbers are negative. Be's is positive

		// FT2 doesn't provide a linegap, but according to the docs, we can
		// calculate it because height = ascending + descending + leading
		fHeight.leading = (double)(face->height - face->ascender
			+ face->descender) / face->units_per_EM;
	} else {
		// We don't have global metrics, get them from a bitmap
		FT_Pos size = face->available_sizes[0].size;
		for (int i = 1; i < face->num_fixed_sizes; i++)
			size = max_c(size, face->available_sizes[i].size);
		FT_Set_Pixel_Sizes(face, 0, size / 64);
			// Size is encoded as 26.6 fixed point, while FT_Set_Pixel_Sizes
			// uses the integer unencoded value

		FT_Size_Metrics metrics = face->size->metrics;
		fHeight.ascent = (double)metrics.ascender / size;
		fHeight.descent = (double)-metrics.descender / size;
		fHeight.leading = (double)(metrics.height - metrics.ascender
			+ metrics.descender) / size;
	}

	// manually check if all applicable chars are the same width
	if (!IsFixedWidth())
		fFullAndHalfFixed = is_full_and_half_fixed(face);

	if (fOpenOnDemand) {
		FT_Done_Face(face);
		fFreeTypeFace = NULL;
	}
}


FontStyle::~FontStyle()
{
	// make sure the font server is ours
	if (fFamily.IsSet() && fFontManager->Lock()) {
		fFontManager->RemoveStyle(this);
		fFontManager->Unlock();
	}

	if (fOpenOnDemand) {
		BAutolock locker(sFontLock);
		if (fFreeTypeFace != NULL) {
			sOpenFaces.Remove(this);
			sOpenFaceCount--;
			FT_Done_Face(fFreeTypeFace);
		}
	} else
		FT_Done_Face(fFreeTypeFace);

	if (fFontData != NULL)
		free(fFontData);
}


bool
FontStyle::Lock()
{
	return sFontLock.Lock();
}


void
FontStyle::Unlock()
{
	// When the last lock goes, nobody uses a face any more
	if (sOpenFaceCount > kMaxOpenFaces && sFontLock.IsLocked()
		&& sFontLock.CountLocks() == 1) {
		_CloseUnusedFaces();
	}

	sFontLock.Unlock();
}


/*!	Returns the FreeType face of this style, opening its file again if it was
	closed. It returns NULL if the file cannot be opened any more.
*/
FT_Face
FontStyle::FreeTypeFace()
{
	if (!fOpenOnDemand)
		return fFreeTypeFace;

	BAutolock locker(sFontLock);

	if (fFreeTypeFace != NULL) {
		// the most recently used face goes last
		sOpenFaces.Remove(this);
		sOpenFaces.Add(this);
		return fFreeTypeFace;
	}

	fFreeTypeFace = _OpenFace();
	if (fFreeTypeFace != NULL) {
		sOpenFaces.Add(this);
		sOpenFaceCount++;
	}

	return fFreeTypeFace;
}


void
FontStyle::GetHeight(float size, font_height& height) const
{
	height.ascent = fHeight.ascent * size;
	height.descent = fHeight.descent * size;
	height.leading = fHeight.leading * size;
}


/*!
	\brief Returns the path to the style's font file
	\return The style's font file path
*/
const char*
FontStyle::Path() const
{
	return fPath.Path();
}


/*!
	\brief Updates the path of the font style in case the style
		has been moved around.
*/
void
FontStyle::UpdatePath(const node_ref& parentNodeRef)
{
	entry_ref ref;
	ref.device = parentNodeRef.device;
	ref.directory = parentNodeRef.node;
	ref.set_name(fPath.Leaf());

	fPath.SetTo(&ref);
}


/*!
	\brief Unlike BFont::Flags() this returns the extra flags field as used
		in the private part of BFont.
*/
uint32
FontStyle::Flags() const
{
	uint32 flags = uint32(Direction()) << B_PRIVATE_FONT_DIRECTION_SHIFT;

	if (IsFixedWidth())
		flags |= B_IS_FIXED;
	if (IsFullAndHalfFixed())
		flags |= B_PRIVATE_FONT_IS_FULL_AND_HALF_FIXED;
	if (TunedCount() > 0)
		flags |= B_HAS_TUNED_FONT;
	if (HasKerning())
		flags |= B_PRIVATE_FONT_HAS_KERNING;

	return flags;
}


/*!
	\brief Updates the given face to match the one from this style

	The specified font face often doesn't match the exact face of
	a style. This method will preserve the attributes of the face
	that this style does not alter, and will only update the
	attributes that matter to this style.
	The font renderer could then emulate the other face attributes
	taking this style as a base.
*/
uint16
FontStyle::PreservedFace(uint16 face) const
{
	// TODO: make this better
	face &= ~(B_REGULAR_FACE | B_BOLD_FACE | B_ITALIC_FACE | B_CONDENSED_FACE
		| B_LIGHT_FACE | B_HEAVY_FACE);
	face |= Face();

	return face;
}


void
FontStyle::_SetFontFamily(FontFamily* family, uint16 id)
{
	if (fFamily.IsSet())
		fFamily->RemoveStyle(this);

	fFamily.SetTo(family);
	fID = id;
}


/*!	Opens the face of this style from its file. sFontLock must be held: it
	protects the FreeType library the faces opened on demand share.
*/
FT_Face
FontStyle::_OpenFace()
{
	if (sFaceLibrary == NULL && FT_Init_FreeType(&sFaceLibrary) != 0) {
		sFaceLibrary = NULL;
		return NULL;
	}

	FT_Face face;
	if (FT_New_Face(sFaceLibrary, fPath.Path(), fFaceIndex, &face) != 0)
		return NULL;

	// the file may have been replaced by another font since it was indexed
	BString name(face->style_name);
	name.Truncate(B_FONT_STYLE_LENGTH);
	if (name != fName) {
		FT_Done_Face(face);
		return NULL;
	}

	return face;
}


/*!	Closes the least recently used faces until no more than kMaxOpenFaces
	are open. sFontLock must be held, and nobody may use a face.
*/
/*static*/ void
FontStyle::_CloseUnusedFaces()
{
	while (sOpenFaceCount > kMaxOpenFaces) {
		FontStyle* style = sOpenFaces.RemoveHead();
		if (style == NULL)
			break;

		FT_Done_Face(style->fFreeTypeFace);
		style->fFreeTypeFace = NULL;
		sOpenFaceCount--;
	}
}


uint16
FontStyle::_TranslateStyleToFace(const char* name) const
{
	if (name == NULL)
		return 0;

	BString string(name);
	uint16 face = 0;

	if (string.IFindFirst("bold") >= 0)
		face |= B_BOLD_FACE;

	if (string.IFindFirst("italic") >= 0
		|| string.IFindFirst("oblique") >= 0)
		face |= B_ITALIC_FACE;

	if (string.IFindFirst("condensed") >= 0)
		face |= B_CONDENSED_FACE;

	if (string.IFindFirst("light") >= 0
		|| string.IFindFirst("thin") >= 0)
		face |= B_LIGHT_FACE;

	if (string.IFindFirst("heavy") >= 0
		|| string.IFindFirst("black") >= 0)
		face |= B_HEAVY_FACE;

	if (face == 0)
		return B_REGULAR_FACE;

	return face;
}


void
FontStyle::SetFontData(FT_Byte* location, uint32 size)
{
	// if memory was already allocated here, we should free it so it's not leaked
	if (fFontData != NULL)
		free(fFontData);

	fFontDataSize = size;
	fFontData = location;
}
