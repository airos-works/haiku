/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TRACKER_PANE_SPLIT_VIEW_H
#define _TRACKER_PANE_SPLIT_VIEW_H


#include <SplitView.h>


class BLayoutItem;


namespace BPrivate {


// The split view of a folder window: the folder and the panes beside it.
// A plain BSplitView shares out its width by weight, so a wider window
// makes every pane wider. Here a pane keeps the width it was given or the
// user dragged it to, and the folder takes whatever is left; only when
// the window gets too narrow for that are the panes squeezed, and they
// come back to their width when it grows again.
class PaneSplitView : public BSplitView {
public:
								PaneSplitView();

			// a width <= 0 makes the view share the rest with the folder
			void				SetPaneWidth(BView* pane, float width);
			float				PaneWidth(BView* pane) const;

	virtual	void				MouseUp(BPoint where);

protected:
	virtual	void				DoLayout();

private:
	static	const int32			kMaxPanes = 4;

			int32				_IndexOf(BView* pane) const;
			float				_PaneSize(BLayoutItem* item,
									int32 index) const;

			struct PaneInfo {
				BView*			view;
				float			width;
				float			laidOutWidth;
			};

			PaneInfo			fPanes[kMaxPanes];
			int32				fPaneCount;
};


} // namespace BPrivate

using namespace BPrivate;


#endif	// _TRACKER_PANE_SPLIT_VIEW_H
