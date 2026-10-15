/*
 * Copyright 2003-2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *		Jérôme Duval
 *		François Revol
 *		Axel Dörfler, axeld@pinc-software.de.
 *		John Scipione, jscipione@gmail.com
 */


#include "VolumeWindow.h"

#include <algorithm>

#include <Box.h>
#include <Catalog.h>
#include <GroupLayout.h>
#include <LayoutBuilder.h>
#include <MenuItem.h>
#include <MessageRunner.h>
#include <Screen.h>
#include <StringView.h>

#include "AudioDevices.h"
#include "VolumeControl.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "VolumeWindow"


/*!	A small label naming the audio output or input in use, under the slider.
*/
static BStringView*
make_device_label(const char* name, const char* prefix, const char* device,
	float maxWidth)
{
	BString text(prefix);
	text << " " << device;
	BStringView* view = new BStringView(name, text.String());
	BFont font;
	view->GetFont(&font);
	font.SetSize(ceilf(font.Size() * 0.85f));
	view->SetFont(&font);
	view->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_1_TINT);
	// (A layout frame is whole pixels, the string width is not: without a
	// rounded-up minimum, the label is truncated over a fraction of a pixel.)
	float width = ceilf(view->StringWidth(text.String())) + 1;
	if (width <= maxWidth)
		view->SetExplicitMinSize(BSize(width, B_SIZE_UNSET));
	view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	view->SetTruncation(B_TRUNCATE_END);
	return view;
}


VolumeWindow::VolumeWindow(BRect frame)
	:
	BWindow(frame, "VolumeWindow", B_BORDERED_WINDOW_LOOK, B_FLOATING_ALL_WINDOW_FEEL,
		B_ASYNCHRONOUS_CONTROLS | B_WILL_ACCEPT_FIRST_CLICK | B_AUTO_UPDATE_SIZE_LIMITS, 0),
	fUpdatedCount(0)
{
	SetLayout(new BGroupLayout(B_HORIZONTAL));

	BGroupLayout* layout = new BGroupLayout(B_VERTICAL, 0);
	layout->SetInsets(5, 5, 5, 5);

	BBox* box = new BBox("sliderbox");
	box->SetLayout(layout);
	box->SetBorder(B_PLAIN_BORDER);
	AddChild(box);

	fVolumeSlider = new VolumeControl();
	fVolumeSlider->SetModificationMessage(new BMessage(kMsgVolumeUpdate));
	box->AddChild(fVolumeSlider);

	// which devices the sound goes to and comes from
	AudioDevices devices;
	BString output;
	if (!devices.GetDefaultOutputName(output))
		output = B_TRANSLATE("<none>");
	BString input;
	if (!devices.GetDefaultInputName(input))
		input = B_TRANSLATE("<none>");

	float fontScale = be_plain_font->Size() / 12.0f;
	BStringView* outputView = make_device_label("output",
		B_TRANSLATE_COMMENT("Output:", "Audio output device label"),
		output.String(), 280 * fontScale);
	BStringView* inputView = make_device_label("input",
		B_TRANSLATE_COMMENT("Input:", "Audio input device label"),
		input.String(), 280 * fontScale);
	BLayoutBuilder::Group<>(layout)
		.AddGroup(B_HORIZONTAL, 10)
			.Add(outputView)
			.AddGlue()
			.Add(inputView)
		.End();

	fVolumeSlider->SetTarget(this);

	// The window grows with the font (the screen's scaling), and as wide as
	// the device names need, within reason (longer ones are truncated).
	float labelsWidth = outputView->MinSize().width
		+ inputView->MinSize().width + 10 * fontScale + 30;
	float width = std::max(300.0f * fontScale, labelsWidth);
	width = std::min(width, 600.0f * fontScale);
	font_height fontHeight;
	outputView->GetFontHeight(&fontHeight);
	float height = 50 * fontScale
		+ ceilf(fontHeight.ascent + fontHeight.descent + fontHeight.leading);
	ResizeTo(width, height);

	// Make sure it's not outside the screen.
	const int32 kMargin = 3;
	BRect windowRect = Frame();
	BRect screenFrame(BScreen(B_MAIN_SCREEN_ID).Frame());
	if (screenFrame.right < windowRect.right + kMargin)
		MoveBy(- kMargin - windowRect.right + screenFrame.right, 0);
	if (screenFrame.bottom < windowRect.bottom + kMargin)
		MoveBy(0, - kMargin - windowRect.bottom + screenFrame.bottom);
	if (screenFrame.left > windowRect.left - kMargin)
		MoveBy(kMargin + screenFrame.left - windowRect.left, 0);
	if (screenFrame.top > windowRect.top - kMargin)
		MoveBy(0, kMargin + screenFrame.top - windowRect.top);
}


VolumeWindow::~VolumeWindow()
{
}


void
VolumeWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgVolumeUpdate:
			fUpdatedCount++;
			break;

		case kMsgVolumeChanged:
			if (fUpdatedCount < 2) {
				// If the slider was set by click only, wait a bit until
				// closing the window to give some feedback that how volume
				// was changed, and that it is.
				BMessage quit(B_QUIT_REQUESTED);
				BMessageRunner::StartSending(this, &quit, 150000, 1);
			} else
				Quit();
			break;

		case B_QUIT_REQUESTED:
			Quit();
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}

