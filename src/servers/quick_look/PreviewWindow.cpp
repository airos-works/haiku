/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "PreviewWindow.h"

#include <algorithm>
#include <stdio.h>

#include <Application.h>
#include <Button.h>
#include <Catalog.h>
#include <ControlLook.h>
#include <MessageRunner.h>
#include <Mime.h>
#include <Roster.h>
#include <Screen.h>
#include <StringView.h>
#include <syslog.h>

#include <add-ons/quick_look/QuickLook.h>
#include <QuickLookDefs.h>

#include "AddOnManager.h"
#include "GenericPreview.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "QuickLook"


enum {
	kMsgPrevious	= 'prev',
	kMsgNext		= 'next',
	kMsgOpen		= 'open',
	kMsgAnimate		= 'anim'
};

// The zoom: this many frames, this far apart.
static const int32 kAnimationSteps = 12;
static const bigtime_t kAnimationInterval = 14000;

static const float kMinimumWidth = 420;
static const float kMinimumContentHeight = 160;
static const float kScreenShare = 0.85f;
static const rgb_color kContentColor = {46, 46, 46, 255};
static const rgb_color kBorderColor = {20, 20, 20, 255};


//! The bar under the preview.
class FooterView : public BView {
public:
	FooterView()
		:
		BView("footer", B_WILL_DRAW)
	{
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	}

	virtual void Draw(BRect updateRect)
	{
		BRect bounds = Bounds();
		SetHighColor(tint_color(ViewColor(), B_DARKEN_2_TINT));
		StrokeLine(bounds.LeftTop(), bounds.RightTop());
	}
};


PreviewWindow::PreviewWindow(AddOnManager* addOns)
	:
	BDirectWindow(BRect(100, 100, 100 + kMinimumWidth, 400),
		"Quick Look", B_NO_BORDER_WINDOW_LOOK, B_FLOATING_ALL_WINDOW_FEEL,
		B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_RESIZABLE | B_AVOID_FOCUS
			| B_NO_WORKSPACE_ACTIVATION | B_NOT_ANCHORED_ON_ACTIVATE
			| B_ASYNCHRONOUS_CONTROLS),
	fAddOns(addOns),
	fIndex(0),
	fAnimation(NULL),
	fAnimationStep(0),
	fAnimationHides(false),
	fNotifyAfterHide(false),
	fPreview(NULL),
	fPreviewStarted(false),
	fUserResized(false)
{
	// No window frame: a thin dark line around the panel is all.
	fBorder = new BView("border", 0);
	fBorder->SetViewColor(kBorderColor);
	AddChild(fBorder);

	fContent = new BView("content", 0);
	fContent->SetViewColor(kContentColor);
	fBorder->AddChild(fContent);

	fFooter = new FooterView();
	fBorder->AddChild(fFooter);

	fName = new BStringView("name", "");
	fName->SetFont(be_bold_font);
	fName->SetHighUIColor(B_PANEL_TEXT_COLOR);
	fName->SetTruncation(B_TRUNCATE_MIDDLE);
	fFooter->AddChild(fName);

	fSummary = new BStringView("summary", "");
	fSummary->SetHighUIColor(B_PANEL_TEXT_COLOR);
	fSummary->SetTruncation(B_TRUNCATE_END);
	fFooter->AddChild(fSummary);

	fPrevious = new BButton("previous", "\xE2\x80\xB9",
		new BMessage(kMsgPrevious));
	fNext = new BButton("next", "\xE2\x80\xBA", new BMessage(kMsgNext));
	fCount = new BStringView("count", "");
	fCount->SetHighUIColor(B_PANEL_TEXT_COLOR);
	fCount->SetAlignment(B_ALIGN_CENTER);
	fFooter->AddChild(fPrevious);
	fFooter->AddChild(fCount);
	fFooter->AddChild(fNext);

	fOpen = new BButton("open", B_TRANSLATE("Open"), new BMessage(kMsgOpen));
	fFooter->AddChild(fOpen);

	// Resizing goes through here to keep the layout; the controls' sizes
	// are fixed by the font.
	_Layout();
	// The zoom passes through every size.
	SetSizeLimits(1, 100000, 1, 100000);
	SetPulseRate(0);
}


PreviewWindow::~PreviewWindow()
{
	delete fAnimation;
	_SetPreview(NULL);
	Hide();
	Sync();
}


