/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TRACKER_TAGS_H
#define _TRACKER_TAGS_H


#include <Entry.h>
#include <GraphicsDefs.h>
#include <ObjectList.h>
#include <Rect.h>
#include <String.h>
#include <StringList.h>


class BBitmap;
class BHandler;
class BMenu;
class BView;


namespace BPrivate {


// Colored tags on files: the names are kept in a string attribute
// ("Red,Work"), indexed on BFS volumes so a live query finds every tagged
// file. The list of tags and their colors is per user.

#define kAttrTags "META:tags"

const uint32 kToggleTag = 'Ttag';
	// "tag": the tag to add to or remove from the selected items
const uint32 kFindTagged = 'Tftg';
	// "tag": open a query for the items with it


struct TagInfo {
	BString		name;
	rgb_color	color;
};


// The user's tags, in menu order
const BObjectList<TagInfo, true>& TrackerTags();
rgb_color TagColor(const char* name);

void ParseTags(const char* value, BStringList& tags);
BString JoinTags(const BStringList& tags);

// Adds or removes a tag on an item; creates the index on the volume first
status_t SetTag(const entry_ref& ref, const char* tag, bool on);

// Dots of dotSize for tags, overlapping a little, from left on, centered
// on centerY
void DrawTagDots(BView* view, float left, float centerY,
	const BStringList& tags, float dotSize);
float TagDotsWidth(const BStringList& tags, float dotSize);

// A colored dot for a menu item
BBitmap* TagIcon(rgb_color color, float size);

// Fills a menu with the tags, marked when every one of refs has the tag
void BuildTagsMenu(BMenu* menu, const BObjectList<entry_ref, true>& refs,
	BHandler* target);

// Opens a live query for the items with a tag
void FindTaggedItems(const char* tag);


} // namespace BPrivate

using namespace BPrivate;


#endif	// _TRACKER_TAGS_H
