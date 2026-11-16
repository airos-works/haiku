/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "NodeText.h"

#include <string.h>

#include <StringList.h>

#include <Accessible.h>
#include <AccessibilityProtocol.h>


const char*
spoken_role(int32 role)
{
	switch (role) {
		case B_ACCESSIBLE_ROLE_UNKNOWN:
		case B_ACCESSIBLE_ROLE_GROUP:
		case B_ACCESSIBLE_ROLE_SCROLL_PANE:
		case B_ACCESSIBLE_ROLE_SPLIT_PANE:
		case B_ACCESSIBLE_ROLE_LABEL:
		case B_ACCESSIBLE_ROLE_PARAGRAPH:
		case B_ACCESSIBLE_ROLE_CANVAS:
			return NULL;
		case B_ACCESSIBLE_ROLE_POP_UP_BUTTON:
			return "pop-up menu";
		default:
			return BAccessible::RoleName((accessible_role)role);
	}
}


BString
spoken_text(const BString& text)
{
	if (text == " ")
		return "space";
	if (text == "\n")
		return "new line";
	if (text == "\t")
		return "tab";
	BString trimmed(text);
	trimmed.Trim();
	if (trimmed.IsEmpty())
		return "blank";
	return text;
}


/*!	A landmark's kind as Summit gives it (WebCore's role without
	"Landmark") said the way screen readers say it: "navigation landmark".
*/
static BString
landmark_role(const BMessage& node)
{
	struct kind {
		const char*	name;
		const char*	spoken;
	};
	static const kind kKinds[] = {
		{ "Banner", "banner" },
		{ "Complementary", "complementary" },
		{ "ContentInfo", "content information" },
		{ "Main", "main" },
		{ "Navigation", "navigation" },
		{ "Region", "region" },
		{ "Search", "search" },
		{ "Form", "form" },
	};

	BString kindName = node.GetString("landmark", "");
	BString text;
	for (size_t i = 0; i < sizeof(kKinds) / sizeof(kKinds[0]); i++) {
		if (kindName.ICompare(kKinds[i].name) == 0) {
			text = kKinds[i].spoken;
			break;
		}
	}
	if (text.IsEmpty() && !kindName.IsEmpty()) {
		text = kindName;
		text.ToLower();
	}
	if (!text.IsEmpty())
		text << " ";
	text << "landmark";
	return text;
}


BString
describe_node(const BMessage& node, bool withDescription)
{
	int32 role = node.GetInt32(B_ACCESSIBLE_FIELD_ROLE, 0);
	BString name = node.GetString(B_ACCESSIBLE_FIELD_NAME, "");
	BString value = node.GetString(B_ACCESSIBLE_FIELD_VALUE, "");
	uint64 states = (uint64)node.GetInt64(B_ACCESSIBLE_FIELD_STATES, 0);

	BStringList parts;
	if (role == B_ACCESSIBLE_ROLE_LANDMARK) {
		// "navigation landmark, Main": the kind is what tells landmarks apart
		parts.Add(landmark_role(node));
		if (!name.IsEmpty())
			parts.Add(name);
	} else {
		if (!name.IsEmpty())
			parts.Add(name);
		if (const char* roleName = spoken_role(role))
			parts.Add(roleName);
	}

	bool textRole = role == B_ACCESSIBLE_ROLE_TEXT_FIELD
		|| role == B_ACCESSIBLE_ROLE_TEXT_AREA;
	if ((states & B_ACCESSIBLE_STATE_PROTECTED) != 0) {
		parts.Add("protected");
	} else if (!value.IsEmpty() && value != name) {
		if (textRole && value.Length() > 300) {
			value.Truncate(300);
			value << "…";
		}
		parts.Add(value);
	} else if (textRole && value.IsEmpty())
		parts.Add("blank");

	if ((states & B_ACCESSIBLE_STATE_CHECKABLE) != 0) {
		if ((states & B_ACCESSIBLE_STATE_CHECKED) != 0)
			parts.Add("checked");
		else if ((states & B_ACCESSIBLE_STATE_MIXED) != 0)
			parts.Add("partially checked");
		else
			parts.Add("not checked");
	} else if (role == B_ACCESSIBLE_ROLE_MENU_ITEM
		&& (states & B_ACCESSIBLE_STATE_CHECKED) != 0) {
		parts.Add("checked");
	}
	if (role == B_ACCESSIBLE_ROLE_TOGGLE_BUTTON)
		parts.Add((states & B_ACCESSIBLE_STATE_PRESSED) != 0
			? "pressed" : "not pressed");
	if ((states & B_ACCESSIBLE_STATE_EXPANDABLE) != 0)
		parts.Add((states & B_ACCESSIBLE_STATE_EXPANDED) != 0
			? "expanded" : "collapsed");
	if (role == B_ACCESSIBLE_ROLE_MENU_ITEM
		&& (states & B_ACCESSIBLE_STATE_HAS_POPUP) != 0) {
		parts.Add("submenu");
	}
	if ((states & B_ACCESSIBLE_STATE_DEFAULT) != 0)
		parts.Add("default");
	if ((states & B_ACCESSIBLE_STATE_DISABLED) != 0)
		parts.Add("unavailable");
	if (textRole && (states & B_ACCESSIBLE_STATE_READ_ONLY) != 0)
		parts.Add("read only");

	int32 index;
	if (node.FindInt32(B_ACCESSIBLE_FIELD_INDEX, &index) == B_OK) {
		int32 count = node.GetInt32(B_ACCESSIBLE_FIELD_COUNT,
			node.GetInt32(B_ACCESSIBLE_FIELD_CHILD_COUNT, 0));
		if (count > 0) {
			BString position;
			position << index + 1 << " of " << count;
			parts.Add(position);
		}
	}
	int32 level;
	if (node.FindInt32(B_ACCESSIBLE_FIELD_LEVEL, &level) == B_OK && level > 1) {
		BString text;
		text << "level " << level;
		parts.Add(text);
	}
	const char* shortcut;
	if (node.FindString(B_ACCESSIBLE_FIELD_SHORTCUT, &shortcut) == B_OK)
		parts.Add(shortcut);

	if (withDescription) {
		BString description = node.GetString(B_ACCESSIBLE_FIELD_DESCRIPTION,
			"");
		if (!description.IsEmpty() && description != name)
			parts.Add(description);
	}

	return parts.Join(", ");
}


