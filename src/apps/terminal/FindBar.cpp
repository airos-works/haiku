/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "FindBar.h"

#include <math.h>

#include <algorithm>

#include <Catalog.h>
#include <Control.h>
#include <ControlLook.h>
#include <MenuItem.h>
#include <MessageFilter.h>
#include <PopUpMenu.h>
#include <StringView.h>
#include <TextControl.h>
#include <Window.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Terminal FindBar"


static const uint32 kOptionsClicked = 'fbop';
static const uint32 kSetCaseMode = 'fbcm';
static const uint32 kToggleRegularExpression = 'fbre';
static const uint32 kToggleWholeWords = 'fbww';
static const uint32 kFieldChanged = 'fbfc';

static const float kInset = 4;
static const float kMargin = 8;


// #pragma mark - FindBarButton


/*!	A small flat button with a drawn symbol: the options, up, down, close.
*/
class FindBarButton : public BControl {
public:
	enum {
		OPTIONS,
		UP,
		DOWN,
		CLOSE
	};

	FindBarButton(BRect frame, int32 symbol, BMessage* message,
		const char* toolTip)
		:
		BControl(frame, "find bar button", NULL, message,
			B_FOLLOW_LEFT_TOP, B_WILL_DRAW | B_NAVIGABLE_JUMP),
		fSymbol(symbol),
		fHovering(false),
		fPressed(false)
	{
		SetToolTip(toolTip);
	}

	virtual void AttachedToWindow()
	{
		BControl::AttachedToWindow();
		SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
		SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
	}

	virtual void Draw(BRect updateRect)
	{
		BRect bounds = Bounds();
		rgb_color base = ui_color(B_PANEL_BACKGROUND_COLOR);
		if (fHovering || fPressed) {
			BRect frame = bounds;
			uint32 flags = fPressed ? BControlLook::B_ACTIVATED : 0;
			be_control_look->DrawButtonFrame(this, frame, updateRect, base,
				base, flags);
			be_control_look->DrawButtonBackground(this, frame, updateRect,
				base, flags);
		}

		rgb_color color = ui_color(B_PANEL_TEXT_COLOR);
		if (!IsEnabled())
			color = tint_color(base, B_DISABLED_LABEL_TINT);
		SetHighColor(color);
		SetPenSize(std::max(1.0f, floorf(be_plain_font->Size() / 12.0f
			+ 0.5f)));

		float size = floorf(std::min(bounds.Width(), bounds.Height()) * 0.36f);
		BPoint center((bounds.left + bounds.right) / 2,
			(bounds.top + bounds.bottom) / 2);
		switch (fSymbol) {
			case UP:
			case DOWN:
			{
				float direction = fSymbol == UP ? -1 : 1;
				BPoint points[3] = {
					BPoint(center.x - size, center.y - direction * size / 2),
					BPoint(center.x, center.y + direction * size / 2),
					BPoint(center.x + size, center.y - direction * size / 2)
				};
				SetDrawingMode(B_OP_OVER);
				StrokeLine(points[0], points[1]);
				StrokeLine(points[1], points[2]);
				break;
			}
			case CLOSE:
				size *= 0.8f;
				StrokeLine(center + BPoint(-size, -size),
					center + BPoint(size, size));
				StrokeLine(center + BPoint(-size, size),
					center + BPoint(size, -size));
				break;
			case OPTIONS:
			{
				// a magnifying glass and a small arrow: there is a menu
				float radius = size * 0.7f;
				BPoint glass = center + BPoint(-size * 0.45f, -size * 0.3f);
				StrokeEllipse(glass, radius, radius);
				StrokeLine(glass + BPoint(radius * 0.7f, radius * 0.7f),
					glass + BPoint(radius * 1.6f, radius * 1.6f));
				float arrow = size * 0.4f;
				BPoint tip = center + BPoint(size * 1.0f, size * 0.55f);
				FillTriangle(tip + BPoint(-arrow, -arrow / 1.5f),
					tip + BPoint(arrow, -arrow / 1.5f), tip);
				break;
			}
		}
		SetPenSize(1);
	}

	virtual void MouseDown(BPoint where)
	{
		if (!IsEnabled())
			return;
		fPressed = true;
		SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
		Invalidate();
		if (fSymbol == OPTIONS) {
			// the menu opens at once
			fPressed = false;
			Invoke();
		}
	}

	virtual void MouseMoved(BPoint where, uint32 transit,
		const BMessage* dragMessage)
	{
		bool hovering = transit != B_EXITED_VIEW
			&& transit != B_OUTSIDE_VIEW && Bounds().Contains(where);
		if (hovering != fHovering) {
			fHovering = hovering;
			Invalidate();
		}
	}

	virtual void MouseUp(BPoint where)
	{
		if (!fPressed)
			return;
		fPressed = false;
		Invalidate();
		if (Bounds().Contains(where))
			Invoke();
	}

private:
	int32	fSymbol;
	bool	fHovering;
	bool	fPressed;
};


// #pragma mark - key filter