void
PreviewWindow::ShowEntries(const BMessage* message)
{
	std::vector<entry_ref> refs;
	entry_ref ref;
	for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++)
		refs.push_back(ref);
	if (refs.empty())
		return;

	int32 index = 0;
	message->FindInt32("index", &index);
	index = std::max((int32)0, std::min(index, (int32)refs.size() - 1));

	BMessenger owner;
	message->FindMessenger("owner", &owner);
	BRect frame;
	if (message->FindRect("frame", &frame) != B_OK)
		frame = BRect();
	BRect iconRect;
	if (message->FindRect("icon", &iconRect) != B_OK)
		iconRect = BRect();

	if (IsShowing() && owner != fOwner && fOwner.IsValid())
		fOwner.SendMessage(kMsgQuickLookClosed);
	fOwner = owner;
	fOwnerFrame = frame;
	fIconRect = iconRect;

	// Asked for something new while zooming: the zoom ends where it was
	// going, at once.
	if (fAnimation != NULL) {
		bool hiding = fAnimationHides;
		_FinishAnimation();
		if (hiding)
			return ShowEntries(message);
	}

	bool same = refs.size() == fRefs.size() && index == fIndex;
	for (size_t i = 0; same && i < refs.size(); i++)
		same = refs[i] == fRefs[i];
	fRefs = refs;
	fIndex = index;

	if (same && IsShowing() && fPreview != NULL) {
		_UpdateFooter();
		_NotifyShown();
		return;
	}

	bool wasHidden = IsHidden();
	if (wasHidden) {
		fUserResized = false;
		// On the screen of the window that asked.
		if (frame.IsValid())
			MoveTo(frame.left + 40, frame.top + 40);
	}
	_ShowCurrent();
	if (wasHidden) {
		BRect target = Frame();
		if (fIconRect.IsValid()) {
			// Out of the entry's icon, growing to its place.
			MoveTo(fIconRect.LeftTop());
			ResizeTo(fIconRect.Width(), fIconRect.Height());
			_Layout();
			Show();
			_StartAnimation(fIconRect, target, false);
		} else
			Show();
		if (fPreview != NULL && !fPreviewStarted) {
			fPreview->Start();
			fPreviewStarted = true;
		}
	}
	_NotifyShown();
}


void
PreviewWindow::HidePanel(bool notifyOwner)
{
	if (IsHidden())
		return;
	if (fAnimation != NULL) {
		if (fAnimationHides) {
			fNotifyAfterHide = fNotifyAfterHide || notifyOwner;
			return;
		}
		_FinishAnimation();
	}
	if (fIconRect.IsValid() && fPreview != NULL) {
		// Back into the icon: the picture stops, the view shrinks with the
		// panel, and the panel goes once it is icon sized.
		if (fPreviewStarted)
			fPreview->Stop();
		fPreviewStarted = false;
		fNotifyAfterHide = notifyOwner;
		_StartAnimation(Frame(), fIconRect, true);
		return;
	}
	_SetPreview(NULL);
	Hide();
	if (notifyOwner)
		_NotifyClosed();
	be_app->PostMessage(kMsgQuickLookHide);
}


void
PreviewWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgPrevious:
			_Step(-1);
			break;
		case kMsgNext:
			_Step(1);
			break;
		case kMsgOpen:
			_OpenCurrent();
			break;

		case B_QUICK_LOOK_PREVIEW_CHANGED:
			_UpdateFooter();
			if (fAnimation != NULL && !fAnimationHides)
				fAnimationTo = _TargetFrame();
			else if (!fUserResized)
				_FitToContent();
			break;

		case kMsgAnimate:
			_AnimationStep();
			break;

		case kMsgQuickLookShow:
			ShowEntries(message);
			break;
		case kMsgQuickLookHide:
			HidePanel(false);
			break;
		case kMsgQuickLookToggle:
		{
			BMessenger owner;
			message->FindMessenger("owner", &owner);
			if (IsShowing() && owner == fOwner)
				HidePanel(true);
			else
				ShowEntries(message);
			break;
		}

		default:
			BDirectWindow::MessageReceived(message);
	}
}


bool
PreviewWindow::QuitRequested()
{
	// The close button hides the panel; the server quits on its own later.
	HidePanel(true);
	return false;
}


