/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "Cards.h"

#include <algorithm>
#include <float.h>
#include <math.h>

#include <ControlLook.h>
#include <GroupLayout.h>
#include <LayoutUtils.h>
#include <MenuBar.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <SpaceLayoutItem.h>


static float
ui_scale()
{
	return std::max(1.0f, be_plain_font->Size() / 12.0f);
}


rgb_color
card_color()
{
	return mix_color(ui_color(B_PANEL_BACKGROUND_COLOR),
		ui_color(B_DOCUMENT_BACKGROUND_COLOR), 150);
}


rgb_color
card_border_color()
{
	return mix_color(card_color(), ui_color(B_PANEL_TEXT_COLOR), 36);
}


rgb_color
dimmed_text_color()
{
	return mix_color(ui_color(B_PANEL_TEXT_COLOR),
		ui_color(B_PANEL_BACKGROUND_COLOR), 100);
}


static void
apply_background(BView* view, rgb_color background)
{
	for (BView* child = view->ChildAt(0); child != NULL;
			child = child->NextSibling()) {
		if (dynamic_cast<WrappingText*>(child) == NULL
			&& (dynamic_cast<BTextView*>(child) != NULL
				|| dynamic_cast<BMenuBar*>(child) != NULL
				|| dynamic_cast<BScrollBar*>(child) != NULL
				|| dynamic_cast<BScrollView*>(child) != NULL)) {
			continue;
		}
		child->SetViewColor(background);
		child->SetLowColor(background);
		child->Invalidate();
		apply_background(child, background);
	}
}


// #pragma mark - Card


Card::Card(const char* name)
	:
	BView(name, B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
	fLayout(new BGroupLayout(B_VERTICAL, 0)),
	fLabelWidth(-1)
{
	SetLayout(fLayout);

	float spacing = be_control_look->DefaultItemSpacing();
	fLayout->SetInsets(ceilf(spacing * 1.2f), ceilf(spacing * 0.6f),
		ceilf(spacing * 1.2f), ceilf(spacing * 0.6f));
	fLayout->SetSpacing(ceilf(spacing * 1.2f));

	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
}


void
Card::AddRow(const char* label, BView* control)
{
	AddRow(new BStringView("label", label), control);
}


void
Card::AddRow(BView* label, BView* control)
{
	if (label == NULL) {
		AddFullRow(control);
		return;
	}

	BGroupView* row = new BGroupView(B_HORIZONTAL, B_USE_DEFAULT_SPACING);
	label->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT,
		B_ALIGN_VERTICAL_CENTER));
	control->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT,
		B_ALIGN_VERTICAL_CENTER));
	row->GroupLayout()->AddView(label, 0.0f);
	if (control->MaxSize().width < 10000) {
		// A control that does not grow leaves the room to a glue, not to
		// the label, so it stays next to the label.
		BGroupView* holder = new BGroupView(B_HORIZONTAL, 0);
		holder->GroupLayout()->AddView(control, 0.0f);
		holder->GroupLayout()->AddItem(BSpaceLayoutItem::CreateGlue(), 1.0f);
		control = holder;
	}
	row->GroupLayout()->AddView(control, 1.0f);
	fLayout->AddView(row);
	fRowViews.push_back(row);
	fLabels.push_back(label);
	_UpdateLabels();

	if (Window() != NULL)
		ApplyColors();
}


void
Card::AddFullRow(BView* view)
{
	fLayout->AddView(view);
	fRowViews.push_back(view);

	if (Window() != NULL)
		ApplyColors();
}


float
Card::NaturalLabelWidth() const
{
	float width = 0;
	for (size_t i = 0; i < fLabels.size(); i++) {
		BStringView* label = dynamic_cast<BStringView*>(fLabels[i]);
		float labelWidth = label != NULL
			? ceilf(label->StringWidth(label->Text())) + 2
			: fLabels[i]->PreferredSize().width;
		width = std::max(width, labelWidth);
	}
	return std::min(width, ceilf(be_plain_font->Size() * 16));
}


void
Card::SetLabelWidth(float width)
{
	fLabelWidth = width;
	_UpdateLabels();
}


void
Card::_UpdateLabels()
{
	float width = fLabelWidth >= 0 ? fLabelWidth : NaturalLabelWidth();
	for (size_t i = 0; i < fLabels.size(); i++) {
		fLabels[i]->SetExplicitMinSize(BSize(width, B_SIZE_UNSET));
		fLabels[i]->SetExplicitMaxSize(BSize(width, B_SIZE_UNSET));
		BStringView* label = dynamic_cast<BStringView*>(fLabels[i]);
		if (label != NULL
			&& ceilf(label->StringWidth(label->Text())) + 2 > width) {
			label->SetToolTip(label->Text());
		}
	}
}