/*!	Keys typed into the field that are the bar's: Escape closes it, Return
	goes to the match above (Shift+Return below), as do the arrow keys.
*/
static filter_result
find_field_key_filter(BMessage* message, BHandler** target,
	BMessageFilter* filter)
{
	const char* bytes;
	if (message->FindString("bytes", &bytes) != B_OK)
		return B_DISPATCH_MESSAGE;
	int32 modifiers = message->GetInt32("modifiers", 0);
	if ((modifiers & (B_COMMAND_KEY | B_CONTROL_KEY | B_OPTION_KEY)) != 0)
		return B_DISPATCH_MESSAGE;

	BView* view = dynamic_cast<BView*>(*target);
	FindBar* bar = NULL;
	for (BView* parent = view; parent != NULL; parent = parent->Parent()) {
		bar = dynamic_cast<FindBar*>(parent);
		if (bar != NULL)
			break;
	}
	if (bar == NULL)
		return B_DISPATCH_MESSAGE;

	uint32 what = 0;
	switch (bytes[0]) {
		case B_ESCAPE:
			what = MSG_FIND_BAR_CLOSE;
			break;
		case B_ENTER:
			what = (modifiers & B_SHIFT_KEY) != 0
				? MSG_FIND_BAR_DOWN : MSG_FIND_BAR_UP;
			break;
		case B_UP_ARROW:
			what = MSG_FIND_BAR_UP;
			break;
		case B_DOWN_ARROW:
			what = MSG_FIND_BAR_DOWN;
			break;
		default:
			return B_DISPATCH_MESSAGE;
	}
	BMessage request(what);
	bar->MessageReceived(&request);
	return B_SKIP_MESSAGE;
}


// #pragma mark - FindBar


FindBar::FindBar(BHandler* target)
	:
	BView(BRect(0, 0, 100, 20), "find bar", B_FOLLOW_RIGHT | B_FOLLOW_TOP,
		B_WILL_DRAW),
	fTarget(target),
	fCaseMode(FIND_SMART_CASE),
	fRegularExpression(false),
	fWholeWords(false),
	fError(false)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);

	fField = new BTextControl(BRect(0, 0, 100, 20), "find field", NULL, "",
		NULL, B_FOLLOW_LEFT_TOP);
	fField->SetModificationMessage(new BMessage(kFieldChanged));
	fField->TextView()->AddFilter(new BMessageFilter(B_KEY_DOWN,
		&find_field_key_filter));
	AddChild(fField);

	fStatus = new BStringView(BRect(0, 0, 10, 10), "find status", "",
		B_FOLLOW_LEFT_TOP);
	fStatus->SetAlignment(B_ALIGN_CENTER);
	AddChild(fStatus);

	fOptionsButton = new FindBarButton(BRect(0, 0, 10, 10),
		FindBarButton::OPTIONS, new BMessage(kOptionsClicked),
		B_TRANSLATE("Search options"));
	fPreviousButton = new FindBarButton(BRect(0, 0, 10, 10),
		FindBarButton::UP, new BMessage(MSG_FIND_BAR_UP),
		B_TRANSLATE("Find previous (above)"));
	fNextButton = new FindBarButton(BRect(0, 0, 10, 10),
		FindBarButton::DOWN, new BMessage(MSG_FIND_BAR_DOWN),
		B_TRANSLATE("Find next (below)"));
	fCloseButton = new FindBarButton(BRect(0, 0, 10, 10),
		FindBarButton::CLOSE, new BMessage(MSG_FIND_BAR_CLOSE),
		B_TRANSLATE("Close"));
	AddChild(fOptionsButton);
	AddChild(fPreviousButton);
	AddChild(fNextButton);
	AddChild(fCloseButton);

	_Layout();
}


FindBar::~FindBar()
{
}


void
FindBar::AttachedToWindow()
{
	BView::AttachedToWindow();
	fField->SetTarget(this);
	fOptionsButton->SetTarget(this);
	fPreviousButton->SetTarget(this);
	fNextButton->SetTarget(this);
	fCloseButton->SetTarget(this);
	_Layout();
	UpdatePosition();
}


const char*
FindBar::Text() const
{
	return fField->Text();
}


void
FindBar::SetText(const char* text)
{
	fField->SetText(text);
}


void
FindBar::SetStatus(const char* status, bool error)
{
	fError = error;
	fStatus->SetText(status);
	fStatus->SetHighUIColor(error ? B_FAILURE_COLOR : B_PANEL_TEXT_COLOR);
}


void
FindBar::FocusField()
{
	fField->MakeFocus(true);
	fField->TextView()->SelectAll();
}


bool
FindBar::HasFocus() const
{
	return fField->TextView()->IsFocus();
}


void
FindBar::GetPreferredSize(float* _width, float* _height)
{
	float fieldWidth, fieldHeight;
	fField->GetPreferredSize(&fieldWidth, &fieldHeight);
	float scale = be_plain_font->Size() / 12.0f;
	float button = fieldHeight;
	float statusWidth = ceilf(be_plain_font->StringWidth("8888 of 8888"))
		+ 8 * scale;
	fieldWidth = floorf(190 * scale);
	if (_width != NULL) {
		*_width = kInset + button + 2 + fieldWidth + 4 + statusWidth
			+ 3 * (button + 2) + kInset - 1;
	}
	if (_height != NULL)
		*_height = fieldHeight + 2 * kInset - 1;
}


