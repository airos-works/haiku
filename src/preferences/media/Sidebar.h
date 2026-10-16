/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SIDEBAR_H
#define SIDEBAR_H


#include <ListItem.h>
#include <ListView.h>
#include <String.h>

#include "Glyphs.h"


class SidebarItem : public BListItem {
public:
	//! A heading between the groups of pages.
								SidebarItem(const char* heading);
								SidebarItem(const char* key,
									const char* title, const char* subtitle,
									glyph_kind glyph, rgb_color color);

			bool				IsHeading() const { return fKey.IsEmpty(); }
			const BString&		Key() const { return fKey; }
			const BString&		Title() const { return fTitle; }

	virtual	void				DrawItem(BView* owner, BRect frame,
									bool complete = false);
	virtual	void				Update(BView* owner, const BFont* font);

private:
			BString				fKey;
			BString				fTitle;
			BString				fSubtitle;
			glyph_kind			fGlyph;
			rgb_color			fColor;
};


/*!	The list of pages: headings that cannot be selected, and pages with a
	badge, a name and a line saying what is chosen on them.
*/
class Sidebar : public BListView {
public:
								Sidebar();

			bool				SelectKey(const char* key);
			BString				SelectedKey() const;

	virtual	void				AttachedToWindow();
	virtual	void				SelectionChanged();
	virtual	void				MessageReceived(BMessage* message);

private:
			void				_ScrollBy(float delta);

			int32				fLastSelection;
			bool				fAdjusting;
};


rgb_color sidebar_color();


#endif	// SIDEBAR_H
