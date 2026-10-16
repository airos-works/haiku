/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CARDS_H
#define CARDS_H


#include <Button.h>
#include <GroupView.h>
#include <StringView.h>
#include <TextView.h>
#include <View.h>

#include <vector>

#include "Glyphs.h"


class BBitmap;
class BGroupLayout;
class BScrollView;


// Colors the pages use; they follow the system colors.
rgb_color card_color();
rgb_color card_border_color();
rgb_color dimmed_text_color();


/*!	A rounded box of rows, like the grouped lists of modern settings
	panels. A row has a label on the left and a control on the right, or one
	view across the whole width; lines separate the rows. The labels of a
	card are as wide as the widest of them, so the controls line up.
*/
class Card : public BView {
public:
								Card(const char* name = "card");

			void				AddRow(const char* label, BView* control);
			void				AddRow(BView* label, BView* control);
			void				AddFullRow(BView* view);
			int32				CountRows() const
									{ return fRowViews.size(); }

			//! The width the labels want, at most a limit.
			float				NaturalLabelWidth() const;
			//! Gives all labels this width (several cards can line up).
			void				SetLabelWidth(float width);

	virtual	void				AllAttached();
	virtual	void				Draw(BRect updateRect);
	virtual	void				FrameResized(float width, float height);
	virtual	void				MessageReceived(BMessage* message);

			void				ApplyColors();

private:
			void				_UpdateLabels();

			BGroupLayout*		fLayout;
			std::vector<BView*>	fRowViews;
			std::vector<BView*>	fLabels;
			float				fLabelWidth;
};


/*!	Holds a page's content in a BScrollView: the content gets the width of
	the view and as much height as it wants, so the window can be any size
	and the page scrolls vertically when it does not fit.
*/
class PageViewport : public BView {
public:
								PageViewport(BView* content);

			BView*				Content() const { return fContent; }

	virtual	BSize				MinSize();
	virtual	BSize				MaxSize();
	virtual	BSize				PreferredSize();

	virtual	void				DoLayout();
	virtual	void				FrameResized(float width, float height);

			float				ScrollPosition();
			void				SetScrollPosition(float position);

private:
			void				_Update();

			BView*				fContent;
};


BScrollView* make_page_scroller(BView* content, PageViewport** _viewport);


//! Bold heading of a group of cards.
BStringView* make_section_title(const char* title);

//! Smaller, dimmed text.
BStringView* make_caption(const char* text);

//! Read-only text that wraps to the available width.
class WrappingText : public BTextView {
public:
								WrappingText(const char* name,
									const char* text, bool dimmed = false);

	virtual	void				AttachedToWindow();
	virtual	void				AllAttached();
	virtual	void				GetHeightForWidth(float width, float* min,
									float* max, float* preferred);

private:
			bool				fDimmed;
};


//! A flat button that reads like a link to another page.
class LinkButton : public BButton {
public:
								LinkButton(const char* label,
									BMessage* message);
};


//! A view that draws one of the glyph badges.
class BadgeView : public BView {
public:
								BadgeView(glyph_kind kind, rgb_color color,
									float size);

			void				SetGlyph(glyph_kind kind, rgb_color color);

	virtual	void				Draw(BRect updateRect);

private:
			glyph_kind			fKind;
			rgb_color			fColor;
};


//! A label with a small icon in front, like an application's.
class IconLabel : public BView {
public:
								IconLabel(const BBitmap* icon,
									const char* text);
	virtual						~IconLabel();

			const char*			Text() const { return fText.String(); }

	virtual	BSize				MinSize();
	virtual	BSize				PreferredSize();
	virtual	BSize				MaxSize();
	virtual	void				Draw(BRect updateRect);

private:
			BBitmap*			fIcon;
			BString				fText;
};


//! Frame-less text of a given font with ellipsis instead of clipping.
class TitleView : public BView {
public:
								TitleView(const char* text, const BFont& font,
									bool dimmed);

			void				SetText(const char* text);

	virtual	BSize				MinSize();
	virtual	BSize				PreferredSize();
	virtual	BSize				MaxSize();
	virtual	void				Draw(BRect updateRect);
	virtual	void				FrameResized(float width, float height);

private:
			BString				fText;
			bool				fDimmed;
};


#endif	// CARDS_H