BString
describe_window(const BMessage& window)
{
	int32 role = window.GetInt32(B_ACCESSIBLE_FIELD_ROLE,
		B_ACCESSIBLE_ROLE_WINDOW);
	BString text = window.GetString(B_ACCESSIBLE_FIELD_NAME, "");
	if (text.IsEmpty())
		text = "Untitled";
	text << ", " << BAccessible::RoleName((accessible_role)role);
	BString description = window.GetString(B_ACCESSIBLE_FIELD_DESCRIPTION, "");
	if (!description.IsEmpty())
		text << ", " << description;
	return text;
}


/*!	What changed in the states of \a node, the focus: the event's "changed"
	mask names the bits. Without one, only "unavailable" or "available" can
	be said, which is what Interface Kit controls send it for.
*/
BString
describe_state_change(const BMessage& node, uint64 changed)
{
	uint64 states = (uint64)node.GetInt64(B_ACCESSIBLE_FIELD_STATES, 0);
	int32 role = node.GetInt32(B_ACCESSIBLE_FIELD_ROLE, 0);

	BStringList parts;
	if ((changed & B_ACCESSIBLE_STATE_EXPANDED) != 0)
		parts.Add((states & B_ACCESSIBLE_STATE_EXPANDED) != 0
			? "expanded" : "collapsed");
	if ((changed & (B_ACCESSIBLE_STATE_CHECKED | B_ACCESSIBLE_STATE_MIXED))
			!= 0) {
		if ((states & B_ACCESSIBLE_STATE_CHECKED) != 0)
			parts.Add("checked");
		else if ((states & B_ACCESSIBLE_STATE_MIXED) != 0)
			parts.Add("partially checked");
		else
			parts.Add("not checked");
	}
	if ((changed & B_ACCESSIBLE_STATE_PRESSED) != 0)
		parts.Add((states & B_ACCESSIBLE_STATE_PRESSED) != 0
			? "pressed" : "not pressed");
	if ((changed & B_ACCESSIBLE_STATE_SELECTED) != 0)
		parts.Add((states & B_ACCESSIBLE_STATE_SELECTED) != 0
			? "selected" : "not selected");
	if ((changed & B_ACCESSIBLE_STATE_BUSY) != 0)
		parts.Add((states & B_ACCESSIBLE_STATE_BUSY) != 0 ? "busy" : "done");
	if ((changed & B_ACCESSIBLE_STATE_INVALID) != 0)
		parts.Add((states & B_ACCESSIBLE_STATE_INVALID) != 0
			? "invalid entry" : "valid entry");
	if ((changed & B_ACCESSIBLE_STATE_READ_ONLY) != 0
		&& (role == B_ACCESSIBLE_ROLE_TEXT_FIELD
			|| role == B_ACCESSIBLE_ROLE_TEXT_AREA)) {
		parts.Add((states & B_ACCESSIBLE_STATE_READ_ONLY) != 0
			? "read only" : "editable");
	}
	if ((changed & B_ACCESSIBLE_STATE_DISABLED) != 0)
		parts.Add((states & B_ACCESSIBLE_STATE_DISABLED) != 0
			? "unavailable" : "available");
	return parts.Join(", ");
}


