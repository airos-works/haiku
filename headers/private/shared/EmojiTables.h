/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef EMOJI_TABLES_H
#define EMOJI_TABLES_H


#include <SupportDefs.h>


// Emoji properties from the Unicode emoji data (UTS #51), generated into
// EmojiTables.cpp by tools/gen_emoji_tables.py.

struct emoji_range {
	uint32	first;
	uint32	last;
};

extern const emoji_range kEmojiPresentationRanges[];
extern const int32 kEmojiPresentationRangeCount;
extern const emoji_range kExtendedPictographicRanges[];
extern const int32 kExtendedPictographicRangeCount;


static const uint32 kVariationSelectorText = 0xfe0e;
static const uint32 kVariationSelectorEmoji = 0xfe0f;
static const uint32 kZeroWidthJoiner = 0x200d;
static const uint32 kCombiningKeycap = 0x20e3;


static inline bool
emoji_in_ranges(uint32 c, const emoji_range* ranges, int32 count)
{
	int32 low = 0;
	int32 high = count - 1;
	while (low <= high) {
		int32 middle = (low + high) / 2;
		if (c < ranges[middle].first)
			high = middle - 1;
		else if (c > ranges[middle].last)
			low = middle + 1;
		else
			return true;
	}
	return false;
}


//! Shown as an emoji even without a variation selector (Emoji_Presentation).
static inline bool
emoji_has_emoji_presentation(uint32 c)
{
	return c >= 0x231a && emoji_in_ranges(c, kEmojiPresentationRanges,
		kEmojiPresentationRangeCount);
}


static inline bool
emoji_is_extended_pictographic(uint32 c)
{
	return c >= 0xa9 && emoji_in_ranges(c, kExtendedPictographicRanges,
		kExtendedPictographicRangeCount);
}


//! The skin tones (Emoji_Modifier).
static inline bool
emoji_is_modifier(uint32 c)
{
	return c >= 0x1f3fb && c <= 0x1f3ff;
}


static inline bool
emoji_is_regional_indicator(uint32 c)
{
	return c >= 0x1f1e6 && c <= 0x1f1ff;
}


//! The tags of a tag sequence (flags of subdivisions), and its cancel tag.
static inline bool
emoji_is_tag(uint32 c)
{
	return c >= 0xe0020 && c <= 0xe007f;
}


static inline bool
emoji_is_keycap_base(uint32 c)
{
	return (c >= '0' && c <= '9') || c == '#' || c == '*';
}


//! Whether \a c can begin an emoji presentation sequence or cluster.
static inline bool
emoji_can_start_cluster(uint32 c)
{
	return emoji_is_keycap_base(c) || emoji_is_regional_indicator(c)
		|| emoji_is_extended_pictographic(c) || emoji_is_modifier(c);
}


/*!	Returns how many of \a codes form the emoji cluster that \a codes[0]
	begins: a flag (two regional indicators), a keycap, or an emoji with
	variation selectors, skin tones, tags and further emoji joined by zero
	width joiners. That is 1 for anything else.
*/
static inline int32
emoji_cluster_length(const uint32* codes, int32 count)
{
	if (count <= 1)
		return count;

	uint32 first = codes[0];
	if (emoji_is_regional_indicator(first))
		return emoji_is_regional_indicator(codes[1]) ? 2 : 1;

	if (emoji_is_keycap_base(first)) {
		if (codes[1] == kCombiningKeycap)
			return 2;
		if (codes[1] == kVariationSelectorEmoji && count > 2
			&& codes[2] == kCombiningKeycap) {
			return 3;
		}
		return 1;
	}

	if (!emoji_is_extended_pictographic(first) && !emoji_is_modifier(first))
		return 1;

	int32 length = 1;
	while (length < count) {
		uint32 code = codes[length];
		if (code == kVariationSelectorEmoji || code == kVariationSelectorText
			|| code == kCombiningKeycap || emoji_is_modifier(code)
			|| emoji_is_tag(code)) {
			length++;
		} else if (code == kZeroWidthJoiner && length + 1 < count
			&& emoji_is_extended_pictographic(codes[length + 1])) {
			length += 2;
		} else
			break;
	}
	return length;
}


#endif	// EMOJI_TABLES_H
