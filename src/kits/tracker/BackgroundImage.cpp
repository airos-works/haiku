/*
Open Tracker License

Terms and Conditions

Copyright (c) 1991-2000, Be Incorporated. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice applies to all licensees
and shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF TITLE, MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
BE INCORPORATED BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF, OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of Be Incorporated shall not be
used in advertising or otherwise to promote the sale, use or other dealings in
this Software without prior written authorization from Be Incorporated.

Tracker(TM), Be(R), BeOS(R), and BeIA(TM) are trademarks or registered trademarks
of Be Incorporated in the United States and other countries. Other brand product
names are registered trademarks or trademarks of their respective holders.
All rights reserved.
*/

//  Classes used for setting up and managing background images


#include <Bitmap.h>
#include <ByteOrder.h>
#include <ControlLook.h>
#include <DataIO.h>
#include <File.h>
#include <Node.h>
#include <TranslationKit.h>
#include <View.h>
#include <Window.h>

#include <fs_attr.h>
#include <math.h>
#include <string.h>

#include <InterfacePrivate.h>
#include <ViewPrivate.h>

#include <algorithm>
#include <vector>

#include "BackgroundImage.h"

#include "Background.h"
#include "Commands.h"
#include "PoseView.h"


namespace BPrivate {

const char* kBackgroundImageInfo 			= B_BACKGROUND_INFO;
const char* kBackgroundImageInfoOffset 		= B_BACKGROUND_ORIGIN;
const char* kBackgroundImageInfoTextOutline	= B_BACKGROUND_TEXT_OUTLINE;
const char* kBackgroundImageInfoMode 		= B_BACKGROUND_MODE;
const char* kBackgroundImageInfoWorkspaces 	= B_BACKGROUND_WORKSPACES;
const char* kBackgroundImageInfoPath 		= B_BACKGROUND_IMAGE;

}	// namespace BPrivate


/*!	A picture file the entries show; entries with the same file share it.
	Only the pictures of the entry on screen and of the one shown before it
	hold pixels, and only of the part that is on screen, at the size it is
	drawn at (a 1920x1080 picture fitted to 1024x768 is held as about
	1024x768). The file is decoded again when another part or size is
	needed: on another workspace, in a larger mode, or with another monitor.
*/
struct BackgroundImage::Picture {
	Picture(const char* path)
		:
		path(path),
		width(0),
		height(0),
		bitmap(NULL)
	{
	}

	~Picture()
	{
		delete bitmap;
	}

	BString			path;
	int32			width;
	int32			height;
		// of the file; 0 before it was decoded, -1 if it cannot be
	BBitmap*		bitmap;
	clipping_rect	crop;
		// the part of the file bitmap holds, scaled to its size
};


namespace {

/*!	A picture file decoded into an area of its own, deleted with it: not
	into a bitmap of the app_server, and not on the heap, which would keep
	some of the pages after the picture is held at a smaller size.
	The translator writes a TranslatorBitmap header, then the pixels.
*/
class DecodedFile : public BPositionIO {
public:
	DecodedFile()
		:
		fArea(-1),
		fBits(NULL),
		fCapacity(0),
		fPosition(0),
		fSize(0),
		fHeaderWritten(0)
	{
	}

	virtual ~DecodedFile()
	{
		if (fArea >= 0)
			delete_area(fArea);
	}

	//! Whether all of the pixels the header announces were written.
	bool IsValid() const
	{
		return fBits != NULL
			&& fSize >= (off_t)(sizeof(TranslatorBitmap) + header.dataSize);
	}

	int32 Width() const
	{
		return header.bounds.IntegerWidth() + 1;
	}

	int32 Height() const
	{
		return header.bounds.IntegerHeight() + 1;
	}

	const uint8* Bits() const
	{
		return fBits;
	}