//	#pragma mark - read all


static bool
is_text_run(const BMessage& node)
{
	int32 role = node.GetInt32(B_ACCESSIBLE_FIELD_ROLE, 0);
	return role == B_ACCESSIBLE_ROLE_LABEL
		&& node.GetInt32(B_ACCESSIBLE_FIELD_CHILD_COUNT, 0) == 0
		&& !node.HasMessage(B_ACCESSIBLE_FIELD_CHILD);
}


static bool
is_invisible(const BMessage& node)
{
	return ((uint64)node.GetInt64(B_ACCESSIBLE_FIELD_STATES, 0)
		& B_ACCESSIBLE_STATE_INVISIBLE) != 0;
}


static BString
run_text(const BMessage& node)
{
	BString text = node.GetString(B_ACCESSIBLE_FIELD_NAME, "");
	if (text.IsEmpty())
		text = node.GetString(B_ACCESSIBLE_FIELD_VALUE, "");
	return text;
}


/*!	A link inside running text: its words, then "link", so that a sentence
	with a link in it is still read as one sentence.
*/
static bool
is_inline_link(const BMessage& node, BString& text)
{
	if (node.GetInt32(B_ACCESSIBLE_FIELD_ROLE, 0) != B_ACCESSIBLE_ROLE_LINK)
		return false;
	text = node.GetString(B_ACCESSIBLE_FIELD_NAME, "");
	BMessage child;
	for (int32 i = 0; node.FindMessage(B_ACCESSIBLE_FIELD_CHILD, i, &child)
			== B_OK; i++) {
		if (!is_text_run(child))
			return false;
		if (text.IsEmpty())
			text = run_text(child);
	}
	text.Trim();
	if (text.IsEmpty())
		return false;
	text << " (link)";
	return true;
}


static void
collapse_spaces(BString& text)
{
	BString result;
	bool space = false;
	for (int32 i = 0; i < text.Length(); i++) {
		char c = text[i];
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			space = true;
			continue;
		}
		if (space && !result.IsEmpty())
			result << ' ';
		space = false;
		result << c;
	}
	text = result;
}


/*!	Whether a node takes its name from its contents, as headings, links,
	buttons, list items and cells do on web pages: its text children only
	repeat it.
*/
static bool
names_from_contents(int32 role)
{
	switch (role) {
		case B_ACCESSIBLE_ROLE_HEADING:
		case B_ACCESSIBLE_ROLE_LINK:
		case B_ACCESSIBLE_ROLE_BUTTON:
		case B_ACCESSIBLE_ROLE_TOGGLE_BUTTON:
		case B_ACCESSIBLE_ROLE_LIST_ITEM:
		case B_ACCESSIBLE_ROLE_TREE_ITEM:
		case B_ACCESSIBLE_ROLE_CELL:
		case B_ACCESSIBLE_ROLE_COLUMN_HEADER:
		case B_ACCESSIBLE_ROLE_MENU_ITEM:
		case B_ACCESSIBLE_ROLE_TAB:
		case B_ACCESSIBLE_ROLE_CHECK_BOX:
		case B_ACCESSIBLE_ROLE_RADIO_BUTTON:
			return true;
		default:
			return false;
	}
}


/*!	What the tree walk collects: the parts read-all says, each with the node
	it came from (a browse item).
*/
struct TextParts {
	std::vector<browse_item>	items;

	void Add(const BString& text, const BMessage& node, int32 role = -1)
	{
		browse_item item;
		item.id = node.GetInt64(B_ACCESSIBLE_FIELD_ID, 0);
		item.role = role >= 0 ? role : node.GetInt32(B_ACCESSIBLE_FIELD_ROLE, 0);
		item.text = text;
		item.inlineLink = false;
		items.push_back(item);
	}

	void AddLinks(const std::vector<browse_item>& links)
	{
		items.insert(items.end(), links.begin(), links.end());
	}
};


static void collect_text(const BMessage& node, TextParts& parts,
	bool inDocument);


