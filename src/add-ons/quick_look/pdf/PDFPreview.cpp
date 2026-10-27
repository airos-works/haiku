/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 *
 * Quick Look for documents MuPDF reads: PDF, XPS, EPUB, FB2 and comic
 * book archives. The pages are stacked in a scrolling view at the width
 * of the panel and rendered on a thread of their own as they come into
 * view, at the desktop's density so that text stays sharp.
 */


#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <stdio.h>
#include <string.h>

#include <Bitmap.h>
#include <Catalog.h>
#include <Font.h>
#include <Messenger.h>
#include <Path.h>
#include <Screen.h>
#include <ScrollBar.h>
#include <StringFormat.h>
#include <Window.h>

#include <add-ons/quick_look/QuickLook.h>

extern "C" {
#include <mupdf/fitz.h>
}


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "QuickLook"


static const uint32 kMsgDocumentReady = 'pdoc';
static const uint32 kMsgPageReady = 'ppag';
static const float kPageGap = 14;
static const float kMargin = 14;
static const int32 kMaxCachedPages = 24;


struct PageInfo {
	float	width;		// in points (1/72 inch)
	float	height;
};


/*!	Owns the MuPDF context and document; everything MuPDF touches happens
	on this thread, which MuPDF requires of a context without locks. */
class Renderer {
public:
	Renderer(const char* path, const BMessenger& target)
		:
		fPath(path),
		fTarget(target),
		fQuit(false),
		fScale(1),
		fGeneration(0)
	{
		fThread = std::thread(&Renderer::_Run, this);
	}

	~Renderer()
	{
		{
			std::lock_guard<std::mutex> lock(fLock);
			fQuit = true;
			fWake.notify_all();
		}
		fThread.join();
	}

	//! Asks for pages [first, last] at the scale; earlier requests at
	//! another scale are dropped.
	void Request(int32 first, int32 last, float scale)
	{
		std::lock_guard<std::mutex> lock(fLock);
		if (scale != fScale) {
			fScale = scale;
			fGeneration++;
			fQueue.clear();
		}
		for (int32 page = first; page <= last; page++) {
			if (std::find(fQueue.begin(), fQueue.end(), page) == fQueue.end())
				fQueue.push_back(page);
		}
		fWake.notify_all();
	}

	void ForgetAll()
	{
		std::lock_guard<std::mutex> lock(fLock);
		fQueue.clear();
	}

private:
	void _Run()
	{
		fz_context* context = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
		if (context == NULL) {
			_Fail("MuPDF could not start.");
			return;
		}
		fz_document* document = NULL;
		fz_try(context) {
			fz_register_document_handlers(context);
			document = fz_open_document(context, fPath.String());
		} fz_catch(context) {
			_Fail(fz_caught_message(context));
			fz_drop_context(context);
			return;
		}
		if (fz_needs_password(context, document)) {
			_Fail(B_TRANSLATE("The document is protected by a password."));
			fz_drop_document(context, document);
			fz_drop_context(context);
			return;
		}

		// Reflowable books are laid out for a page of readable size.
		fz_try(context) {
			if (fz_is_document_reflowable(context, document))
				fz_layout_document(context, document, 480, 640, 12);
		} fz_catch(context) {
		}

		BMessage ready(kMsgDocumentReady);
		int32 count = 0;
		fz_try(context) {
			count = fz_count_pages(context, document);
			for (int32 i = 0; i < count; i++) {
				fz_page* page = fz_load_page(context, document, i);
				fz_rect bounds = fz_bound_page(context, page);
				fz_drop_page(context, page);
				ready.AddFloat("width", bounds.x1 - bounds.x0);
				ready.AddFloat("height", bounds.y1 - bounds.y0);
			}
			char buffer[256];
			if (fz_lookup_metadata(context, document, FZ_META_INFO_TITLE,
					buffer, sizeof(buffer)) > 0 && buffer[0] != '\0')
				ready.AddString("title", buffer);
			if (fz_lookup_metadata(context, document, FZ_META_FORMAT, buffer,
					sizeof(buffer)) > 0)
				ready.AddString("format", buffer);
		} fz_catch(context) {
			_Fail(fz_caught_message(context));
			fz_drop_document(context, document);
			fz_drop_context(context);
			return;
		}
		ready.AddInt32("pages", count);
		fTarget.SendMessage(&ready);

		while (true) {
			int32 pageNumber;
			float scale;
			int32 generation;
			{
				std::unique_lock<std::mutex> lock(fLock);
				fWake.wait(lock, [&] { return fQuit || !fQueue.empty(); });
				if (fQuit)
					break;
				pageNumber = fQueue.front();
				fQueue.pop_front();
				scale = fScale;
				generation = fGeneration;
			}
			BBitmap* bitmap = _RenderPage(context, document, pageNumber, scale);
			if (bitmap == NULL)
				continue;
			BMessage done(kMsgPageReady);
			done.AddInt32("page", pageNumber);
			done.AddInt32("generation", generation);
			done.AddFloat("scale", scale);
			done.AddPointer("bitmap", bitmap);
			if (fTarget.SendMessage(&done) != B_OK)
				delete bitmap;
		}

		fz_drop_document(context, document);
		fz_drop_context(context);
	}

