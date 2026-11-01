/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef LOCATION_MENU_ITEM_H
#define LOCATION_MENU_ITEM_H


#include <MenuItem.h>

#include "Model.h"


class BMenu;


// One of the places the desktop shows (Home, the boot volume, Trash, the
// other mounted volumes), listed in Tracker's window menu; it opens a Tracker
// window for its folder on the main display. Drawn like the window items.
class TLocationMenuItem : public BMenuItem {
public:
								TLocationMenuItem(const BEntry& entry,
									const char* label = NULL);

			status_t			InitCheck() const;

	static	void				AddLocations(BMenu* menu);

protected:
	virtual	void				GetContentSize(float* width, float* height);
	virtual	void				DrawContent();
	virtual	status_t			Invoke(BMessage* message = NULL);

private:
			BPrivate::Model		fModel;
			float				fLabelWidth;
			float				fLabelAscent;
			float				fLabelDescent;
};


#endif	// LOCATION_MENU_ITEM_H
