/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _QUICK_LOOK_H
#define _QUICK_LOOK_H


/*!	Quick Look: previews of files shown by Tracker when the space bar is
	pressed. The quick_look server hosts the panel and loads add-ons from
	the add-ons/quick_look directories; an add-on says how well it can
	preview a file and makes a view for it. Add-ons that play media are
	shipped by the applications that own the playback code, so the stack
	exists once.
*/


#include <DirectWindow.h>
#include <Entry.h>
#include <String.h>
#include <View.h>
#include <image.h>


//! Posted by a preview to its window when its content size or summary changed.
#define B_QUICK_LOOK_PREVIEW_CHANGED 'QLpc'


class BQuickLookPreview : public BView {
public:
								BQuickLookPreview(const char* name,
									uint32 flags = B_WILL_DRAW
										| B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE);
	virtual						~BQuickLookPreview();

	/*!	The size the content would like to be shown at, in view coordinates:
		the image's pixel size, the video's display size, a comfortable size
		for a page of text. The host fits it to the screen and keeps the
		panel resizable; the view's frame may change at any time. An invalid
		size leaves the choice to the host. */
	virtual	BSize				PreferredContentSize();

	/*!	One line shown under the preview: "1920 × 1080 pixels",
		"H.264 1080p, hardware decoded". Empty for the host's default
		(type and size). */
	virtual	BString				Summary();

	/*!	Start() is called once the view is in the panel and the panel is on
		screen; Stop() before the view is removed, or when the panel is
		hidden. After Stop() returns no thread of the add-on may touch the
		view or the frame buffer any more. */
	virtual	void				Start();
	virtual	void				Stop();

	/*!	The panel is a BDirectWindow; its DirectConnected() is passed on
		here, on the app_server's thread, for previews that draw into the
		frame buffer themselves. */
	virtual	void				DirectConnected(direct_buffer_info* info);

protected:
	//! Tells the host that PreferredContentSize() or Summary() changed.
			void				NotifyChanged();

private:
	virtual	void				_ReservedQuickLookPreview1();
	virtual	void				_ReservedQuickLookPreview2();
	virtual	void				_ReservedQuickLookPreview3();
	virtual	void				_ReservedQuickLookPreview4();

			uint32				_reserved[8];
};


class BQuickLookAddOn {
public:
								BQuickLookAddOn(image_id image);
	virtual						~BQuickLookAddOn();

			image_id			Image() const { return fImage; }

	/*!	How well this add-on previews the entry: 0 not at all, 1 perfectly.
		The highest bidder makes the preview; when it fails, the next one is
		asked. Keep this cheap: it is asked for every file shown, and the
		type is the one the MIME database gave. */
	virtual	float				Supports(const entry_ref& ref,
									const char* mimeType) = 0;

	/*!	Makes the preview view for the entry, or returns NULL and says why.
		The view is owned by the host, which deletes it after Stop(). */
	virtual	BQuickLookPreview*	CreatePreview(const entry_ref& ref,
									const char* mimeType, BString* error) = 0;

private:
	virtual	void				_ReservedQuickLookAddOn1();
	virtual	void				_ReservedQuickLookAddOn2();
	virtual	void				_ReservedQuickLookAddOn3();
	virtual	void				_ReservedQuickLookAddOn4();

			image_id			fImage;
			uint32				_reserved[8];
};


//! Every add-on exports this.
extern "C" _EXPORT BQuickLookAddOn* instantiate_quick_look_add_on(
	image_id image);


#endif	// _QUICK_LOOK_H
