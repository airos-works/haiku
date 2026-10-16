/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Quick Look for pictures: whatever the Translation Kit reads is shown,
 * scaled to fit, on a dark ground.
 */


#include <algorithm>
#include <stdio.h>
#include <string.h>

#include <Bitmap.h>
#include <BitmapStream.h>
#include <Catalog.h>
#include <File.h>
#include <Mime.h>
#include <Path.h>
#include <TranslationUtils.h>
#include <TranslatorRoster.h>

#include <add-ons/quick_look/QuickLook.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "QuickLook"


class ImagePreview : public BQuickLookPreview {
public:
	ImagePreview(BBitmap* bitmap, const char* format)
		:
		BQuickLookPreview("image preview"),
		fBitmap(bitmap),
		fFormat(format)
	{
		SetViewColor(46, 46, 46);
	}

	virtual ~ImagePreview()
	{
		delete fBitmap;
	}

	virtual BSize PreferredContentSize()
	{
		BRect bounds = fBitmap->Bounds();
		return BSize(bounds.Width() + 1, bounds.Height() + 1);
	}

	virtual BString Summary()
	{
		BRect bounds = fBitmap->Bounds();
		BString summary;
		summary.SetToFormat(B_TRANSLATE("%d \xC3\x97 %d pixels"),
			(int)bounds.IntegerWidth() + 1, (int)bounds.IntegerHeight() + 1);
		if (fFormat.Length() > 0)
			summary << " \xE2\x80\xA2 " << fFormat;
		return summary;
	}

	virtual void Draw(BRect updateRect)
	{
		BRect bounds = Bounds();
		BRect source = fBitmap->Bounds();
		float width = source.Width() + 1;
		float height = source.Height() + 1;
		float scale = std::min(bounds.Width() + 1, width)
			/ width;
		scale = std::min(scale, std::min(bounds.Height() + 1, height)
			/ height);
		// Small pictures are shown at their size, large ones fitted.
		float shownWidth = floorf(width * scale);
		float shownHeight = floorf(height * scale);
		BRect destination(0, 0, shownWidth - 1, shownHeight - 1);
		destination.OffsetTo(floorf((bounds.Width() - shownWidth + 1) / 2),
			floorf((bounds.Height() - shownHeight + 1) / 2));

		if (fBitmap->ColorSpace() == B_RGBA32
			|| fBitmap->ColorSpace() == B_RGBA32_BIG) {
			SetDrawingMode(B_OP_ALPHA);
			SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
		}
		DrawBitmap(fBitmap, source, destination,
			scale < 1 ? B_FILTER_BITMAP_BILINEAR : 0);
	}

private:
	BBitmap*	fBitmap;
	BString		fFormat;
};


class ImageAddOn : public BQuickLookAddOn {
public:
	ImageAddOn(image_id image)
		:
		BQuickLookAddOn(image)
	{
	}

	virtual float Supports(const entry_ref& ref, const char* mimeType)
	{
		if (strncmp(mimeType, "image/", 6) == 0)
			return 0.5f;
		return 0;
	}

	virtual BQuickLookPreview* CreatePreview(const entry_ref& ref,
		const char* mimeType, BString* error)
	{
		BFile file(&ref, B_READ_ONLY);
		if (file.InitCheck() != B_OK) {
			*error = strerror(file.InitCheck());
			return NULL;
		}

		BTranslatorRoster* roster = BTranslatorRoster::Default();
		translator_info info;
		memset(&info, 0, sizeof(info));
		if (roster->Identify(&file, NULL, &info, 0, NULL,
				B_TRANSLATOR_BITMAP) != B_OK) {
			*error = B_TRANSLATE("No translator reads this picture.");
			return NULL;
		}
		file.Seek(0, SEEK_SET);
		// The kind as the file type database names it ("PNG image").
		BString format;
		BMimeType type(mimeType);
		char description[B_MIME_TYPE_LENGTH];
		if (type.GetShortDescription(description) == B_OK)
			format = description;

		BBitmapStream stream;
		if (roster->Translate(&file, &info, NULL, &stream,
				B_TRANSLATOR_BITMAP) != B_OK) {
			*error = B_TRANSLATE("No translator reads this picture.");
			return NULL;
		}
		BBitmap* bitmap = NULL;
		if (stream.DetachBitmap(&bitmap) != B_OK || bitmap == NULL) {
			*error = B_TRANSLATE("The picture could not be decoded.");
			return NULL;
		}
		return new ImagePreview(bitmap, format.String());
	}
};


extern "C" BQuickLookAddOn*
instantiate_quick_look_add_on(image_id image)
{
	return new ImageAddOn(image);
}
