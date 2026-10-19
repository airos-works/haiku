/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "TerminalHistory.h"

#include <stdlib.h>

#include <new>


// The second cell of a double width character, in an entry's character
// array; its style flags are not kept in the runs.
static const uint32 kWideTailMarker = 0xfffffffe;


struct TerminalHistory::Entry {
	uint64		version;
	uint16		flags;
	uint16		length;
	uint16		runCount;
	uint16		reserved;

	struct Run {
		uint16		start;
		uint16		reserved;
		uint32		hyperlink;
		CellStyle	style;
	};

	uint32* Characters()
	{
		return (uint32*)(this + 1);
	}

	const uint32* Characters() const
	{
		return (const uint32*)(this + 1);
	}

	Run* Runs()
	{
		return (Run*)(Characters() + ((length + 1) & ~1));
	}

	const Run* Runs() const
	{
		return (const Run*)(Characters() + ((length + 1) & ~1));
	}

	static size_t Size(int32 length, int32 runCount)
	{
		return sizeof(Entry) + sizeof(uint32) * ((length + 1) & ~1)
			+ sizeof(Run) * runCount;
	}
};


static inline bool
is_trailing_blank(const TerminalCell& cell)
{
	return cell.character == 0 && cell.hyperlink == 0
		&& cell.style == CellStyle();
}


static inline CellStyle
run_style(const CellStyle& style)
{
	CellStyle runStyle = style;
	runStyle.flags &= ~(CELL_WIDE | CELL_WIDE_TAIL);
	return runStyle;
}


TerminalHistory::TerminalHistory()
	:
	fEntries(NULL),
	fCapacity(0),
	fSize(0),
	fNewest(-1)
{
}


TerminalHistory::~TerminalHistory()
{
	Clear();
	free(fEntries);
}


status_t
TerminalHistory::SetCapacity(int32 capacity)
{
	if (capacity < 0)
		capacity = 0;
	if (capacity == fCapacity)
		return B_OK;

	Entry** entries = NULL;
	if (capacity > 0) {
		entries = (Entry**)calloc(capacity, sizeof(Entry*));
		if (entries == NULL)
			return B_NO_MEMORY;
	}

	// keep the newest lines, oldest first
	int32 keep = fSize < capacity ? fSize : capacity;
	for (int32 i = keep - 1, slot = 0; i >= 0; i--, slot++) {
		Entry*& entry = _EntryAt(i);
		entries[slot] = entry;
		entry = NULL;
	}
	DropOldest(fSize);

	free(fEntries);
	fEntries = entries;
	fCapacity = capacity;
	fSize = keep;
	fNewest = keep - 1;
	return B_OK;
}


void
TerminalHistory::Clear()
{
	DropOldest(fSize);
	fNewest = -1;
}


bool
TerminalHistory::AddLine(const TerminalLine& line, int32 width)
{
	if (fCapacity == 0)
		return true;

	Entry* entry = _Encode(line, width);
	if (entry == NULL)
		return false;

	bool dropped = false;
	fNewest = (fNewest + 1) % fCapacity;
	if (fSize == fCapacity) {
		free(fEntries[fNewest]);
		dropped = true;
	} else
		fSize++;
	fEntries[fNewest] = entry;
	return dropped;
}


bool
TerminalHistory::GetLine(int32 index, TerminalCell* cells, int32 width,
	uint16* _flags, uint64* _version) const
{
	if (index < 0 || index >= fSize)
		return false;

	const Entry* entry = _EntryAt(index);
	_Decode(entry, cells, width);
	if (_flags != NULL)
		*_flags = entry->flags;
	if (_version != NULL)
		*_version = entry->version;
	return true;
}


int32
TerminalHistory::LineLength(int32 index) const
{
	if (index < 0 || index >= fSize)
		return 0;
	return _EntryAt(index)->length;
}


uint16
TerminalHistory::LineFlags(int32 index) const
{
	if (index < 0 || index >= fSize)
		return 0;
	return _EntryAt(index)->flags;
}


void
TerminalHistory::SetLineFlags(int32 index, uint16 flags)
{
	if (index >= 0 && index < fSize)
		_EntryAt(index)->flags = flags;
}


