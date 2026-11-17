/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "PaneView.h"

#include <math.h>

#include <algorithm>

#include <ControlLook.h>
#include <Cursor.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <Window.h>


static const float kMinimumPaneSize = 24;


struct PaneView::Node {
	Node*		parent;
	Node*		children[2];
	BView*		pane;			// a leaf's
	bool		sideBySide;		// a split's children: left and right, or
								// top and bottom
	float		ratio;			// of the first child
	BRect		frame;
	BRect		divider;

	Node(BView* pane = NULL)
		:
		parent(NULL),
		pane(pane),
		sideBySide(true),
		ratio(0.5f)
	{
		children[0] = children[1] = NULL;
	}

	bool IsLeaf() const	{ return pane != NULL; }
};


PaneView::Listener::~Listener()
{
}


void
PaneView::Listener::PanesResized(PaneView* view)
{
}


PaneView::PaneView(BView* firstPane)
	:
	BView(BRect(0, 0, 100, 100), "panes", B_FOLLOW_ALL,
		B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
	fListener(NULL),
	fRoot(new Node(firstPane)),
	fMaximized(NULL),
	fCornerInset(0),
	fDragNode(NULL),
	fDragOffset(0),
	fCursorOrientation(-1)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
	// the layout places the panes
	firstPane->SetResizingMode(B_FOLLOW_LEFT_TOP);
	firstPane->MoveTo(0, 0);
	ResizeTo(firstPane->Bounds().Width(), firstPane->Bounds().Height());
	AddChild(firstPane);
}


PaneView::~PaneView()
{
	_DeleteTree(fRoot);
}


void
PaneView::_DeleteTree(Node* node)
{
	if (node == NULL)
		return;
	_DeleteTree(node->children[0]);
	_DeleteTree(node->children[1]);
	delete node;
}


float
PaneView::_DividerSize() const
{
	return std::max(4.0f, floorf(be_plain_font->Size() / 12.0f * 5.0f));
}


// #pragma mark - the tree


PaneView::Node*
PaneView::_FindLeaf(Node* node, BView* pane) const
{
	if (node == NULL)
		return NULL;
	if (node->IsLeaf())
		return node->pane == pane ? node : NULL;
	Node* found = _FindLeaf(node->children[0], pane);
	return found != NULL ? found : _FindLeaf(node->children[1], pane);
}


void
PaneView::_CollectLeaves(Node* node, std::vector<Node*>& leaves) const
{
	if (node == NULL)
		return;
	if (node->IsLeaf()) {
		leaves.push_back(node);
		return;
	}
	_CollectLeaves(node->children[0], leaves);
	_CollectLeaves(node->children[1], leaves);
}


int32
PaneView::CountPanes() const
{
	std::vector<Node*> leaves;
	_CollectLeaves(fRoot, leaves);
	return leaves.size();
}


BView*
PaneView::PaneAt(int32 index) const
{
	std::vector<Node*> leaves;
	_CollectLeaves(fRoot, leaves);
	if (index < 0 || index >= (int32)leaves.size())
		return NULL;
	return leaves[index]->pane;
}


int32
PaneView::IndexOf(BView* pane) const
{
	std::vector<Node*> leaves;
	_CollectLeaves(fRoot, leaves);
	for (size_t i = 0; i < leaves.size(); i++) {
		if (leaves[i]->pane == pane)
			return i;
	}
	return -1;
}


bool
PaneView::CanSplit(BView* pane, bool sideBySide, float minimumWidth,
	float minimumHeight) const
{
	Node* leaf = _FindLeaf(fRoot, pane);
	if (leaf == NULL)
		return false;
	BRect frame = fMaximized != NULL ? Bounds() : leaf->frame;
	if (!frame.IsValid())
		frame = Bounds();
	float divider = _DividerSize();
	if (sideBySide) {
		return (frame.Width() + 1 - divider) / 2 >= minimumWidth
			&& frame.Height() + 1 >= minimumHeight;
	}
	return (frame.Height() + 1 - divider) / 2 >= minimumHeight
		&& frame.Width() + 1 >= minimumWidth;
}


