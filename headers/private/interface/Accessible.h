/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _ACCESSIBLE_H
#define _ACCESSIBLE_H


/*!	The system accessibility interface (air/OS, haiku#54).

	This header is private on purpose: the roles, states and the BAccessible
	class may still change before the interface is frozen. See
	docs/airos/ACCESSIBILITY.md for the design and the wire protocol.

	Interface Kit views get a default accessible node derived from the view
	itself (labels, values, enabled and focus state); nothing has to be done
	for a window built from standard controls. Applications can
	 - give an unlabeled view a name or description (SetName(),
	   SetDescription()), for example an icon-only button;
	 - replace the node of a view by their own tree of BAccessible objects
	   (Attach()), for example a web view that publishes its document tree.

	All of it costs a single predictable branch while no assistive
	technology is running: the per-application bridge only exists while the
	accessibility_server has a client.
*/


#include <Rect.h>
#include <String.h>
#include <SupportDefs.h>


class BMessage;
class BView;


enum accessible_role {
	B_ACCESSIBLE_ROLE_UNKNOWN = 0,
	B_ACCESSIBLE_ROLE_APPLICATION,
	B_ACCESSIBLE_ROLE_WINDOW,
	B_ACCESSIBLE_ROLE_DIALOG,
	B_ACCESSIBLE_ROLE_ALERT,
	B_ACCESSIBLE_ROLE_GROUP,
	B_ACCESSIBLE_ROLE_SCROLL_PANE,
	B_ACCESSIBLE_ROLE_SPLIT_PANE,
	B_ACCESSIBLE_ROLE_LABEL,
	B_ACCESSIBLE_ROLE_BUTTON,
	B_ACCESSIBLE_ROLE_TOGGLE_BUTTON,
	B_ACCESSIBLE_ROLE_CHECK_BOX,
	B_ACCESSIBLE_ROLE_RADIO_BUTTON,
	B_ACCESSIBLE_ROLE_TEXT_FIELD,
	B_ACCESSIBLE_ROLE_TEXT_AREA,
	B_ACCESSIBLE_ROLE_SPIN_BUTTON,
	B_ACCESSIBLE_ROLE_SLIDER,
	B_ACCESSIBLE_ROLE_SCROLL_BAR,
	B_ACCESSIBLE_ROLE_PROGRESS_BAR,
	B_ACCESSIBLE_ROLE_POP_UP_BUTTON,
	B_ACCESSIBLE_ROLE_COLOR_PICKER,
	B_ACCESSIBLE_ROLE_LIST,
	B_ACCESSIBLE_ROLE_LIST_ITEM,
	B_ACCESSIBLE_ROLE_TREE,
	B_ACCESSIBLE_ROLE_TREE_ITEM,
	B_ACCESSIBLE_ROLE_TABLE,
	B_ACCESSIBLE_ROLE_ROW,
	B_ACCESSIBLE_ROLE_CELL,
	B_ACCESSIBLE_ROLE_COLUMN_HEADER,
	B_ACCESSIBLE_ROLE_MENU_BAR,
	B_ACCESSIBLE_ROLE_MENU,
	B_ACCESSIBLE_ROLE_MENU_ITEM,
	B_ACCESSIBLE_ROLE_SEPARATOR,
	B_ACCESSIBLE_ROLE_TAB_LIST,
	B_ACCESSIBLE_ROLE_TAB,
	B_ACCESSIBLE_ROLE_TAB_PANEL,
	B_ACCESSIBLE_ROLE_TOOL_BAR,
	B_ACCESSIBLE_ROLE_STATUS_BAR,
	B_ACCESSIBLE_ROLE_TOOL_TIP,
	B_ACCESSIBLE_ROLE_IMAGE,
	B_ACCESSIBLE_ROLE_CANVAS,
	B_ACCESSIBLE_ROLE_LINK,
	B_ACCESSIBLE_ROLE_HEADING,
	B_ACCESSIBLE_ROLE_PARAGRAPH,
	B_ACCESSIBLE_ROLE_DOCUMENT,
	B_ACCESSIBLE_ROLE_ARTICLE,
	B_ACCESSIBLE_ROLE_LANDMARK,
	B_ACCESSIBLE_ROLE_FORM,
	B_ACCESSIBLE_ROLE_COMBO_BOX,
	B_ACCESSIBLE_ROLE_MATH,