	BBitmap* _RenderPage(fz_context* context, fz_document* document,
		int32 number, float scale)
	{
		fz_pixmap* pixmap = NULL;
		BBitmap* bitmap = NULL;
		fz_try(context) {
			pixmap = fz_new_pixmap_from_page_number(context, document, number,
				fz_scale(scale, scale), fz_device_bgr(context), 0);
			int width = fz_pixmap_width(context, pixmap);
			int height = fz_pixmap_height(context, pixmap);
			int components = fz_pixmap_components(context, pixmap);
			int stride = fz_pixmap_stride(context, pixmap);
			const uint8* samples = fz_pixmap_samples(context, pixmap);
			bitmap = new BBitmap(BRect(0, 0, width - 1, height - 1), B_RGB32);
			if (bitmap->IsValid() && components == 3) {
				uint8* bits = (uint8*)bitmap->Bits();
				int32 bytesPerRow = bitmap->BytesPerRow();
				for (int y = 0; y < height; y++) {
					const uint8* in = samples + y * stride;
					uint8* out = bits + y * bytesPerRow;
					for (int x = 0; x < width; x++) {
						out[0] = in[0];
						out[1] = in[1];
						out[2] = in[2];
						out[3] = 255;
						in += 3;
						out += 4;
					}
				}
			} else {
				delete bitmap;
				bitmap = NULL;
			}
		} fz_always(context) {
			fz_drop_pixmap(context, pixmap);
		} fz_catch(context) {
			delete bitmap;
			bitmap = NULL;
		}
		return bitmap;
	}

	void _Fail(const char* why)
	{
		BMessage failed(kMsgDocumentReady);
		failed.AddString("error", why != NULL ? why : "");
		fTarget.SendMessage(&failed);
	}

	BString					fPath;
	BMessenger				fTarget;
	std::thread				fThread;
	std::mutex				fLock;
	std::condition_variable	fWake;
	bool					fQuit;
	std::deque<int32>		fQueue;
	float					fScale;
	int32					fGeneration;
};


