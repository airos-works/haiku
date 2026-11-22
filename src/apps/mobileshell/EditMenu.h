/*
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */
#ifndef EDIT_MENU_H
#define EDIT_MENU_H

/*	The edit menu: the row of Cut, Copy, Paste and Select all that comes up
	over an application's window when a long press has selected something
	there (the touch filter says so), as on a phone. Its buttons type the
	shortcuts through the on-screen keyboard's add-on, so they work in any
	application, and the window never takes the focus from it. The last
	button, "...", clicks the secondary button where the finger was held:
	the application's own context menu.

	It goes away when a button is used (but Select all), and when a finger
	comes down anywhere else.
*/

#include <View.h>
#include <Window.h>

#include <vector>


class EditMenuView : public BView {
public:
								EditMenuView(BRect frame);

	virtual	void				AttachedToWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				MouseUp(BPoint where);

			// the size it wants
			BRect				PreferredFrame() const;
			void				SetHeld(BPoint held) { fHeld = held; }

private:
			int32				_ItemAt(BPoint where) const;
			BRect				_ItemFrame(int32 index) const;
			void				_Invoke(int32 index);

			std::vector<float>	fWidths;
			int32				fPressed;
			BPoint				fHeld;
};


class EditMenuWindow : public BWindow {
public:
								EditMenuWindow();

			// Shows the menu by the selection the finger made, from \a held
			// to \a lifted, inside \a area (screen coordinates).
			void				ShowAt(BPoint held, BPoint lifted, BRect area);
			void				Dismiss();

private:
			EditMenuView*		fView;
};


#endif	// EDIT_MENU_H
