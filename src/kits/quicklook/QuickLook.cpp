/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include <add-ons/quick_look/QuickLook.h>

#include <Window.h>


BQuickLookPreview::BQuickLookPreview(const char* name, uint32 flags)
	:
	BView(name, flags)
{
}


BQuickLookPreview::~BQuickLookPreview()
{
}


BSize
BQuickLookPreview::PreferredContentSize()
{
	return BSize();
}


BString
BQuickLookPreview::Summary()
{
	return BString();
}


void
BQuickLookPreview::Start()
{
}


void
BQuickLookPreview::Stop()
{
}


void
BQuickLookPreview::DirectConnected(direct_buffer_info* info)
{
}


void
BQuickLookPreview::NotifyChanged()
{
	BWindow* window = Window();
	if (window != NULL)
		window->PostMessage(B_QUICK_LOOK_PREVIEW_CHANGED, window);
}


void BQuickLookPreview::_ReservedQuickLookPreview1() {}
void BQuickLookPreview::_ReservedQuickLookPreview2() {}
void BQuickLookPreview::_ReservedQuickLookPreview3() {}
void BQuickLookPreview::_ReservedQuickLookPreview4() {}


// #pragma mark - BQuickLookAddOn


BQuickLookAddOn::BQuickLookAddOn(image_id image)
	:
	fImage(image)
{
}


BQuickLookAddOn::~BQuickLookAddOn()
{
}


status_t
BQuickLookAddOn::CreateThumbnail(const entry_ref& ref, const char* mimeType,
	int32 size, BBitmap** _bitmap)
{
	return B_NOT_SUPPORTED;
}


// Add-ons built before CreateThumbnail() took the first reserved slot point
// that slot here.
extern "C" status_t
_ZN15BQuickLookAddOn24_ReservedQuickLookAddOn1Ev(BQuickLookAddOn* addOn)
{
	return B_NOT_SUPPORTED;
}


void BQuickLookAddOn::_ReservedQuickLookAddOn2() {}
void BQuickLookAddOn::_ReservedQuickLookAddOn3() {}
void BQuickLookAddOn::_ReservedQuickLookAddOn4() {}
