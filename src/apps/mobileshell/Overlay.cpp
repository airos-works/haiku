/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Overlay.h"

#include "Theme.h"

#include <Application.h>
#include <Bitmap.h>
#include <InterfaceDefs.h>
#include <MessageRunner.h>
#include <Screen.h>

#include <algorithm>


static const bigtime_t kAnimationDuration = 250000;


OverlayView::OverlayView(BRect frame, const char* name)
	:
	BView(frame, name, B_FOLLOW_ALL, B_WILL_DRAW | B_FRAME_EVENTS),
	fBackdrop(NULL),
	fBuffer(NULL),
	fBufferView(NULL),
	fAnimation(NULL),
	fProgress(0),
	fOpening(false),
	fFrom(0),
	fStart(0)
{
	SetViewColor(B_TRANSPARENT_COLOR);
}


OverlayView::~OverlayView()
{
	delete fAnimation;
	delete fBackdrop;
	delete fBuffer;
}


void
OverlayView::AttachedToWindow()
{
	_MakeBuffer();
}


void
OverlayView::FrameResized(float width, float height)
{
	_MakeBuffer();
}


void
OverlayView::Draw(BRect updateRect)
{
	if (fBuffer != NULL)
		DrawBitmap(fBuffer, updateRect, updateRect);
}


void
OverlayView::MessageReceived(BMessage* message)
{
	if (message->what != kMsgAnimate) {
		BView::MessageReceived(message);
		return;
	}
	if (fAnimation == NULL)
		return;

	// By the clock rather than a step a frame, so it takes as long however
	// often the frames come; easing out (cubic), it slows as it arrives.
	float t = std::min(1.0f,
		(float)(system_time() - fStart) / kAnimationDuration);
	float eased = 1 - (1 - t) * (1 - t) * (1 - t);
	float target = fOpening ? 1 : 0;
	fProgress = fFrom + (target - fFrom) * eased;
	Render();
	if (t >= 1) {
		delete fAnimation;
		fAnimation = NULL;
		AnimationDone(fOpening);
	}
}


void
OverlayView::Prepare()
{
	delete fBackdrop;
	fBackdrop = NULL;
	BScreen(Window()).GetBitmap(&fBackdrop, false);
}


void
OverlayView::Render()
{
	if (fBuffer == NULL || !fBuffer->Lock())
		return;

	BView* view = fBufferView;
	BRect bounds = view->Bounds();
	view->SetDrawingMode(B_OP_COPY);
	if (fBackdrop != NULL)
		view->DrawBitmap(fBackdrop, ConvertToScreen(bounds), bounds);
	else {
		view->SetHighColor(Theme::kPanel);
		view->FillRect(bounds);
	}

	// Dim the screen behind, more as the overlay opens.
	rgb_color scrim = Theme::kScrim;
	scrim.alpha = (uint8)(scrim.alpha * fProgress);
	view->SetDrawingMode(B_OP_ALPHA);
	view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
	view->SetHighColor(scrim);
	view->FillRect(bounds);

	RenderContent(view);

	view->Sync();
	fBuffer->Unlock();
	Invalidate();
}


void
OverlayView::StartAnimation(bool opening)
{
	fOpening = opening;
	fFrom = fProgress;
	fStart = system_time();
	delete fAnimation;
	BMessage tick(kMsgAnimate);
	fAnimation = new BMessageRunner(BMessenger(this), &tick, 16000);
	Render();
}


void
OverlayView::AnimationDone(bool opened)
{
	if (!opened) {
		if (!Window()->IsHidden())
			Window()->Hide();
		be_app->PostMessage(kMsgOverlayChanged);
	}
}


void
OverlayView::_MakeBuffer()
{
	BRect bounds = Bounds().OffsetToCopy(B_ORIGIN);
	if (fBuffer != NULL && fBuffer->Bounds() == bounds)
		return;

	delete fBuffer;
		// its view goes with it
	fBuffer = new BBitmap(bounds, B_BITMAP_ACCEPTS_VIEWS, B_RGB32);
	fBufferView = new BView(bounds, "buffer", B_FOLLOW_NONE, 0);
	fBuffer->AddChild(fBufferView);
}


// #pragma mark - OverlayWindow


OverlayWindow::OverlayWindow(BRect frame, const char* name)
	:
	BWindow(frame, name, B_NO_BORDER_WINDOW_LOOK, B_FLOATING_ALL_WINDOW_FEEL,
		B_NOT_MOVABLE | B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE
			| B_NOT_CLOSABLE | B_AVOID_FOCUS | B_ASYNCHRONOUS_CONTROLS,
		B_ALL_WORKSPACES)
{
}


void
OverlayWindow::Open(OverlayView* view, bool prepared)
{
	if (!Lock())
		return;
	if (IsHidden()) {
		if (!prepared)
			view->Prepare();
		view->Render();
		Show();
		be_app->PostMessage(kMsgOverlayChanged);
	}
	Unlock();
}