bool
TerminalHistory::RemoveLastLine(TerminalCell* cells, int32 width,
	uint16* _flags, uint64* _version)
{
	if (fSize == 0)
		return false;

	Entry*& entry = _EntryAt(0);
	if (cells != NULL)
		_Decode(entry, cells, width);
	if (_flags != NULL)
		*_flags = entry->flags;
	if (_version != NULL)
		*_version = entry->version;
	free(entry);
	entry = NULL;

	fSize--;
	fNewest = fSize == 0 ? -1 : (fNewest - 1 + fCapacity) % fCapacity;
	return true;
}


void
TerminalHistory::DropOldest(int32 count)
{
	if (count > fSize)
		count = fSize;
	for (int32 i = 0; i < count; i++) {
		Entry*& entry = _EntryAt(fSize - 1);
		free(entry);
		entry = NULL;
		fSize--;
	}
	if (fSize == 0)
		fNewest = -1;
}


TerminalHistory::Entry*
TerminalHistory::_Encode(const TerminalLine& line, int32 width) const
{
	const TerminalCell* cells = line.cells;

	int32 length = width;
	while (length > 0 && is_trailing_blank(cells[length - 1]))
		length--;

	int32 runCount = 0;
	const TerminalCell* previous = NULL;
	for (int32 i = 0; i < length; i++) {
		if (cells[i].IsWideTail())
			continue;
		if (previous == NULL
			|| run_style(cells[i].style) != run_style(previous->style)
			|| cells[i].hyperlink != previous->hyperlink) {
			runCount++;
		}
		previous = &cells[i];
	}

	Entry* entry = (Entry*)malloc(Entry::Size(length, runCount));
	if (entry == NULL)
		return NULL;

	entry->version = line.version;
	entry->flags = line.flags;
	entry->length = length;
	entry->runCount = 0;
	entry->reserved = 0;

	uint32* characters = entry->Characters();
	Entry::Run* runs = entry->Runs();
	const Entry::Run* lastRun = NULL;
	for (int32 i = 0; i < length; i++) {
		const TerminalCell& cell = cells[i];
		if (cell.IsWideTail()) {
			characters[i] = kWideTailMarker;
			continue;
		}
		characters[i] = cell.character;

		CellStyle style = run_style(cell.style);
		if (lastRun == NULL || lastRun->style != style
			|| lastRun->hyperlink != cell.hyperlink) {
			Entry::Run& run = runs[entry->runCount++];
			run.start = i;
			run.reserved = 0;
			run.hyperlink = cell.hyperlink;
			run.style = style;
			lastRun = &run;
		}
	}

	return entry;
}


void
TerminalHistory::_Decode(const Entry* entry, TerminalCell* cells,
	int32 width) const
{
	const uint32* characters = entry->Characters();
	const Entry::Run* runs = entry->Runs();
	int32 length = entry->length < width ? entry->length : width;

	int32 run = -1;
	for (int32 i = 0; i < length; i++) {
		while (run + 1 < entry->runCount && runs[run + 1].start <= i)
			run++;

		TerminalCell& cell = cells[i];
		if (run >= 0) {
			cell.style = runs[run].style;
			cell.hyperlink = runs[run].hyperlink;
		} else {
			cell.style = CellStyle();
			cell.hyperlink = 0;
		}

		if (characters[i] == kWideTailMarker) {
			cell.character = 0;
			cell.style.flags |= CELL_WIDE_TAIL;
			if (i > 0)
				cells[i - 1].style.flags |= CELL_WIDE;
		} else
			cell.character = characters[i];
	}

	// a double width character cut by a narrower width
	if (length > 0 && length < entry->length
		&& characters[length] == kWideTailMarker) {
		cells[length - 1].character = 0;
		cells[length - 1].style.flags &= ~CELL_WIDE;
	}

	for (int32 i = length; i < width; i++) {
		cells[i].character = 0;
		cells[i].hyperlink = 0;
		cells[i].style = CellStyle();
	}
}


TerminalHistory::Entry*&
TerminalHistory::_EntryAt(int32 index) const
{
	return fEntries[(fNewest - index + fCapacity) % fCapacity];
}