bool
PaneView::Split(BView* pane, BView* newPane, bool sideBySide)
{
	Node* leaf = _FindLeaf(fRoot, pane);
	if (leaf == NULL || newPane == NULL)
		return false;

	fMaximized = NULL;

	// the leaf becomes a split of the old pane and the new one
	Node* first = new Node(pane);
	Node* second = new Node(newPane);
	first->parent = second->parent = leaf;
	first->frame = leaf->frame;
	leaf->pane = NULL;
	leaf->children[0] = first;
	leaf->children[1] = second;
	leaf->sideBySide = sideBySide;
	leaf->ratio = 0.5f;

	// Sized once attached: a detached view does not resize its children,
	// and the terminal inside would keep its old size.
	newPane->SetResizingMode(B_FOLLOW_LEFT_TOP);
	AddChild(newPane);
	_Layout();
	Invalidate();
	return true;
}


void
PaneView::RemovePane(BView* pane)
{
	Node* leaf = _FindLeaf(fRoot, pane);
	if (leaf == NULL)
		return;

	if (fMaximized == pane)
		fMaximized = NULL;
	fDragNode = NULL;

	// give back the room left for the resize knob
	std::map<BView*, float>::iterator applied = fAppliedInsets.find(pane);
	if (applied != fAppliedInsets.end()) {
		BScrollView* scrollView = dynamic_cast<BScrollView*>(pane);
		BScrollBar* bar = scrollView != NULL
			? scrollView->ScrollBar(B_VERTICAL) : NULL;
		if (bar != NULL)
			bar->ResizeBy(0, applied->second);
		fAppliedInsets.erase(applied);
	}

	RemoveChild(pane);

	Node* parent = leaf->parent;
	if (parent == NULL) {
		delete leaf;
		fRoot = NULL;
		return;
	}

	// the sibling takes the parent's place
	Node* sibling = parent->children[0] == leaf
		? parent->children[1] : parent->children[0];
	Node* grandParent = parent->parent;
	sibling->parent = grandParent;
	if (grandParent == NULL)
		fRoot = sibling;
	else if (grandParent->children[0] == parent)
		grandParent->children[0] = sibling;
	else
		grandParent->children[1] = sibling;

	delete leaf;
	delete parent;

	_Layout();
	Invalidate();
}


BView*
PaneView::Neighbor(BView* pane, int32 direction) const
{
	Node* leaf = _FindLeaf(fRoot, pane);
	if (leaf == NULL || fMaximized != NULL)
		return NULL;

	BRect frame = leaf->frame;
	BPoint center((frame.left + frame.right) / 2,
		(frame.top + frame.bottom) / 2);
	std::vector<Node*> leaves;
	_CollectLeaves(fRoot, leaves);

	Node* best = NULL;
	float bestDistance = 0;
	float bestOffset = 0;
	for (size_t i = 0; i < leaves.size(); i++) {
		Node* other = leaves[i];
		if (other == leaf)
			continue;
		BRect otherFrame = other->frame;
		float distance;
		bool overlaps;
		float offset;
		switch (direction) {
			case LEFT:
				distance = frame.left - otherFrame.right;
				overlaps = otherFrame.top <= frame.bottom
					&& otherFrame.bottom >= frame.top;
				offset = fabsf((otherFrame.top + otherFrame.bottom) / 2
					- center.y);
				break;
			case RIGHT:
				distance = otherFrame.left - frame.right;
				overlaps = otherFrame.top <= frame.bottom
					&& otherFrame.bottom >= frame.top;
				offset = fabsf((otherFrame.top + otherFrame.bottom) / 2
					- center.y);
				break;
			case UP:
				distance = frame.top - otherFrame.bottom;
				overlaps = otherFrame.left <= frame.right
					&& otherFrame.right >= frame.left;
				offset = fabsf((otherFrame.left + otherFrame.right) / 2
					- center.x);
				break;
			default:
				distance = otherFrame.top - frame.bottom;
				overlaps = otherFrame.left <= frame.right
					&& otherFrame.right >= frame.left;
				offset = fabsf((otherFrame.left + otherFrame.right) / 2
					- center.x);
				break;
		}
		if (distance <= 0 || !overlaps)
			continue;
		// the closest; of those, the one most in line with this one
		if (best == NULL || distance < bestDistance - 0.5f
			|| (fabsf(distance - bestDistance) <= 0.5f
				&& offset < bestOffset)) {
			best = other;
			bestDistance = distance;
			bestOffset = offset;
		}
	}
	return best != NULL ? best->pane : NULL;
}