void
Card::AllAttached()
{
	BView::AllAttached();
	ApplyColors();
}


void
Card::MessageReceived(BMessage* message)
{
	if (message->what == B_COLORS_UPDATED)
		ApplyColors();
	BView::MessageReceived(message);
}


void
Card::ApplyColors()
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	rgb_color background = card_color();
	SetLowColor(background);
	apply_background(this, background);
	Invalidate();
}


void
Card::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	float radius = 7 * ui_scale();

	SetDrawingMode(B_OP_OVER);
	SetHighColor(card_color());
	FillRoundRect(bounds.InsetByCopy(0.5f, 0.5f), radius, radius);
	SetHighColor(card_border_color());
	StrokeRoundRect(bounds.InsetByCopy(0.5f, 0.5f), radius, radius);

	// lines between the rows
	float left, top, right, bottom;
	fLayout->GetInsets(&left, &top, &right, &bottom);
	SetHighColor(mix_color(card_color(), ui_color(B_PANEL_TEXT_COLOR), 22));
	for (size_t row = 1; row < fRowViews.size(); row++) {
		BView* view = fRowViews[row];
		BView* previous = fRowViews[row - 1];
		if (view->IsHidden(this) || previous->IsHidden(this))
			continue;

		float y = floorf((previous->Frame().bottom + view->Frame().top) / 2)
			+ 0.5f;
		StrokeLine(BPoint(left, y), BPoint(bounds.right - right, y));
	}
	SetDrawingMode(B_OP_COPY);
}


void
Card::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	Invalidate();
}


// #pragma mark - PageViewport


PageViewport::PageViewport(BView* content)
	:
	BView("page viewport", B_FRAME_EVENTS | B_SCROLL_VIEW_AWARE),
	fContent(content)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	AddChild(content);
}


BSize
PageViewport::MinSize()
{
	BSize size = fContent->MinSize();
	return BSize(size.width, std::min(size.height, 100.0f));
}


BSize
PageViewport::MaxSize()
{
	return BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED);
}


BSize
PageViewport::PreferredSize()
{
	BSize size = fContent->PreferredSize();
	return BSize(size.width, std::min(size.height, 400.0f));
}


void
PageViewport::DoLayout()
{
	_Update();
}


void
PageViewport::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	_Update();
}


float
PageViewport::ScrollPosition()
{
	return Bounds().top;
}


void
PageViewport::SetScrollPosition(float position)
{
	_Update();
	BScrollBar* bar = ScrollBar(B_VERTICAL);
	if (bar != NULL) {
		float min, max;
		bar->GetRange(&min, &max);
		position = std::min(position, max);
	}
	ScrollTo(0, std::max(0.0f, position));
}


void
PageViewport::_Update()
{
	BRect bounds = Bounds();
	float width = bounds.Width();
	float viewHeight = bounds.Height();

	float height = std::max(fContent->MinSize().height,
		fContent->PreferredSize().height);
	if (fContent->HasHeightForWidth()) {
		float min, max, preferred;
		fContent->GetHeightForWidth(width, &min, &max, &preferred);
		height = std::max(min, preferred);
	}

	float contentHeight = std::max(height, viewHeight);
	if (fContent->Frame().LeftTop() != B_ORIGIN)
		fContent->MoveTo(B_ORIGIN);
	if (fContent->Bounds().Width() != width
		|| fContent->Bounds().Height() != contentHeight) {
		fContent->ResizeTo(width, contentHeight);
	}

	float range = std::max(0.0f, contentHeight - viewHeight);
	BScrollBar* bar = ScrollBar(B_VERTICAL);
	if (bar != NULL) {
		bar->SetRange(0, range);
		bar->SetProportion(contentHeight > 0 ? viewHeight / contentHeight : 1);
		bar->SetSteps(ceilf(be_plain_font->Size() * 3),
			std::max(1.0f, viewHeight - be_plain_font->Size() * 3));
	}
	if (bounds.top > range)
		ScrollTo(0, range);
}


BScrollView*
make_page_scroller(BView* content, PageViewport** _viewport)
{
	PageViewport* viewport = new PageViewport(content);
	if (_viewport != NULL)
		*_viewport = viewport;

	BScrollView* scrollView = new BScrollView("page scroller", viewport, 0,
		false, true, B_NO_BORDER);
	return scrollView;
}


// #pragma mark - text


BStringView*
make_section_title(const char* title)
{
	BStringView* view = new BStringView("section title", title);
	BFont font(be_bold_font);
	view->SetFont(&font);
	view->SetExplicitMinSize(BSize(
		std::min(ceilf(font.StringWidth(title)) + 2, 120.0f), B_SIZE_UNSET));
	view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	return view;
}


