/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef EMOJI_PANEL_H
#define EMOJI_PANEL_H

/*	EmojiPanel: the keyboard's emoji. A grid in place of the letters, filled
	column by column and scrolled sideways with a finger: recently used emoji
	first, then each category. The keyboard puts a bar under it with a key
	per category, which jumps there.

	Recent emoji are kept in "MobileShell/recent emoji" in the user's
	settings.
*/

#include <Rect.h>
#include <String.h>

#include <vector>


class BView;


class EmojiPanel {
public:
								EmojiPanel();

			void				SetFrame(BRect frame);
			BRect				Frame() const { return fFrame; }
			void				Draw(BView* view, const char* pressed);

			// Sections: kRecent, then the categories of EmojiData.h.
	static	const int			kRecent = -1;
			void				JumpTo(int section);
			int					Section() const;

			void				SetScroll(float scroll);
			float				Scroll() const { return fScroll; }
			// To the nearest column, after a drag.
			void				Snap();

			// NULL when there's none under it.
			const char*			EmojiAt(BPoint where) const;
			void				AddRecent(const char* emoji);

private:
			int					_Count(int section) const;
			const char*			_Emoji(int section, int index) const;
			int					_Columns(int section) const;
			int					_FirstColumn(int section) const;
			float				_MaxScroll() const;
			void				_Load();
			void				_Save() const;

			BRect				fFrame;
			int					fRows;
			int					fColumns;
			float				fColumnWidth;
			float				fScroll;
			std::vector<BString> fRecent;
};


#endif	// EMOJI_PANEL_H
