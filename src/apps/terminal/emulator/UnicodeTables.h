/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef UNICODE_TABLES_H
#define UNICODE_TABLES_H


#include <SupportDefs.h>


// Character properties the emulator needs, generated from the Unicode
// Character Database by tools/gen_unicode_tables.py.

extern const char* const kUnicodeVersion;
extern const uint16 kUnicodeStage1[];
extern const uint16 kUnicodeStage2[][256];


// Grapheme_Cluster_Break values (UAX #29), in the generator's order.
enum {
	GRAPHEME_OTHER = 0,
	GRAPHEME_CR,
	GRAPHEME_LF,
	GRAPHEME_CONTROL,
	GRAPHEME_EXTEND,
	GRAPHEME_ZWJ,
	GRAPHEME_REGIONAL_INDICATOR,
	GRAPHEME_PREPEND,
	GRAPHEME_SPACING_MARK,
	GRAPHEME_L,
	GRAPHEME_V,
	GRAPHEME_T,
	GRAPHEME_LV,
	GRAPHEME_LVT
};

// Indic_Conjunct_Break values (InCB)
enum {
	INCB_NONE = 0,
	INCB_LINKER,
	INCB_CONSONANT,
	INCB_EXTEND
};


static inline uint16
unicode_properties(uint32 c)
{
	if (c >= 0x110000)
		return 1;
	return kUnicodeStage2[kUnicodeStage1[c >> 8]][c & 0xff];
}


//! Number of cells a code point takes on its own: 0, 1 or 2.
static inline int
unicode_width(uint32 c)
{
	if (c < 0x7f)
		return c >= 0x20 ? 1 : 0;
	return unicode_properties(c) & 0x3;
}


static inline int
unicode_grapheme_break(uint32 c)
{
	return (unicode_properties(c) >> 2) & 0xf;
}


static inline bool
unicode_is_extended_pictographic(uint32 c)
{
	return (unicode_properties(c) & 0x40) != 0;
}


static inline bool
unicode_has_emoji_presentation(uint32 c)
{
	return (unicode_properties(c) & 0x80) != 0;
}


static inline int
unicode_indic_conjunct_break(uint32 c)
{
	return (unicode_properties(c) >> 8) & 0x3;
}


#endif	// UNICODE_TABLES_H
