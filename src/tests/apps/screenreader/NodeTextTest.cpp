/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	What ScreenReader says for hand-made accessibility trees (#119): read
	all on a web page and in an application window, landmarks, state
	changes. Prints "PASS name" or "FAIL name: what came out".
*/


#include <stdio.h>
#include <string.h>

#include <Accessible.h>
#include <AccessibilityProtocol.h>

#include "NodeText.h"


static int sFailures = 0;


static BMessage
make_node(int32 role, const char* name = NULL, const char* value = NULL,
	uint64 states = 0)
{
	BMessage node;
	node.AddInt32(B_ACCESSIBLE_FIELD_ROLE, role);
	if (name != NULL)
		node.AddString(B_ACCESSIBLE_FIELD_NAME, name);
	if (value != NULL)
		node.AddString(B_ACCESSIBLE_FIELD_VALUE, value);
	if (states != 0)
		node.AddInt64(B_ACCESSIBLE_FIELD_STATES, (int64)states);
	return node;
}


static void
add_child(BMessage& parent, const BMessage& child)
{
	parent.AddMessage(B_ACCESSIBLE_FIELD_CHILD, &child);
	parent.RemoveName(B_ACCESSIBLE_FIELD_CHILD_COUNT);
	type_code type;
	int32 count = 0;
	parent.GetInfo(B_ACCESSIBLE_FIELD_CHILD, &type, &count);
	parent.AddInt32(B_ACCESSIBLE_FIELD_CHILD_COUNT, count);
}


static BMessage
text(const char* run)
{
	return make_node(B_ACCESSIBLE_ROLE_LABEL, run);
}


static BMessage
named_by_text(int32 role, const char* name)
{
	BMessage node = make_node(role, name);
	add_child(node, text(name));
	return node;
}


static void
expect(const char* name, const BString& got, const char* expected)
{
	if (got == expected) {
		printf("PASS %s\n", name);
		return;
	}
	sFailures++;
	printf("FAIL %s:\n  got:      \"%s\"\n  expected: \"%s\"\n", name,
		got.String(), expected);
}


static void
test_web_page()
{
	BMessage window = make_node(B_ACCESSIBLE_ROLE_WINDOW,
		"Summit screen reader test - Summit");
	BMessage document = make_node(B_ACCESSIBLE_ROLE_DOCUMENT,
		"Summit screen reader test");

	BMessage heading = named_by_text(B_ACCESSIBLE_ROLE_HEADING,
		"Summit screen reader test");
	heading.AddInt32(B_ACCESSIBLE_FIELD_LEVEL, 1);
	add_child(document, heading);

	BMessage paragraph = make_node(B_ACCESSIBLE_ROLE_PARAGRAPH);
	add_child(paragraph, text("This paragraph has "));
	add_child(paragraph, named_by_text(B_ACCESSIBLE_ROLE_LINK, "some text"));
	add_child(paragraph, text(" for the screen reader to read."));
	add_child(document, paragraph);

	BMessage navigation = make_node(B_ACCESSIBLE_ROLE_LANDMARK, "Main");
	navigation.AddString("landmark", "Navigation");
	BMessage list = make_node(B_ACCESSIBLE_ROLE_LIST);
	const char* items[] = { "Home", "About" };
	for (int i = 0; i < 2; i++) {
		BMessage item = make_node(B_ACCESSIBLE_ROLE_LIST_ITEM);
		BMessage link = named_by_text(B_ACCESSIBLE_ROLE_LINK, items[i]);
		add_child(item, link);
		add_child(list, item);
	}
	add_child(navigation, list);
	add_child(document, navigation);

	BMessage form = make_node(B_ACCESSIBLE_ROLE_FORM);
	add_child(form, text("Your name"));
	add_child(form, make_node(B_ACCESSIBLE_ROLE_TEXT_FIELD, "Your name",
		"Ada", B_ACCESSIBLE_STATE_EDITABLE | B_ACCESSIBLE_STATE_FOCUSABLE));
	add_child(document, form);

	add_child(document, named_by_text(B_ACCESSIBLE_ROLE_BUTTON, "Send"));
	add_child(window, document);

	expect("web-page", read_all_text(window),
		"Summit screen reader test - Summit, window. "
		"Summit screen reader test, document. "
		"Summit screen reader test, heading. "
		"This paragraph has some text (link) for the screen reader to read. "
		"navigation landmark, Main. list, 2 items. Home, link. About, link. "
		"form. Your name, text field, Ada. Send, button");
}


static void
test_application_window()
{
	// labels next to each other stay separate; lists are only counted
	BMessage window = make_node(B_ACCESSIBLE_ROLE_WINDOW, "Get info");
	BMessage group = make_node(B_ACCESSIBLE_ROLE_GROUP);
	add_child(group, text("Name"));
	add_child(group, text("Size"));
	BMessage list = make_node(B_ACCESSIBLE_ROLE_LIST, "Files");
	add_child(list, make_node(B_ACCESSIBLE_ROLE_LIST_ITEM, "a"));
	add_child(list, make_node(B_ACCESSIBLE_ROLE_LIST_ITEM, "b"));
	add_child(group, list);
	add_child(group, make_node(B_ACCESSIBLE_ROLE_BUTTON, "OK"));
	add_child(window, group);

	expect("application-window", read_all_text(window),
		"Get info, window. Name. Size. Files, list, 2 items. OK, button");
}


static void
test_landmarks()
{
	BMessage node = make_node(B_ACCESSIBLE_ROLE_LANDMARK);
	node.AddString("landmark", "ContentInfo");
	expect("landmark-unnamed", describe_node(node), "content information landmark");

	BMessage search = make_node(B_ACCESSIBLE_ROLE_LANDMARK, "Site");
	search.AddString("landmark", "Search");
	expect("landmark-named", describe_node(search), "search landmark, Site");
}


static void
test_state_changes()
{
	BMessage button = make_node(B_ACCESSIBLE_ROLE_BUTTON, "Menu", NULL,
		B_ACCESSIBLE_STATE_EXPANDABLE | B_ACCESSIBLE_STATE_EXPANDED);
	expect("state-expanded",
		describe_state_change(button, B_ACCESSIBLE_STATE_EXPANDED), "expanded");

	BMessage collapsed = make_node(B_ACCESSIBLE_ROLE_BUTTON, "Menu", NULL,
		B_ACCESSIBLE_STATE_EXPANDABLE);
	expect("state-collapsed",
		describe_state_change(collapsed, B_ACCESSIBLE_STATE_EXPANDED),
		"collapsed");

	BMessage tab = make_node(B_ACCESSIBLE_ROLE_TAB, "General", NULL,
		B_ACCESSIBLE_STATE_SELECTED);
	expect("state-selected",
		describe_state_change(tab, B_ACCESSIBLE_STATE_SELECTED), "selected");

	BMessage control = make_node(B_ACCESSIBLE_ROLE_BUTTON, "OK", NULL,
		B_ACCESSIBLE_STATE_DISABLED);
	expect("state-disabled",
		describe_state_change(control, B_ACCESSIBLE_STATE_DISABLED),
		"unavailable");

	BMessage toggle = make_node(B_ACCESSIBLE_ROLE_TOGGLE_BUTTON, "Bold", NULL,
		B_ACCESSIBLE_STATE_PRESSED);
	expect("state-two",
		describe_state_change(toggle,
			B_ACCESSIBLE_STATE_PRESSED | B_ACCESSIBLE_STATE_DISABLED),
		"pressed, available");
}


int
main()
{
	test_web_page();
	test_application_window();
	test_landmarks();
	test_state_changes();
	printf("NodeTextTest: %d failure(s)\n", sFailures);
	return sFailures != 0;
}