/*!	The children of \a node: runs of text and links next to each other are
	one sentence, joined as the page wrote them; anything else is said on
	its own.
*/
static void
collect_children(const BMessage& node, TextParts& parts, bool inDocument)
{
	// links only join running text where there is some
	bool hasText = false;
	BMessage child;
	for (int32 i = 0; inDocument && !hasText
			&& node.FindMessage(B_ACCESSIBLE_FIELD_CHILD, i, &child) == B_OK;
			i++) {
		hasText = is_text_run(child) && !is_invisible(child);
	}

	BString run;
	std::vector<browse_item> links;
		// links in the running text: browse items of their own
	for (int32 i = 0; node.FindMessage(B_ACCESSIBLE_FIELD_CHILD, i, &child)
			== B_OK; i++) {
		if (is_invisible(child))
			continue;
		// (in applications, labels next to each other are separate)
		BString link;
		if (inDocument && is_text_run(child)) {
			run << run_text(child);
			continue;
		}
		if (hasText && is_inline_link(child, link)) {
			if (!run.IsEmpty() && run[run.Length() - 1] != ' ')
				run << ' ';
			run << link << ' ';
			browse_item item;
			item.id = child.GetInt64(B_ACCESSIBLE_FIELD_ID, 0);
			item.role = B_ACCESSIBLE_ROLE_LINK;
			item.text = describe_node(child, false);
			item.inlineLink = true;
			links.push_back(item);
			continue;
		}
		collapse_spaces(run);
		// a label in front of the field it names is said with the field
		if (!run.IsEmpty()
			&& run != BString(child.GetString(B_ACCESSIBLE_FIELD_NAME, ""))) {
			parts.Add(run, node, B_ACCESSIBLE_ROLE_PARAGRAPH);
		}
		parts.AddLinks(links);
		links.clear();
		run = "";
		collect_text(child, parts, inDocument);
	}
	collapse_spaces(run);
	if (!run.IsEmpty())
		parts.Add(run, node, B_ACCESSIBLE_ROLE_PARAGRAPH);
	parts.AddLinks(links);
}


static void
collect_text(const BMessage& node, TextParts& parts, bool inDocument)
{
	if (is_invisible(node))
		return;

	int32 role = node.GetInt32(B_ACCESSIBLE_FIELD_ROLE, 0);
	BString name = node.GetString(B_ACCESSIBLE_FIELD_NAME, "");
	BString value = node.GetString(B_ACCESSIBLE_FIELD_VALUE, "");

	switch (role) {
		case B_ACCESSIBLE_ROLE_SCROLL_BAR:
		case B_ACCESSIBLE_ROLE_SEPARATOR:
			return;

		case B_ACCESSIBLE_ROLE_POP_UP_BUTTON:
			// its menu's items are its children (for agents); the field
			// says its choice
			parts.Add(describe_node(node, false), node);
			return;

		case B_ACCESSIBLE_ROLE_DOCUMENT:
			inDocument = true;
			if (!name.IsEmpty())
				parts.Add(describe_node(node, false), node);
			break;

		case B_ACCESSIBLE_ROLE_LIST:
		case B_ACCESSIBLE_ROLE_TREE:
		case B_ACCESSIBLE_ROLE_TABLE:
		case B_ACCESSIBLE_ROLE_MENU_BAR:
		{
			BString text = describe_node(node, false);
			int32 count = node.GetInt32(B_ACCESSIBLE_FIELD_CHILD_COUNT, 0);
			text << ", " << count << (count == 1 ? " item" : " items");
			parts.Add(text, node);
			// On a web page the items are what there is to read; in an
			// application they can be a whole disk's files.
			if (!inDocument || role == B_ACCESSIBLE_ROLE_MENU_BAR)
				return;
			break;
		}

		case B_ACCESSIBLE_ROLE_UNKNOWN:
		case B_ACCESSIBLE_ROLE_GROUP:
		case B_ACCESSIBLE_ROLE_SCROLL_PANE:
		case B_ACCESSIBLE_ROLE_SPLIT_PANE:
		case B_ACCESSIBLE_ROLE_TAB_PANEL:
		case B_ACCESSIBLE_ROLE_PARAGRAPH:
			if (!name.IsEmpty() || !value.IsEmpty())
				parts.Add(describe_node(node, false), node);
			break;

		case B_ACCESSIBLE_ROLE_LIST_ITEM:
		case B_ACCESSIBLE_ROLE_TREE_ITEM:
		case B_ACCESSIBLE_ROLE_ROW:
		case B_ACCESSIBLE_ROLE_CELL:
			// on a web page, unnamed structure only holds what to read
			if (!inDocument || !name.IsEmpty() || !value.IsEmpty()
				|| node.GetInt64(B_ACCESSIBLE_FIELD_STATES, 0) != 0) {
				parts.Add(describe_node(node, false), node);
			}
			break;

		default:
			parts.Add(describe_node(node, false), node);
			break;
	}

	if (!name.IsEmpty() && names_from_contents(role)) {
		// Said with its name already; only children that add to it count
		TextParts childParts;
		collect_children(node, childParts, inDocument);
		for (size_t i = 0; i < childParts.items.size(); i++) {
			BString trimmed(childParts.items[i].text);
			trimmed.Trim();
			if (childParts.items[i].inlineLink
				|| (!trimmed.IsEmpty() && name.FindFirst(trimmed) < 0))
				parts.items.push_back(childParts.items[i]);
		}
		return;
	}

	collect_children(node, parts, inDocument);
}


