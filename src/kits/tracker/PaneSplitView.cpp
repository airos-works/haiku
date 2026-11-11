/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "PaneSplitView.h"

#include <algorithm>

#include <Layout.h>
#include <LayoutItem.h>


namespace BPrivate {


PaneSplitView::PaneSplitView()
	:
	BSplitView(B_HORIZONTAL, 0),
	fPaneCount(0)
{
}


void
PaneSplitView::SetPaneWidth(BView* pane, float width)
{
	int32 index = _IndexOf(pane);
	if (index < 0) {
		if (width <= 0 || fPaneCount == kMaxPanes)
			return;

		index = fPaneCount++;
		fPanes[index].view = pane;
		fPanes[index].laidOutWidth = -1;
	} else if (width <= 0) {
		fPanes[index] = fPanes[--fPaneCount];
		InvalidateLayout();
		return;
	}

	fPanes[index].width = width;
	InvalidateLayout();
}


float
PaneSplitView::PaneWidth(BView* pane) const
{
	int32 index = _IndexOf(pane);
	return index >= 0 ? fPanes[index].width : -1;
}


void
PaneSplitView::MouseUp(BPoint where)
{
	// A pane whose width is not the one the last layout gave it was resized
	// by dragging a splitter: that is its width from now on. This has to be
	// noted before BSplitView lays the items out again at the end of the
	// drag.
	bool dragged[kMaxPanes] = {};
	for (int32 i = 0; i < fPaneCount; i++) {
		BView* view = fPanes[i].view;
		if (view->IsHidden(this) || fPanes[i].laidOutWidth < 0)
			continue;

		float width = view->Frame().Width();
		if (width != fPanes[i].laidOutWidth) {
			fPanes[i].width = width;
			dragged[i] = true;
		}
	}

	BSplitView::MouseUp(where);

	// The folder keeps a third of the window, so a pane dragged wider than
	// that was laid out narrower again: that is the width the user sees.
	for (int32 i = 0; i < fPaneCount; i++) {
		// (a pixel either way is the layouter rounding)
		if (dragged[i] && fPanes[i].laidOutWidth > 0
			&& fPanes[i].laidOutWidth < fPanes[i].width - 2) {
			fPanes[i].width = fPanes[i].laidOutWidth;
		}
	}
}


void
PaneSplitView::DoLayout()
{
	BLayout* layout = GetLayout();
	if (layout != NULL && fPaneCount > 0) {
		// The layouter shares out the whole width by weight, so the weights
		// are the widths: the panes' own ones, and the rest for the folder
		// (but never less than a third of the window). Sizes are in pixels,
		// one more than a frame's width; the splitters take their share
		// first (there is one between any two items, shown or not).
		int32 count = layout->CountItems();
		float total = std::max(Bounds().Width() + 1
			- (count - 1) * SplitterSize(), 1.0f);
		float panes = 0;
		int32 flexibleCount = 0;
		for (int32 i = 0; i < count; i++) {
			BLayoutItem* item = layout->ItemAt(i);
			if (item == NULL || !item->IsVisible())
				continue;

			int32 index = _IndexOf(item->View());
			if (index >= 0)
				panes += _PaneSize(item, index);
			else
				flexibleCount++;
		}

		float rest = std::max(total - panes, total / 3)
			/ std::max(flexibleCount, (int32)1);
		for (int32 i = 0; i < count; i++) {
			BLayoutItem* item = layout->ItemAt(i);
			int32 index = item != NULL ? _IndexOf(item->View()) : -1;
			float weight = index >= 0 ? _PaneSize(item, index) : rest;
			SetItemWeight(i, std::max(weight, 1.0f) / total, false);
		}
	}

	BSplitView::DoLayout();

	for (int32 i = 0; i < fPaneCount; i++)
		fPanes[i].laidOutWidth = fPanes[i].view->Frame().Width();
}


float
PaneSplitView::_PaneSize(BLayoutItem* item, int32 index) const
{
	// The layouter keeps an item within its limits and hands what it could
	// not give to the others by weight, so a weight beyond a pane's limits
	// would widen the other panes.
	float size = fPanes[index].width + 1;
	float max = item->MaxSize().width;
	if (max < B_SIZE_UNLIMITED)
		size = std::min(size, max + 1);
	return std::max(size, item->MinSize().width + 1);
}


int32
PaneSplitView::_IndexOf(BView* pane) const
{
	for (int32 i = 0; i < fPaneCount; i++) {
		if (fPanes[i].view == pane)
			return i;
	}

	return -1;
}


} // namespace BPrivate