BStringView*
make_caption(const char* text)
{
	BStringView* view = new BStringView("caption", text);
	BFont font(be_plain_font);
	font.SetSize(ceilf(font.Size() * 0.9f));
	view->SetFont(&font);
	view->SetHighColor(dimmed_text_color());
	view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	return view;
}


WrappingText::WrappingText(const char* name, const char* text, bool dimmed)
	:
	BTextView(name),
	fDimmed(dimmed)
{
	SetText(text);
	MakeEditable(false);
	MakeSelectable(false);
	SetWordWrap(true);
	SetStylable(true);
	SetInsets(0, 0, 0, 0);
	SetExplicitMinSize(BSize(120, B_SIZE_UNSET));
}


void
WrappingText::AttachedToWindow()
{
	BTextView::AttachedToWindow();
	if (Parent() != NULL) {
		SetViewColor(Parent()->ViewColor());
		SetLowColor(Parent()->ViewColor());
	}

	BFont font(be_plain_font);
	if (fDimmed)
		font.SetSize(ceilf(font.Size() * 0.9f));
	rgb_color color = fDimmed ? dimmed_text_color()
		: ui_color(B_PANEL_TEXT_COLOR);
	SetFontAndColor(&font, B_FONT_ALL, &color);
}


void
WrappingText::AllAttached()
{
	BTextView::AllAttached();
	if (Parent() != NULL) {
		SetViewColor(Parent()->LowColor());
		SetLowColor(Parent()->LowColor());
	}
}


void
WrappingText::GetHeightForWidth(float width, float* min, float* max,
	float* preferred)
{
	float minHeight, maxHeight, preferredHeight;
	BTextView::GetHeightForWidth(width, &minHeight, &maxHeight,
		&preferredHeight);
	// as tall as the text, not taller
	if (min != NULL)
		*min = preferredHeight;
	if (max != NULL)
		*max = preferredHeight;
	if (preferred != NULL)
		*preferred = preferredHeight;
}


LinkButton::LinkButton(const char* label, BMessage* message)
	:
	BButton("link", label, message)
{
	SetFlat(true);
}


// #pragma mark - BadgeView


BadgeView::BadgeView(glyph_kind kind, rgb_color color, float size)
	:
	BView("badge", B_WILL_DRAW),
	fKind(kind),
	fColor(color)
{
	SetExplicitSize(BSize(size, size));
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
}


void
BadgeView::SetGlyph(glyph_kind kind, rgb_color color)
{
	fKind = kind;
	fColor = color;
	Invalidate();
}


void
BadgeView::Draw(BRect updateRect)
{
	draw_glyph_badge(this, Bounds(), fKind, fColor);
}


// #pragma mark - TitleView


TitleView::TitleView(const char* text, const BFont& font, bool dimmed)
	:
	BView("title", B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
	fText(text),
	fDimmed(dimmed)
{
	SetFont(&font);
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	SetLowUIColor(B_PANEL_BACKGROUND_COLOR);
	if (dimmed)
		SetHighColor(dimmed_text_color());
	else
		SetHighUIColor(B_PANEL_TEXT_COLOR);
}


void
TitleView::SetText(const char* text)
{
	if (fText == text)
		return;
	fText = text;
	InvalidateLayout();
	Invalidate();
}


BSize
TitleView::MinSize()
{
	font_height height;
	GetFontHeight(&height);
	return BLayoutUtils::ComposeSize(ExplicitMinSize(),
		BSize(StringWidth("WWW…"),
			ceilf(height.ascent + height.descent + height.leading)));
}


BSize
TitleView::PreferredSize()
{
	font_height height;
	GetFontHeight(&height);
	return BLayoutUtils::ComposeSize(ExplicitPreferredSize(),
		BSize(ceilf(StringWidth(fText)) + 2,
			ceilf(height.ascent + height.descent + height.leading)));
}


BSize
TitleView::MaxSize()
{
	return BLayoutUtils::ComposeSize(ExplicitMaxSize(),
		BSize(B_SIZE_UNLIMITED, PreferredSize().height));
}


void
TitleView::Draw(BRect updateRect)
{
	BString text(fText);
	BRect bounds = Bounds();
	TruncateString(&text, B_TRUNCATE_END, bounds.Width());

	font_height height;
	GetFontHeight(&height);
	SetDrawingMode(B_OP_OVER);
	DrawString(text, BPoint(0, bounds.top + ceilf(height.ascent)));
	SetDrawingMode(B_OP_COPY);
}


void
TitleView::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	Invalidate();
}
