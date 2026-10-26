/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "OpenTypeLigatures.h"

#include <stdlib.h>
#include <string.h>

#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H


static const int32 kMaxLookups = 64;
static const uint16 kLookupTypeLigature = 4;
static const uint16 kLookupTypeExtension = 7;


static int
compare_uint16(const void* a, const void* b)
{
	return int(*(const uint16*)a) - int(*(const uint16*)b);
}


OpenTypeLigatures::OpenTypeLigatures()
	:
	fTable(NULL),
	fTableSize(0),
	fLookups(NULL),
	fLookupCount(0),
	fSubtables(NULL),
	fSubtableCount(0),
	fSubtableCapacity(0)
{
}


OpenTypeLigatures::~OpenTypeLigatures()
{
	free(fTable);
	free(fLookups);
	free(fSubtables);
}


bool
OpenTypeLigatures::Init(FT_Face face)
{
	FT_ULong length = 0;
	if (FT_Load_Sfnt_Table(face, TTAG_GSUB, 0, NULL, &length) != 0
		|| length < 10 || length > 16 * 1024 * 1024) {
		return false;
	}

	fTable = (uint8*)malloc(length);
	if (fTable == NULL)
		return false;
	if (FT_Load_Sfnt_Table(face, TTAG_GSUB, 0, fTable, &length) != 0)
		return false;
	fTableSize = length;

	uint16 major, featureListOffset, lookupListOffset;
	if (!_U16(0, major) || major != 1 || !_U16(6, featureListOffset)
		|| !_U16(8, lookupListOffset)) {
		return false;
	}

	// Collect the lookups of the features that make ligatures by default
	uint16 indices[kMaxLookups];
	int32 indexCount = 0;
	uint16 featureCount;
	if (featureListOffset == 0 || !_U16(featureListOffset, featureCount))
		return false;
	for (uint32 i = 0; i < featureCount; i++) {
		uint32 record = featureListOffset + 2 + i * 6;
		uint32 tag;
		uint16 featureOffset;
		if (!_U32(record, tag) || !_U16(record + 4, featureOffset))
			return false;
		if (tag != FT_MAKE_TAG('c', 'c', 'm', 'p')
			&& tag != FT_MAKE_TAG('l', 'i', 'g', 'a')
			&& tag != FT_MAKE_TAG('c', 'l', 'i', 'g')
			&& tag != FT_MAKE_TAG('r', 'l', 'i', 'g')) {
			continue;
		}

		uint32 feature = featureListOffset + featureOffset;
		uint16 lookupCount;
		if (!_U16(feature + 2, lookupCount))
			return false;
		for (uint32 j = 0; j < lookupCount; j++) {
			uint16 index;
			if (!_U16(feature + 4 + j * 2, index))
				return false;
			bool known = false;
			for (int32 k = 0; k < indexCount; k++)
				known |= indices[k] == index;
			if (!known && indexCount < kMaxLookups)
				indices[indexCount++] = index;
		}
	}

	// Lookups apply in the order of the lookup list, not of the features
	qsort(indices, indexCount, sizeof(uint16), compare_uint16);
	return _AddLookups(lookupListOffset, indices, indexCount);
}


int32
OpenTypeLigatures::Apply(uint32* glyphs, int32 count) const
{
	for (int32 i = 0; i < fLookupCount; i++) {
		const Lookup& lookup = fLookups[i];
		for (int32 position = 0; position < count; position++) {
			for (int32 j = 0; j < lookup.subtableCount; j++) {
				int32 newCount = _ApplySubtable(
					fSubtables[lookup.firstSubtable + j], glyphs, count,
					position);
				if (newCount >= 0) {
					count = newCount;
					break;
				}
			}
		}
	}
	return count;
}


bool
OpenTypeLigatures::_U16(uint32 offset, uint16& value) const
{
	if (offset > fTableSize || fTableSize - offset < 2)
		return false;
	value = (fTable[offset] << 8) | fTable[offset + 1];
	return true;
}


bool
OpenTypeLigatures::_U32(uint32 offset, uint32& value) const
{
	uint16 high, low;
	if (!_U16(offset, high) || !_U16(offset + 2, low))
		return false;
	value = (uint32(high) << 16) | low;
	return true;
}


