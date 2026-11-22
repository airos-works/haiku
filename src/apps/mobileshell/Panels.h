/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef PANELS_H
#define PANELS_H

/*	Panels: the status bar along the top and the navigation panel along the
	bottom. Both float above every application window on every workspace and
	never take the keyboard focus.
*/

#include <View.h>
#include <Window.h>

#include "SystemStatus.h"


class PanelWindow : public BWindow {
public:
								PanelWindow(BRect frame, const char* name);
};


class StatusBarView : public BView {
public:
								StatusBarView(BRect frame);

	virtual	void				AttachedToWindow();
	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				MouseUp(BPoint where);
	virtual	void				MessageReceived(BMessage* message);

private:
			SystemStatus		fStatus;
			bool				fTracking;
			BPoint				fDownPoint;
			int32				fRefreshCountdown;
};


class NavBarView : public BView {
public:
								NavBarView(BRect frame);

	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				MouseUp(BPoint where);

private:
			enum Button { kNone = -1, kTasks, kHome, kClose, kKeyboard,
				kButtonCount };

			BRect				_ButtonFrame(int32 button) const;
			int32				_ButtonAt(BPoint where) const;

			int32				fPressed;
			BPoint				fDownPoint;
};


#endif	// PANELS_H
