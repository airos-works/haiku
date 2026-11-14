/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef NODE_TEXT_H
#define NODE_TEXT_H


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


#endif	// NODE_TEXT_H
