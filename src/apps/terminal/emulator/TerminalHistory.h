/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef TERMINAL_HISTORY_H
#define TERMINAL_HISTORY_H


#include "TerminalCell.h"


/*!	The scrollback: lines that scrolled off the top of the main screen,
	kept compactly (trailing empty cells dropped, styles as runs) in a ring
	of at most Capacity() lines. Index 0 is the most recent line.
*/
class TerminalHistory {
public:
								TerminalHistory();
								~TerminalHistory();

			status_t			SetCapacity(int32 capacity);
			int32				Capacity() const	{ return fCapacity; }
			int32				Size() const		{ return fSize; }

			void				Clear();

			//! Adds \a line as the most recent one; drops the oldest line
			//! when the history is full and returns whether it did.
			bool				AddLine(const TerminalLine& line, int32 width);

			//! Copies line \a index (0 = most recent) into \a cells, which
			//! has room for \a width cells.
			bool				GetLine(int32 index, TerminalCell* cells,
									int32 width, uint16* _flags,
									uint64* _version) const;

			//! The number of cells line \a index has (without trailing empty
			//! cells).
			int32				LineLength(int32 index) const;
			uint16				LineFlags(int32 index) const;
			void				SetLineFlags(int32 index, uint16 flags);

			//! Removes the most recent line, copying it into \a cells.
			bool				RemoveLastLine(TerminalCell* cells,
									int32 width, uint16* _flags,
									uint64* _version);

			//! Removes the oldest \a count lines.
			void				DropOldest(int32 count);

private:
			struct Entry;

			Entry*				_Encode(const TerminalLine& line,
									int32 width) const;
			void				_Decode(const Entry* entry,
									TerminalCell* cells, int32 width) const;
			Entry*&				_EntryAt(int32 index) const;

private:
			Entry**				fEntries;
			int32				fCapacity;
			int32				fSize;
			int32				fNewest;	// ring index of the newest line
};


#endif	// TERMINAL_HISTORY_H