void
PreviewWindow::DirectConnected(direct_buffer_info* info)
{
	BDirectWindow::DirectConnected(info);
	std::lock_guard<std::mutex> lock(fPreviewLock);
	if (fPreview != NULL)
		fPreview->DirectConnected(info);
}


void
PreviewWindow::FrameResized(float width, float height)
{
	BDirectWindow::FrameResized(width, height);
	_Layout();
}


void
PreviewWindow::FrameMoved(BPoint origin)
{
	BDirectWindow::FrameMoved(origin);
}


// #pragma mark - private


void
PreviewWindow::_ShowCurrent()
{
	if (fRefs.empty())
		return;
	const entry_ref& ref = fRefs[fIndex];

	// The old preview stops before the new one starts: one player at a
	// time, and no add-on is asked for a view while another's threads run.
	_SetPreview(NULL);

	BString error;
	BQuickLookPreview* preview = fAddOns->CreatePreview(ref, &fMimeType,
		&error);
	if (preview == NULL)
		preview = new GenericPreview(ref, fMimeType.String(), error.String());

	// What the footer says when the preview has nothing to add.
	BMimeType type(fMimeType.String());
	char description[B_MIME_TYPE_LENGTH];
	if (type.GetShortDescription(description) == B_OK)
		fDefaultSummary = description;
	else
		fDefaultSummary = fMimeType;
	BEntry entry(&ref, true);
	off_t size;
	if (entry.IsFile() && entry.GetSize(&size) == B_OK) {
		BString sizeText;
		if (size < 1024)
			sizeText.SetToFormat("%" B_PRIdOFF " bytes", size);
		else if (size < 1024 * 1024)
			sizeText.SetToFormat("%.1f KiB", size / 1024.0);
		else if (size < 1024LL * 1024 * 1024)
			sizeText.SetToFormat("%.1f MiB", size / (1024.0 * 1024));
		else
			sizeText.SetToFormat("%.2f GiB", size / (1024.0 * 1024 * 1024));
		fDefaultSummary << " \xE2\x80\xA2 " << sizeText;
	}

	SetTitle(ref.name);
	fName->SetText(ref.name);
	_SetPreview(preview);
	if (!fUserResized)
		_FitToContent();
	_UpdateFooter();
	if (IsShowing()) {
		fPreview->Start();
		fPreviewStarted = true;
	}
}


void
PreviewWindow::_SetPreview(BQuickLookPreview* preview)
{
	if (fPreview != NULL) {
		if (fPreviewStarted)
			fPreview->Stop();
		fPreviewStarted = false;
		BQuickLookPreview* old = fPreview;
		{
			std::lock_guard<std::mutex> lock(fPreviewLock);
			fPreview = NULL;
		}
		old->RemoveSelf();
		delete old;
	}
	if (preview == NULL)
		return;

	preview->MoveTo(0, 0);
	preview->ResizeTo(fContent->Bounds().Width(), fContent->Bounds().Height());
	fContent->AddChild(preview);
	std::lock_guard<std::mutex> lock(fPreviewLock);
	fPreview = preview;
}


float
PreviewWindow::_FooterHeight() const
{
	font_height height;
	be_plain_font->GetHeight(&height);
	return ceilf((height.ascent + height.descent) * 2.6f);
}


void
PreviewWindow::_FitToContent()
{
	if (fPreview == NULL)
		return;
	BRect target = _TargetFrame();
	MoveTo(target.LeftTop());
	ResizeTo(target.Width(), target.Height());
	_Layout();
}


/*!	Where the panel belongs for the current preview: at the content's
	preferred size, at most most of the screen, centred on the window
	that asked (the first time) or where it is. */
