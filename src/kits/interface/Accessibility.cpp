/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	The accessibility bridge of an application (haiku#54).

	Nothing in here runs while no assistive technology is active: the
	accessibility_server sends B_ACCESSIBILITY_ENABLE to every application
	when its first client (a screen reader) subscribes, and
	B_ACCESSIBILITY_DISABLE when the last one leaves. Only then does the
	application start a bridge looper that answers tree queries, and do the
	Interface Kit hooks (guarded by gAccessibilityEnabled) send events.

	Node IDs are 64 bit: the upper 32 bits identify a registered object (a
	window, a view or a BAccessible), the lower 32 bits a virtual item of it
	(0 = the object itself, n = item n - 1 of a list, menu, tab view or
	column list view).
*/


#include <Accessible.h>
#include <AccessibilityPrivate.h>
#include <AccessibilityProtocol.h>

#include <map>
#include <new>

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <Alert.h>
#include <Application.h>
#include <Autolock.h>
#include <Box.h>
#include <Button.h>
#include <ChannelSlider.h>
#include <CheckBox.h>
#include <ColorControl.h>
#include <Looper.h>
#include <ListView.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <Message.h>
#include <Messenger.h>
#include <OutlineListView.h>
#include <PictureButton.h>
#include <RadioButton.h>
#include <Roster.h>
#include <ScrollBar.h>
#include <ScrollView.h>
#include <SeparatorItem.h>
#include <SeparatorView.h>
#include <Slider.h>
#include <StatusBar.h>
#include <StringItem.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>
#include <TextView.h>
#include <View.h>
#include <Window.h>

#include <AbstractSpinner.h>
#include <DecimalSpinner.h>
#include <Spinner.h>
#include <ToolTip.h>


namespace BPrivate {

bool gAccessibilityEnabled = false;
bool gAccessibilityViewData = false;

}	// namespace BPrivate


using BPrivate::gAccessibilityEnabled;
using BPrivate::gAccessibilityViewData;


static const int32 kAppRootID = 1;
static const bigtime_t kLockTimeout = 250000;
static const int32 kMaxValueLength = 4096;
static const int32 kMaxEventTextLength = 1024;
static const int32 kMaxDepth = 64;
static const uint32 kMsgInitialState = 'acIS';


enum entry_type {
	ENTRY_WINDOW,
	ENTRY_VIEW,
	ENTRY_OBJECT
};

struct RegistryEntry {
	entry_type	type;
	void*		object;
	BMessenger	looper;
};

typedef std::map<int32, RegistryEntry> EntryMap;
typedef std::map<const void*, int32> IDMap;
typedef std::map<const BView*, BString> StringMap;
typedef std::map<const BView*, BAccessible*> AttachMap;


static BLocker sRegistryLock("accessibility registry");
static EntryMap sEntries;
static IDMap sIDs;
static StringMap sNames;
static StringMap sDescriptions;
static AttachMap sAttached;
static int32 sNextID = kAppRootID + 1;

static BMessenger sServer;
static BLooper* sBridge = NULL;


// Destroyed before the registry above: views deleted during static
// destruction must not touch it any more.
static struct RegistryGuard {
	~RegistryGuard()
	{
		gAccessibilityEnabled = false;
		gAccessibilityViewData = false;
	}
} sRegistryGuard;


class BAccessiblePrivate {
public:
	static BView*& Host(BAccessible* object) { return object->fHost; }
	static int32& RegistryID(BAccessible* object)
		{ return object->fRegistryID; }
};


// #pragma mark - registry


static int32
register_object(entry_type type, void* object, BLooper* looper)
{
	// sRegistryLock must be held
	if (looper == NULL)
		return 0;

	BMessenger messenger(looper);
	IDMap::iterator found = sIDs.find(object);
	if (found != sIDs.end()) {
		EntryMap::iterator entry = sEntries.find(found->second);
		if (entry != sEntries.end() && entry->second.type == type) {
			if (!(entry->second.looper == messenger))
				entry->second.looper = messenger;
			return found->second;
		}
		// a stale entry of a deleted window at the same address
		if (entry != sEntries.end())
			sEntries.erase(entry);
		sIDs.erase(found);
	}

	int32 id = sNextID++;
	RegistryEntry entry;
	entry.type = type;
	entry.object = object;
	entry.looper = messenger;
	sEntries[id] = entry;
	sIDs[object] = id;
	if (type == ENTRY_VIEW)
		gAccessibilityViewData = true;
	return id;
}


static void
unregister_object(const void* object)
{
	// sRegistryLock must be held
	IDMap::iterator found = sIDs.find(object);
	if (found == sIDs.end())
		return;
	sEntries.erase(found->second);
	sIDs.erase(found);
}


static inline int64
make_id(int32 registryID, int32 sub = 0)
{
	if (registryID == 0)
		return 0;
	return ((int64)registryID << 32) | (uint32)sub;
}


static BAccessible*
attached_root(const BView* view)
{
	if (sAttached.empty())
		return NULL;
	AttachMap::iterator found = sAttached.find(view);
	return found != sAttached.end() ? found->second : NULL;
}


static int32
object_registry_id(BAccessible* object)
{
	BView* host = BAccessiblePrivate::Host(object);
	if (host == NULL || host->Window() == NULL)
		return 0;
	int32& id = BAccessiblePrivate::RegistryID(object);
	if (id == 0)
		id = register_object(ENTRY_OBJECT, object, host->Window());
	return id;
}


static int64
window_id(BWindow* window)
{
	return make_id(register_object(ENTRY_WINDOW, window, window));
}


// #pragma mark - default nodes of Interface Kit views


/*!	Maps the parts of composite controls (the text view of a BTextControl,
	the outline view of a BColumnListView...) to the control. A view with
	an attached accessible tree covers all of its child views.
*/
static BView*
effective_view(BView* view)
{
	if (view == NULL)
		return NULL;

	if (!sAttached.empty()) {
		for (BView* parent = view->Parent(); parent != NULL;
				parent = parent->Parent()) {
			if (sAttached.find(parent) != sAttached.end())
				return parent;
		}
	}

	BView* parent = view->Parent();
	if (parent == NULL)
		return view;
	if (dynamic_cast<BTextControl*>(parent) != NULL
		|| dynamic_cast<BAbstractSpinner*>(parent) != NULL
		|| dynamic_cast<BMenuField*>(parent) != NULL) {
		return parent;
	}
	return view;
}


static bool
is_composite(BView* view)
{
	return dynamic_cast<BTextControl*>(view) != NULL
		|| dynamic_cast<BAbstractSpinner*>(view) != NULL
		|| dynamic_cast<BMenuField*>(view) != NULL
		|| dynamic_cast<BListView*>(view) != NULL
		|| dynamic_cast<BMenu*>(view) != NULL;
}


static int64
view_id(BView* view)
{
	view = effective_view(view);
	if (view == NULL || view->Window() == NULL)
		return 0;
	if (BAccessible* root = attached_root(view))
		return make_id(object_registry_id(root));
	return make_id(register_object(ENTRY_VIEW, view, view->Window()));
}


static BString
clean_label(const char* label)
{
	BString string(label);
	string.Trim();
	if (string.EndsWith(":"))
		string.Truncate(string.Length() - 1);
	return string;
}


static accessible_role
view_role(BView* view)
{
	if (dynamic_cast<BMenuBar*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_MENU_BAR;
	if (dynamic_cast<BMenu*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_MENU;
	if (dynamic_cast<BTextControl*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_TEXT_FIELD;
	if (dynamic_cast<BAbstractSpinner*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_SPIN_BUTTON;
	if (dynamic_cast<BCheckBox*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_CHECK_BOX;
	if (dynamic_cast<BRadioButton*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_RADIO_BUTTON;
	if (BButton* button = dynamic_cast<BButton*>(view)) {
		return button->Behavior() == BButton::B_TOGGLE_BEHAVIOR
			? B_ACCESSIBLE_ROLE_TOGGLE_BUTTON : B_ACCESSIBLE_ROLE_BUTTON;
	}
	if (dynamic_cast<BPictureButton*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_BUTTON;
	if (dynamic_cast<BChannelSlider*>(view) != NULL
		|| dynamic_cast<BSlider*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_SLIDER;
	if (dynamic_cast<BColorControl*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_COLOR_PICKER;
	if (dynamic_cast<BMenuField*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_POP_UP_BUTTON;
	if (dynamic_cast<BScrollBar*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_SCROLL_BAR;
	if (dynamic_cast<BStatusBar*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_PROGRESS_BAR;
	if (dynamic_cast<BOutlineListView*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_TREE;
	if (dynamic_cast<BListView*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_LIST;
	if (dynamic_cast<BTabView*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_TAB_LIST;
	if (BTextView* textView = dynamic_cast<BTextView*>(view)) {
		if (!textView->IsEditable() && !textView->IsSelectable())
			return B_ACCESSIBLE_ROLE_LABEL;
		return B_ACCESSIBLE_ROLE_TEXT_AREA;
	}
	if (dynamic_cast<BStringView*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_LABEL;
	if (dynamic_cast<BSeparatorView*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_SEPARATOR;
	if (dynamic_cast<BScrollView*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_SCROLL_PANE;
	if (dynamic_cast<BBox*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_GROUP;
	if (dynamic_cast<BControl*>(view) != NULL)
		return B_ACCESSIBLE_ROLE_UNKNOWN;
	if (view->CountChildren() > 0)
		return B_ACCESSIBLE_ROLE_GROUP;
	return B_ACCESSIBLE_ROLE_UNKNOWN;
}


static BString
tool_tip_text(BView* view)
{
	BTextToolTip* toolTip = dynamic_cast<BTextToolTip*>(view->ToolTip());
	if (toolTip == NULL || toolTip->Text() == NULL)
		return BString();
	return BString(toolTip->Text());
}


static BString
view_name(BView* view, accessible_role role)
{
	StringMap::iterator found = sNames.find(view);
	if (found != sNames.end())
		return found->second;

	BString name;
	if (BControl* control = dynamic_cast<BControl*>(view))
		name = clean_label(control->Label());
	else if (BMenuField* field = dynamic_cast<BMenuField*>(view))
		name = clean_label(field->Label());
	else if (BStringView* stringView = dynamic_cast<BStringView*>(view))
		name = stringView->Text();
	else if (BBox* box = dynamic_cast<BBox*>(view))
		name = clean_label(box->Label());
	else if (BStatusBar* statusBar = dynamic_cast<BStatusBar*>(view))
		name = clean_label(statusBar->Label());
	else if (BMenu* menu = dynamic_cast<BMenu*>(view)) {
		if (menu->Superitem() != NULL)
			name = menu->Superitem()->Label();
	}

	if (name.IsEmpty() && role != B_ACCESSIBLE_ROLE_GROUP
		&& role != B_ACCESSIBLE_ROLE_SCROLL_PANE
		&& role != B_ACCESSIBLE_ROLE_LABEL
		&& role != B_ACCESSIBLE_ROLE_UNKNOWN) {
		// an unlabeled control: use its tool tip, or the label in front of it
		name = tool_tip_text(view);
		if (name.IsEmpty() && view->Parent() != NULL) {
			BView* previous = view->PreviousSibling();
			if (BStringView* label = dynamic_cast<BStringView*>(previous))
				name = clean_label(label->Text());
		}
	}
	return name;
}


static void
add_text_value(BMessage& node, BTextView* textView)
{
	if (textView->IsTypingHidden()) {
		node.AddString(B_ACCESSIBLE_FIELD_VALUE, "");
		return;
	}
	int32 length = textView->TextLength();
	if (length <= kMaxValueLength)
		node.AddString(B_ACCESSIBLE_FIELD_VALUE, textView->Text());
	else {
		BString value(textView->Text(), kMaxValueLength);
		node.AddString(B_ACCESSIBLE_FIELD_VALUE, value);
	}
	node.AddInt32("length", length);

	int32 start, end;
	textView->GetSelection(&start, &end);
	node.AddInt32(B_ACCESSIBLE_FIELD_SEL_START, start);
	node.AddInt32(B_ACCESSIBLE_FIELD_SEL_END, end);
}


static BString
shortcut_text(BMenuItem* item)
{
	uint32 modifiers = 0;
	char shortcut = item->Shortcut(&modifiers);
	if (shortcut == 0)
		return BString();

	BString text;
	if ((modifiers & B_CONTROL_KEY) != 0)
		text << "Ctrl+";
	if ((modifiers & B_OPTION_KEY) != 0)
		text << "Option+";
	if ((modifiers & B_SHIFT_KEY) != 0)
		text << "Shift+";
	if ((modifiers & B_COMMAND_KEY) != 0)
		text << "Alt+";
	text << (char)toupper(shortcut);
	return text;
}


static void describe_view(BView* view, int32 sub, BMessage& node,
	int32 depth);
static void describe_object(BAccessible* object, BMessage& node,
	int32 depth);


static void
add_common(BMessage& node, int64 id, int64 parent, accessible_role role,
	const BString& name, uint64 states, BRect frame, uint32 actions)
{
	node.AddInt64(B_ACCESSIBLE_FIELD_ID, id);
	node.AddInt64(B_ACCESSIBLE_FIELD_PARENT, parent);
	node.AddInt32(B_ACCESSIBLE_FIELD_ROLE, role);
	node.AddString(B_ACCESSIBLE_FIELD_ROLE_NAME, BAccessible::RoleName(role));
	node.AddString(B_ACCESSIBLE_FIELD_NAME, name);
	node.AddInt64(B_ACCESSIBLE_FIELD_STATES, (int64)states);
	if (frame.IsValid())
		node.AddRect(B_ACCESSIBLE_FIELD_FRAME, frame);
	node.AddInt32(B_ACCESSIBLE_FIELD_ACTIONS, actions);
}


static int64
parent_id_of_view(BView* view)
{
	BView* parent = view->Parent();
	if (parent == NULL)
		return view->Window() != NULL ? window_id(view->Window()) : 0;
	return view_id(parent);
}


/*!	Lists the virtual items of a container view (list, menu, tabs,
	column list view). Returns -1 when the view has none.
*/
static int32
count_items(BView* view)
{
	if (BListView* list = dynamic_cast<BListView*>(view))
		return list->CountItems();
	if (BMenu* menu = dynamic_cast<BMenu*>(view))
		return menu->CountItems();
	if (BTabView* tabView = dynamic_cast<BTabView*>(view))
		return tabView->CountTabs();
	return -1;
}


static bool
describe_item(BView* view, int32 index, BMessage& node)
{
	int32 registryID = register_object(ENTRY_VIEW, view, view->Window());
	int64 id = make_id(registryID, index + 1);
	int64 parent = make_id(registryID);
	int32 count = count_items(view);
	if (index < 0 || index >= count)
		return false;

	accessible_role role = B_ACCESSIBLE_ROLE_LIST_ITEM;
	BString name;
	uint64 states = 0;
	uint32 actions = 0;
	BRect frame;
	frame.Set(0, 0, -1, -1);

	if (BListView* list = dynamic_cast<BListView*>(view)) {
		BListItem* item = list->ItemAt(index);
		BOutlineListView* outline = dynamic_cast<BOutlineListView*>(list);
		role = outline != NULL
			? B_ACCESSIBLE_ROLE_TREE_ITEM : B_ACCESSIBLE_ROLE_LIST_ITEM;
		if (BStringItem* stringItem = dynamic_cast<BStringItem*>(item))
			name = stringItem->Text();
		if (name.IsEmpty())
			name.SetToFormat("Item %" B_PRId32, index + 1);
		states = B_ACCESSIBLE_STATE_SELECTABLE;
		if (list->IsItemSelected(index))
			states |= B_ACCESSIBLE_STATE_SELECTED;
		if (item != NULL && !item->IsEnabled())
			states |= B_ACCESSIBLE_STATE_DISABLED;
		if (outline != NULL && item != NULL) {
			node.AddInt32(B_ACCESSIBLE_FIELD_LEVEL, item->OutlineLevel() + 1);
			if (outline->CountItemsUnder(item, true) > 0) {
				states |= B_ACCESSIBLE_STATE_EXPANDABLE;
				if (item->IsExpanded())
					states |= B_ACCESSIBLE_STATE_EXPANDED;
				actions |= B_ACCESSIBLE_ACTION_EXPAND
					| B_ACCESSIBLE_ACTION_COLLAPSE;
			}
		}
		frame = list->ConvertToScreen(list->ItemFrame(index));
		actions |= B_ACCESSIBLE_ACTION_SELECT | B_ACCESSIBLE_ACTION_PRESS;
	} else if (BMenu* menu = dynamic_cast<BMenu*>(view)) {
		BMenuItem* item = menu->ItemAt(index);
		if (dynamic_cast<BSeparatorItem*>(item) != NULL)
			role = B_ACCESSIBLE_ROLE_SEPARATOR;
		else {
			role = B_ACCESSIBLE_ROLE_MENU_ITEM;
			name = item->Label();
			if (!item->IsEnabled())
				states |= B_ACCESSIBLE_STATE_DISABLED;
			if (item->IsMarked())
				states |= B_ACCESSIBLE_STATE_CHECKED;
			if (item->Submenu() != NULL)
				states |= B_ACCESSIBLE_STATE_HAS_POPUP;
			BString shortcut = shortcut_text(item);
			if (!shortcut.IsEmpty())
				node.AddString(B_ACCESSIBLE_FIELD_SHORTCUT, shortcut);
			actions = B_ACCESSIBLE_ACTION_PRESS;
		}
		frame = menu->ConvertToScreen(item->Frame());
	} else if (BTabView* tabView = dynamic_cast<BTabView*>(view)) {
		role = B_ACCESSIBLE_ROLE_TAB;
		BTab* tab = tabView->TabAt(index);
		if (tab != NULL)
			name = tab->Label();
		states = B_ACCESSIBLE_STATE_SELECTABLE;
		if (tabView->Selection() == index)
			states |= B_ACCESSIBLE_STATE_SELECTED;
		if (tabView->IsFocus() && tabView->FocusTab() == index)
			states |= B_ACCESSIBLE_STATE_FOCUSED;
		if (tab != NULL && !tab->IsEnabled())
			states |= B_ACCESSIBLE_STATE_DISABLED;
		frame = tabView->ConvertToScreen(tabView->TabFrame(index));
		actions = B_ACCESSIBLE_ACTION_SELECT | B_ACCESSIBLE_ACTION_PRESS;
	} else
		return false;

	add_common(node, id, parent, role, name, states, frame, actions);
	node.AddInt32(B_ACCESSIBLE_FIELD_INDEX, index);
	node.AddInt32(B_ACCESSIBLE_FIELD_COUNT, count);
	node.AddInt32(B_ACCESSIBLE_FIELD_CHILD_COUNT, 0);
	return true;
}


static void
describe_window(BWindow* window, BMessage& node, int32 depth)
{
	accessible_role role = B_ACCESSIBLE_ROLE_WINDOW;
	uint64 states = 0;
	BAlert* alert = dynamic_cast<BAlert*>(window);
	if (alert != NULL)
		role = B_ACCESSIBLE_ROLE_ALERT;
	else if (window->IsModal())
		role = B_ACCESSIBLE_ROLE_DIALOG;
	if (window->IsModal())
		states |= B_ACCESSIBLE_STATE_MODAL;
	if (window->IsActive())
		states |= B_ACCESSIBLE_STATE_ACTIVE | B_ACCESSIBLE_STATE_FOCUSED;
	if (window->IsHidden())
		states |= B_ACCESSIBLE_STATE_INVISIBLE;
	if (window->IsMinimized())
		states |= B_ACCESSIBLE_STATE_OFFSCREEN;

	add_common(node, window_id(window), make_id(kAppRootID), role,
		window->Title(), states, window->Frame(), B_ACCESSIBLE_ACTION_FOCUS);
	if (alert != NULL && alert->TextView() != NULL)
		node.AddString(B_ACCESSIBLE_FIELD_DESCRIPTION, alert->TextView()->Text());

	int32 count = 0;
	for (int32 i = 0; i < window->CountChildren(); i++) {
		BView* child = window->ChildAt(i);
		if (child->IsHidden(child))
			continue;
		count++;
		node.AddInt64(B_ACCESSIBLE_FIELD_CHILD_IDS, view_id(child));
		if (depth != 0) {
			BMessage childNode;
			describe_view(child, 0, childNode, depth - 1);
			node.AddMessage(B_ACCESSIBLE_FIELD_CHILD, &childNode);
		}
	}
	node.AddInt32(B_ACCESSIBLE_FIELD_CHILD_COUNT, count);
}


static void
describe_view(BView* view, int32 sub, BMessage& node, int32 depth)
{
	if (depth < -kMaxDepth)
		return;

	if (sub > 0) {
		describe_item(view, sub - 1, node);
		return;
	}

	view = effective_view(view);
	if (BAccessible* root = attached_root(view)) {
		describe_object(root, node, depth);
		return;
	}

	BWindow* window = view->Window();
	accessible_role role = view_role(view);
	BString name = view_name(view, role);
	uint64 states = 0;
	uint32 actions = 0;

	BView* focus = window != NULL ? window->CurrentFocus() : NULL;
	if (focus != NULL && effective_view(focus) == view)
		states |= B_ACCESSIBLE_STATE_FOCUSED;
	if ((view->Flags() & B_NAVIGABLE) != 0
		|| dynamic_cast<BTextControl*>(view) != NULL
		|| dynamic_cast<BAbstractSpinner*>(view) != NULL) {
		states |= B_ACCESSIBLE_STATE_FOCUSABLE;
		actions |= B_ACCESSIBLE_ACTION_FOCUS;
	}
	if (view->IsHidden(view))
		states |= B_ACCESSIBLE_STATE_INVISIBLE;

	BControl* control = dynamic_cast<BControl*>(view);
	if (control != NULL) {
		if (!control->IsEnabled())
			states |= B_ACCESSIBLE_STATE_DISABLED;
		else {
			states |= B_ACCESSIBLE_STATE_FOCUSABLE;
			actions |= B_ACCESSIBLE_ACTION_FOCUS;
		}
	}

	switch (role) {
		case B_ACCESSIBLE_ROLE_CHECK_BOX:
		case B_ACCESSIBLE_ROLE_RADIO_BUTTON:
			states |= B_ACCESSIBLE_STATE_CHECKABLE;
			if (control->Value() == B_CONTROL_ON)
				states |= B_ACCESSIBLE_STATE_CHECKED;
			else if (control->Value() == B_CONTROL_PARTIALLY_ON)
				states |= B_ACCESSIBLE_STATE_MIXED;
			actions |= B_ACCESSIBLE_ACTION_PRESS;
			break;
		case B_ACCESSIBLE_ROLE_TOGGLE_BUTTON:
			if (control->Value() == B_CONTROL_ON)
				states |= B_ACCESSIBLE_STATE_PRESSED;
			// fall through
		case B_ACCESSIBLE_ROLE_BUTTON:
			if (BButton* button = dynamic_cast<BButton*>(view)) {
				if (button->IsDefault())
					states |= B_ACCESSIBLE_STATE_DEFAULT;
				if (button->Behavior() == BButton::B_POP_UP_BEHAVIOR)
					states |= B_ACCESSIBLE_STATE_HAS_POPUP;
			}
			actions |= B_ACCESSIBLE_ACTION_PRESS;
			break;
		default:
			break;
	}

	if (BTextControl* textControl = dynamic_cast<BTextControl*>(view)) {
		BTextView* textView = textControl->TextView();
		states |= textView->IsEditable()
			? B_ACCESSIBLE_STATE_EDITABLE : B_ACCESSIBLE_STATE_READ_ONLY;
		if (textView->IsTypingHidden())
			states |= B_ACCESSIBLE_STATE_PROTECTED;
		add_text_value(node, textView);
	} else if (BAbstractSpinner* spinner
			= dynamic_cast<BAbstractSpinner*>(view)) {
		if (spinner->TextView() != NULL)
			node.AddString(B_ACCESSIBLE_FIELD_VALUE, spinner->TextView()->Text());
		if (BSpinner* intSpinner = dynamic_cast<BSpinner*>(spinner)) {
			node.AddDouble(B_ACCESSIBLE_FIELD_CURRENT, intSpinner->Value());
			node.AddDouble(B_ACCESSIBLE_FIELD_MINIMUM, intSpinner->MinValue());
			node.AddDouble(B_ACCESSIBLE_FIELD_MAXIMUM, intSpinner->MaxValue());
		} else if (BDecimalSpinner* decimal
				= dynamic_cast<BDecimalSpinner*>(spinner)) {
			node.AddDouble(B_ACCESSIBLE_FIELD_CURRENT, decimal->Value());
			node.AddDouble(B_ACCESSIBLE_FIELD_MINIMUM, decimal->MinValue());
			node.AddDouble(B_ACCESSIBLE_FIELD_MAXIMUM, decimal->MaxValue());
		}
		states |= B_ACCESSIBLE_STATE_EDITABLE;
		actions |= B_ACCESSIBLE_ACTION_INCREMENT | B_ACCESSIBLE_ACTION_DECREMENT;
	} else if (BSlider* slider = dynamic_cast<BSlider*>(view)) {
		int32 minimum, maximum;
		slider->GetLimits(&minimum, &maximum);
		const char* text = slider->UpdateText();
		BString value;
		if (text != NULL && text[0] != '\0')
			value = text;
		else
			value << slider->Value();
		node.AddString(B_ACCESSIBLE_FIELD_VALUE, value);
		node.AddDouble(B_ACCESSIBLE_FIELD_CURRENT, slider->Value());
		node.AddDouble(B_ACCESSIBLE_FIELD_MINIMUM, minimum);
		node.AddDouble(B_ACCESSIBLE_FIELD_MAXIMUM, maximum);
		states |= slider->Orientation() == B_HORIZONTAL
			? B_ACCESSIBLE_STATE_HORIZONTAL : B_ACCESSIBLE_STATE_VERTICAL;
		actions |= B_ACCESSIBLE_ACTION_INCREMENT | B_ACCESSIBLE_ACTION_DECREMENT;
	} else if (BChannelSlider* channel = dynamic_cast<BChannelSlider*>(view)) {
		int32 minimum, maximum;
		channel->GetLimits(&minimum, &maximum);
		BString value;
		value << channel->Value();
		node.AddString(B_ACCESSIBLE_FIELD_VALUE, value);
		node.AddDouble(B_ACCESSIBLE_FIELD_CURRENT, channel->Value());
		node.AddDouble(B_ACCESSIBLE_FIELD_MINIMUM, minimum);
		node.AddDouble(B_ACCESSIBLE_FIELD_MAXIMUM, maximum);
	} else if (BScrollBar* scrollBar = dynamic_cast<BScrollBar*>(view)) {
		float minimum, maximum;
		scrollBar->GetRange(&minimum, &maximum);
		BString value;
		value << (int32)scrollBar->Value();
		node.AddString(B_ACCESSIBLE_FIELD_VALUE, value);
		node.AddDouble(B_ACCESSIBLE_FIELD_CURRENT, scrollBar->Value());
		node.AddDouble(B_ACCESSIBLE_FIELD_MINIMUM, minimum);
		node.AddDouble(B_ACCESSIBLE_FIELD_MAXIMUM, maximum);
		states |= scrollBar->Orientation() == B_HORIZONTAL
			? B_ACCESSIBLE_STATE_HORIZONTAL : B_ACCESSIBLE_STATE_VERTICAL;
		actions |= B_ACCESSIBLE_ACTION_INCREMENT | B_ACCESSIBLE_ACTION_DECREMENT;
	} else if (BColorControl* color = dynamic_cast<BColorControl*>(view)) {
		rgb_color value = color->ValueAsColor();
		BString text;
		text.SetToFormat("#%02x%02x%02x", value.red, value.green, value.blue);
		node.AddString(B_ACCESSIBLE_FIELD_VALUE, text);
	} else if (BMenuField* field = dynamic_cast<BMenuField*>(view)) {
		if (field->MenuItem() != NULL)
			node.AddString(B_ACCESSIBLE_FIELD_VALUE, field->MenuItem()->Label());
		if (!field->IsEnabled())
			states |= B_ACCESSIBLE_STATE_DISABLED;
		states |= B_ACCESSIBLE_STATE_HAS_POPUP | B_ACCESSIBLE_STATE_FOCUSABLE;
		actions |= B_ACCESSIBLE_ACTION_SHOW_MENU | B_ACCESSIBLE_ACTION_FOCUS;
	} else if (BStatusBar* statusBar = dynamic_cast<BStatusBar*>(view)) {
		BString value;
		if (statusBar->Text() != NULL && statusBar->Text()[0] != '\0')
			value << statusBar->Text() << " ";
		if (statusBar->MaxValue() > 0) {
			value << (int32)(100 * statusBar->CurrentValue()
				/ statusBar->MaxValue()) << "%";
		}
		node.AddString(B_ACCESSIBLE_FIELD_VALUE, value);
		node.AddDouble(B_ACCESSIBLE_FIELD_CURRENT, statusBar->CurrentValue());
		node.AddDouble(B_ACCESSIBLE_FIELD_MINIMUM, 0);
		node.AddDouble(B_ACCESSIBLE_FIELD_MAXIMUM, statusBar->MaxValue());
	} else if (BListView* list = dynamic_cast<BListView*>(view)) {
		if (list->ListType() == B_MULTIPLE_SELECTION_LIST)
			states |= B_ACCESSIBLE_STATE_MULTI_SELECTABLE;
		int32 selected = list->CurrentSelection();
		if (BStringItem* item
				= dynamic_cast<BStringItem*>(list->ItemAt(selected))) {
			node.AddString(B_ACCESSIBLE_FIELD_VALUE, item->Text());
		}
		if (selected >= 0)
			node.AddInt32(B_ACCESSIBLE_FIELD_INDEX, selected);
		states |= B_ACCESSIBLE_STATE_FOCUSABLE;
		actions |= B_ACCESSIBLE_ACTION_FOCUS;
	} else if (BTabView* tabView = dynamic_cast<BTabView*>(view)) {
		BTab* tab = tabView->TabAt(tabView->Selection());
		if (tab != NULL) {
			node.AddString(B_ACCESSIBLE_FIELD_VALUE, tab->Label());
			node.AddInt32(B_ACCESSIBLE_FIELD_INDEX, tabView->Selection());
		}
		states |= B_ACCESSIBLE_STATE_FOCUSABLE;
	} else if (BTextView* textView = dynamic_cast<BTextView*>(view)) {
		if (role == B_ACCESSIBLE_ROLE_LABEL) {
			if (name.IsEmpty())
				name = textView->Text();
		} else {
			states |= textView->IsEditable()
				? B_ACCESSIBLE_STATE_EDITABLE : B_ACCESSIBLE_STATE_READ_ONLY;
			states |= B_ACCESSIBLE_STATE_MULTI_LINE
				| B_ACCESSIBLE_STATE_FOCUSABLE;
			if (textView->IsTypingHidden())
				states |= B_ACCESSIBLE_STATE_PROTECTED;
			add_text_value(node, textView);
			actions |= B_ACCESSIBLE_ACTION_FOCUS;
		}
	}

	BRect frame = window != NULL
		? view->ConvertToScreen(view->Bounds()) : BRect(0, 0, -1, -1);
	add_common(node, view_id(view), parent_id_of_view(view), role, name,
		states, frame, actions);

	StringMap::iterator description = sDescriptions.find(view);
	if (description != sDescriptions.end()) {
		node.AddString(B_ACCESSIBLE_FIELD_DESCRIPTION, description->second);
	} else {
		BString toolTip = tool_tip_text(view);
		if (!toolTip.IsEmpty() && toolTip != name)
			node.AddString(B_ACCESSIBLE_FIELD_DESCRIPTION, toolTip);
	}

	// children: virtual items, then child views (except for composites)
	int32 registryID = register_object(ENTRY_VIEW, view, window);
	int32 itemCount = count_items(view);
	int32 count = 0;
	for (int32 i = 0; i < itemCount; i++) {
		node.AddInt64(B_ACCESSIBLE_FIELD_CHILD_IDS, make_id(registryID, i + 1));
		count++;
		if (depth != 0) {
			BMessage childNode;
			if (describe_item(view, i, childNode))
				node.AddMessage(B_ACCESSIBLE_FIELD_CHILD, &childNode);
		}
	}
	if (!is_composite(view)) {
		for (int32 i = 0; i < view->CountChildren(); i++) {
			BView* child = view->ChildAt(i);
			if (child->IsHidden(child))
				continue;
			node.AddInt64(B_ACCESSIBLE_FIELD_CHILD_IDS, view_id(child));
			count++;
			if (depth != 0) {
				BMessage childNode;
				describe_view(child, 0, childNode, depth - 1);
				node.AddMessage(B_ACCESSIBLE_FIELD_CHILD, &childNode);
			}
		}
	}
	node.AddInt32(B_ACCESSIBLE_FIELD_CHILD_COUNT, count);
}


static void
describe_object(BAccessible* object, BMessage& node, int32 depth)
{
	if (depth < -kMaxDepth)
		return;

	BView* host = BAccessiblePrivate::Host(object);
	int64 id = make_id(object_registry_id(object));
	int64 parent = 0;
	if (BAccessible* parentObject = object->Parent()) {
		if (BAccessiblePrivate::Host(parentObject) == NULL)
			BAccessiblePrivate::Host(parentObject) = host;
		parent = make_id(object_registry_id(parentObject));
	} else if (host != NULL)
		parent = parent_id_of_view(host);

	add_common(node, id, parent, object->Role(), object->Name(),
		object->States(), object->Frame(), object->Actions());

	BString description = object->Description();
	if (!description.IsEmpty())
		node.AddString(B_ACCESSIBLE_FIELD_DESCRIPTION, description);
	BString value = object->Value();
	if (!value.IsEmpty())
		node.AddString(B_ACCESSIBLE_FIELD_VALUE, value);
	double current, minimum, maximum;
	if (object->GetRange(current, minimum, maximum)) {
		node.AddDouble(B_ACCESSIBLE_FIELD_CURRENT, current);
		node.AddDouble(B_ACCESSIBLE_FIELD_MINIMUM, minimum);
		node.AddDouble(B_ACCESSIBLE_FIELD_MAXIMUM, maximum);
	}
	int32 start, end;
	if (object->GetTextSelection(start, end)) {
		node.AddInt32(B_ACCESSIBLE_FIELD_SEL_START, start);
		node.AddInt32(B_ACCESSIBLE_FIELD_SEL_END, end);
	}
	object->AddProperties(node);

	int32 count = object->CountChildren();
	int32 added = 0;
	for (int32 i = 0; i < count; i++) {
		BAccessible* child = object->ChildAt(i);
		if (child == NULL)
			continue;
		if (BAccessiblePrivate::Host(child) == NULL)
			BAccessiblePrivate::Host(child) = host;
		node.AddInt64(B_ACCESSIBLE_FIELD_CHILD_IDS,
			make_id(object_registry_id(child)));
		added++;
		if (depth != 0) {
			BMessage childNode;
			describe_object(child, childNode, depth - 1);
			node.AddMessage(B_ACCESSIBLE_FIELD_CHILD, &childNode);
		}
	}
	node.AddInt32(B_ACCESSIBLE_FIELD_CHILD_COUNT, added);
}


static BString
app_name()
{
	app_info info;
	if (be_app != NULL && be_app->GetAppInfo(&info) == B_OK)
		return BString(info.ref.name);
	return BString();
}


static void
describe_app(BMessage& node, int32 depth)
{
	add_common(node, make_id(kAppRootID), 0, B_ACCESSIBLE_ROLE_APPLICATION,
		app_name(), 0, BRect(0, 0, -1, -1), 0);

	int32 count = 0;
	for (int32 i = 0; be_app != NULL; i++) {
		BWindow* window = be_app->WindowAt(i);
		if (window == NULL)
			break;
		BMessenger messenger(window);
		if (messenger.LockTargetWithTimeout(kLockTimeout) != B_OK)
			continue;
		if (!window->IsHidden()) {
			BAutolock registryLocker(sRegistryLock);
			node.AddInt64(B_ACCESSIBLE_FIELD_CHILD_IDS, window_id(window));
			count++;
			if (depth != 0) {
				BMessage childNode;
				describe_window(window, childNode, depth - 1);
				node.AddMessage(B_ACCESSIBLE_FIELD_CHILD, &childNode);
			}
		}
		window->Unlock();
	}
	node.AddInt32(B_ACCESSIBLE_FIELD_CHILD_COUNT, count);
}


// #pragma mark - resolving IDs


/*!	Finds the object behind an ID and locks its looper. On success, the
	looper and sRegistryLock are locked; release both with
	unlock_resolved().
*/
static status_t
resolve_id(int64 id, RegistryEntry& _entry, BLooper*& _looper)
{
	int32 registryID = (int32)(id >> 32);

	BMessenger messenger;
	{
		BAutolock locker(sRegistryLock);
		EntryMap::iterator found = sEntries.find(registryID);
		if (found == sEntries.end())
			return B_ENTRY_NOT_FOUND;
		messenger = found->second.looper;
	}

	status_t status = messenger.LockTargetWithTimeout(kLockTimeout);
	if (status != B_OK) {
		if (status == B_BAD_PORT_ID || status == B_BAD_VALUE) {
			BAutolock locker(sRegistryLock);
			EntryMap::iterator found = sEntries.find(registryID);
			if (found != sEntries.end() && found->second.looper == messenger) {
				sIDs.erase(found->second.object);
				sEntries.erase(found);
			}
		}
		return status == B_TIMED_OUT ? B_BUSY : B_ENTRY_NOT_FOUND;
	}

	BLooper* looper = NULL;
	messenger.Target(&looper);
	sRegistryLock.Lock();
	EntryMap::iterator found = sEntries.find(registryID);
	if (found == sEntries.end() || !(found->second.looper == messenger)) {
		sRegistryLock.Unlock();
		looper->Unlock();
		return B_ENTRY_NOT_FOUND;
	}
	if (found->second.type == ENTRY_VIEW) {
		BView* view = (BView*)found->second.object;
		if (view->Looper() != looper) {
			sRegistryLock.Unlock();
			looper->Unlock();
			return B_ENTRY_NOT_FOUND;
		}
	}
	_entry = found->second;
	_looper = looper;
	return B_OK;
}


static void
unlock_resolved(BLooper* looper)
{
	sRegistryLock.Unlock();
	looper->Unlock();
}


static status_t
describe_id(int64 id, BMessage& node, int32 depth)
{
	if ((int32)(id >> 32) == kAppRootID) {
		describe_app(node, depth);
		return B_OK;
	}

	RegistryEntry entry;
	BLooper* looper;
	status_t status = resolve_id(id, entry, looper);
	if (status != B_OK)
		return status;

	int32 sub = (int32)(id & 0xffffffff);
	switch (entry.type) {
		case ENTRY_WINDOW:
			describe_window((BWindow*)entry.object, node, depth);
			break;
		case ENTRY_VIEW:
			if (sub > 0) {
				if (!describe_item((BView*)entry.object, sub - 1, node))
					status = B_ENTRY_NOT_FOUND;
			} else
				describe_view((BView*)entry.object, 0, node, depth);
			break;
		case ENTRY_OBJECT:
			describe_object((BAccessible*)entry.object, node, depth);
			break;
	}
	unlock_resolved(looper);
	return status;
}


// #pragma mark - actions


static status_t
perform_item_action(BView* view, int32 index, accessible_action action)
{
	if (BListView* list = dynamic_cast<BListView*>(view)) {
		if (index >= list->CountItems())
			return B_ENTRY_NOT_FOUND;
		BOutlineListView* outline = dynamic_cast<BOutlineListView*>(list);
		switch (action) {
			case B_ACCESSIBLE_ACTION_SELECT:
				list->Select(index);
				return B_OK;
			case B_ACCESSIBLE_ACTION_PRESS:
				list->Select(index);
				list->Invoke();
				return B_OK;
			case B_ACCESSIBLE_ACTION_EXPAND:
				if (outline == NULL)
					break;
				outline->Expand(list->ItemAt(index));
				return B_OK;
			case B_ACCESSIBLE_ACTION_COLLAPSE:
				if (outline == NULL)
					break;
				outline->Collapse(list->ItemAt(index));
				return B_OK;
			default:
				break;
		}
	} else if (BMenu* menu = dynamic_cast<BMenu*>(view)) {
		BMenuItem* item = menu->ItemAt(index);
		if (item == NULL)
			return B_ENTRY_NOT_FOUND;
		if (action == B_ACCESSIBLE_ACTION_PRESS && item->IsEnabled()
			&& item->Message() != NULL) {
			// what BMenuItem::Invoke() (protected) sends
			BMessage message(*item->Message());
			message.AddInt64("when", system_time());
			message.AddPointer("source", item);
			message.AddInt32("index", index);
			item->BInvoker::Invoke(&message);
			return B_OK;
		}
	} else if (BTabView* tabView = dynamic_cast<BTabView*>(view)) {
		if (index >= tabView->CountTabs())
			return B_ENTRY_NOT_FOUND;
		if (action == B_ACCESSIBLE_ACTION_SELECT
			|| action == B_ACCESSIBLE_ACTION_PRESS) {
			tabView->Select(index);
			return B_OK;
		}
	}
	return B_NOT_SUPPORTED;
}


static status_t
perform_view_action(BView* view, accessible_action action)
{
	view = effective_view(view);
	if (BAccessible* root = attached_root(view))
		return root->PerformAction(action);

	if (action == B_ACCESSIBLE_ACTION_FOCUS) {
		view->MakeFocus(true);
		if (view->Window() != NULL && !view->Window()->IsActive())
			view->Window()->Activate();
		return B_OK;
	}

	BControl* control = dynamic_cast<BControl*>(view);
	if (control != NULL && !control->IsEnabled())
		return B_NOT_ALLOWED;

	if (action == B_ACCESSIBLE_ACTION_PRESS && control != NULL) {
		if (dynamic_cast<BCheckBox*>(control) != NULL) {
			control->SetValue(control->Value() == B_CONTROL_ON
				? B_CONTROL_OFF : B_CONTROL_ON);
		} else if (dynamic_cast<BRadioButton*>(control) != NULL)
			control->SetValue(B_CONTROL_ON);
		else if (BButton* button = dynamic_cast<BButton*>(control)) {
			if (button->Behavior() == BButton::B_TOGGLE_BEHAVIOR) {
				control->SetValue(control->Value() == B_CONTROL_ON
					? B_CONTROL_OFF : B_CONTROL_ON);
			}
		}
		control->Invoke();
		return B_OK;
	}

	if (action == B_ACCESSIBLE_ACTION_INCREMENT
		|| action == B_ACCESSIBLE_ACTION_DECREMENT) {
		bool up = action == B_ACCESSIBLE_ACTION_INCREMENT;
		if (BAbstractSpinner* spinner = dynamic_cast<BAbstractSpinner*>(view)) {
			if (up)
				spinner->Increment();
			else
				spinner->Decrement();
			return B_OK;
		}
		if (BSlider* slider = dynamic_cast<BSlider*>(view)) {
			int32 step = max_c(1, slider->KeyIncrementValue());
			slider->SetValue(slider->Value() + (up ? step : -step));
			slider->Invoke();
			return B_OK;
		}
		if (BScrollBar* scrollBar = dynamic_cast<BScrollBar*>(view)) {
			float small, big;
			scrollBar->GetSteps(&small, &big);
			scrollBar->SetValue(scrollBar->Value() + (up ? small : -small));
			return B_OK;
		}
	}
	return B_NOT_SUPPORTED;
}


static status_t
perform_action(int64 id, accessible_action action)
{
	RegistryEntry entry;
	BLooper* looper;
	status_t status = resolve_id(id, entry, looper);
	if (status != B_OK)
		return status;

	int32 sub = (int32)(id & 0xffffffff);
	switch (entry.type) {
		case ENTRY_WINDOW:
			if (action == B_ACCESSIBLE_ACTION_FOCUS)
				((BWindow*)entry.object)->Activate();
			else
				status = B_NOT_SUPPORTED;
			break;
		case ENTRY_VIEW:
			status = sub > 0
				? perform_item_action((BView*)entry.object, sub - 1, action)
				: perform_view_action((BView*)entry.object, action);
			break;
		case ENTRY_OBJECT:
			status = ((BAccessible*)entry.object)->PerformAction(action);
			break;
	}
	unlock_resolved(looper);
	return status;
}


// #pragma mark - events


/*!	Value, state, text and selection events are only sent for the control
	that has the keyboard focus, so that applications updating sliders,
	lists or text views in the background cause no traffic. Applications
	can use BAccessible::Announce() for anything else worth saying.
*/
static bool
has_focus(BView* view)
{
	BWindow* window = view->Window();
	if (window == NULL)
		return false;
	BView* focus = window->CurrentFocus();
	return focus != NULL && effective_view(focus) == effective_view(view);
}


static void
send_event(BMessage& event)
{
	BMessenger server;
	{
		BAutolock locker(sRegistryLock);
		server = sServer;
	}
	if (!server.IsValid())
		return;
	// never block the window thread: drop the event if the server lags
	server.SendMessage(&event, (BHandler*)NULL, 0);
}


static void
make_event(BMessage& event, accessible_event what)
{
	event.what = B_ACCESSIBILITY_EVENT;
	event.AddInt32("event", what);
	event.AddInt64("when", system_time());
}


static void
add_window(BMessage& event, BWindow* window)
{
	if (window == NULL)
		return;
	BMessage windowNode;
	describe_window(window, windowNode, 0);
	event.AddMessage("window", &windowNode);
	event.AddBool("window_active", window->IsActive());
}


static void
send_window_state(BWindow* window, accessible_event what)
{
	BMessage event;
	make_event(event, what);
	{
		BAutolock locker(sRegistryLock);
		add_window(event, window);
		BView* focus = window->CurrentFocus();
		if (focus != NULL && what == B_ACCESSIBLE_EVENT_WINDOW_ACTIVATED) {
			BMessage node;
			describe_view(focus, 0, node, 0);
			event.AddMessage("node", &node);
		}
	}
	send_event(event);
}


// #pragma mark - bridge looper


namespace {

class AccessibilityBridge : public BLooper {
public:
	AccessibilityBridge()
		:
		BLooper("accessibility bridge", B_NORMAL_PRIORITY)
	{
	}

	virtual void MessageReceived(BMessage* message)
	{
		BMessage reply(B_ACCESSIBILITY_REPLY);
		status_t status = B_OK;

		switch (message->what) {
			case kMsgInitialState:
				_SendInitialState();
				return;

			case B_ACCESSIBILITY_GET_ROOT:
			{
				BMessage node;
				describe_app(node, message->GetInt32("depth", 0));
				reply.AddMessage("node", &node);
				break;
			}

			case B_ACCESSIBILITY_GET_NODE:
			{
				BMessage node;
				status = describe_id(message->GetInt64("id", 0), node,
					message->GetInt32("depth", 0));
				if (status == B_OK)
					reply.AddMessage("node", &node);
				break;
			}

			case B_ACCESSIBILITY_GET_FOCUS:
				status = _GetFocus(reply);
				break;

			case B_ACCESSIBILITY_PERFORM_ACTION:
				status = perform_action(message->GetInt64("id", 0),
					(accessible_action)message->GetInt32("action", 0));
				break;

			case B_ACCESSIBILITY_GET_TEXT:
				status = _GetText(message, reply);
				break;

			default:
				BLooper::MessageReceived(message);
				return;
		}

		reply.AddInt32("status", status);
		message->SendReply(&reply, (BHandler*)NULL, kLockTimeout);
	}

private:
	BWindow* _ActiveWindow()
	{
		for (int32 i = 0; be_app != NULL; i++) {
			BWindow* window = be_app->WindowAt(i);
			if (window == NULL)
				break;
			if (window->IsActive())
				return window;
		}
		return NULL;
	}

	void _SendInitialState()
	{
		BWindow* window = _ActiveWindow();
		if (window == NULL)
			return;
		BMessenger messenger(window);
		if (messenger.LockTargetWithTimeout(kLockTimeout) != B_OK)
			return;
		send_window_state(window, B_ACCESSIBLE_EVENT_WINDOW_ACTIVATED);
		window->Unlock();
	}

	status_t _GetFocus(BMessage& reply)
	{
		BWindow* window = _ActiveWindow();
		if (window == NULL)
			return B_ENTRY_NOT_FOUND;
		BMessenger messenger(window);
		if (messenger.LockTargetWithTimeout(kLockTimeout) != B_OK)
			return B_BUSY;
		{
			BAutolock locker(sRegistryLock);
			BMessage windowNode;
			describe_window(window, windowNode, 0);
			reply.AddMessage("window", &windowNode);
			if (BView* focus = window->CurrentFocus()) {
				BMessage node;
				describe_view(focus, 0, node, 0);
				reply.AddMessage("node", &node);
			}
		}
		window->Unlock();
		return B_OK;
	}

	status_t _GetText(BMessage* message, BMessage& reply)
	{
		RegistryEntry entry;
		BLooper* looper;
		status_t status = resolve_id(message->GetInt64("id", 0), entry,
			looper);
		if (status != B_OK)
			return status;

		int32 start = message->GetInt32("start", 0);
		int32 end = message->GetInt32("end", INT32_MAX);
		BString text;
		if (entry.type == ENTRY_OBJECT)
			text = ((BAccessible*)entry.object)->Text(start, end);
		else if (entry.type == ENTRY_VIEW) {
			BView* view = effective_view((BView*)entry.object);
			BTextView* textView = dynamic_cast<BTextView*>(view);
			if (BTextControl* control = dynamic_cast<BTextControl*>(view))
				textView = control->TextView();
			if (textView == NULL || textView->IsTypingHidden())
				status = B_NOT_SUPPORTED;
			else {
				end = min_c(end, textView->TextLength());
				start = max_c(0, min_c(start, end));
				char* buffer = text.LockBuffer(end - start + 1);
				textView->GetText(start, end - start, buffer);
				text.UnlockBuffer(end - start);
			}
		}
		unlock_resolved(looper);
		if (status == B_OK)
			reply.AddString("text", text);
		return status;
	}
};

}	// namespace


// #pragma mark - hooks


namespace BPrivate {


void
accessibility_enable(BMessage* message)
{
	BMessenger server;
	if (message->FindMessenger("server", &server) != B_OK)
		return;

	{
		BAutolock locker(sRegistryLock);
		sServer = server;
	}

	if (sBridge == NULL) {
		sBridge = new(std::nothrow) AccessibilityBridge();
		if (sBridge == NULL)
			return;
		sBridge->Run();
	}

	gAccessibilityEnabled = true;

	app_info info;
	BMessage reg(B_ACCESSIBILITY_REGISTER);
	reg.AddMessenger("bridge", BMessenger(sBridge));
	reg.AddInt32("team", be_app != NULL ? be_app->Team() : -1);
	if (be_app != NULL && be_app->GetAppInfo(&info) == B_OK)
		reg.AddString("signature", info.signature);
	server.SendMessage(&reg, (BHandler*)NULL, kLockTimeout);

	sBridge->PostMessage(kMsgInitialState);
}


void
accessibility_disable()
{
	gAccessibilityEnabled = false;
	{
		BAutolock locker(sRegistryLock);
		sServer = BMessenger();
	}

	if (sBridge != NULL) {
		BLooper* bridge = sBridge;
		sBridge = NULL;
		if (bridge->Lock())
			bridge->Quit();
	}
}


void
accessibility_view_deleted(BView* view)
{
	BAutolock locker(sRegistryLock);
	unregister_object(view);
	sNames.erase(view);
	sDescriptions.erase(view);
	AttachMap::iterator found = sAttached.find(view);
	if (found != sAttached.end()) {
		BAccessiblePrivate::Host(found->second) = NULL;
		sAttached.erase(found);
	}
}


void
accessibility_notify(BView* view, accessible_event what,
	const BMessage* details)
{
	if (view == NULL || !has_focus(view))
		return;

	BMessage event;
	if (details != NULL)
		event = *details;
	make_event(event, what);
	{
		BAutolock locker(sRegistryLock);
		BMessage node;
		describe_view(view, 0, node, 0);
		event.AddMessage("node", &node);
		event.AddBool("window_active", view->Window()->IsActive());
	}
	send_event(event);
}


void
accessibility_focus_changed(BWindow* window, BView* focus)
{
	if (focus == NULL || window == NULL)
		return;

	BMessage event;
	make_event(event, B_ACCESSIBLE_EVENT_FOCUS_CHANGED);
	{
		BAutolock locker(sRegistryLock);
		BMessage node;
		describe_view(focus, 0, node, 0);
		event.AddMessage("node", &node);
		add_window(event, window);
	}
	send_event(event);
}


void
accessibility_window_activated(BWindow* window, bool active)
{
	send_window_state(window, active ? B_ACCESSIBLE_EVENT_WINDOW_ACTIVATED
		: B_ACCESSIBLE_EVENT_WINDOW_DEACTIVATED);
}


static void
add_event_text(BMessage& event, BTextView* view, int32 offset, int32 length)
{
	if (view->IsTypingHidden()) {
		event.AddBool("hidden", true);
		return;
	}
	length = min_c(length, kMaxEventTextLength);
	BString text;
	char* buffer = text.LockBuffer(length + 1);
	view->GetText(offset, length, buffer);
	text.UnlockBuffer(length);
	event.AddString("text", text);
}


void
accessibility_text_inserted(BTextView* view, const char* text, int32 length,
	int32 offset)
{
	if (length <= 0 || !has_focus(view))
		return;

	BMessage details;
	details.AddInt32("offset", offset);
	details.AddInt32("length", length);
	add_event_text(details, view, offset, length);
	accessibility_notify(view, B_ACCESSIBLE_EVENT_TEXT_INSERTED, &details);
}


void
accessibility_text_deleted(BTextView* view, int32 fromOffset, int32 toOffset)
{
	if (toOffset <= fromOffset || !has_focus(view))
		return;

	BMessage details;
	details.AddInt32("offset", fromOffset);
	details.AddInt32("length", toOffset - fromOffset);
	add_event_text(details, view, fromOffset, toOffset - fromOffset);
	accessibility_notify(view, B_ACCESSIBLE_EVENT_TEXT_DELETED, &details);
}


void
accessibility_caret_moved(BTextView* view)
{
	if (view->Window() == NULL || !view->IsFocus())
		return;

	int32 start, end;
	view->GetSelection(&start, &end);
	BMessage details;
	details.AddInt32("caret", end);
	details.AddInt32(B_ACCESSIBLE_FIELD_SEL_START, start);
	details.AddInt32(B_ACCESSIBLE_FIELD_SEL_END, end);
	if (!view->IsTypingHidden()) {
		if (start != end)
			add_event_text(details, view, start, end - start);
		else {
			// the character after the caret and the caret's line
			int32 length = 0;
			if (end < view->TextLength()) {
				uchar first = (uchar)view->ByteAt(end);
				length = first < 0x80 ? 1 : first < 0xe0 ? 2
					: first < 0xf0 ? 3 : 4;
				length = min_c(length, view->TextLength() - end);
			}
			BString character;
			if (length > 0) {
				char* buffer = character.LockBuffer(length + 1);
				view->GetText(end, length, buffer);
				character.UnlockBuffer(length);
			}
			details.AddString("char", character);

			int32 line = view->CurrentLine();
			int32 lineStart = view->OffsetAt(line);
			int32 lineEnd = line + 1 < view->CountLines()
				? view->OffsetAt(line + 1) : view->TextLength();
			details.AddInt32("line", line);
			int32 lineLength = min_c(lineEnd - lineStart, kMaxEventTextLength);
			BString lineText;
			if (lineLength > 0) {
				char* buffer = lineText.LockBuffer(lineLength + 1);
				view->GetText(lineStart, lineLength, buffer);
				lineText.UnlockBuffer(lineLength);
			}
			details.AddString("line_text", lineText);
		}
	}
	accessibility_notify(view, B_ACCESSIBLE_EVENT_CARET_MOVED, &details);
}


void
accessibility_item_selected(BView* view, int32 index)
{
	if (view == NULL || index < 0 || !has_focus(view))
		return;

	BMessage event;
	make_event(event, B_ACCESSIBLE_EVENT_SELECTION_CHANGED);
	{
		BAutolock locker(sRegistryLock);
		BMessage node;
		if (!describe_item(view, index, node))
			return;
		event.AddMessage("node", &node);
		BMessage container;
		describe_view(view, 0, container, 0);
		event.AddMessage("container", &container);
		event.AddBool("window_active", view->Window()->IsActive());
	}
	send_event(event);
}


void
accessibility_menu_item_selected(BMenu* menu, BMenuItem* item)
{
	if (menu == NULL || item == NULL || menu->Window() == NULL)
		return;

	int32 index = menu->IndexOf(item);
	if (index < 0)
		return;

	BMessage event;
	make_event(event, B_ACCESSIBLE_EVENT_FOCUS_CHANGED);
	{
		BAutolock locker(sRegistryLock);
		BMessage node;
		if (!describe_item(menu, index, node))
			return;
		event.AddMessage("node", &node);
		BMessage container;
		describe_view(menu, 0, container, 0);
		event.AddMessage("container", &container);
		// menus never become the active window, but are what the user is in
		event.AddBool("window_active", true);
		event.AddBool("menu", true);
	}
	send_event(event);
}


}	// namespace BPrivate


// #pragma mark - BAccessible


BAccessible::BAccessible()
	:
	fHost(NULL),
	fRegistryID(0)
{
}


BAccessible::~BAccessible()
{
	BAutolock locker(sRegistryLock);
	if (fRegistryID != 0)
		unregister_object(this);
	for (AttachMap::iterator it = sAttached.begin(); it != sAttached.end();
			it++) {
		if (it->second == this) {
			sAttached.erase(it);
			break;
		}
	}
}


BString
BAccessible::Name() const
{
	return BString();
}


BString
BAccessible::Description() const
{
	return BString();
}


BString
BAccessible::Value() const
{
	return BString();
}


bool
BAccessible::GetRange(double& value, double& minimum, double& maximum) const
{
	return false;
}


uint64
BAccessible::States() const
{
	return 0;
}


BRect
BAccessible::Frame() const
{
	return BRect(0, 0, -1, -1);
}


BAccessible*
BAccessible::Parent() const
{
	return NULL;
}


int32
BAccessible::CountChildren() const
{
	return 0;
}


BAccessible*
BAccessible::ChildAt(int32 index) const
{
	return NULL;
}


uint32
BAccessible::Actions() const
{
	return 0;
}


status_t
BAccessible::PerformAction(accessible_action action)
{
	return B_NOT_SUPPORTED;
}


bool
BAccessible::GetTextSelection(int32& start, int32& end) const
{
	return false;
}


BString
BAccessible::Text(int32 start, int32 end) const
{
	return BString();
}


void
BAccessible::AddProperties(BMessage& node) const
{
}


int64
BAccessible::ID()
{
	if (!gAccessibilityEnabled)
		return 0;
	BAutolock locker(sRegistryLock);
	if (fHost == NULL) {
		// inherit the host of the parent
		for (BAccessible* parent = Parent(); parent != NULL;
				parent = parent->Parent()) {
			if (parent->fHost != NULL) {
				fHost = parent->fHost;
				break;
			}
		}
	}
	return make_id(object_registry_id(this));
}


BView*
BAccessible::Host() const
{
	return fHost;
}


void
BAccessible::NotifyEvent(accessible_event what, const BMessage* details)
{
	if (!gAccessibilityEnabled)
		return;

	BMessage event;
	if (details != NULL)
		event = *details;
	make_event(event, what);
	{
		BAutolock locker(sRegistryLock);
		ID();
		BMessage node;
		describe_object(this, node, 0);
		event.AddMessage("node", &node);
		if (fHost != NULL && fHost->Window() != NULL) {
			add_window(event, fHost->Window());
		}
	}
	send_event(event);
}


/*static*/ status_t
BAccessible::Attach(BView* view, BAccessible* root)
{
	if (view == NULL || root == NULL)
		return B_BAD_VALUE;
	BAutolock locker(sRegistryLock);
	sAttached[view] = root;
	root->fHost = view;
	gAccessibilityViewData = true;
	return B_OK;
}


/*static*/ void
BAccessible::Detach(BView* view)
{
	BAutolock locker(sRegistryLock);
	AttachMap::iterator found = sAttached.find(view);
	if (found == sAttached.end())
		return;
	found->second->fHost = NULL;
	sAttached.erase(found);
}


/*static*/ status_t
BAccessible::SetName(BView* view, const char* name)
{
	if (view == NULL)
		return B_BAD_VALUE;
	BAutolock locker(sRegistryLock);
	if (name == NULL)
		sNames.erase(view);
	else
		sNames[view] = name;
	gAccessibilityViewData = true;
	return B_OK;
}


/*static*/ status_t
BAccessible::SetDescription(BView* view, const char* description)
{
	if (view == NULL)
		return B_BAD_VALUE;
	BAutolock locker(sRegistryLock);
	if (description == NULL)
		sDescriptions.erase(view);
	else
		sDescriptions[view] = description;
	gAccessibilityViewData = true;
	return B_OK;
}


/*static*/ bool
BAccessible::IsEnabled()
{
	return gAccessibilityEnabled;
}


/*static*/ void
BAccessible::Announce(const char* text, bool interrupt)
{
	if (!gAccessibilityEnabled || text == NULL)
		return;
	BMessage event;
	make_event(event, B_ACCESSIBLE_EVENT_ANNOUNCEMENT);
	event.AddString("text", text);
	event.AddBool("interrupt", interrupt);
	send_event(event);
}


/*static*/ const char*
BAccessible::RoleName(accessible_role role)
{
	static const char* kNames[] = {
		"unknown", "application", "window", "dialog", "alert", "group",
		"scroll pane", "split pane", "label", "button", "toggle button",
		"check box", "radio button", "text field", "text area",
		"spin button", "slider", "scroll bar", "progress bar",
		"pop-up button", "color picker", "list", "list item", "tree",
		"tree item", "table", "row", "cell", "column header", "menu bar",
		"menu", "menu item", "separator", "tab list", "tab", "tab panel",
		"tool bar", "status bar", "tool tip", "image", "canvas", "link",
		"heading", "paragraph", "document", "article", "landmark", "form",
		"combo box", "math"
	};
	static_assert(sizeof(kNames) / sizeof(kNames[0])
		== B_ACCESSIBLE_ROLE_COUNT, "role names out of sync");

	if (role < 0 || role >= B_ACCESSIBLE_ROLE_COUNT)
		return "unknown";
	return kNames[role];
}
