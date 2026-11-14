/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef NODE_TEXT_H
#define NODE_TEXT_H


#include <vector>

#include <Message.h>
#include <String.h>


// What ScreenReader says for accessibility nodes, as BAccessible's bridge
// sends them (fields B_ACCESSIBLE_FIELD_*). Free of the application, so that
// a test can feed it trees.

const char*	spoken_role(int32 role);
BString		spoken_text(const BString& text);
BString		describe_node(const BMessage& node, bool withDescription = true);
BString		describe_window(const BMessage& window);
BString		describe_state_change(const BMessage& node, uint64 changed);
BString		read_all_text(const BMessage& window);


// browse mode: what read-all says, item by item, with the node of each
struct browse_item {
	int64		id;
	int32		role;
	BString		text;
	bool		inlineLink;
		// a link inside running text: its words are in the sentence before;
		// found by "next link", skipped by "next item"
};

enum {
	BROWSE_ANY = 0,
	BROWSE_HEADING,
	BROWSE_LINK,
	BROWSE_FORM_FIELD,
	BROWSE_LIST,
	BROWSE_LANDMARK
};

void		collect_browse_items(const BMessage& window,
				std::vector<browse_item>& items);
int32		find_browse_item(const std::vector<browse_item>& items,
				int32 from, int32 direction, int32 kind);
				// the next item of that kind after (direction 1) or before
				// (-1) index from; -1 if there is none
const char*	browse_kind_name(int32 kind);


#endif	// NODE_TEXT_H