	B_ACCESSIBLE_ROLE_COUNT
		// new roles are only ever appended
};


// state bits (uint64)
enum {
	B_ACCESSIBLE_STATE_FOCUSABLE		= 1ULL << 0,
	B_ACCESSIBLE_STATE_FOCUSED			= 1ULL << 1,
	B_ACCESSIBLE_STATE_DISABLED			= 1ULL << 2,
	B_ACCESSIBLE_STATE_CHECKABLE		= 1ULL << 3,
	B_ACCESSIBLE_STATE_CHECKED			= 1ULL << 4,
	B_ACCESSIBLE_STATE_MIXED			= 1ULL << 5,
	B_ACCESSIBLE_STATE_PRESSED			= 1ULL << 6,
	B_ACCESSIBLE_STATE_SELECTABLE		= 1ULL << 7,
	B_ACCESSIBLE_STATE_SELECTED			= 1ULL << 8,
	B_ACCESSIBLE_STATE_MULTI_SELECTABLE	= 1ULL << 9,
	B_ACCESSIBLE_STATE_EXPANDABLE		= 1ULL << 10,
	B_ACCESSIBLE_STATE_EXPANDED			= 1ULL << 11,
	B_ACCESSIBLE_STATE_EDITABLE			= 1ULL << 12,
	B_ACCESSIBLE_STATE_READ_ONLY		= 1ULL << 13,
	B_ACCESSIBLE_STATE_MULTI_LINE		= 1ULL << 14,
	B_ACCESSIBLE_STATE_PROTECTED		= 1ULL << 15,
		// password field: never expose or speak its text
	B_ACCESSIBLE_STATE_INVISIBLE		= 1ULL << 16,
	B_ACCESSIBLE_STATE_OFFSCREEN		= 1ULL << 17,
	B_ACCESSIBLE_STATE_ACTIVE			= 1ULL << 18,
		// active window
	B_ACCESSIBLE_STATE_MODAL			= 1ULL << 19,
	B_ACCESSIBLE_STATE_DEFAULT			= 1ULL << 20,
		// default button
	B_ACCESSIBLE_STATE_REQUIRED			= 1ULL << 21,
	B_ACCESSIBLE_STATE_INVALID			= 1ULL << 22,
	B_ACCESSIBLE_STATE_BUSY				= 1ULL << 23,
	B_ACCESSIBLE_STATE_HAS_POPUP		= 1ULL << 24,
	B_ACCESSIBLE_STATE_VISITED			= 1ULL << 25,
	B_ACCESSIBLE_STATE_HORIZONTAL		= 1ULL << 26,
	B_ACCESSIBLE_STATE_VERTICAL			= 1ULL << 27,
};


// action bits (uint32): what PerformAction() accepts
enum accessible_action {
	B_ACCESSIBLE_ACTION_PRESS			= 1 << 0,
		// click a button, toggle a check box, invoke a menu or list item
	B_ACCESSIBLE_ACTION_FOCUS			= 1 << 1,
	B_ACCESSIBLE_ACTION_INCREMENT		= 1 << 2,
	B_ACCESSIBLE_ACTION_DECREMENT		= 1 << 3,
	B_ACCESSIBLE_ACTION_SELECT			= 1 << 4,
	B_ACCESSIBLE_ACTION_EXPAND			= 1 << 5,
	B_ACCESSIBLE_ACTION_COLLAPSE		= 1 << 6,
	B_ACCESSIBLE_ACTION_SHOW_MENU		= 1 << 7,
	B_ACCESSIBLE_ACTION_SCROLL_TO		= 1 << 8,
};


