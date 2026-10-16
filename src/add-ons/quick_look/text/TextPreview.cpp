/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Quick Look for text: plain text, source code, JSON, XML and the like
 * are shown in a read-only text view; CSV as a table.
 */


#include <algorithm>
#include <stdio.h>
#include <string.h>

#include <Catalog.h>
#include <File.h>
#include <Font.h>
#include <ScrollView.h>
#include <StringFormat.h>
#include <TextView.h>

#include <add-ons/quick_look/QuickLook.h>

#include "TablePreview.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "QuickLook"


static const off_t kMaxBytes = 2 * 1024 * 1024;
static const char* const kOctetStream = "application/octet-stream";


static bool
is_valid_utf8(const char* text, size_t length)
{
	const uint8* s = (const uint8*)text;
	size_t i = 0;
	while (i < length) {
		uint8 c = s[i];
		size_t extra;
		if (c < 0x80) {
			i++;
			continue;
		} else if ((c & 0xE0) == 0xC0)
			extra = 1;
		else if ((c & 0xF0) == 0xE0)
			extra = 2;
		else if ((c & 0xF8) == 0xF0)
			extra = 3;
		else
			return false;
		if (i + extra >= length + (i + extra == length ? 1 : 0)) {
			// A sequence cut off by the end of what was read is fine.
			return i + extra >= length;
		}
		for (size_t j = 1; j <= extra; j++) {
			if ((s[i + j] & 0xC0) != 0x80)
				return false;
		}
		i += extra + 1;
	}
	return true;
}


static bool
looks_like_text(const char* data, size_t length)
{
	if (length == 0)
		return true;
	size_t control = 0;
	for (size_t i = 0; i < length; i++) {
		uint8 c = (uint8)data[i];
		if (c == 0)
			return false;
		if (c < 32 && c != '\n' && c != '\r' && c != '\t' && c != '\f'
			&& c != 0x1b)
			control++;
	}
	return control * 50 < length;
}


static void
latin1_to_utf8(const char* source, size_t length, BString& target)
{
	target.Truncate(0);
	char* buffer = target.LockBuffer(length * 2 + 1);
	size_t out = 0;
	for (size_t i = 0; i < length; i++) {
		uint8 c = (uint8)source[i];
		if (c < 0x80)
			buffer[out++] = c;
		else {
			buffer[out++] = 0xC0 | (c >> 6);
			buffer[out++] = 0x80 | (c & 0x3F);
		}
	}
	buffer[out] = '\0';
	target.UnlockBuffer(out);
}


class TextPreview : public BQuickLookPreview {
public:
	TextPreview(const BString& text, bool monospace, bool wrap,
		bool truncated, const char* encoding)
		:
		BQuickLookPreview("text preview"),
		fTruncated(truncated),
		fEncoding(encoding)
	{
		BRect bounds(0, 0, 639, 479);
		BRect textRect(bounds);
		textRect.InsetBy(8, 6);
		fText = new BTextView(bounds, "text", textRect, B_FOLLOW_ALL,
			B_WILL_DRAW | B_FRAME_EVENTS | B_NAVIGABLE);
		fText->MakeEditable(false);
		fText->SetStylable(false);
		fText->SetWordWrap(wrap);
		fText->SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
		fText->SetLowUIColor(B_DOCUMENT_BACKGROUND_COLOR);
		BFont font(monospace ? be_fixed_font : be_plain_font);
		rgb_color color = ui_color(B_DOCUMENT_TEXT_COLOR);
		fText->SetFontAndColor(&font, B_FONT_ALL, &color);
		fText->SetTabWidth(font.StringWidth("    "));
		fText->SetText(text.String(), text.Length());

		fLines = fText->CountLines();
		fScroll = new BScrollView("scroll", fText, B_FOLLOW_ALL, 0, !wrap,
			true, B_NO_BORDER);
		AddChild(fScroll);
	}

	virtual BSize PreferredContentSize()
	{
		return BSize(fText->TextRect().Width() > 900 ? 1000 : 700, 620);
	}

	virtual BString Summary()
	{
		BString summary;
		static BStringFormat format(B_TRANSLATE("{0, plural, "
			"=1{one line} other{# lines}}"));
		format.Format(summary, fLines);
		summary << " \xE2\x80\xA2 " << fEncoding;
		if (fTruncated)
			summary << " \xE2\x80\xA2 " << B_TRANSLATE("first 2 MiB");
		return summary;
	}

	virtual void AttachedToWindow()
	{
		BQuickLookPreview::AttachedToWindow();
		_Fit();
	}

	virtual void FrameResized(float width, float height)
	{
		BQuickLookPreview::FrameResized(width, height);
		_Fit();
	}

private:
	void _Fit()
	{
		fScroll->ResizeTo(Bounds().Width(), Bounds().Height());
		BRect textRect = fText->Bounds();
		textRect.InsetBy(8, 6);
		if (!fText->DoesWordWrap())
			textRect.right = textRect.left + std::max(textRect.Width(),
				fText->TextRect().Width());
		fText->SetTextRect(textRect);
	}

	BTextView*		fText;
	BScrollView*	fScroll;
	int32			fLines;
	bool			fTruncated;
	BString			fEncoding;
};


