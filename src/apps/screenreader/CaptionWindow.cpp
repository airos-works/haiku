/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "CaptionWindow.h"

#include <Screen.h>
#include <String.h>
#include <StringList.h>
#include <View.h>


static const uint32 kMsgCaption = 'capt';
static const int32 kMaxLines = 3;


class CaptionView : public BView {
public:
	CaptionView(BRect frame)
		:
		BView(frame, "captions", B_FOLLOW_ALL, B_WILL_DRAW)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
		BFont font(be_bold_font);
		font.SetSize(be_plain_font->Size() * 1.5f);
		SetFont(&font);
	}

	void AddLine(const char* text)
	{
		fLines.Add(text);
		while (fLines.CountStrings() > kMaxLines)
			fLines.Remove(0);
		Invalidate();
	}

	virtual void Draw(BRect updateRect)
	{
		BRect bounds = Bounds();
		SetHighColor(24, 24, 28);
		FillRect(bounds);
		SetHighColor(90, 90, 100);
		StrokeRect(bounds);

		font_height height;
		GetFontHeight(&height);
		float lineHeight = ceilf(height.ascent + height.descent
			+ height.leading) + 4;
		float y = bounds.bottom - 8 - height.descent
			- lineHeight * (fLines.CountStrings() - 1);
		for (int32 i = 0; i < fLines.CountStrings(); i++) {
			bool latest = i == fLines.CountStrings() - 1;
			if (latest)
				SetHighColor(255, 255, 255);
			else
				SetHighColor(150, 150, 160);
			BString line = fLines.StringAt(i);
			TruncateString(&line, B_TRUNCATE_END, bounds.Width() - 24);
			DrawString(line.String(), BPoint(12, y));
			y += lineHeight;
		}
	}

private:
	BStringList fLines;
};


CaptionWindow::CaptionWindow()
	:
	BWindow(BRect(0, 0, 600, 100), "Screen reader captions",
		B_BORDERED_WINDOW_LOOK, B_FLOATING_ALL_WINDOW_FEEL,
		B_AVOID_FOCUS | B_NOT_MOVABLE | B_NOT_RESIZABLE | B_NOT_ZOOMABLE
			| B_NOT_MINIMIZABLE | B_NOT_CLOSABLE | B_NO_WORKSPACE_ACTIVATION,
		B_ALL_WORKSPACES)
{
	fView = new CaptionView(Bounds());
	AddChild(fView);
	_Place();
}


void
CaptionWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgCaption:
			fView->AddLine(message->GetString("text", ""));
			break;

		case B_SCREEN_CHANGED:
			_Place();
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


void
CaptionWindow::AddCaption(const char* text)
{
	BMessage message(kMsgCaption);
	message.AddString("text", text);
	PostMessage(&message);
}


void
CaptionWindow::_Place()
{
	BRect screen = BScreen(this).Frame();
	font_height height;
	BFont font(be_bold_font);
	font.SetSize(be_plain_font->Size() * 1.5f);
	font.GetHeight(&height);
	float lineHeight = ceilf(height.ascent + height.descent + height.leading)
		+ 4;
	float width = floorf(screen.Width() * 0.7f);
	float windowHeight = lineHeight * kMaxLines + 16;
	ResizeTo(width, windowHeight);
	MoveTo(screen.left + floorf((screen.Width() - width) / 2),
		screen.bottom - windowHeight - floorf(lineHeight * 1.5f));
}