enum accessible_event {
	B_ACCESSIBLE_EVENT_FOCUS_CHANGED = 1,
		// "node" got the keyboard focus (or a menu/list item got highlighted)
	B_ACCESSIBLE_EVENT_WINDOW_ACTIVATED,
	B_ACCESSIBLE_EVENT_WINDOW_DEACTIVATED,
	B_ACCESSIBLE_EVENT_VALUE_CHANGED,
	B_ACCESSIBLE_EVENT_NAME_CHANGED,
	B_ACCESSIBLE_EVENT_STATE_CHANGED,
		// "changed" (int64): the state bits that changed; without it,
		// only B_ACCESSIBLE_STATE_DISABLED is assumed
	B_ACCESSIBLE_EVENT_SELECTION_CHANGED,
		// list/table/tab selection; "node" is the selected item
	B_ACCESSIBLE_EVENT_TEXT_INSERTED,
	B_ACCESSIBLE_EVENT_TEXT_DELETED,
	B_ACCESSIBLE_EVENT_CARET_MOVED,
		// caret or text selection moved
	B_ACCESSIBLE_EVENT_CHILDREN_CHANGED,
	B_ACCESSIBLE_EVENT_ANNOUNCEMENT,
		// the application asks the assistive technology to say "text"
		// (live regions, progress, completed operations)
	B_ACCESSIBLE_EVENT_DOCUMENT_LOADED,
	B_ACCESSIBLE_EVENT_APP_REGISTERED,
		// sent by the accessibility_server, not by applications
	B_ACCESSIBLE_EVENT_APP_QUIT,
		// sent by the accessibility_server, not by applications
};


/*!	A node of an accessibility tree that an application provides itself.

	Every method is called with the looper of the host view locked (the
	window the tree is attached to), from the window thread for events and
	from the application's accessibility bridge thread for queries. Objects
	may be created lazily; the ID of an object stays the same while it
	exists. Delete a node only with the host's looper locked; the destructor
	retires its ID.
*/
class BAccessible {
public:
								BAccessible();
	virtual						~BAccessible();

	virtual	accessible_role		Role() const = 0;
	virtual	BString				Name() const;
	virtual	BString				Description() const;
	virtual	BString				Value() const;
	virtual	bool				GetRange(double& value, double& minimum,
									double& maximum) const;
	virtual	uint64				States() const;
	virtual	BRect				Frame() const;
									// in screen coordinates

	virtual	BAccessible*		Parent() const;
									// NULL for the root of an attached tree
	virtual	int32				CountChildren() const;
	virtual	BAccessible*		ChildAt(int32 index) const;

	virtual	uint32				Actions() const;
	virtual	status_t			PerformAction(accessible_action action);

	virtual	bool				GetTextSelection(int32& start,
									int32& end) const;
	virtual	BString				Text(int32 start, int32 end) const;

	// extra properties ("level", "index", "count", "url", "shortcut"...)
	virtual	void				AddProperties(BMessage& node) const;

			int64				ID();
									// 0 while no assistive technology
									// is running
			BView*				Host() const;

			void				NotifyEvent(accessible_event event,
									const BMessage* details = NULL);
									// does nothing while no assistive
									// technology is running

	// attaching custom trees and names to Interface Kit views
	static	status_t			Attach(BView* view, BAccessible* root);
									// the caller keeps ownership of root
	static	void				Detach(BView* view);
	static	status_t			SetName(BView* view, const char* name);
	static	status_t			SetDescription(BView* view,
									const char* description);

	static	bool				IsEnabled();
									// an assistive technology listens
	static	void				Announce(const char* text,
									bool interrupt = false);

	static	const char*			RoleName(accessible_role role);

private:
	friend class BAccessiblePrivate;

			BView*				fHost;
			int32				fRegistryID;
			uint32				_reserved[4];
};


#endif	// _ACCESSIBLE_H
