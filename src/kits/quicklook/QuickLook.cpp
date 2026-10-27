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


void BQuickLookAddOn::_ReservedQuickLookAddOn1() {}
void BQuickLookAddOn::_ReservedQuickLookAddOn2() {}
void BQuickLookAddOn::_ReservedQuickLookAddOn3() {}
void BQuickLookAddOn::_ReservedQuickLookAddOn4() {}