class DocumentPreview : public BQuickLookPreview {
public:
	DocumentPreview(const entry_ref& ref, const char* mimeType)
		:
		BQuickLookPreview("document preview"),
		fRef(ref),
		fMimeType(mimeType),
		fRenderer(NULL),
		fScrollBar(NULL),
		fPageCount(-1),
		fScale(0),
		fGeneration(0),
		fDeviceScale(1),
		fScrollOffset(0)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
		// The desktop may be drawn at a higher density than the view
		// coordinates say: render the pages at that density.
		BScreen screen;
		display_mode mode;
		if (screen.GetMode(&mode) == B_OK && screen.Frame().Width() > 0) {
			float ratio = mode.virtual_width / (screen.Frame().Width() + 1);
			if (ratio >= 1 && ratio <= 4)
				fDeviceScale = ratio;
		}
	}

	virtual ~DocumentPreview()
	{
		delete fRenderer;
		_DropPages();
	}

	// BQuickLookPreview

	virtual BSize PreferredContentSize()
	{
		// A page of letter or A4 size at a comfortable reading scale, as
		// tall as a page and a bit, so the next page shows below.
		if (fPageCount > 0) {
			float width = std::max(fPages[0].width, 400.0f);
			float scale = std::min(1.3f, 720 / width);
			return BSize(width * scale + 2 * kMargin,
				std::min(fPages[0].height * scale + 2 * kMargin, 900.0f));
		}
		return BSize(640, 800);
	}

	virtual BString Summary()
	{
		BString summary;
		if (fError.Length() > 0)
			return fError;
		if (fPageCount < 0)
			return B_TRANSLATE("Opening" B_UTF8_ELLIPSIS);
		static BStringFormat format(B_TRANSLATE("{0, plural, "
			"=1{one page} other{# pages}}"));
		format.Format(summary, fPageCount);
		if (fTitle.Length() > 0)
			summary << " \xE2\x80\xA2 " << fTitle;
		if (fFormat.Length() > 0)
			summary << " \xE2\x80\xA2 " << fFormat;
		return summary;
	}

	virtual void Start()
	{
	}

	virtual void Stop()
	{
		// The renderer must not post to a view that is going away.
		delete fRenderer;
		fRenderer = NULL;
	}

	// BView

	virtual void AttachedToWindow()
	{
		BQuickLookPreview::AttachedToWindow();
		fScrollBar = new BScrollBar("pages", NULL, 0, 0, B_VERTICAL);
		fScrollBar->SetTarget(this);
		AddChild(fScrollBar);
		_Layout();
		BPath path(&fRef);
		fRenderer = new Renderer(path.Path(), BMessenger(this));
	}

	virtual void FrameResized(float width, float height)
	{
		BQuickLookPreview::FrameResized(width, height);
		_Layout();
	}

	virtual void ScrollTo(BPoint where)
	{
		// The scroll bar's target: scroll the pages, not the view.
		fScrollOffset = where.y;
		Invalidate();
		_RequestVisible();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case kMsgDocumentReady:
			{
				const char* error;
				if (message->FindString("error", &error) == B_OK) {
					fError = error;
					fPageCount = 0;
					NotifyChanged();
					Invalidate();
					break;
				}
				fPageCount = message->GetInt32("pages", 0);
				fPages.clear();
				for (int32 i = 0; i < fPageCount; i++) {
					PageInfo info;
					info.width = std::max(1.0f, message->GetFloat("width", i, 1));
					info.height = std::max(1.0f,
						message->GetFloat("height", i, 1));
					fPages.push_back(info);
				}
				message->FindString("title", &fTitle);
				message->FindString("format", &fFormat);
				NotifyChanged();
				_Layout();
				break;
			}

			case kMsgPageReady:
			{
				int32 page = message->GetInt32("page", -1);
				int32 generation = message->GetInt32("generation", -1);
				BBitmap* bitmap = NULL;
				message->FindPointer("bitmap", (void**)&bitmap);
				if (bitmap == NULL)
					break;
				if (generation != fGeneration || page < 0
					|| page >= fPageCount) {
					delete bitmap;
					break;
				}
				delete fBitmaps[page];
				fBitmaps[page] = bitmap;
				fRecent.push_back(page);
				_TrimCache();
				Invalidate(_PageFrame(page));
				break;
			}

			case B_MOUSE_WHEEL_CHANGED:
			{
				float delta = message->GetFloat("be:wheel_delta_y", 0);
				if (delta != 0 && fScrollBar != NULL) {
					fScrollBar->SetValue(fScrollBar->Value()
						+ delta * fScrollStep);
				}
				break;
			}

			default:
				BQuickLookPreview::MessageReceived(message);
		}
	}

	virtual void Draw(BRect updateRect)
	{
		SetHighColor(96, 96, 96);
		FillRect(updateRect);
		if (fPageCount <= 0) {
			_DrawMessage(fError.Length() > 0 ? fError.String()
				: B_TRANSLATE("Opening" B_UTF8_ELLIPSIS));
			return;
		}
		for (int32 i = 0; i < fPageCount; i++) {
			BRect frame = _PageFrame(i);
			if (!frame.Intersects(updateRect))
				continue;
			// Shadow, then the paper.
			SetHighColor(60, 60, 60);
			FillRect(frame.OffsetByCopy(2, 2));
			BBitmap* bitmap = fBitmaps.count(i) > 0 ? fBitmaps[i] : NULL;
			if (bitmap != NULL) {
				DrawBitmap(bitmap, bitmap->Bounds(), frame,
					fDeviceScale != 1 ? B_FILTER_BITMAP_BILINEAR : 0);
			} else {
				SetHighColor(255, 255, 255);
				FillRect(frame);
			}
		}
	}

