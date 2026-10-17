/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "QueueView.h"

#include <Bitmap.h>
#include <Catalog.h>
#include <ControlLook.h>
#include <NodeInfo.h>
#include <Path.h>
#include <Window.h>

#include <stdio.h>

#include "DiscSource.h"
#include "MediaProbe.h"
#include "Messages.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Queue"


int32 QueueItem::sNextID = 1;


QueueItem::QueueItem(const entry_ref& ref)
	:
	BListItem(),
	fID(atomic_add(&sNextID, 1)),
	fRef(ref),
	fDisc(false),
	fDiscTitle(-1),
	fState(kJobProbing),
	fProgress(0),
	fIcon(NULL),
	fBaseline(0),
	fLineHeight(0)
{
	memset(&fLastProgress, 0, sizeof(fLastProgress));
}


QueueItem::~QueueItem()
{
	delete fIcon;
}


BString
QueueItem::Path() const
{
	BPath path(&fRef);
	if (fDisc)
		return disc_source(path.Path(), fDiscTitle);
	return BString(path.Path());
}


BString
QueueItem::Name() const
{
	if (!fInfo.name.IsEmpty())
		return fInfo.name;
	if (fDisc) {
		BPath path(&fRef);
		return disc_name(path.Path());
	}
	return BString(fRef.name);
}


void
QueueItem::SetInfo(const SourceInfo& info)
{
	fInfo = info;
}


void
QueueItem::SetProgress(const TranscodeProgress& progress)
{
	fLastProgress = progress;
	fProgress = progress.fraction;
}


void
QueueItem::Update(BView* owner, const BFont* font)
{
	font_height height;
	font->GetHeight(&height);
	fLineHeight = ceilf(height.ascent + height.descent + height.leading);
	fBaseline = ceilf(height.ascent);
	float iconSize = be_control_look->ComposeIconSize(32).Width() + 1;
	SetHeight(max_c(iconSize, fLineHeight * 2 + 4) + 8);
	SetWidth(owner->Bounds().Width());

	if (fIcon == NULL) {
		BRect bounds(0, 0, iconSize - 1, iconSize - 1);
		fIcon = new(std::nothrow) BBitmap(bounds, B_RGBA32);
		if (fIcon != NULL && BNodeInfo::GetTrackerIcon(&fRef, fIcon,
				(icon_size)(int)iconSize) != B_OK) {
			delete fIcon;
			fIcon = NULL;
		}
	}
}


BString
QueueItem::_StatusText() const
{
	BString text;
	switch (fState) {
		case kJobProbing:
			text = B_TRANSLATE("Reading…");
			break;
		case kJobWaiting:
		{
			BString duration = MediaProbe::FormatDuration(fInfo.duration);
			const VideoTrackInfo* video = fInfo.MainVideo();
			if (video != NULL) {
				text.SetToFormat("%s · %d×%d · %s", duration.String(),
					video->width, video->height,
					MediaProbe::FormatSize(fInfo.fileSize).String());
			} else {
				text.SetToFormat("%s · %s", duration.String(),
					MediaProbe::FormatSize(fInfo.fileSize).String());
			}
			break;
		}
		case kJobConverting:
		{
			BString remaining;
			if (fLastProgress.remaining >= 0) {
				remaining.SetToFormat(B_TRANSLATE(", %s left"),
					MediaProbe::FormatDuration(
						fLastProgress.remaining).String());
			}
			if (fLastProgress.framesPerSecond > 0) {
				text.SetToFormat(B_TRANSLATE("%.0f%% · %.0f fps%s"),
					fProgress * 100, fLastProgress.framesPerSecond,
					remaining.String());
			} else {
				text.SetToFormat(B_TRANSLATE("%.0f%% · %.1f× real time%s"),
					fProgress * 100, fLastProgress.speed, remaining.String());
			}
			break;
		}
		case kJobDone:
		case kJobFailed:
		case kJobCancelled:
		case kJobUnreadable:
			text = fMessage;
			break;
	}
	return text;
}