void
PaneView::SetMaximized(BView* pane)
{
	if (pane != NULL && _FindLeaf(fRoot, pane) == NULL)
		return;
	if (pane == fMaximized)
		return;
	fMaximized = pane;
	_Layout();
	Invalidate();
}


int32
PaneView::_Weight(Node* node, bool sideBySide) const
{
	if (node->IsLeaf())
		return 1;
	int32 first = _Weight(node->children[0], sideBySide);
	int32 second = _Weight(node->children[1], sideBySide);
	return node->sideBySide == sideBySide ? first + second
		: std::max(first, second);
}


void
PaneView::_Equalize(Node* node)
{
	if (node == NULL || node->IsLeaf())
		return;
	int32 first = _Weight(node->children[0], node->sideBySide);
	int32 second = _Weight(node->children[1], node->sideBySide);
	node->ratio = (float)first / (first + second);
	_Equalize(node->children[0]);
	_Equalize(node->children[1]);
}


void
PaneView::Equalize()
{
	_Equalize(fRoot);
	_Layout();
	Invalidate();
}


void
PaneView::SetCornerInset(float inset)
{
	fCornerInset = inset;
	_UpdateScrollBarInsets();
}


// #pragma mark - layout


void
PaneView::AttachedToWindow()
{
	BView::AttachedToWindow();
	_Layout();
}


void
PaneView::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	_Layout();
}


void
PaneView::_Layout()
{
	if (fRoot == NULL)
		return;

	std::vector<Node*> leaves;
	_CollectLeaves(fRoot, leaves);
	if (fMaximized != NULL) {
		for (size_t i = 0; i < leaves.size(); i++) {
			BView* pane = leaves[i]->pane;
			if (pane == fMaximized) {
				pane->MoveTo(0, 0);
				pane->ResizeTo(Bounds().Width(), Bounds().Height());
				if (pane->IsHidden(this))
					pane->Show();
			} else if (!pane->IsHidden(this))
				pane->Hide();
		}
	} else {
		for (size_t i = 0; i < leaves.size(); i++) {
			if (leaves[i]->pane->IsHidden(this))
				leaves[i]->pane->Show();
		}
		_Layout(fRoot, Bounds());
	}

	_UpdateScrollBarInsets();
	if (fListener != NULL)
		fListener->PanesResized(this);
}


void
PaneView::_Layout(Node* node, BRect frame)
{
	node->frame = frame;
	if (node->IsLeaf()) {
		node->divider = BRect();
		BView* pane = node->pane;
		if (pane->Frame() != frame) {
			pane->MoveTo(frame.LeftTop());
			pane->ResizeTo(frame.Width(), frame.Height());
		}
		return;
	}

	float divider = _DividerSize();
	float size = (node->sideBySide ? frame.Width() : frame.Height()) + 1;
	float available = size - divider;
	float first = floorf(available * node->ratio + 0.5f);
	float minimum = std::min(kMinimumPaneSize, floorf(available / 2));
	first = std::max(minimum, std::min(first, available - minimum));

	BRect firstFrame = frame;
	BRect secondFrame = frame;
	if (node->sideBySide) {
		firstFrame.right = frame.left + first - 1;
		node->divider = BRect(firstFrame.right + 1, frame.top,
			firstFrame.right + divider, frame.bottom);
		secondFrame.left = node->divider.right + 1;
	} else {
		firstFrame.bottom = frame.top + first - 1;
		node->divider = BRect(frame.left, firstFrame.bottom + 1,
			frame.right, firstFrame.bottom + divider);
		secondFrame.top = node->divider.bottom + 1;
	}
	_Layout(node->children[0], firstFrame);
	_Layout(node->children[1], secondFrame);
}


/*!	The window's resize knob sits in its bottom right corner: the scroll
	bar of the pane there is shorter. Changes are applied as differences,
	so other adjustments of the scroll bars stay.
*/
void
PaneView::_UpdateScrollBarInsets()
{
	std::vector<Node*> leaves;
	_CollectLeaves(fRoot, leaves);
	BRect bounds = Bounds();
	for (size_t i = 0; i < leaves.size(); i++) {
		BView* pane = leaves[i]->pane;
		BScrollView* scrollView = dynamic_cast<BScrollView*>(pane);
		BScrollBar* bar = scrollView != NULL
			? scrollView->ScrollBar(B_VERTICAL) : NULL;
		if (bar == NULL)
			continue;
		BRect frame = fMaximized != NULL ? bounds : leaves[i]->frame;
		bool corner = (fMaximized == NULL || fMaximized == pane)
			&& frame.right >= bounds.right - 0.5f
			&& frame.bottom >= bounds.bottom - 0.5f;
		float wanted = corner ? fCornerInset : 0;
		float applied = 0;
		std::map<BView*, float>::iterator found = fAppliedInsets.find(pane);
		if (found != fAppliedInsets.end())
			applied = found->second;
		if (wanted != applied) {
			bar->ResizeBy(0, applied - wanted);
			fAppliedInsets[pane] = wanted;
		}
	}
}