private:
	void _Layout()
	{
		BRect bounds = Bounds();
		float barWidth = be_plain_font->Size() * 1.2f;
		if (fScrollBar != NULL) {
			fScrollBar->MoveTo(bounds.right - barWidth + 1, 0);
			fScrollBar->ResizeTo(barWidth, bounds.Height());
		}
		float available = bounds.Width() - barWidth - 2 * kMargin;
		if (fPageCount <= 0 || available < 10)
			return;

		float widest = 1;
		for (int32 i = 0; i < fPageCount; i++)
			widest = std::max(widest, fPages[i].width);
		float scale = available / widest;
		if (scale != fScale) {
			fScale = scale;
			fGeneration++;
			_DropPages();
		}
		float total = kMargin;
		for (int32 i = 0; i < fPageCount; i++)
			total += fPages[i].height * fScale + kPageGap;
		total += kMargin - kPageGap;
		if (fScrollBar != NULL) {
			float range = std::max(0.0f, total - bounds.Height());
			fScrollBar->SetRange(0, range);
			fScrollBar->SetProportion(bounds.Height() / std::max(total, 1.0f));
			fScrollStep = 60;
			fScrollBar->SetSteps(fScrollStep, bounds.Height() * 0.9f);
			if (fScrollOffset > range)
				fScrollOffset = range;
		}
		Invalidate();
		_RequestVisible();
	}

	BRect _PageFrame(int32 page) const
	{
		float y = kMargin - fScrollOffset;
		for (int32 i = 0; i < page; i++)
			y += fPages[i].height * fScale + kPageGap;
		float width = fPages[page].width * fScale;
		float height = fPages[page].height * fScale;
		float x = kMargin;
		return BRect(floorf(x), floorf(y), floorf(x + width) - 1,
			floorf(y + height) - 1);
	}

	void _RequestVisible()
	{
		if (fRenderer == NULL || fPageCount <= 0 || fScale <= 0)
			return;
		BRect bounds = Bounds();
		int32 first = -1;
		int32 last = -1;
		for (int32 i = 0; i < fPageCount; i++) {
			BRect frame = _PageFrame(i);
			if (frame.bottom < bounds.top - bounds.Height())
				continue;
			if (frame.top > bounds.bottom + bounds.Height())
				break;
			if (first < 0)
				first = i;
			last = i;
		}
		if (first < 0)
			return;
		// What is on screen first, then a page either side.
		int32 wantedFirst = first;
		int32 wantedLast = last;
		for (int32 i = first; i <= last; i++) {
			BRect frame = _PageFrame(i);
			if (frame.bottom >= bounds.top && frame.top <= bounds.bottom
				&& fBitmaps.count(i) == 0) {
				fRenderer->Request(i, i, fScale * fDeviceScale);
			}
		}
		for (int32 i = wantedFirst; i <= wantedLast; i++) {
			if (fBitmaps.count(i) == 0)
				fRenderer->Request(i, i, fScale * fDeviceScale);
		}
	}

	void _TrimCache()
	{
		while ((int32)fRecent.size() > kMaxCachedPages) {
			int32 old = fRecent.front();
			fRecent.pop_front();
			if (fBitmaps.count(old) > 0 && std::find(fRecent.begin(),
					fRecent.end(), old) == fRecent.end()) {
				delete fBitmaps[old];
				fBitmaps.erase(old);
			}
		}
	}

	void _DropPages()
	{
		for (std::map<int32, BBitmap*>::iterator it = fBitmaps.begin();
				it != fBitmaps.end(); ++it)
			delete it->second;
		fBitmaps.clear();
		fRecent.clear();
		if (fRenderer != NULL)
			fRenderer->ForgetAll();
	}

	void _DrawMessage(const char* text)
	{
		BRect bounds = Bounds();
		SetFont(be_plain_font);
		SetHighColor(200, 200, 200);
		font_height height;
		GetFontHeight(&height);
		float width = StringWidth(text);
		DrawString(text, BPoint(floorf((bounds.Width() - width) / 2),
			floorf(bounds.Height() / 2 + height.ascent / 2)));
	}

	entry_ref				fRef;
	BString					fMimeType;
	Renderer*				fRenderer;
	BScrollBar*				fScrollBar;
	int32					fPageCount;
	std::vector<PageInfo>	fPages;
	std::map<int32, BBitmap*> fBitmaps;
	std::deque<int32>		fRecent;
	float					fScale;
	int32					fGeneration;
	float					fDeviceScale;
	float					fScrollOffset;
	float					fScrollStep = 60;
	BString					fTitle;
	BString					fFormat;
	BString					fError;
};