BRect
PreviewWindow::_TargetFrame()
{
	BSize content = fPreview->PreferredContentSize();
	if (!content.IsWidthSet() || !content.IsHeightSet()
		|| content.width < 1 || content.height < 1)
		content.Set(640, 480);

	BScreen screen(this);
	BRect screenFrame = screen.Frame();
	float footer = _FooterHeight();
	// Room for the tab and the frame.
	float maxWidth = screenFrame.Width() * kScreenShare;
	float maxHeight = screenFrame.Height() * kScreenShare - footer - 30;
	float scale = std::min(1.0f, std::min(maxWidth / content.width,
		maxHeight / content.height));
	float width = std::max(kMinimumWidth, floorf(content.width * scale));
	float height = std::max(kMinimumContentHeight,
		floorf(content.height * scale)) + footer;

	BRect frame = Frame();
	BPoint center(frame.left + frame.Width() / 2,
		frame.top + frame.Height() / 2);
	if (IsHidden()) {
		// The first appearance is centred on the window that asked (which
		// keeps it on that window's monitor), else on the screen.
		if (fOwnerFrame.IsValid() && screenFrame.Contains(fOwnerFrame)) {
			center.Set(fOwnerFrame.left + fOwnerFrame.Width() / 2,
				fOwnerFrame.top + fOwnerFrame.Height() / 2);
		} else {
			center.Set(screenFrame.left + screenFrame.Width() / 2,
				screenFrame.top + screenFrame.Height() / 2);
		}
	}
	BRect target(center.x - width / 2, center.y - height / 2,
		center.x + width / 2, center.y + height / 2);
	target.OffsetTo(floorf(target.left), floorf(target.top));
	// Keep it on the screen, tab included.
	BRect inside = screenFrame;
	inside.top += 30;
	inside.InsetBy(8, 8);
	if (target.right > inside.right)
		target.OffsetBy(inside.right - target.right, 0);
	if (target.bottom > inside.bottom)
		target.OffsetBy(0, inside.bottom - target.bottom);
	if (target.left < inside.left)
		target.OffsetTo(inside.left, target.top);
	if (target.top < inside.top)
		target.OffsetTo(target.left, inside.top);
	return target;
}


void
PreviewWindow::_StartAnimation(BRect from, BRect to, bool hiding)
{
	delete fAnimation;
	fAnimationFrom = from;
	fAnimationTo = to;
	fAnimationStep = 0;
	fAnimationHides = hiding;
	BMessage step(kMsgAnimate);
	fAnimation = new BMessageRunner(BMessenger(this), &step,
		kAnimationInterval, kAnimationSteps);
}


void
PreviewWindow::_AnimationStep()
{
	if (fAnimation == NULL)
		return;
	fAnimationStep++;
	float t = (float)fAnimationStep / kAnimationSteps;
	// Fast at first, settling at the end.
	float eased = 1 - (1 - t) * (1 - t) * (1 - t);
	BRect frame;
	frame.left = fAnimationFrom.left
		+ (fAnimationTo.left - fAnimationFrom.left) * eased;
	frame.top = fAnimationFrom.top
		+ (fAnimationTo.top - fAnimationFrom.top) * eased;
	frame.right = fAnimationFrom.right
		+ (fAnimationTo.right - fAnimationFrom.right) * eased;
	frame.bottom = fAnimationFrom.bottom
		+ (fAnimationTo.bottom - fAnimationFrom.bottom) * eased;
	if (fAnimationStep >= kAnimationSteps) {
		_FinishAnimation();
		return;
	}
	MoveTo(floorf(frame.left), floorf(frame.top));
	ResizeTo(floorf(frame.Width()), floorf(frame.Height()));
	_Layout();
}


void
PreviewWindow::_FinishAnimation()
{
	delete fAnimation;
	fAnimation = NULL;
	if (fAnimationHides) {
		fAnimationHides = false;
		_SetPreview(NULL);
		Hide();
		if (fNotifyAfterHide)
			_NotifyClosed();
		fNotifyAfterHide = false;
		be_app->PostMessage(kMsgQuickLookHide);
		return;
	}
	MoveTo(fAnimationTo.LeftTop());
	ResizeTo(fAnimationTo.Width(), fAnimationTo.Height());
	_Layout();
}