	virtual ssize_t ReadAt(off_t position, void* buffer, size_t size)
	{
		if (position < 0)
			return B_BAD_VALUE;
		if (position >= fSize)
			return 0;
		size = std::min((off_t)size, fSize - position);
		for (size_t done = 0; done < size; ) {
			off_t at = position + done;
			size_t bytes;
			if (at < (off_t)sizeof(TranslatorBitmap)) {
				bytes = std::min(size - done,
					(size_t)(sizeof(TranslatorBitmap) - at));
				memcpy((uint8*)buffer + done, fRawHeader + at, bytes);
			} else {
				bytes = size - done;
				memcpy((uint8*)buffer + done,
					fBits + at - sizeof(TranslatorBitmap), bytes);
			}
			done += bytes;
		}
		return size;
	}

	virtual ssize_t WriteAt(off_t position, const void* buffer, size_t size)
	{
		if (position < 0)
			return B_BAD_VALUE;
		for (size_t done = 0; done < size; ) {
			off_t at = position + done;
			size_t bytes;
			if (at < (off_t)sizeof(TranslatorBitmap)) {
				bytes = std::min(size - done,
					(size_t)(sizeof(TranslatorBitmap) - at));
				memcpy(fRawHeader + at, (const uint8*)buffer + done, bytes);
				fHeaderWritten = std::max(fHeaderWritten, at + (off_t)bytes);
				if (fHeaderWritten == sizeof(TranslatorBitmap)) {
					status_t status = _Allocate();
					if (status != B_OK)
						return status;
				}
			} else {
				off_t offset = at - sizeof(TranslatorBitmap);
				if (fBits == NULL || offset >= (off_t)fCapacity)
					return B_BAD_DATA;
				bytes = std::min(size - done, (size_t)(fCapacity - offset));
				memcpy(fBits + offset, (const uint8*)buffer + done, bytes);
			}
			done += bytes;
			fSize = std::max(fSize, at + (off_t)bytes);
		}
		return size;
	}

	virtual off_t Seek(off_t position, uint32 seekMode)
	{
		if (seekMode == SEEK_CUR)
			position += fPosition;
		else if (seekMode == SEEK_END)
			position += fSize;
		if (position < 0)
			return B_BAD_VALUE;
		fPosition = position;
		return fPosition;
	}

	virtual off_t Position() const
	{
		return fPosition;
	}

	virtual ssize_t Read(void* buffer, size_t size)
	{
		ssize_t result = ReadAt(fPosition, buffer, size);
		if (result > 0)
			fPosition += result;
		return result;
	}

	virtual ssize_t Write(const void* buffer, size_t size)
	{
		ssize_t result = WriteAt(fPosition, buffer, size);
		if (result > 0)
			fPosition += result;
		return result;
	}

	virtual status_t SetSize(off_t size)
	{
		if (size < 0
			|| size > (off_t)(sizeof(TranslatorBitmap) + fCapacity)) {
			return B_NOT_SUPPORTED;
		}
		fSize = size;
		return B_OK;
	}