class PDFAddOn : public BQuickLookAddOn {
public:
	PDFAddOn(image_id image)
		:
		BQuickLookAddOn(image)
	{
	}

	virtual float Supports(const entry_ref& ref, const char* mimeType)
	{
		if (strcasecmp(mimeType, "application/pdf") == 0
			|| strcasecmp(mimeType, "application/x-pdf") == 0)
			return 0.8f;
		static const char* kOthers[] = {
			"application/vnd.ms-xpsdocument", "application/oxps",
			"application/epub+zip", "application/x-fictionbook+xml",
			"application/vnd.comicbook+zip", "application/x-cbz",
			"application/x-cbr", "application/x-mobipocket-ebook",
			NULL
		};
		for (int i = 0; kOthers[i] != NULL; i++) {
			if (strcasecmp(mimeType, kOthers[i]) == 0)
				return 0.7f;
		}
		// Untyped files by name.
		const char* extension = strrchr(ref.name, '.');
		if (extension != NULL && (strcmp(mimeType,
				"application/octet-stream") == 0)) {
			extension++;
			if (strcasecmp(extension, "pdf") == 0
				|| strcasecmp(extension, "xps") == 0
				|| strcasecmp(extension, "epub") == 0
				|| strcasecmp(extension, "cbz") == 0
				|| strcasecmp(extension, "fb2") == 0)
				return 0.6f;
		}
		return 0;
	}

	virtual BQuickLookPreview* CreatePreview(const entry_ref& ref,
		const char* mimeType, BString* error)
	{
		return new DocumentPreview(ref, mimeType);
	}
};


extern "C" BQuickLookAddOn*
instantiate_quick_look_add_on(image_id image)
{
	return new PDFAddOn(image);
}
