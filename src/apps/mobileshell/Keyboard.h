/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef KEYBOARD_H
#define KEYBOARD_H

/*	Keyboard: the on-screen keyboard above the navigation panel. It never
	takes the focus, so what it types goes to the application that has it,
	through the mobile_keyboard input_server add-on.

	The globe key beside the space bar opens a menu of the layouts, with
	Emoji and Settings below them, and the space bar names the layout in
	use. The Terminal layout adds a row above the letters with the keys a
	terminal wants: Escape, Tab, Control, Alt (the Command key, as on a PC
	keyboard), Home, End and the arrows. Control and Alt hold for the next
	key, as Shift does. The window grows by the row, and the shell moves the
	applications up.

	The menu's Emoji puts emoji in place of the letters (see EmojiPanel),
	with a bar of categories under them, a key back to the letters and
	backspace. It stays until that key.

	Swiping across the letters types a word: a trail in the accent colour
	follows the finger, thinning and fading as it ages, and the strip on top
	shows the likeliest words as the finger moves, the best in the middle.
	On lift the best goes in, a space before it when it follows another
	word; tapping one of the others swaps it in, and backspace takes the
	whole word out.

	The Corners layout puts symbols in the corners of the letter keys, typed
	by flicking the key towards one, and has no symbols page; its space bar
	slides the cursor, and its arrow key is flicked in the arrow's
	direction. Swipe typing is off there.
*/

#include "EmojiPanel.h"
#include "SwipeDecoder.h"

#include <String.h>

#include <View.h>

#include <vector>


class BBitmap;
class BMessageRunner;


class KeyboardView : public BView {
public:
								KeyboardView(BRect frame);
	virtual						~KeyboardView();

	virtual	void				Draw(BRect updateRect);
	virtual	void				FrameResized(float width, float height);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				MouseUp(BPoint where);
	virtual	void				MessageReceived(BMessage* message);

			// When the keyboard goes, its menu goes with it...
			void				CloseMenu();
			// ...and what it knew of the word it typed last.
			void				Reset();

private:
	enum Special { kCharacter, kShift, kBackspace, kReturn, kSymbols,
		kSpace, kLayout, kControl, kAlt, kNamed, kLetters, kCategory,
		kArrows };

	struct Key {
		const char*	label;
		Special		special;
		float		width;		// in key widths
		BRect		frame;
		char		byte;		// kNamed: what it types
		int32		code;		// kNamed: its key code; kCategory: its section
		const char* const* corners;	// eight, nw to se, or NULL
	};

			void				_Layout();
			void				_SetLayout(int32 layout);
			BRect				_MenuFrame() const;
			BRect				_MenuRowFrame(int32 row) const;
			int32				_MenuRowAt(BPoint where) const;
			void				_Render(BView* view, BRect updateRect);
			void				_DrawMenu(BView* view);
			void				_Send(const char* bytes, int32 key, char raw);
			void				_SendText(const char* text);
			void				_Backspace(int32 count);
			bool				_CanSwipe(int32 key) const;
			void				_Decode();
			void				_CommitSwipe(const BString& word);
			void				_EndWord();
			BRect				_SuggestionFrame(int32 slot) const;
			int32				_SuggestionAt(BPoint where) const;
			void				_DrawSuggestions(BView* view);
			void				_DrawTrail(BView* view);
			BRect				_TrailBounds() const;
			int32				_KeyAt(BPoint where) const;
			bool				_IsOn(const Key& key) const;
			bool				_Corners() const;
			int32				_CornerAt(const Key& key, BPoint where) const;
			void				_SendSymbol(const char* symbol);

			std::vector<Key>	fKeys;
			bool				fShift;
			bool				fSymbols;
			int32				fLayout;
			bool				fControl;
			bool				fAlt;
			int32				fPressed;
			bool				fMenu;
			int32				fMenuPressed;	// a row, or -1 outside

			bool				fEmoji;
			EmojiPanel			fEmojiPanel;
			const char*			fPressedEmoji;
			BPoint				fDragStart;
			float				fScrollStart;
			bool				fPanelPressed;	// tracking a finger on the grid
			bool				fDragging;

			struct TrailPoint {
				BPoint		where;
				bigtime_t	when;
			};
			SwipeDecoder		fDecoder;
			BPoint				fDownPoint;
			bool				fSwiping;
			std::vector<TrailPoint> fTrail;		// what's still drawn
			std::vector<SwipePoint> fSwipePath;	// the whole swipe
			BMessageRunner*		fTrailRunner;
			bigtime_t			fLastDecode;
			std::vector<BString> fSuggestions;	// best first
			int32				fSuggestionPressed;	// a slot, or -1
			BString				fSwiped;	// the word just swiped, while it
											// can still be swapped
			bool				fSpaceBefore;	// before the next word

			int32				fCorner;	// flicked to on the pressed key
			float				fSlideX;	// where the space bar slide is
			bool				fSlid;

			BBitmap*			fBuffer;
			BView*				fBufferView;
};


#endif	// KEYBOARD_H