class TextAddOn : public BQuickLookAddOn {
public:
	TextAddOn(image_id image)
		:
		BQuickLookAddOn(image)
	{
	}

	virtual float Supports(const entry_ref& ref, const char* mimeType)
	{
		if (strncmp(mimeType, "text/", 5) == 0)
			return 0.5f;
		static const char* kTypes[] = {
			"application/json", "application/xml", "application/javascript",
			"application/x-javascript", "application/x-sh",
			"application/x-shellscript", "application/x-perl",
			"application/x-python", "application/x-yaml",
			"application/yaml", "application/toml", "application/x-csh",
			"application/x-vnd.Be.URL.http", "application/x-vnd.Be.URL.https",
			"application/x-vnd.Be.URL.file", "application/x-desktop",
			"application/x-wine-extension-ini", "application/rss+xml",
			"application/atom+xml", "application/xhtml+xml",
			"application/x-makefile", "application/x-ruby",
			"application/x-php", "application/sql", "application/x-tex",
			"application/x-troff-man", "application/ld+json",
			"application/x-ndjson", "application/x-subrip",
			NULL
		};
		for (int i = 0; kTypes[i] != NULL; i++) {
			if (strcasecmp(mimeType, kTypes[i]) == 0)
				return 0.6f;
		}
		// Untyped files that read as text are shown as text.
		if (strcmp(mimeType, "application/octet-stream") == 0
			|| strcmp(mimeType, kOctetStream) == 0) {
			BFile file(&ref, B_READ_ONLY);
			char buffer[4096];
			ssize_t read = file.Read(buffer, sizeof(buffer));
			if (read > 0 && looks_like_text(buffer, read))
				return 0.3f;
		}
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
		off_t size = 0;
		file.GetSize(&size);
		bool truncated = size > kMaxBytes;
		size_t length = truncated ? kMaxBytes : size;
		BString raw;
		char* buffer = raw.LockBuffer(length + 1);
		if (buffer == NULL) {
			*error = strerror(B_NO_MEMORY);
			return NULL;
		}
		ssize_t read = file.Read(buffer, length);
		if (read < 0) {
			raw.UnlockBuffer(0);
			*error = strerror(read);
			return NULL;
		}
		buffer[read] = '\0';
		if (memchr(buffer, '\0', read) != NULL) {
			raw.UnlockBuffer(0);
			*error = B_TRANSLATE("This is not a text file.");
			return NULL;
		}
		raw.UnlockBuffer(read);

		BString text;
		const char* encoding = "UTF-8";
		if (is_valid_utf8(raw.String(), raw.Length()))
			text = raw;
		else {
			latin1_to_utf8(raw.String(), raw.Length(), text);
			encoding = "Latin-1";
		}
		// A UTF-8 byte order mark shows as a glyph otherwise.
		if (text.Length() >= 3 && (uint8)text[0] == 0xEF
			&& (uint8)text[1] == 0xBB && (uint8)text[2] == 0xBF)
			text.Remove(0, 3);

		// The MIME database files many of these as text/plain; the name
		// says more.
		const char* extension = strrchr(ref.name, '.');
		extension = extension != NULL ? extension + 1 : "";

		if (strcasecmp(mimeType, "text/csv") == 0
			|| strcasecmp(mimeType, "text/comma-separated-values") == 0
			|| strcasecmp(mimeType, "text/tab-separated-values") == 0
			|| strcasecmp(mimeType, "text/x-comma-separated-values") == 0
			|| strcasecmp(extension, "csv") == 0
			|| strcasecmp(extension, "tsv") == 0)
			return new TablePreview(text, truncated);

		bool plain = strcasecmp(mimeType, "text/plain") == 0
			|| strcasecmp(mimeType, "text/markdown") == 0
			|| strcasecmp(mimeType, "text/x-markdown") == 0
			|| strcasecmp(mimeType, "text/x-email") == 0
			|| strcasecmp(mimeType, "text/rtf") == 0
			|| strcmp(mimeType, kOctetStream) == 0;
		if (plain) {
			static const char* kCodeExtensions[] = {
				"json", "xml", "yaml", "yml", "toml", "ini", "cfg", "conf",
				"c", "h", "cpp", "cc", "cxx", "hpp", "m", "mm", "java", "js",
				"ts", "py", "rb", "pl", "sh", "bash", "zsh", "php", "go",
				"rs", "swift", "kt", "lua", "sql", "css", "html", "htm",
				"svg", "rdef", "jam", "mk", "make", "cmake", "diff", "patch",
				"log", "srt", "vtt", "ass", "tex", "bib", "plist", "ps1",
				"bat", "asm", "s", "hs", "ml", "erl", "ex", "dart", "scala",
				"r", "jl", "nim", "zig", "v", "sv", "vhd",
				NULL
			};
			for (int i = 0; kCodeExtensions[i] != NULL; i++) {
				if (strcasecmp(extension, kCodeExtensions[i]) == 0) {
					plain = false;
					break;
				}
			}
		}
		return new TextPreview(text, !plain, plain, truncated, encoding);
	}
};


extern "C" BQuickLookAddOn*
instantiate_quick_look_add_on(image_id image)
{
	return new TextAddOn(image);
}
