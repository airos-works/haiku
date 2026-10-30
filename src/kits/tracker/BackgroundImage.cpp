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
#include <ControlLook.h>
#include <Node.h>
#include <TranslationKit.h>
#include <View.h>
#include <Window.h>

#include <fs_attr.h>

#include <InterfacePrivate.h>
#include <ViewPrivate.h>

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
		BBitmap* bitmap = NULL;

		if (container.FindString(kBackgroundImageInfoPath, index, &path)
				== B_OK) {
			bitmap = BTranslationUtils::GetBitmap(path);
			if (!bitmap)
				PRINT(("failed to load background bitmap from path\n"));
		} else
			break;

		if (isDesktop)
			be_control_look->SetBackgroundInfo(container);

		container.FindInt32(kBackgroundImageInfoWorkspaces, index,
			(int32*)&workspaces);
		container.FindInt32(kBackgroundImageInfoMode, index, (int32*)&mode);
		container.FindBool(kBackgroundImageInfoTextOutline, index,
			&textWidgetLabelOutline);
		container.FindPoint(kBackgroundImageInfoOffset, index, &offset);

		BackgroundImage::BackgroundImageInfo* imageInfo = new
			BackgroundImage::BackgroundImageInfo(workspaces, bitmap, mode,
				offset, textWidgetLabelOutline);

		if (backgroundImage == NULL)
			backgroundImage = new BackgroundImage(node, isDesktop);

		backgroundImage->Add(imageInfo);
	}

	return backgroundImage;
}


BackgroundImage::BackgroundImageInfo::BackgroundImageInfo(uint32 workspaces,
	BBitmap* bitmap, Mode mode, BPoint offset, bool textWidgetOutline)
	:
	fWorkspace(workspaces),
	fBitmap(bitmap),
	fMode(mode),
	fOffset(offset),
	fTextWidgetOutline(textWidgetOutline)
{
}


BackgroundImage::BackgroundImageInfo::~BackgroundImageInfo()
{
	delete fBitmap;
}


BackgroundImage::BackgroundImage(const BNode* node, bool desktop)
	:
	fIsDesktop(desktop),
	fDefinedByNode(*node),
	fView(NULL),
	fShowingBitmap(NULL),
	fBitmapForWorkspaceList(1)
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
	it; empty when the layout is unknown.
*/
static void
get_display_frames(std::vector<BRect>& frames)
{
	BMessage layout;
	if (BPrivate::get_display_layout(layout) != B_OK)
		return;

	BMessage display;
	for (int32 index = 0;
			layout.FindMessage("display", index, &display) == B_OK;
			index++) {
		BRect frame;
		if (!display.GetBool("enabled", false)
			|| display.GetInt32("mirror", -1) >= 0
			|| display.FindRect("frame", &frame) != B_OK
			|| !frame.IsValid())
			continue;
		frames.push_back(frame);
	}
}


void
BackgroundImage::Show(BackgroundImageInfo* info, BView* view)
{
	BPoseView* poseView = dynamic_cast<BPoseView*>(view);
	if (poseView != NULL)
		poseView->SetWidgetTextOutline(info->fTextWidgetOutline);

	if (info->fBitmap == NULL) {
		view->ClearViewBitmap();
		view->Invalidate();
		fShowingBitmap = info;
		return;
	}
	BRect bitmapBounds(info->fBitmap->Bounds());

	uint32 options = 0;
	uint32 followFlags = B_FOLLOW_TOP | B_FOLLOW_LEFT;

	if (fIsDesktop) {
		// with more than one monitor, each shows the whole picture
		std::vector<BRect> displays;
		get_display_frames(displays);
		if (displays.size() > 1) {
			std::vector<BRect> destinations;
			for (size_t i = 0; i < displays.size(); i++) {
				destinations.push_back(Destination(info, displays[i], view,
					options, followFlags));
			}

			if (BView::Private(view).SetViewBitmapPlacements(info->fBitmap,
					bitmapBounds, destinations.data(), displays.data(),
					(int32)displays.size(), options) == B_OK) {
				view->Invalidate();
				fShowingBitmap = info;
				return;
			}
			options = 0;
			followFlags = B_FOLLOW_TOP | B_FOLLOW_LEFT;
		}
	}

	BRect destinationBitmapBounds = Destination(info, view->Bounds(), view,
		options, followFlags);

	// switch to the bitmap and force a redraw
	view->SetViewBitmap(info->fBitmap, bitmapBounds, destinationBitmapBounds,
		followFlags, options);
	view->Invalidate();
	fShowingBitmap = info;
}


BRect
BackgroundImage::Destination(const BackgroundImageInfo* info, BRect host,
	BView* view, uint32& options, uint32& followFlags)
{
	BPoseView* poseView = dynamic_cast<BPoseView*>(view);
	BRect hostBounds(host.OffsetToCopy(B_ORIGIN));
	BRect bitmapBounds(info->fBitmap->Bounds());
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
			view->Invalidate();
		}

		fShowingBitmap = info;
	}
}


void
BackgroundImage::ScreenChanged(BRect, color_space)
{
	if (!fIsDesktop || fShowingBitmap == NULL || fShowingBitmap->fBitmap == NULL)
		return;

	// The picture was placed for the old screen, on each of its monitors,
	// and a centered or fitted one would only follow the view's size from
	// there: a screen of another shape (a monitor added, moved, or
	// mirroring another) needs it placed anew.
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