// #pragma mark - drawing and dragging


void
PaneView::_DrawDividers(Node* node, BRect updateRect)
{
	if (node == NULL || node->IsLeaf())
		return;
	BRect divider = node->divider;
	if (divider.IsValid() && divider.Intersects(updateRect)) {
		rgb_color base = ui_color(B_PANEL_BACKGROUND_COLOR);
		be_control_look->DrawSplitter(this, divider, updateRect, base,
			node->sideBySide ? B_HORIZONTAL : B_VERTICAL,
			node == fDragNode ? BControlLook::B_ACTIVATED : 0);
	}
	_DrawDividers(node->children[0], updateRect);
	_DrawDividers(node->children[1], updateRect);
}


void
PaneView::Draw(BRect updateRect)
{
	if (fMaximized == NULL)
		_DrawDividers(fRoot, updateRect);
}


PaneView::Node*
PaneView::_DividerAt(Node* node, BPoint where) const
{
	if (node == NULL || node->IsLeaf())
		return NULL;
	BRect divider = node->divider;
	// a little wider than drawn, for thin dividers
	if (node->sideBySide)
		divider.InsetBy(-2, 0);
	else
		divider.InsetBy(0, -2);
	if (divider.Contains(where))
		return node;
	Node* found = _DividerAt(node->children[0], where);
	return found != NULL ? found : _DividerAt(node->children[1], where);
}


void
PaneView::MouseDown(BPoint where)
{
	if (fMaximized != NULL)
		return;
	Node* node = _DividerAt(fRoot, where);
	if (node == NULL)
		return;

	int32 clicks = 1;
	if (Window()->CurrentMessage() != NULL)
		Window()->CurrentMessage()->FindInt32("clicks", &clicks);
	if (clicks == 2) {
		// a double click shares the room out evenly again
		_Equalize(node);
		_Layout();
		Invalidate();
		return;
	}

	fDragNode = node;
	fDragOffset = node->sideBySide ? where.x - node->divider.left
		: where.y - node->divider.top;
	SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
	Invalidate(node->divider);
}


void
PaneView::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage)
{
	if (fDragNode != NULL) {
		Node* node = fDragNode;
		BRect frame = node->frame;
		float divider = _DividerSize();
		float available = (node->sideBySide ? frame.Width() : frame.Height())
			+ 1 - divider;
		if (available <= 0)
			return;
		float position = node->sideBySide ? where.x - frame.left
			: where.y - frame.top;
		float ratio = (position - fDragOffset) / available;
		ratio = std::max(0.0f, std::min(1.0f, ratio));
		if (ratio == node->ratio)
			return;
		node->ratio = ratio;
		_Layout(node, frame);
		_UpdateScrollBarInsets();
		Invalidate(frame);
		if (fListener != NULL)
			fListener->PanesResized(this);
		return;
	}

	// the resize pointer over dividers
	int32 orientation = -1;
	if (transit != B_EXITED_VIEW && fMaximized == NULL) {
		Node* node = _DividerAt(fRoot, where);
		if (node != NULL)
			orientation = node->sideBySide ? B_HORIZONTAL : B_VERTICAL;
	}
	if (orientation != fCursorOrientation) {
		fCursorOrientation = orientation;
		if (orientation < 0)
			SetViewCursor(B_CURSOR_SYSTEM_DEFAULT);
		else {
			BCursor cursor(orientation == B_HORIZONTAL
				? B_CURSOR_ID_RESIZE_EAST_WEST
				: B_CURSOR_ID_RESIZE_NORTH_SOUTH);
			SetViewCursor(&cursor);
		}
	}
}


void
PaneView::MouseUp(BPoint where)
{
	if (fDragNode == NULL)
		return;
	BRect divider = fDragNode->divider;
	fDragNode = NULL;
	Invalidate(divider);
}