bool
OpenTypeLigatures::_AddLookups(uint32 lookupList, const uint16* indices,
	int32 count)
{
	uint16 lookupCount;
	if (!_U16(lookupList, lookupCount))
		return false;

	fLookups = (Lookup*)malloc(sizeof(Lookup) * (count > 0 ? count : 1));
	if (fLookups == NULL)
		return false;

	for (int32 i = 0; i < count; i++) {
		if (indices[i] >= lookupCount)
			continue;
		uint16 lookupOffset;
		if (!_U16(lookupList + 2 + indices[i] * 2, lookupOffset))
			return false;

		uint32 lookup = lookupList + lookupOffset;
		uint16 type, subtableCount;
		if (!_U16(lookup, type) || !_U16(lookup + 4, subtableCount))
			return false;
		if (type != kLookupTypeLigature && type != kLookupTypeExtension)
			continue;

		Lookup& added = fLookups[fLookupCount];
		added.firstSubtable = fSubtableCount;
		added.subtableCount = 0;
		for (uint32 j = 0; j < subtableCount; j++) {
			uint16 subtableOffset;
			if (!_U16(lookup + 6 + j * 2, subtableOffset))
				return false;
			uint32 subtable = lookup + subtableOffset;

			if (type == kLookupTypeExtension) {
				uint16 format, extensionType;
				uint32 extensionOffset;
				if (!_U16(subtable, format) || format != 1
					|| !_U16(subtable + 2, extensionType)
					|| !_U32(subtable + 4, extensionOffset)) {
					return false;
				}
				if (extensionType != kLookupTypeLigature)
					continue;
				if (extensionOffset > fTableSize - subtable)
					return false;
				subtable += extensionOffset;
			}

			if (!_AddSubtable(subtable))
				return false;
			added.subtableCount++;
		}
		if (added.subtableCount > 0)
			fLookupCount++;
	}

	return true;
}


bool
OpenTypeLigatures::_AddSubtable(uint32 subtable)
{
	uint16 format;
	if (!_U16(subtable, format) || format != 1)
		return true;

	if (fSubtableCount == fSubtableCapacity) {
		int32 capacity = fSubtableCapacity > 0 ? fSubtableCapacity * 2 : 8;
		uint32* subtables = (uint32*)realloc(fSubtables,
			sizeof(uint32) * capacity);
		if (subtables == NULL)
			return false;
		fSubtables = subtables;
		fSubtableCapacity = capacity;
	}
	fSubtables[fSubtableCount++] = subtable;
	return true;
}


int32
OpenTypeLigatures::_CoverageIndex(uint32 coverage, uint32 glyph) const
{
	uint16 format, count;
	if (!_U16(coverage, format) || !_U16(coverage + 2, count))
		return -1;

	int32 low = 0;
	int32 high = int32(count) - 1;
	if (format == 1) {
		while (low <= high) {
			int32 middle = (low + high) / 2;
			uint16 value;
			if (!_U16(coverage + 4 + middle * 2, value))
				return -1;
			if (glyph < value)
				high = middle - 1;
			else if (glyph > value)
				low = middle + 1;
			else
				return middle;
		}
	} else if (format == 2) {
		while (low <= high) {
			int32 middle = (low + high) / 2;
			uint32 range = coverage + 4 + middle * 6;
			uint16 start, end, startIndex;
			if (!_U16(range, start) || !_U16(range + 2, end)
				|| !_U16(range + 4, startIndex)) {
				return -1;
			}
			if (glyph < start)
				high = middle - 1;
			else if (glyph > end)
				low = middle + 1;
			else
				return startIndex + glyph - start;
		}
	}
	return -1;
}


/*!	Applies the ligature subtable at \a position. Returns the new glyph
	count, or -1 when the subtable has no ligature for the glyphs there.
*/
int32
OpenTypeLigatures::_ApplySubtable(uint32 subtable, uint32* glyphs,
	int32 count, int32 position) const
{
	uint16 coverageOffset, setCount;
	if (!_U16(subtable + 2, coverageOffset) || !_U16(subtable + 4, setCount))
		return -1;

	int32 coverageIndex = _CoverageIndex(subtable + coverageOffset,
		glyphs[position]);
	if (coverageIndex < 0 || coverageIndex >= setCount)
		return -1;

	uint16 setOffset, ligatureCount;
	if (!_U16(subtable + 6 + coverageIndex * 2, setOffset))
		return -1;
	uint32 set = subtable + setOffset;
	if (!_U16(set, ligatureCount))
		return -1;

	// The ligatures are in order of preference
	for (uint32 i = 0; i < ligatureCount; i++) {
		uint16 ligatureOffset, ligatureGlyph, componentCount;
		if (!_U16(set + 2 + i * 2, ligatureOffset))
			return -1;
		uint32 ligature = set + ligatureOffset;
		if (!_U16(ligature, ligatureGlyph)
			|| !_U16(ligature + 2, componentCount)) {
			return -1;
		}
		if (componentCount == 0 || componentCount > count - position)
			continue;

		bool matches = true;
		for (uint32 j = 1; j < componentCount && matches; j++) {
			uint16 component;
			if (!_U16(ligature + 4 + (j - 1) * 2, component))
				return -1;
			matches = glyphs[position + j] == component;
		}
		if (!matches)
			continue;

		glyphs[position] = ligatureGlyph;
		memmove(glyphs + position + 1, glyphs + position + componentCount,
			sizeof(uint32) * (count - position - componentCount));
		return count - componentCount + 1;
	}
	return -1;
}
