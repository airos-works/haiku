/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef PANE_VIEW_H
#define PANE_VIEW_H


#include <map>
#include <vector>

#include <View.h>


/*!	The contents of a tab: one pane, or panes split side by side and on
	top of each other (iTerm2's split panes), any number of times. The
	panes are views (the terminals' scroll views); between them are
	dividers that can be dragged. One pane can take up the whole tab for a
	while (maximized).
*/
class PaneView : public BView {
public:
			class Listener;

	enum {
		LEFT = 0,
		RIGHT,
		UP,
		DOWN
	};

public:
								PaneView(BView* firstPane);
	virtual						~PaneView();

			void				SetListener(Listener* listener)
									{ fListener = listener; }

			int32				CountPanes() const;
			//! The panes from left to right and top to bottom.
			BView*				PaneAt(int32 index) const;
			int32				IndexOf(BView* pane) const;

			//! Whether \a pane is large enough to be split in two, each
			//! half at least \a minimumWidth by \a minimumHeight.
			bool				CanSplit(BView* pane, bool sideBySide,
									float minimumWidth,
									float minimumHeight) const;
			//! Splits \a pane, putting \a newPane right of it or below.
			bool				Split(BView* pane, BView* newPane,
									bool sideBySide);
			//! Takes \a pane out (it is not deleted); the pane next to it
			//! takes its place.
			void				RemovePane(BView* pane);

			//! The pane next to \a pane in a direction, or NULL.
			BView*				Neighbor(BView* pane, int32 direction) const;

			void				SetMaximized(BView* pane);
			BView*				Maximized() const	{ return fMaximized; }

			//! Gives the panes the same size, side by side and on top of
			//! each other.
			void				Equalize();

			//! Room left at the bottom of the scroll bar of the pane in
			//! the bottom right corner, for the window's resize knob.
			void				SetCornerInset(float inset);

	virtual	void				AttachedToWindow();
	virtual	void				FrameResized(float width, float height);
	virtual	void				Draw(BRect updateRect);
	virtual	void				MouseDown(BPoint where);
	virtual	void				MouseMoved(BPoint where, uint32 transit,
									const BMessage* dragMessage);
	virtual	void				MouseUp(BPoint where);

private:
			struct Node;

			Node*				_FindLeaf(Node* node, BView* pane) const;
			void				_CollectLeaves(Node* node,
									std::vector<Node*>& leaves) const;
			void				_Layout();
			void				_Layout(Node* node, BRect frame);
			void				_UpdateScrollBarInsets();
			Node*				_DividerAt(Node* node, BPoint where) const;
			void				_DrawDividers(Node* node, BRect updateRect);
			int32				_Weight(Node* node, bool sideBySide) const;
			void				_Equalize(Node* node);
			void				_DeleteTree(Node* node);
			float				_DividerSize() const;

private:
			Listener*			fListener;
			Node*				fRoot;
			BView*				fMaximized;
			float				fCornerInset;
			std::map<BView*, float> fAppliedInsets;

			Node*				fDragNode;
			float				fDragOffset;
			int32				fCursorOrientation;
};


class PaneView::Listener {
public:
	virtual						~Listener();

	//! The panes moved or changed their size.
	virtual	void				PanesResized(PaneView* view);
};


#endif	// PANE_VIEW_H