void
QueueItem::DrawItem(BView* owner, BRect frame, bool complete)
{
	rgb_color background = IsSelected()
		? ui_color(B_LIST_SELECTED_BACKGROUND_COLOR)
		: ui_color(B_LIST_BACKGROUND_COLOR);
	rgb_color text = IsSelected() ? ui_color(B_LIST_SELECTED_ITEM_TEXT_COLOR)
		: ui_color(B_LIST_ITEM_TEXT_COLOR);
	owner->SetLowColor(background);
	owner->FillRect(frame, B_SOLID_LOW);

	float x = frame.left + 6;
	if (fIcon != NULL) {
		owner->SetDrawingMode(B_OP_ALPHA);
		BRect iconFrame = fIcon->Bounds();
		float top = frame.top + (frame.Height() - iconFrame.Height()) / 2;
		owner->DrawBitmap(fIcon, BPoint(x, floorf(top)));
		owner->SetDrawingMode(B_OP_COPY);
		x += iconFrame.Width() + 8;
	}

	BFont font;
	owner->GetFont(&font);
	BFont bold(font);
	bold.SetFace(B_BOLD_FACE);
	owner->SetFont(&bold);
	owner->SetHighColor(text);
	float top = frame.top + 4;
	BString name = Name();
	owner->TruncateString(&name, B_TRUNCATE_MIDDLE, frame.right - x - 6);
	owner->DrawString(name.String(), BPoint(x, top + fBaseline));
	owner->SetFont(&font);

	BString status = _StatusText();
	rgb_color dim = tint_color(text, IsSelected() ? B_NO_TINT
		: (text.IsLight() ? B_DARKEN_2_TINT : B_LIGHTEN_1_TINT));
	if (fState == kJobFailed || fState == kJobUnreadable)
		dim = (rgb_color){ 220, 50, 40, 255 };
	else if (fState == kJobDone)
		dim = IsSelected() ? text : (rgb_color){ 40, 150, 70, 255 };
	owner->SetHighColor(dim);
	float statusTop = top + fLineHeight + 2;
	if (fState == kJobConverting) {
		// a bar under the name, with the numbers beside it
		BRect bar(x, statusTop + 2, x + min_c(120.0f, (frame.right - x) / 3),
			statusTop + fLineHeight - 3);
		rgb_color barColor = ui_color(B_STATUS_BAR_COLOR);
		owner->SetHighColor(tint_color(background, B_DARKEN_2_TINT));
		owner->StrokeRect(bar);
		bar.InsetBy(1, 1);
		BRect done = bar;
		done.right = bar.left + bar.Width() * fProgress;
		owner->SetHighColor(barColor);
		owner->FillRect(done);
		owner->SetHighColor(dim);
		x = bar.right + 8;
	}
	owner->TruncateString(&status, B_TRUNCATE_END, frame.right - x - 6);
	owner->DrawString(status.String(), BPoint(x, statusTop + fBaseline));
}


// #pragma mark - QueueView


QueueView::QueueView()
	:
	BListView("queue", B_MULTIPLE_SELECTION_LIST)
{
}


void
QueueView::MessageReceived(BMessage* message)
{
	if (message->WasDropped() && message->HasRef("refs")) {
		BMessage refs(*message);
		refs.what = B_REFS_RECEIVED;
		Window()->PostMessage(&refs);
		return;
	}
	BListView::MessageReceived(message);
}


void
QueueView::KeyDown(const char* bytes, int32 count)
{
	if (count == 1 && (bytes[0] == B_DELETE || bytes[0] == B_BACKSPACE)) {
		Window()->PostMessage(kMsgRemoveFiles);
		return;
	}
	BListView::KeyDown(bytes, count);
}


void
QueueView::Draw(BRect updateRect)
{
	BListView::Draw(updateRect);
	if (CountItems() > 0)
		return;
	// an empty queue says what to do with it
	BString hint(B_TRANSLATE("Drop video or audio files here"));
	BFont font;
	GetFont(&font);
	SetHighColor(tint_color(ui_color(B_LIST_BACKGROUND_COLOR),
		ui_color(B_LIST_BACKGROUND_COLOR).IsLight() ? B_DARKEN_3_TINT
			: B_LIGHTEN_2_TINT));
	BRect bounds = Bounds();
	TruncateString(&hint, B_TRUNCATE_END, bounds.Width() - 16);
	float width = StringWidth(hint.String());
	font_height height;
	font.GetHeight(&height);
	DrawString(hint.String(), BPoint(bounds.left + (bounds.Width() - width) / 2,
		bounds.top + bounds.Height() / 2 + height.ascent / 2));
}


QueueItem*
QueueView::ItemByID(int32 id) const
{
	for (int32 i = 0; i < CountItems(); i++) {
		QueueItem* item = QueueItemAt(i);
		if (item->ID() == id)
			return item;
	}
	return NULL;
}


QueueItem*
QueueView::SelectedItem() const
{
	int32 index = CurrentSelection();
	return index >= 0 ? QueueItemAt(index) : NULL;
}


void
QueueView::InvalidateItem(QueueItem* item)
{
	int32 index = IndexOf(item);
	if (index >= 0)
		BListView::InvalidateItem(index);
}