void
FindBar::_Layout()
{
	float fieldWidth, fieldHeight;
	fField->GetPreferredSize(&fieldWidth, &fieldHeight);
	float scale = be_plain_font->Size() / 12.0f;
	float button = fieldHeight;
	float statusWidth = ceilf(be_plain_font->StringWidth("8888 of 8888"))
		+ 8 * scale;
	fieldWidth = floorf(190 * scale);

	float width, height;
	GetPreferredSize(&width, &height);
	ResizeTo(width, height);

	float x = kInset;
	float y = kInset;
	fOptionsButton->MoveTo(x, y);
	fOptionsButton->ResizeTo(button - 1, button - 1);
	x += button + 2;
	fField->MoveTo(x, y);
	fField->ResizeTo(fieldWidth - 1, fieldHeight - 1);
	fField->SetDivider(0);
	x += fieldWidth + 4;
	fStatus->MoveTo(x, y);
	fStatus->ResizeTo(statusWidth - 1, fieldHeight - 1);
	x += statusWidth;
	FindBarButton* buttons[3] = { fPreviousButton, fNextButton, fCloseButton };
	for (int32 i = 0; i < 3; i++) {
		buttons[i]->MoveTo(x, y);
		buttons[i]->ResizeTo(button - 1, button - 1);
		x += button + 2;
	}
}


void
FindBar::UpdatePosition()
{
	BView* parent = Parent();
	if (parent == NULL)
		return;
	BRect bounds = parent->Bounds();
	float x = std::max(0.0f, bounds.right - Bounds().Width() - kMargin);
	MoveTo(floorf(x), kMargin / 2);
}


void
FindBar::Draw(BRect updateRect)
{
	// a raised panel over the text
	BRect bounds = Bounds();
	rgb_color base = ui_color(B_PANEL_BACKGROUND_COLOR);
	SetHighColor(tint_color(base, B_DARKEN_3_TINT));
	StrokeRect(bounds);
	bounds.InsetBy(1, 1);
	SetHighColor(tint_color(base, B_LIGHTEN_1_TINT));
	StrokeLine(bounds.LeftTop(), bounds.RightTop());
	StrokeLine(bounds.LeftTop(), bounds.LeftBottom());
}


void
FindBar::_Notify(uint32 what)
{
	if (fTarget == NULL || fTarget->Looper() == NULL)
		return;
	BMessage message(what);
	fTarget->Looper()->PostMessage(&message, fTarget);
}


void
FindBar::_ShowOptions()
{
	BPopUpMenu* menu = new BPopUpMenu("search options", false, false);
	const char* caseLabels[3] = {
		B_TRANSLATE("Smart case"),
		B_TRANSLATE("Match case"),
		B_TRANSLATE("Ignore case")
	};
	for (int32 i = 0; i < 3; i++) {
		BMessage* message = new BMessage(kSetCaseMode);
		message->AddInt32("mode", i);
		BMenuItem* item = new BMenuItem(caseLabels[i], message);
		item->SetMarked(fCaseMode == i);
		menu->AddItem(item);
	}
	menu->AddSeparatorItem();
	BMenuItem* item = new BMenuItem(B_TRANSLATE("Regular expression"),
		new BMessage(kToggleRegularExpression));
	item->SetMarked(fRegularExpression);
	menu->AddItem(item);
	item = new BMenuItem(B_TRANSLATE("Whole words"),
		new BMessage(kToggleWholeWords));
	item->SetMarked(fWholeWords);
	menu->AddItem(item);

	menu->SetTargetForItems(this);
	menu->SetAsyncAutoDestruct(true);
	BRect frame = fOptionsButton->Frame();
	BPoint where = ConvertToScreen(frame.LeftBottom() + BPoint(0, 1));
	menu->Go(where, true, true, ConvertToScreen(frame), true);
}


void
FindBar::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kFieldChanged:
			_Notify(MSG_FIND_BAR_CHANGED);
			break;
		case kOptionsClicked:
			_ShowOptions();
			break;
		case kSetCaseMode:
			fCaseMode = message->GetInt32("mode", FIND_SMART_CASE);
			fField->MakeFocus(true);
			_Notify(MSG_FIND_BAR_CHANGED);
			break;
		case kToggleRegularExpression:
			fRegularExpression = !fRegularExpression;
			fField->MakeFocus(true);
			_Notify(MSG_FIND_BAR_CHANGED);
			break;
		case kToggleWholeWords:
			fWholeWords = !fWholeWords;
			fField->MakeFocus(true);
			_Notify(MSG_FIND_BAR_CHANGED);
			break;
		case MSG_FIND_BAR_UP:
		case MSG_FIND_BAR_DOWN:
		case MSG_FIND_BAR_CLOSE:
			_Notify(message->what);
			break;
		default:
			BView::MessageReceived(message);
			break;
	}
}