void
PreviewWindow::_Layout()
{
	BRect bounds = Bounds();
	float footer = _FooterHeight();
	fBorder->MoveTo(0, 0);
	fBorder->ResizeTo(bounds.Width(), bounds.Height());
	// Inside the line.
	bounds.InsetBy(1, 1);
	if (bounds.Height() < footer + 2) {
		// Icon sized, mid zoom: the footer waits below.
		footer = std::max(0.0f, bounds.Height() - 2);
	}
	fContent->MoveTo(bounds.left, bounds.top);
	fContent->ResizeTo(bounds.Width(), bounds.Height() - footer);
	if (fPreview != NULL) {
		fPreview->MoveTo(0, 0);
		fPreview->ResizeTo(fContent->Bounds().Width(),
			fContent->Bounds().Height());
	}

	fFooter->MoveTo(bounds.left, bounds.bottom - footer + 1);
	fFooter->ResizeTo(bounds.Width(), footer);
	if (footer < _FooterHeight())
		return;

	float spacing = be_control_look->DefaultItemSpacing();
	float buttonHeight = footer - spacing;
	float buttonWidth = fOpen->StringWidth(fOpen->Label()) + spacing * 2.5f;
	fOpen->ResizeTo(buttonWidth, buttonHeight);
	fOpen->MoveTo(bounds.Width() - buttonWidth - spacing / 2,
		floorf((footer - buttonHeight) / 2) + 1);
	// The name, then what the preview says of the file.
	float nameWidth = fName->StringWidth(fName->Text()) + 4;

	float arrowWidth = buttonHeight;
	bool several = fRefs.size() > 1;
	if (several) {
		float countWidth = fCount->StringWidth("00 of 00") + spacing;
		float groupWidth = arrowWidth * 2 + countWidth + spacing;
		float left = floorf((bounds.Width() - groupWidth) / 2);
		float top = floorf((footer - buttonHeight) / 2) + 1;
		fPrevious->MoveTo(left, top);
		fPrevious->ResizeTo(arrowWidth, buttonHeight);
		fCount->MoveTo(left + arrowWidth + spacing / 2, top);
		fCount->ResizeTo(countWidth, buttonHeight);
		fNext->MoveTo(left + arrowWidth + spacing + countWidth, top);
		fNext->ResizeTo(arrowWidth, buttonHeight);
		if (fPrevious->IsHidden()) {
			fPrevious->Show();
			fNext->Show();
			fCount->Show();
		}
	} else if (!fPrevious->IsHidden()) {
		fPrevious->Hide();
		fNext->Hide();
		fCount->Hide();
	}

	font_height height;
	be_plain_font->GetHeight(&height);
	float textHeight = ceilf(height.ascent + height.descent);
	float summaryRight = several ? fPrevious->Frame().left - spacing
		: fOpen->Frame().left - spacing;
	float room = std::max(10.0f, summaryRight - spacing);
	nameWidth = std::min(nameWidth, floorf(room * 0.45f));
	fName->MoveTo(spacing, floorf((footer - textHeight) / 2) + 1);
	fName->ResizeTo(nameWidth, textHeight);
	fSummary->MoveTo(spacing + nameWidth + spacing / 2,
		floorf((footer - textHeight) / 2) + 1);
	fSummary->ResizeTo(std::max(10.0f, room - nameWidth - spacing / 2),
		textHeight);
}


void
PreviewWindow::_UpdateFooter()
{
	BString summary;
	if (fPreview != NULL)
		summary = fPreview->Summary();
	if (summary.Length() == 0)
		summary = fDefaultSummary;
	summary.Prepend("\xE2\x80\x94 ");
	fSummary->SetText(summary);

	if (fRefs.size() > 1) {
		BString count;
		count.SetToFormat(B_TRANSLATE_COMMENT("%d of %d",
			"item number of item count"), (int)fIndex + 1, (int)fRefs.size());
		fCount->SetText(count);
	}
	_Layout();
}


void
PreviewWindow::_Step(int32 direction)
{
	if (fRefs.size() < 2)
		return;
	fIndex = (fIndex + direction + (int32)fRefs.size()) % (int32)fRefs.size();
	_ShowCurrent();
}


void
PreviewWindow::_OpenCurrent()
{
	if (fRefs.empty())
		return;
	entry_ref ref = fRefs[fIndex];
	HidePanel(true);
	status_t status = be_roster->Launch(&ref);
	if (status != B_OK && status != B_ALREADY_RUNNING) {
		syslog(LOG_WARNING, "quick_look: cannot open %s: %s", ref.name,
			strerror(status));
	}
}


void
PreviewWindow::_NotifyClosed()
{
	if (fOwner.IsValid())
		fOwner.SendMessage(kMsgQuickLookClosed);
}


void
PreviewWindow::_NotifyShown()
{
	if (fOwner.IsValid())
		fOwner.SendMessage(kMsgQuickLookShown);
}
