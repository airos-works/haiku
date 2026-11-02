/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _ACCESSIBILITY_PRIVATE_H
#define _ACCESSIBILITY_PRIVATE_H


/*!	Hooks the Interface Kit calls to feed the accessibility bridge. Every
	call site tests gAccessibilityEnabled first, so nothing happens (and no
	message is sent) while no assistive technology is running.
*/


#include <Accessible.h>


class BListView;
class BMenu;
class BMenuItem;
class BMessage;
class BTabView;
class BTextView;
class BView;
class BWindow;


namespace BPrivate {


extern bool gAccessibilityEnabled;
	// the accessibility_server has a client; events are wanted
extern bool gAccessibilityViewData;
	// some view has a registered ID, custom name or attached tree


// app kit
void accessibility_enable(BMessage* message);
void accessibility_disable();

// interface kit
void accessibility_view_deleted(BView* view);
void accessibility_notify(BView* view, accessible_event event,
	const BMessage* details = NULL);
void accessibility_focus_changed(BWindow* window, BView* focus);
void accessibility_window_activated(BWindow* window, bool active);
void accessibility_text_inserted(BTextView* view, const char* text,
	int32 length, int32 offset);
void accessibility_text_deleted(BTextView* view, int32 fromOffset,
	int32 toOffset);
void accessibility_caret_moved(BTextView* view);
void accessibility_item_selected(BView* view, int32 index);
	// list, outline list, column list, tab view
void accessibility_menu_item_selected(BMenu* menu, BMenuItem* item);


}	// namespace BPrivate


#define ACCESSIBILITY_ENABLED() \
	__builtin_expect(BPrivate::gAccessibilityEnabled, 0)


#endif	// _ACCESSIBILITY_PRIVATE_H
