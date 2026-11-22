/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef TASK_SWITCHER_H
#define TASK_SWITCHER_H

/*	TaskSwitcher: a card for every open window. Tap a card to switch to it,
	tap its close button to close it, tap anywhere else to go back.

	A card shows its window as it looked when the switcher opened: the one in
	front is cut from the picture of the screen, a covered one draws itself
	for the card (read_window_bitmap()). A minimized window, or one that did
	not draw in time, shows as it last did, or its application's icon.
*/

#include "Overlay.h"
#include "WindowTracker.h"

#include <map>
#include <vector>


class BBitmap;


class TaskSwitcherView : public OverlayView {
public:
								TaskSwitcherView(BRect frame);
	virtual						~TaskSwitcherView();

	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseUp(BPoint where);

			void				Open(const std::vector<TrackedWindow>& windows);
			void				Close();

protected:
	virtual	void				RenderContent(BView* view);

private:
	struct Card {
		int32		token;
		BString		title;
		bool		mobile;
		BBitmap*	icon;
		BRect		frame;
		BRect		previewFrame;
	};

			BRect				_CloseFrame(const Card& card) const;
			void				_Clear();
			void				_TakePreview(const TrackedWindow& window,
									const Card& card, bool front);

			std::vector<Card>	fCards;
			// kept between openings, by window token
			std::map<int32, BBitmap*> fPreviews;

			int32				fPage;
			int32				fPageSize;
			BPoint			fDown;
			int32				fPressed;
			bool				fPressedClose;
};


#endif	// TASK_SWITCHER_H