/*!	Joins what read-all says: a part that ends a sentence already is not
	given another full stop ("to read.." before).
*/
static BString
join_sentences(const std::vector<browse_item>& parts)
{
	BString text;
	for (size_t i = 0; i < parts.size(); i++) {
		if (parts[i].inlineLink)
			continue;
		BString part = parts[i].text;
		part.Trim();
		if (part.IsEmpty())
			continue;
		if (!text.IsEmpty()) {
			char last = text[text.Length() - 1];
			if (strchr(".!?:;", last) != NULL)
				text << " ";
			else
				text << ". ";
		}
		text << part;
	}
	return text;
}


BString
read_all_text(const BMessage& window)
{
	TextParts parts;
	parts.Add(describe_window(window), window);
	bool inDocument = window.GetInt32(B_ACCESSIBLE_FIELD_ROLE, 0)
		== B_ACCESSIBLE_ROLE_DOCUMENT;
	collect_children(window, parts, inDocument);
	return join_sentences(parts.items);
}


//	#pragma mark - browse mode


void
collect_browse_items(const BMessage& window, std::vector<browse_item>& items)
{
	TextParts parts;
	bool inDocument = window.GetInt32(B_ACCESSIBLE_FIELD_ROLE, 0)
		== B_ACCESSIBLE_ROLE_DOCUMENT;
	collect_children(window, parts, inDocument);
	items.clear();
	for (size_t i = 0; i < parts.items.size(); i++) {
		BString trimmed(parts.items[i].text);
		trimmed.Trim();
		if (!trimmed.IsEmpty())
			items.push_back(parts.items[i]);
	}
}


static bool
is_form_field(int32 role)
{
	switch (role) {
		case B_ACCESSIBLE_ROLE_TEXT_FIELD:
		case B_ACCESSIBLE_ROLE_TEXT_AREA:
		case B_ACCESSIBLE_ROLE_CHECK_BOX:
		case B_ACCESSIBLE_ROLE_RADIO_BUTTON:
		case B_ACCESSIBLE_ROLE_COMBO_BOX:
		case B_ACCESSIBLE_ROLE_POP_UP_BUTTON:
		case B_ACCESSIBLE_ROLE_SLIDER:
		case B_ACCESSIBLE_ROLE_SPIN_BUTTON:
		case B_ACCESSIBLE_ROLE_BUTTON:
		case B_ACCESSIBLE_ROLE_TOGGLE_BUTTON:
			return true;
		default:
			return false;
	}
}


static bool
matches_kind(const browse_item& item, int32 kind)
{
	switch (kind) {
		case BROWSE_ANY:
			return !item.inlineLink;
		case BROWSE_HEADING:
			return item.role == B_ACCESSIBLE_ROLE_HEADING;
		case BROWSE_LINK:
			return item.role == B_ACCESSIBLE_ROLE_LINK;
		case BROWSE_FORM_FIELD:
			return is_form_field(item.role);
		case BROWSE_LIST:
			return item.role == B_ACCESSIBLE_ROLE_LIST
				|| item.role == B_ACCESSIBLE_ROLE_TREE
				|| item.role == B_ACCESSIBLE_ROLE_TABLE;
		case BROWSE_LANDMARK:
			return item.role == B_ACCESSIBLE_ROLE_LANDMARK;
		default:
			return false;
	}
}


int32
find_browse_item(const std::vector<browse_item>& items, int32 from,
	int32 direction, int32 kind)
{
	int32 count = (int32)items.size();
	for (int32 i = from + direction; i >= 0 && i < count; i += direction) {
		if (matches_kind(items[i], kind))
			return i;
	}
	return -1;
}


const char*
browse_kind_name(int32 kind)
{
	switch (kind) {
		case BROWSE_HEADING:
			return "heading";
		case BROWSE_LINK:
			return "link";
		case BROWSE_FORM_FIELD:
			return "form field";
		case BROWSE_LIST:
			return "list";
		case BROWSE_LANDMARK:
			return "landmark";
		default:
			return "item";
	}
}
