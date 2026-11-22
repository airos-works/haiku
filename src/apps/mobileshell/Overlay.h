/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef OVERLAY_H
#define OVERLAY_H

/*	Overlay: a shell window that shows over the applications, such as the
	action drawer and the task switcher.

	The windows of this desktop are opaque, so an overlay takes a picture of
	the screen as it opens and draws that, dimmed, behind its own content:
	what is behind it stands still while it is open. Everything is drawn
	into an off-screen bitmap first so that dragging and the slide-in don't
	flicker.
*/

#include <View.h>
#include <Window.h>


class BBitmap;
class BMessageRunner;


class OverlayView : public BView {
public:
								OverlayView(BRect frame, const char* name);
	virtual						~OverlayView();

	virtual	void				AttachedToWindow();
	virtual	void				FrameResized(float width, float height);
	virtual	void				Draw(BRect updateRect);
	virtual	void				MessageReceived(BMessage* message);

			// Captures the screen; call before the window shows.
			void				Prepare();
			void				Render();

protected:
	virtual	void				RenderContent(BView* view) = 0;

			// 0 = closed, 1 = fully open; animated by StartAnimation(), over
			// a quarter second, easing out.
			float				Progress() const { return fProgress; }
			void				StartAnimation(bool opening);
	virtual	void				AnimationDone(bool opened);

			// the screen as it was when the overlay opened
			const BBitmap*		Backdrop() const { return fBackdrop; }

private:
			void				_MakeBuffer();

			BBitmap*			fBackdrop;
			BBitmap*			fBuffer;
			BView*				fBufferView;
			BMessageRunner*		fAnimation;
			float				fProgress;
			bool				fOpening;
			float				fFrom;		// where the animation started
			bigtime_t			fStart;
};


class OverlayWindow : public BWindow {
public:
								OverlayWindow(BRect frame, const char* name);

			// Shows the window, capturing the screen first unless the
			// view has (\a prepared).
			void				Open(OverlayView* view, bool prepared = false);
};


#endif	// OVERLAY_H