	virtual status_t GetSize(off_t* size) const
	{
		*size = fSize;
		return B_OK;
	}

private:
	status_t _Allocate()
	{
		if (fArea >= 0)
			return B_OK;

		memcpy((void*)&header, fRawHeader, sizeof(TranslatorBitmap));
		swap_data(B_UINT32_TYPE, &header, sizeof(TranslatorBitmap),
			B_SWAP_BENDIAN_TO_HOST);
		if (header.magic != B_TRANSLATOR_BITMAP || !header.bounds.IsValid()
			|| header.bounds.left != 0 || header.bounds.top != 0
			|| (uint64)header.rowBytes * Height() > header.dataSize
			|| header.dataSize > 1024 * 1024 * 1024) {
			return B_BAD_DATA;
		}
		size_t pixelChunk;
		size_t pixelsPerChunk;
		if (get_pixel_size_for(header.colors, &pixelChunk, NULL,
				&pixelsPerChunk) != B_OK
			|| header.rowBytes < (Width() + pixelsPerChunk - 1)
				/ pixelsPerChunk * pixelChunk) {
			return B_BAD_DATA;
		}

		fCapacity = (header.dataSize + B_PAGE_SIZE - 1) & ~(B_PAGE_SIZE - 1);
		fArea = create_area("decoded background", (void**)&fBits,
			B_ANY_ADDRESS, fCapacity, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
		if (fArea < 0) {
			fBits = NULL;
			return fArea;
		}
		return B_OK;
	}

public:
	TranslatorBitmap	header;

private:
	area_id				fArea;
	uint8*				fBits;
	size_t				fCapacity;
	off_t				fPosition;
	off_t				fSize;
	off_t				fHeaderWritten;
	uint8				fRawHeader[sizeof(TranslatorBitmap)];
};


status_t
decode_file(const char* path, DecodedFile& decoded)
{
	BFile file(path, B_READ_ONLY);
	status_t status = file.InitCheck();
	if (status != B_OK)
		return status;

	status = BTranslatorRoster::Default()->Translate(&file, NULL, NULL,
		&decoded, B_TRANSLATOR_BITMAP);
	if (status == B_OK && !decoded.IsValid())
		status = B_BAD_DATA;
	return status;
}


/*!	The columns (or rows) of source that one of count target pixels covers,
	for an area average: target pixel i covers [i * step, (i + 1) * step)
	of source, starting at first.
*/
struct Span {
	int32	first;
	int32	count;
	float	firstWeight;
	float	lastWeight;
	float	scale;
};


static void
make_spans(Span* spans, int32 count, int32 first, int32 length)
{
	float step = (float)length / count;
	for (int32 i = 0; i < count; i++) {
		float start = i * step;
		float end = std::min((i + 1) * step, (float)length);
		int32 from = (int32)floorf(start);
		int32 to = std::max(from + 1, (int32)ceilf(end));
		to = std::min(to, length);
		from = std::min(from, to - 1);

		Span& span = spans[i];
		span.first = first + from;
		span.count = to - from;
		span.firstWeight = std::min(end, (float)from + 1) - start;
		span.lastWeight = span.count > 1 ? end - (to - 1) : span.firstWeight;
		float total = end - start;
		span.scale = total > 0 ? 1 / total : 1;
	}
}


static inline float
span_weight(const Span& span, int32 index)
{
	if (index == 0)
		return span.firstWeight;
	if (index == span.count - 1)
		return span.lastWeight;
	return 1;
}


/*!	Copies crop of the decoded file into bitmap, area averaged to the
	bitmap's size; both are of the same color space with 4 bytes a pixel
	when the size differs from crop's.
*/
static void
copy_scaled(const DecodedFile& decoded, clipping_rect crop, BBitmap* bitmap)
{
	const uint8* source = decoded.Bits();
	uint32 sourceRow = decoded.header.rowBytes;
	uint8* target = (uint8*)bitmap->Bits();
	int32 targetRow = bitmap->BytesPerRow();
	int32 width = bitmap->Bounds().IntegerWidth() + 1;
	int32 height = bitmap->Bounds().IntegerHeight() + 1;
	int32 cropWidth = crop.right - crop.left;
	int32 cropHeight = crop.bottom - crop.top;

	if (width == cropWidth && height == cropHeight) {
		size_t offset = crop.left * 4;
		size_t bytes = cropWidth * 4;
		if (cropWidth == decoded.Width()) {
			// whole rows, of any color space
			offset = 0;
			bytes = std::min((size_t)targetRow, (size_t)sourceRow);
		}
		for (int32 y = 0; y < height; y++) {
			memcpy(target + y * targetRow,
				source + (crop.top + y) * sourceRow + offset, bytes);
		}
		return;
	}

	Span* columns = new Span[width];
	Span* rows = new Span[height];
	float* sums = new float[width * 4];
	make_spans(columns, width, crop.left, cropWidth);
	make_spans(rows, height, crop.top, cropHeight);

	for (int32 y = 0; y < height; y++) {
		const Span& row = rows[y];
		memset(sums, 0, sizeof(float) * width * 4);
		for (int32 j = 0; j < row.count; j++) {
			const uint8* line = source + (row.first + j) * sourceRow;
			float rowWeight = span_weight(row, j);
			for (int32 x = 0; x < width; x++) {
				const Span& column = columns[x];
				const uint8* pixel = line + column.first * 4;
				float* sum = sums + x * 4;
				for (int32 i = 0; i < column.count; i++, pixel += 4) {
					float weight = rowWeight * span_weight(column, i);
					sum[0] += pixel[0] * weight;
					sum[1] += pixel[1] * weight;
					sum[2] += pixel[2] * weight;
					sum[3] += pixel[3] * weight;
				}
			}
		}

		uint8* out = target + y * targetRow;
		for (int32 x = 0; x < width; x++) {
			float scale = row.scale * columns[x].scale;
			for (int32 c = 0; c < 4; c++) {
				out[x * 4 + c] = (uint8)std::min(255.f,
					sums[x * 4 + c] * scale + 0.5f);
			}
		}
	}

	delete[] sums;
	delete[] rows;
	delete[] columns;
}

}	// namespace


//	#pragma mark - BackgroundImage


BackgroundImage*
BackgroundImage::GetBackgroundImage(const BNode* node, bool isDesktop)
{
	attr_info info;
	if (node->GetAttrInfo(kBackgroundImageInfo, &info) != B_OK)
		return NULL;

	BMessage container;
	char* buffer = new char[info.size];

	status_t error = node->ReadAttr(kBackgroundImageInfo, info.type, 0,
		buffer, (size_t)info.size);
	if (error == info.size)
		error = container.Unflatten(buffer);

	delete[] buffer;

	if (error != B_OK)
		return NULL;

	BackgroundImage* backgroundImage = NULL;
	for (int32 index = 0; ; index++) {
		const char* path;
		uint32 workspaces = B_ALL_WORKSPACES;
		Mode mode = kTiled;
		bool textWidgetLabelOutline = false;
		BPoint offset;

		if (container.FindString(kBackgroundImageInfoPath, index, &path)
				!= B_OK) {
			break;
		}

		if (isDesktop)
			be_control_look->SetBackgroundInfo(container);

		container.FindInt32(kBackgroundImageInfoWorkspaces, index,
			(int32*)&workspaces);
		container.FindInt32(kBackgroundImageInfoMode, index, (int32*)&mode);
		container.FindBool(kBackgroundImageInfoTextOutline, index,
			&textWidgetLabelOutline);
		container.FindPoint(kBackgroundImageInfoOffset, index, &offset);

		if (backgroundImage == NULL)
			backgroundImage = new BackgroundImage(node, isDesktop);

		// the file is decoded when an entry showing it is shown
		Picture* picture = NULL;
		for (int32 i = 0; i < backgroundImage->fPictures.CountItems(); i++) {
			if (backgroundImage->fPictures.ItemAt(i)->path == path) {
				picture = backgroundImage->fPictures.ItemAt(i);
				break;
			}
		}
		if (picture == NULL) {
			picture = new Picture(path);
			backgroundImage->fPictures.AddItem(picture);
		}

		BackgroundImage::BackgroundImageInfo* imageInfo = new
			BackgroundImage::BackgroundImageInfo(workspaces, picture, mode,
				offset, textWidgetLabelOutline);
		backgroundImage->Add(imageInfo);
	}

	return backgroundImage;
}


BackgroundImage::BackgroundImageInfo::BackgroundImageInfo(uint32 workspaces,
	Picture* picture, Mode mode, BPoint offset, bool textWidgetOutline)
	:
	fWorkspace(workspaces),
	fPicture(picture),
	fMode(mode),
	fOffset(offset),
	fTextWidgetOutline(textWidgetOutline)
{
}


BackgroundImage::BackgroundImageInfo::~BackgroundImageInfo()
{
}


BackgroundImage::BackgroundImage(const BNode* node, bool desktop)
	:
	fIsDesktop(desktop),
	fDefinedByNode(*node),
	fView(NULL),
	fShowingBitmap(NULL),
	fViewBitmap(NULL),
	fShownPicture(NULL),
	fPreviousPicture(NULL),
	fBitmapForWorkspaceList(1),
	fPictures(1)
{
}


BackgroundImage::~BackgroundImage()
{
}


void
BackgroundImage::Add(BackgroundImageInfo* info)
{
	fBitmapForWorkspaceList.AddItem(info);
}


void
BackgroundImage::Show(BView* view, int32 workspace)
{
	fView = view;

	BackgroundImageInfo* info = ImageInfoForWorkspace(workspace);
	if (info) {
		BPoseView* poseView = dynamic_cast<BPoseView*>(fView);
		if (poseView != NULL)
			poseView->SetWidgetTextOutline(info->fTextWidgetOutline);

		Show(info, fView);
	}
}


/*!	The frames of the monitors showing the desktop, each its own part of
	it; empty when the layout is unknown. pixelRatio is set to the most
	pixels a monitor, mirrors included, has for a pixel of the desktop (2 at
	200% scale), or 1 if that is not known.
*/
static void
get_display_frames(std::vector<BRect>& frames, float& pixelRatio)
{
	pixelRatio = 1;

	BMessage layout;
	if (BPrivate::get_display_layout(layout) != B_OK)
		return;

	BMessage display;
	for (int32 index = 0;
			layout.FindMessage("display", index, &display) == B_OK;
			index++) {
		BRect frame;
		if (!display.GetBool("enabled", false)
			|| display.FindRect("frame", &frame) != B_OK
			|| !frame.IsValid())
			continue;

		int32 modeWidth = display.GetInt32("mode width", 0);
		if (modeWidth > 0) {
			pixelRatio = std::max(pixelRatio,
				modeWidth / (frame.Width() + 1));
		}

		if (display.GetInt32("mirror", -1) < 0)
			frames.push_back(frame);
	}
}


void
BackgroundImage::Show(BackgroundImageInfo* info, BView* view)
{
	BPoseView* poseView = dynamic_cast<BPoseView*>(view);
	if (poseView != NULL)
		poseView->SetWidgetTextOutline(info->fTextWidgetOutline);

	if (info->fPicture != fShownPicture) {
		fPreviousPicture = fShownPicture;
		fShownPicture = info->fPicture;
	}

	status_t status = B_ERROR;
	float pixelRatio = 1;
	if (fIsDesktop) {
		// with more than one monitor, each shows the whole picture
		std::vector<BRect> displays;
		get_display_frames(displays, pixelRatio);
		if (displays.size() > 1) {
			status = Place(info, view, displays.data(),
				(int32)displays.size(), pixelRatio, true);
		}
	}
	if (status != B_OK) {
		BRect bounds(view->Bounds());
		status = Place(info, view, &bounds, 1, pixelRatio, false);
	}

	if (status != B_OK) {
		view->ClearViewBitmap();
		fViewBitmap = NULL;
	}
	view->Invalidate();
	fShowingBitmap = info;

	// keep the picture shown before too, for switching back and forth
	// between two workspaces
	ReleasePictures(fShownPicture, fPreviousPicture);
}


/*!	Shows the picture of info on view, on each of count hosts. Holds only
	the part of the picture the hosts show, at the largest size one of them
	draws it at in pixels of the monitor (pixelRatio of the view's), but at
	most at the file's size.
*/
status_t
BackgroundImage::Place(BackgroundImageInfo* info, BView* view,
	const BRect* hosts, int32 count, float pixelRatio, bool placements)
{
	Picture* picture = info->fPicture;
	DecodedFile decoded;
	if (picture->width == 0) {
		if (decode_file(picture->path.String(), decoded) != B_OK) {
			PRINT(("failed to load background bitmap from path\n"));
			picture->width = picture->height = -1;
		} else {
			picture->width = decoded.Width();
			picture->height = decoded.Height();
		}
	}
	if (picture->width <= 0)
		return B_ERROR;

	uint32 options = 0;
	uint32 followFlags = B_FOLLOW_TOP | B_FOLLOW_LEFT;
	std::vector<BRect> destinations(count);
	clipping_rect crop;
	int32 width;
	int32 height;

	for (int32 attempt = 0; ; attempt++) {
		BRect pictureBounds(0, 0, picture->width - 1, picture->height - 1);
		for (int32 i = 0; i < count; i++) {
			destinations[i] = Destination(info, pictureBounds, hosts[i], view,
				options, followFlags);
		}

		// The whole file, unless a centered or fitted picture on the
		// desktop shows only part of it, or smaller than the file.
		crop.left = crop.top = 0;
		crop.right = picture->width;
		crop.bottom = picture->height;
		float scale = 1;
		if (fIsDesktop
			&& (info->fMode == kCentered || info->fMode == kScaledToFit)) {
			float left = picture->width;
			float top = picture->height;
			float right = 0;
			float bottom = 0;
			float largest = 0;
			for (int32 i = 0; i < count; i++) {
				// in pixel edges: a rect's right edge is right + 1
				const BRect& place = destinations[i];
				float scaleX = (place.Width() + 1) / picture->width;
				float scaleY = (place.Height() + 1) / picture->height;
				float visibleLeft = std::max(place.left, hosts[i].left);
				float visibleTop = std::max(place.top, hosts[i].top);
				float visibleRight = std::min(place.right, hosts[i].right) + 1;
				float visibleBottom
					= std::min(place.bottom, hosts[i].bottom) + 1;
				if (visibleLeft >= visibleRight || visibleTop >= visibleBottom)
					continue;

				left = std::min(left, (visibleLeft - place.left) / scaleX);
				top = std::min(top, (visibleTop - place.top) / scaleY);
				right = std::max(right, (visibleRight - place.left) / scaleX);
				bottom = std::max(bottom,
					(visibleBottom - place.top) / scaleY);
				largest = std::max(largest, std::max(scaleX, scaleY));
			}
			if (left < right && top < bottom) {
				crop.left = std::max((int32)floorf(left + 0.001f), (int32)0);
				crop.top = std::max((int32)floorf(top + 0.001f), (int32)0);
				crop.right = std::min((int32)ceilf(right - 0.001f),
					picture->width);
				crop.bottom = std::min((int32)ceilf(bottom - 0.001f),
					picture->height);
				scale = std::min(largest * pixelRatio, 1.f);
			}
		}
		width = std::max((int32)ceilf((crop.right - crop.left) * scale
			- 0.01f), (int32)1);
		height = std::max((int32)ceilf((crop.bottom - crop.top) * scale
			- 0.01f), (int32)1);

		if (picture->bitmap != NULL && picture->crop.left == crop.left
			&& picture->crop.top == crop.top
			&& picture->crop.right == crop.right
			&& picture->crop.bottom == crop.bottom
			&& picture->bitmap->Bounds().IntegerWidth() + 1 == width
			&& picture->bitmap->Bounds().IntegerHeight() + 1 == height) {
			break;
		}

		if (!decoded.IsValid()
			&& decode_file(picture->path.String(), decoded) != B_OK) {
			return B_ERROR;
		}
		if (decoded.Width() != picture->width
			|| decoded.Height() != picture->height) {
			// the file changed since
			picture->width = decoded.Width();
			picture->height = decoded.Height();
			if (attempt == 0)
				continue;
			return B_ERROR;
		}

		size_t bytesPerPixel = 0;
		get_pixel_size_for(decoded.header.colors, &bytesPerPixel, NULL,
			NULL);
		if (bytesPerPixel != 4 && (crop.left != 0 || crop.top != 0
				|| crop.right != picture->width
				|| crop.bottom != picture->height || scale != 1)) {
			// only 32 bit pictures are cut and scaled here
			crop.left = crop.top = 0;
			crop.right = picture->width;
			crop.bottom = picture->height;
			width = picture->width;
			height = picture->height;
		}

		if (picture->bitmap != NULL && picture->bitmap == fViewBitmap) {
			// Let the app_server drop the bitmap shown now before the new
			// one is made, or the new one is put next to it in our memory
			// area, and the area keeps the old one's size when it is gone.
			view->ClearViewBitmap();
			view->Sync();
			fViewBitmap = NULL;
		}
		delete picture->bitmap;
		picture->bitmap = new BBitmap(BRect(0, 0, width - 1, height - 1), 0,
			decoded.header.colors);
		if (picture->bitmap->InitCheck() != B_OK) {
			delete picture->bitmap;
			picture->bitmap = NULL;
			return B_NO_MEMORY;
		}
		copy_scaled(decoded, crop, picture->bitmap);
		picture->crop = crop;
		break;
	}

	// where the part of the picture held goes
	for (int32 i = 0; i < count; i++) {
		BRect& place = destinations[i];
		float scaleX = (place.Width() + 1) / picture->width;
		float scaleY = (place.Height() + 1) / picture->height;
		place.Set(place.left + crop.left * scaleX,
			place.top + crop.top * scaleY,
			place.left + crop.right * scaleX - 1,
			place.top + crop.bottom * scaleY - 1);
	}

	BBitmap* bitmap = picture->bitmap;
	if (placements) {
		status_t status = BView::Private(view).SetViewBitmapPlacements(bitmap,
			bitmap->Bounds(), destinations.data(), hosts, count, options);
		if (status == B_OK)
			fViewBitmap = bitmap;
		return status;
	}

	view->SetViewBitmap(bitmap, bitmap->Bounds(), destinations[0],
		followFlags, options);
	fViewBitmap = bitmap;
	return B_OK;
}


//!	Lets go of the pixels of all pictures but keep and keepToo.
void
BackgroundImage::ReleasePictures(const Picture* keep, const Picture* keepToo)
{
	for (int32 i = 0; i < fPictures.CountItems(); i++) {
		Picture* picture = fPictures.ItemAt(i);
		if (picture != keep && picture != keepToo) {
			delete picture->bitmap;
			picture->bitmap = NULL;
		}
	}
}


BRect
BackgroundImage::Destination(const BackgroundImageInfo* info,
	BRect pictureBounds, BRect host, BView* view, uint32& options,
	uint32& followFlags)
{
	BPoseView* poseView = dynamic_cast<BPoseView*>(view);
	BRect hostBounds(host.OffsetToCopy(B_ORIGIN));
	BRect bitmapBounds(pictureBounds);
	BRect destinationBitmapBounds(bitmapBounds);

	// figure out the display mode and the destination bounds for the bitmap
	switch (info->fMode) {
		case kCentered:
			if (fIsDesktop) {
				destinationBitmapBounds.OffsetBy(
					(hostBounds.Width() - bitmapBounds.Width()) / 2,
					(hostBounds.Height() - bitmapBounds.Height()) / 2);
				break;
			}
			// else fall thru
		case kScaledToFit:
			if (fIsDesktop) {
				if (BRectRatio(destinationBitmapBounds)
						>= BRectRatio(hostBounds)) {
					float overlap = BRectHorizontalOverlap(hostBounds,
						destinationBitmapBounds);
					destinationBitmapBounds.Set(-overlap, 0,
						hostBounds.Width() + overlap, hostBounds.Height());
				} else {
					float overlap = BRectVerticalOverlap(hostBounds,
						destinationBitmapBounds);
					destinationBitmapBounds.Set(0, -overlap,
						hostBounds.Width(), hostBounds.Height() + overlap);
				}
				followFlags = B_FOLLOW_ALL;
				options |= B_FILTER_BITMAP_BILINEAR;
				break;
			}
			// else fall thru
		case kAtOffset:
			if (!fIsDesktop && poseView != NULL)
				destinationBitmapBounds.OffsetTo(poseView->Extent().LeftTop() + info->fOffset);
			else
				destinationBitmapBounds.OffsetTo(info->fOffset);
			break;

		case kTiled:
			if (fIsDesktop) {
				destinationBitmapBounds.OffsetBy(
					(hostBounds.Width() - bitmapBounds.Width()) / 2,
					(hostBounds.Height() - bitmapBounds.Height()) / 2);
			} else if (poseView != NULL) {
				// tile from top left of window even if scrolled over
				destinationBitmapBounds.OffsetTo(poseView->Extent().LeftTop());
			}
			options |= B_TILE_BITMAP;
			break;
	}

	// on the desktop, the host is the part of the screen a monitor shows
	if (fIsDesktop)
		destinationBitmapBounds.OffsetBy(host.LeftTop());

	return destinationBitmapBounds;
}


float
BackgroundImage::BRectRatio(BRect rect)
{
	return rect.Width() / rect.Height();
}


float
BackgroundImage::BRectHorizontalOverlap(BRect hostRect, BRect resizedRect)
{
	return ((hostRect.Height() / resizedRect.Height() * resizedRect.Width())
		- hostRect.Width()) / 2;
}


float
BackgroundImage::BRectVerticalOverlap(BRect hostRect, BRect resizedRect)
{
	return ((hostRect.Width() / resizedRect.Width() * resizedRect.Height())
		- hostRect.Height()) / 2;
}


void
BackgroundImage::Remove()
{
	if (fShowingBitmap != NULL) {
		fView->ClearViewBitmap();
		fView->Invalidate();
		BPoseView* poseView = dynamic_cast<BPoseView*>(fView);
		// make sure text widgets draw the default way, erasing
		// their background
		if (poseView != NULL)
			poseView->SetWidgetTextOutline(true);
	}

	fShowingBitmap = NULL;
	fViewBitmap = NULL;
	fShownPicture = fPreviousPicture = NULL;
	ReleasePictures(NULL, NULL);
}


BackgroundImage::BackgroundImageInfo*
BackgroundImage::ImageInfoForWorkspace(int32 workspace) const
{
	uint32 workspaceMask = 1;

	for ( ; workspace; workspace--)
		workspaceMask *= 2;

	int32 count = fBitmapForWorkspaceList.CountItems();

	// do a simple lookup for the most likely candidate bitmap -
	// pick the imageInfo that is only defined for this workspace over one
	// that supports multiple workspaces
	BackgroundImageInfo* result = NULL;
	for (int32 index = 0; index < count; index++) {
		BackgroundImageInfo* info = fBitmapForWorkspaceList.ItemAt(index);
		if (info->fWorkspace == workspaceMask)
			return info;

		if (info->fWorkspace & workspaceMask)
			result = info;
	}

	return result;
}


void
BackgroundImage::WorkspaceActivated(BView* view, int32 workspace, bool state)
{
	if (!fIsDesktop) {
		// we only care for desktop bitmaps
		return;
	}

	if (!state) {
		// we only care comming into a new workspace, not leaving one
		return;
	}

	BackgroundImageInfo* info = ImageInfoForWorkspace(workspace);
	if (info != fShowingBitmap) {
		if (info != NULL)
			Show(info, view);
		else {
			BPoseView* poseView = dynamic_cast<BPoseView*>(view);
			if (poseView != NULL)
				poseView->SetWidgetTextOutline(true);

			view->ClearViewBitmap();
			fViewBitmap = NULL;
			view->Invalidate();
		}

		fShowingBitmap = info;
	}
}


void
BackgroundImage::ScreenChanged(BRect, color_space)
{
	if (!fIsDesktop || fShowingBitmap == NULL
		|| fShowingBitmap->fPicture->width < 0) {
		return;
	}

	// The picture was placed for the old screen, on each of its monitors,
	// and a centered or fitted one would only follow the view's size from
	// there: a screen of another shape (a monitor added, moved, or
	// mirroring another) needs it placed anew, and a larger one more of
	// its pixels.
	Show(fShowingBitmap, fView);
}


BackgroundImage*
BackgroundImage::Refresh(BackgroundImage* oldBackgroundImage,
	const BNode* fromNode, bool desktop, BPoseView* poseView)
{
	if (oldBackgroundImage != NULL) {
		oldBackgroundImage->Remove();
		delete oldBackgroundImage;
	}

	BackgroundImage* backgroundImage = GetBackgroundImage(fromNode, desktop);
	if (backgroundImage != NULL && poseView != NULL && poseView->ViewMode() != kListMode)
		backgroundImage->Show(poseView, current_workspace());

	return backgroundImage;
}
