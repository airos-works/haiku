/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


//!	The terminal emulator: screens, cursor, printing and scrolling.


#include "Terminal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#include <algorithm>
#include <new>

#include <OS.h>

#include "TerminalGraphics.h"
#include "UnicodeTables.h"


static const int32 kMaxColumns = 4096;
static const int32 kMaxRows = 4096;

// how long synchronized output (DECSET 2026) may hold back drawing
static const bigtime_t kSynchronizedOutputTimeout = 1000000;

// DEC special graphics, 0x5f - 0x7e
static const uint16 kDECSpecialGraphics[] = {
	0x00a0, 0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0,
	0x00b1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
	0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534,
	0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7
};


// #pragma mark - Listener


Terminal::Listener::~Listener()
{
}


void Terminal::Listener::TerminalContentsChanged() {}
void Terminal::Listener::TerminalTitleChanged(const char* title) {}
void Terminal::Listener::TerminalIconTitleChanged(const char* title) {}
void Terminal::Listener::TerminalBell() {}
void Terminal::Listener::TerminalSetClipboard(const char* selections,
	const void* data, size_t length) {}
void Terminal::Listener::TerminalRequestClipboard(const char* selections) {}
void Terminal::Listener::TerminalClipboardWrite(bool primary,
	const ClipboardData& data, const char* identifier) {}
void Terminal::Listener::TerminalClipboardRead(bool primary,
	const std::vector<std::string>& types, const char* identifier,
	const char* programName) {}
void Terminal::Listener::TerminalNotify(const TermNotification& notification)
	{}
void Terminal::Listener::TerminalCloseNotification(const char* identifier) {}
void Terminal::Listener::TerminalPointerShapeChanged(const char* shape) {}
void Terminal::Listener::TerminalColorsChanged() {}
void Terminal::Listener::TerminalWorkingDirectoryChanged(const char* url) {}
void Terminal::Listener::TerminalModesChanged() {}
void Terminal::Listener::TerminalProgressChanged(int32 state, int32 percent)
	{}
void Terminal::Listener::TerminalRemoteControl(const char* json)
	{}
void Terminal::Listener::TerminalDragAndDrop(const char* data)
	{}
void Terminal::Listener::TerminalFileTransfer(const char* data)
	{}


// #pragma mark - Terminal


Terminal::Terminal()
	:
	fListener(NULL),
	fParser(this),
	fColumns(0),
	fRows(0),
	fCellWidth(8),
	fCellHeight(16),
	fScreen(&fMain),
	fLinesScrolled(0),
	fGeneration(0),
	fVersionCounter(0),
	fCursorX(0),
	fCursorY(0),
	fPendingWrap(false),
	fHyperLink(0),
	fCursorShape(CURSOR_SHAPE_DEFAULT),
	fCursorBlinkSet(false),
	fDefaultCursorShape(CURSOR_SHAPE_BLOCK),
	fDefaultCursorBlinking(true),
	fTop(0),
	fBottom(0),
	fLeft(0),
	fRight(0),
	fLastCharacter(0),
	fLastRow(0),
	fLastColumn(0),
	fLastValid(false),
	fColorsGeneration(0),
	fDragAndDropQueryAnswered(0),
	fExtraCursorsGeneration(0),
	fFocused(true),
	fDarkColorScheme(false),
	fRectangularAttributes(false),
	fSynchronizedSince(0),
	fGraphics(NULL)
{
	pthread_mutexattr_t attributes;
	pthread_mutexattr_init(&attributes);
	pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&fLock, &attributes);
	pthread_mutexattr_destroy(&attributes);

	fMain.lines = NULL;
	fMain.storage = NULL;
	fMain.firstLine = 0;
	fMain.keyboardFlags = 0;
	fMain.savedCursor.valid = false;
	fAlternate.lines = NULL;
	fAlternate.storage = NULL;
	fAlternate.firstLine = 0;
	fAlternate.keyboardFlags = 0;
	fAlternate.savedCursor.valid = false;

	memset(&fModes, 0, sizeof(fModes));
	fModes.autoWrap = true;
	fModes.autoRepeat = true;
	fModes.cursorVisible = true;

	memset(fExtraCursorColors, 0, sizeof(fExtraCursorColors));
	memset(&fCharsets, 0, sizeof(fCharsets));
	for (int32 i = 0; i < 4; i++)
		fCharsets.designations[i] = 'B';
	fCharsets.gr = 1;
	fCharsets.singleShift = -1;

	for (int32 i = 0; i < COLOR_COUNT; i++) {
		fDefaultColors[i] = kColorUnset;
		fColors[i] = kColorUnset;
	}

	// slot 0 means "no link" and "no cluster"
	fHyperLinks.push_back(HyperLink());
	fClusters.push_back(std::vector<uint32>());

	fClipboardWrite.active = false;
	fClipboardWrite.failed = false;
	fClipboardWrite.primary = false;
	fClipboardWrite.size = 0;
}


Terminal::~Terminal()
{
	delete fGraphics;
	_FreeScreen(fMain);
	_FreeScreen(fAlternate);
	pthread_mutex_destroy(&fLock);
}


status_t
Terminal::Init(int32 columns, int32 rows, int32 historyCapacity)
{
	columns = std::max(1, std::min(columns, kMaxColumns));
	rows = std::max(1, std::min(rows, kMaxRows));

	status_t status = _InitScreen(fMain, columns, rows);
	if (status == B_OK)
		status = _InitScreen(fAlternate, columns, rows);
	if (status == B_OK)
		status = fHistory.SetCapacity(historyCapacity);
	if (status != B_OK)
		return status;

	fGraphics = new(std::nothrow) TerminalGraphics(this);
	if (fGraphics == NULL)
		return B_NO_MEMORY;

	fColumns = columns;
	fRows = rows;
	fTop = 0;
	fBottom = rows - 1;
	fLeft = 0;
	fRight = columns - 1;
	_ResetTabStops();
	return B_OK;
}


bool
Terminal::Lock()
{
	return pthread_mutex_lock(&fLock) == 0;
}


void
Terminal::Unlock()
{
	pthread_mutex_unlock(&fLock);
}


void
Terminal::Feed(const uint8* data, size_t length)
{
	uint64 generation = fGeneration;
	fParser.Feed(data, length);
	if (fGeneration != generation && fListener != NULL)
		fListener->TerminalContentsChanged();
}


bool
Terminal::TakeOutput(std::string& output)
{
	if (fOutput.empty())
		return false;
	output.append(fOutput);
	fOutput.clear();
	return true;
}


void
Terminal::Send(const char* data, size_t length)
{
	fOutput.append(data, length);
}


void
Terminal::_Reply(const char* format, ...)
{
	char buffer[512];
	va_list args;
	va_start(args, format);
	int length = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	if (length > 0)
		fOutput.append(buffer, std::min(length, (int)sizeof(buffer) - 1));
}


void
Terminal::SetCellSize(int32 width, int32 height)
{
	if (width > 0)
		fCellWidth = width;
	if (height > 0)
		fCellHeight = height;
}


status_t
Terminal::SetHistoryCapacity(int32 lines)
{
	status_t status = fHistory.SetCapacity(lines);
	_Changed();
	return status;
}


int32
Terminal::HistorySize() const
{
	return fScreen == &fMain ? fHistory.Size() : 0;
}


int64
Terminal::AbsoluteLine(int32 row) const
{
	return (int64)fLinesScrolled + row;
}


bool
Terminal::IsSynchronizedOutput() const
{
	return fModes.synchronizedOutput
		&& system_time() - fSynchronizedSince < kSynchronizedOutputTimeout;
}


uint32
Terminal::KeyboardFlags() const
{
	return fScreen->keyboardFlags;
}


Terminal::CursorInfo
Terminal::Cursor() const
{
	CursorInfo info;
	info.x = std::min(fCursorX, fColumns - 1);
	info.y = fCursorY;
	info.visible = fModes.cursorVisible;
	info.shape = fCursorShape != CURSOR_SHAPE_DEFAULT
		? fCursorShape : fDefaultCursorShape;
	info.blinking = fCursorBlinkSet ? fModes.cursorBlink
		: fDefaultCursorBlinking;
	return info;
}


void
Terminal::SetDefaultCursorShape(int32 shape, bool blinking)
{
	fDefaultCursorShape = shape;
	fDefaultCursorBlinking = blinking;
	_Changed();
}


void
Terminal::SetFocused(bool focused)
{
	if (fFocused == focused)
		return;
	fFocused = focused;
	if (fModes.focusReporting)
		_Reply(focused ? "\x1b[I" : "\x1b[O");
}


void
Terminal::SetColorScheme(bool dark)
{
	if (fDarkColorScheme == dark)
		return;
	fDarkColorScheme = dark;
	if (fModes.colorSchemeReports)
		_Reply("\x1b[?997;%dn", dark ? 1 : 2);
}


// #pragma mark - screen storage


status_t
Terminal::_InitScreen(Screen& screen, int32 columns, int32 rows)
{
	TerminalLine* lines = new(std::nothrow) TerminalLine[rows];
	TerminalCell* storage = new(std::nothrow) TerminalCell[
		(size_t)rows * columns];
	if (lines == NULL || storage == NULL) {
		delete[] lines;
		delete[] storage;
		return B_NO_MEMORY;
	}

	for (int32 i = 0; i < rows; i++) {
		lines[i].cells = storage + (size_t)i * columns;
		lines[i].version = ++fVersionCounter;
		lines[i].flags = 0;
		for (int32 x = 0; x < columns; x++) {
			lines[i].cells[x].character = 0;
			lines[i].cells[x].hyperlink = 0;
			lines[i].cells[x].style = CellStyle();
		}
	}

	_FreeScreen(screen);
	screen.lines = lines;
	screen.storage = storage;
	screen.firstLine = 0;
	return B_OK;
}


void
Terminal::_FreeScreen(Screen& screen)
{
	delete[] screen.lines;
	delete[] screen.storage;
	screen.lines = NULL;
	screen.storage = NULL;
}


void
Terminal::_Touch(int32 row)
{
	_Line(row).version = ++fVersionCounter;
	fGeneration++;
}


void
Terminal::_TouchRange(int32 firstRow, int32 lastRow)
{
	for (int32 row = firstRow; row <= lastRow; row++)
		_Line(row).version = ++fVersionCounter;
	fGeneration++;
}


void
Terminal::_Changed()
{
	fGeneration++;
}


/*!	Erases cells the way the erase controls do: empty, in the current
	background color (background color erase).
*/
void
Terminal::_ClearCells(TerminalCell* cells, int32 count, bool keepProtected)
{
	CellStyle style;
	style.background = fStyle.background;
	for (int32 i = 0; i < count; i++) {
		if (keepProtected && (cells[i].style.flags & CELL_PROTECTED) != 0)
			continue;
		cells[i].character = 0;
		cells[i].hyperlink = 0;
		cells[i].style = style;
	}
}


void
Terminal::_ClearLine(int32 row, int32 from, int32 to, bool selective)
{
	from = std::max(from, (int32)0);
	to = std::min(to, fColumns - 1);
	if (from > to)
		return;

	_EraseMulticells(row, row, from, to);
	_FixWideCharacters(row, from, to);
	_ClearCells(_Cells(row) + from, to - from + 1, selective);
	if (fGraphics != NULL)
		fGraphics->CellsChanged(row, from, to);
	_Touch(row);
}


void
Terminal::_ClearRows(int32 firstRow, int32 lastRow, bool selective)
{
	_EraseMulticells(firstRow, lastRow, 0, fColumns - 1);
	for (int32 row = firstRow; row <= lastRow; row++) {
		_ClearCells(_Cells(row), fColumns, selective);
		if (!selective)
			_Line(row).flags = 0;
	}
	_TouchRange(firstRow, lastRow);
}


/*!	Before cells \a from to \a to of \a row are overwritten or erased, makes
	sure no double width character is cut in half at their edges.
*/
void
Terminal::_FixWideCharacters(int32 row, int32 from, int32 to)
{
	TerminalCell* cells = _Cells(row);
	if (from > 0 && from < fColumns && cells[from].IsWideTail()) {
		TerminalCell& head = cells[from - 1];
		head.character = 0;
		head.style.flags &= ~CELL_WIDE;
	}
	if (to >= 0 && to + 1 < fColumns && cells[to].IsWide()
		&& cells[to + 1].IsWideTail()) {
		TerminalCell& tail = cells[to + 1];
		tail.character = 0;
		tail.style.flags &= ~CELL_WIDE_TAIL;
	}
}


// #pragma mark - reading


const TerminalLine*
Terminal::ScreenLine(int32 row) const
{
	if (row < 0 || row >= fRows)
		return NULL;
	return &_Line(row);
}


bool
Terminal::GetLine(int32 row, TerminalCell* cells, uint16* _flags,
	uint64* _version) const
{
	if (row >= fRows)
		return false;

	if (row >= 0) {
		const TerminalLine& line = _Line(row);
		memcpy(cells, line.cells, sizeof(TerminalCell) * fColumns);
		if (_flags != NULL)
			*_flags = line.flags;
		if (_version != NULL)
			*_version = line.version;
		return true;
	}

	if (fScreen != &fMain)
		return false;
	return fHistory.GetLine(-row - 1, cells, fColumns, _flags, _version);
}


int32
Terminal::LineLength(int32 row) const
{
	if (row >= fRows)
		return 0;
	if (row < 0) {
		if (fScreen != &fMain)
			return 0;
		return std::min(fHistory.LineLength(-row - 1), fColumns);
	}

	const TerminalCell* cells = _Line(row).cells;
	int32 length = fColumns;
	while (length > 0 && cells[length - 1].IsEmpty()
		&& !cells[length - 1].IsWideTail()) {
		length--;
	}
	return length;
}


uint16
Terminal::LineFlags(int32 row) const
{
	if (row >= fRows)
		return 0;
	if (row >= 0)
		return _Line(row).flags;
	if (fScreen != &fMain)
		return 0;
	return fHistory.LineFlags(-row - 1);
}


bool
Terminal::GetCell(int32 row, int32 column, TerminalCell& cell) const
{
	if (column < 0 || column >= fColumns || row >= fRows)
		return false;
	if (row >= 0) {
		cell = _Line(row).cells[column];
		return true;
	}

	TerminalCell* cells = new(std::nothrow) TerminalCell[fColumns];
	if (cells == NULL)
		return false;
	bool result = GetLine(row, cells);
	if (result)
		cell = cells[column];
	delete[] cells;
	return result;
}


int32
Terminal::GetCharacter(uint32 character, uint32* codePoints,
	int32 maxCount) const
{
	if (character == 0 || maxCount <= 0)
		return 0;
	if (is_multicell(character)) {
		// the text of a block is in its first cell
		TermMulticell multicell;
		if (multicell_x(character) != 0 || multicell_y(character) != 0
			|| !GetMulticell(multicell_index(character), multicell)) {
			return 0;
		}
		return GetCharacter(multicell.text, codePoints, maxCount);
	}
	if ((character & kClusterBit) == 0) {
		codePoints[0] = character;
		return 1;
	}

	uint32 index = character & ~kClusterBit;
	if (index >= fClusters.size())
		return 0;
	const std::vector<uint32>& cluster = fClusters[index];
	int32 count = std::min((int32)cluster.size(), maxCount);
	for (int32 i = 0; i < count; i++)
		codePoints[i] = cluster[i];
	return count;
}


static void
append_utf8(std::string& text, uint32 c)
{
	if (c < 0x80) {
		text += (char)c;
	} else if (c < 0x800) {
		text += (char)(0xc0 | (c >> 6));
		text += (char)(0x80 | (c & 0x3f));
	} else if (c < 0x10000) {
		text += (char)(0xe0 | (c >> 12));
		text += (char)(0x80 | ((c >> 6) & 0x3f));
		text += (char)(0x80 | (c & 0x3f));
	} else {
		text += (char)(0xf0 | (c >> 18));
		text += (char)(0x80 | ((c >> 12) & 0x3f));
		text += (char)(0x80 | ((c >> 6) & 0x3f));
		text += (char)(0x80 | (c & 0x3f));
	}
}


void
Terminal::AppendCharacterUTF8(uint32 character, std::string& text) const
{
	if (is_multicell(character)) {
		TermMulticell multicell;
		if (multicell_x(character) == 0 && multicell_y(character) == 0
			&& GetMulticell(multicell_index(character), multicell)) {
			AppendCharacterUTF8(multicell.text, text);
		}
		return;
	}
	if ((character & kClusterBit) == 0) {
		append_utf8(text, character);
		return;
	}

	uint32 index = character & ~kClusterBit;
	if (index >= fClusters.size())
		return;
	const std::vector<uint32>& cluster = fClusters[index];
	for (size_t i = 0; i < cluster.size(); i++)
		append_utf8(text, cluster[i]);
}


/*!	The text from \a start up to \a end (exclusive), with line breaks where
	the lines were not wrapped. Empty cells inside a line are spaces; those
	at the end of a line are left out.
*/
void
Terminal::GetText(TermPoint start, TermPoint end, std::string& text,
	bool rectangular) const
{
	if (end < start)
		std::swap(start, end);

	int32 firstRow = std::max(start.y, -HistorySize());
	int32 lastRow = std::min(end.y, fRows - 1);
	if (end.x == 0 && end.y > start.y && !rectangular)
		lastRow = std::min(end.y - 1, fRows - 1);

	TerminalCell* cells = new(std::nothrow) TerminalCell[fColumns];
	if (cells == NULL)
		return;

	for (int32 row = firstRow; row <= lastRow; row++) {
		uint16 flags = 0;
		if (!GetLine(row, cells, &flags))
			continue;

		int32 from = 0;
		int32 to = fColumns;
		if (rectangular) {
			from = std::min(start.x, end.x);
			to = std::max(start.x, end.x);
		} else {
			if (row == start.y)
				from = start.x;
			if (row == end.y)
				to = std::min(end.x, fColumns);
		}

		int32 length = fColumns;
		while (length > 0 && cells[length - 1].IsEmpty())
			length--;
		bool lineEnds = to >= length;
		int32 last = std::min(to, length);

		for (int32 x = std::max(from, (int32)0); x < last; x++) {
			const TerminalCell& cell = cells[x];
			if (cell.IsWideTail())
				continue;
			if (cell.IsEmpty())
				text += ' ';
			else
				AppendCharacterUTF8(cell.character, text);
		}

		bool wrapped = (flags & LINE_WRAPPED) != 0 && !rectangular;
		if (row < lastRow || (lineEnds && row < end.y)) {
			if (!wrapped)
				text += '\n';
		}
	}

	delete[] cells;
}


bool
Terminal::FindWord(TermPoint position,
	bool (*isWordChar)(uint32 c, void* cookie), void* cookie,
	bool findNonWords, TermPoint& _start, TermPoint& _end) const
{
	int32 firstRow = -HistorySize();
	if (position.y < firstRow || position.y >= fRows || position.x < 0
		|| position.x >= fColumns) {
		return false;
	}

	TerminalCell* cells = new(std::nothrow) TerminalCell[fColumns];
	if (cells == NULL)
		return false;

	// classify: 0 = space/empty, 1 = word, 2 = other
	struct Classifier {
		const Terminal*	terminal;
		bool (*isWordChar)(uint32 c, void* cookie);
		void*			cookie;

		int Classify(const TerminalCell& cell) const
		{
			if (cell.IsEmpty())
				return 0;
			uint32 codePoints[1];
			if (terminal->GetCharacter(cell.character, codePoints, 1) < 1)
				return 0;
			uint32 c = codePoints[0];
			if (c == ' ' || c == '\t')
				return 0;
			return isWordChar(c, cookie) ? 1 : 2;
		}
	} classifier = { this, isWordChar, cookie };

	GetLine(position.y, cells);
	int32 x = position.x;
	if (cells[x].IsWideTail() && x > 0)
		x--;
	int type = classifier.Classify(cells[x]);
	if (type != 1 && !findNonWords) {
		delete[] cells;
		return false;
	}

	// backwards, over soft wraps
	TermPoint start(x, position.y);
	{
		int32 row = position.y;
		int32 column = x;
		GetLine(row, cells);
		while (true) {
			int32 previous = column - 1;
			if (previous < 0) {
				if (row - 1 < firstRow || (LineFlags(row - 1) & LINE_WRAPPED) == 0)
					break;
				row--;
				GetLine(row, cells);
				previous = fColumns - 1;
			}
			if (cells[previous].IsWideTail() && previous > 0)
				previous--;
			if (classifier.Classify(cells[previous]) != type)
				break;
			column = previous;
			start = TermPoint(column, row);
		}
	}

	// forwards
	TermPoint end(x, position.y);
	{
		int32 row = position.y;
		int32 column = x;
		GetLine(row, cells);
		while (true) {
			int32 next = column + (cells[column].IsWide() ? 2 : 1);
			if (next >= fColumns) {
				if ((LineFlags(row) & LINE_WRAPPED) == 0 || row + 1 >= fRows)
					break;
				row++;
				GetLine(row, cells);
				next = 0;
			}
			if (classifier.Classify(cells[next]) != type)
				break;
			column = next;
			end = TermPoint(column, row);
		}
		end.x += cells[column].IsWide() ? 2 : 1;
	}

	delete[] cells;
	_start = start;
	_end = end;
	return true;
}


namespace {

struct LogicalLine {
	std::vector<uint32>		characters;
	std::vector<TermPoint>	positions;
};

}	// namespace


static void
decode_pattern(const char* pattern, std::vector<uint32>& codePoints)
{
	const uint8* bytes = (const uint8*)pattern;
	while (*bytes != '\0') {
		uint32 c = *bytes++;
		int32 extra = 0;
		if (c >= 0xf0) {
			c &= 0x07;
			extra = 3;
		} else if (c >= 0xe0) {
			c &= 0x0f;
			extra = 2;
		} else if (c >= 0xc0) {
			c &= 0x1f;
			extra = 1;
		}
		while (extra-- > 0 && (*bytes & 0xc0) == 0x80)
			c = (c << 6) | (*bytes++ & 0x3f);
		codePoints.push_back(c);
	}
}


static inline uint32
fold_case(uint32 c)
{
	if (c < 0x80)
		return c >= 'A' && c <= 'Z' ? c + 'a' - 'A' : c;
	return (uint32)towlower((wint_t)c);
}


static inline bool
is_word_character(uint32 c)
{
	return c == '_' || iswalnum((wint_t)c);
}


/*!	Finds \a pattern after (or before) \a start, across soft wrapped lines.
	The match is from \a _matchStart up to \a _matchEnd (exclusive).
*/
bool
Terminal::Find(const char* pattern, TermPoint start, bool forward,
	bool matchCase, bool matchWord, TermPoint& _matchStart,
	TermPoint& _matchEnd) const
{
	std::vector<uint32> needle;
	decode_pattern(pattern, needle);
	if (needle.empty())
		return false;
	if (!matchCase) {
		for (size_t i = 0; i < needle.size(); i++)
			needle[i] = fold_case(needle[i]);
	}

	int32 firstRow = -HistorySize();
	TerminalCell* cells = new(std::nothrow) TerminalCell[fColumns];
	if (cells == NULL)
		return false;

	// the logical line starting at a row (rows continue while wrapped)
	struct Builder {
		static int32 Build(const Terminal* terminal, int32 row,
			TerminalCell* cells, LogicalLine& line, bool matchCase)
		{
			line.characters.clear();
			line.positions.clear();
			while (row < terminal->Rows()) {
				uint16 flags = 0;
				terminal->GetLine(row, cells, &flags);
				int32 length = terminal->Columns();
				if ((flags & LINE_WRAPPED) == 0) {
					while (length > 0 && cells[length - 1].IsEmpty())
						length--;
				}
				for (int32 x = 0; x < length; x++) {
					if (cells[x].IsWideTail())
						continue;
					uint32 c = ' ';
					uint32 codePoints[1];
					if (!cells[x].IsEmpty() && terminal->GetCharacter(
							cells[x].character, codePoints, 1) == 1) {
						c = codePoints[0];
					}
					line.characters.push_back(matchCase ? c : fold_case(c));
					line.positions.push_back(TermPoint(x, row));
				}
				row++;
				if ((flags & LINE_WRAPPED) == 0)
					break;
			}
			return row;
		}
	};

	// the start of the logical line holding the start position
	int32 row = std::max(firstRow, std::min(start.y, fRows - 1));
	while (row > firstRow && (LineFlags(row - 1) & LINE_WRAPPED) != 0)
		row--;

	bool found = false;
	LogicalLine line;
	if (forward) {
		while (row < fRows && !found) {
			int32 next = Builder::Build(this, row, cells, line, matchCase);
			for (size_t i = 0; i + needle.size() <= line.characters.size();
					i++) {
				if (line.positions[i] < start)
					continue;
				if (memcmp(&line.characters[i], needle.data(),
						needle.size() * sizeof(uint32)) != 0) {
					continue;
				}
				if (matchWord && ((i > 0
						&& is_word_character(line.characters[i - 1]))
					|| (i + needle.size() < line.characters.size()
						&& is_word_character(
							line.characters[i + needle.size()])))) {
					continue;
				}
				_matchStart = line.positions[i];
				TermPoint last = line.positions[i + needle.size() - 1];
				TerminalCell cell;
				GetCell(last.y, last.x, cell);
				_matchEnd = TermPoint(last.x + (cell.IsWide() ? 2 : 1),
					last.y);
				found = true;
				break;
			}
			row = next;
		}
	} else {
		while (row >= firstRow && !found) {
			Builder::Build(this, row, cells, line, matchCase);
			for (int32 i = (int32)line.characters.size()
					- (int32)needle.size(); i >= 0; i--) {
				if (!(line.positions[i] < start))
					continue;
				if (memcmp(&line.characters[i], needle.data(),
						needle.size() * sizeof(uint32)) != 0) {
					continue;
				}
				if (matchWord && ((i > 0
						&& is_word_character(line.characters[i - 1]))
					|| (i + needle.size() < line.characters.size()
						&& is_word_character(
							line.characters[i + needle.size()])))) {
					continue;
				}
				_matchStart = line.positions[i];
				TermPoint last = line.positions[i + needle.size() - 1];
				TerminalCell cell;
				GetCell(last.y, last.x, cell);
				_matchEnd = TermPoint(last.x + (cell.IsWide() ? 2 : 1),
					last.y);
				found = true;
				break;
			}
			// the previous logical line
			row--;
			while (row > firstRow && (LineFlags(row - 1) & LINE_WRAPPED) != 0)
				row--;
		}
	}

	delete[] cells;
	return found;
}


bool
Terminal::FindPrompt(int32 row, bool forward, int32& _row) const
{
	int32 first = -HistorySize();
	if (forward) {
		for (int32 r = row + 1; r < fRows; r++) {
			if ((LineFlags(r) & LINE_PROMPT) != 0) {
				_row = r;
				return true;
			}
		}
	} else {
		for (int32 r = std::min(row - 1, fRows - 1); r >= first; r--) {
			if ((LineFlags(r) & LINE_PROMPT) != 0) {
				_row = r;
				return true;
			}
		}
	}
	return false;
}


bool
Terminal::GetHyperLink(uint32 id, std::string& uri,
	std::string& identifier) const
{
	if (id == 0 || id >= fHyperLinks.size())
		return false;
	uri = fHyperLinks[id].uri;
	identifier = fHyperLinks[id].identifier;
	return true;
}


// #pragma mark - colors


void
Terminal::SetDefaultColor(int32 index, uint32 color)
{
	if (index < 0 || index >= COLOR_COUNT)
		return;
	if (fColors[index] == fDefaultColors[index])
		fColors[index] = color;
	fDefaultColors[index] = color;
	fColorsGeneration++;
	_Changed();
}


uint32
Terminal::DefaultColor(int32 index) const
{
	if (index < 0 || index >= COLOR_COUNT)
		return kColorUnset;
	return fDefaultColors[index];
}


uint32
Terminal::Color(int32 index) const
{
	if (index < 0 || index >= COLOR_COUNT)
		return kColorUnset;
	if (fColors[index] != kColorUnset)
		return fColors[index];
	if (index < 256) {
		// the xterm 256 color palette
		if (index < 16)
			return fDefaultColors[index];
		if (index < 232) {
			static const uint8 kLevels[] = { 0, 95, 135, 175, 215, 255 };
			int32 i = index - 16;
			return ((uint32)kLevels[i / 36] << 16)
				| ((uint32)kLevels[(i / 6) % 6] << 8) | kLevels[i % 6];
		}
		uint32 gray = 8 + (index - 232) * 10;
		return (gray << 16) | (gray << 8) | gray;
	}
	return kColorUnset;
}


uint32
Terminal::ResolveColor(TermColor color, bool foreground) const
{
	if (is_direct_color(color))
		return color & 0xffffff;
	if (is_indexed_color(color)) {
		uint32 value = Color(color & 0xff);
		if (value != kColorUnset)
			return value;
	}
	uint32 value = Color(foreground ? COLOR_FOREGROUND : COLOR_BACKGROUND);
	if (value == kColorUnset)
		return foreground ? 0x000000 : 0xffffff;
	return value;
}


const char*
Terminal::PointerShape() const
{
	if (fScreen->pointerShapes.empty())
		return "";
	return fScreen->pointerShapes.back().c_str();
}


// #pragma mark - printing


void
Terminal::Print(const uint32* codePoints, int32 count)
{
	int32 i = 0;
	while (i < count) {
		// runs of plain ASCII take a shortcut
		if (codePoints[i] < 0x7f && fCharsets.singleShift < 0
			&& fCharsets.designations[fCharsets.gl] == 'B'
			&& !fModes.insert) {
			int32 end = i + 1;
			while (end < count && codePoints[end] < 0x7f)
				end++;
			_PrintASCII(codePoints + i, end - i);
			i = end;
			continue;
		}
		_PrintCodePoint(codePoints[i++]);
	}
}


void
Terminal::_PrintASCII(const uint32* codePoints, int32 count)
{
	while (count > 0) {
		if (fPendingWrap && fModes.autoWrap) {
			_Line(fCursorY).flags |= LINE_WRAPPED;
			_CarriageReturn();
			_LineFeed();
		}

		int32 right = fCursorX <= fRight ? fRight : fColumns - 1;
		int32 room = right - fCursorX + 1;
		if (room <= 0) {
			// right of the margin without wrapping: overwrite the last cell
			fCursorX = right;
			room = 1;
		}
		int32 n = std::min(room, count);

		if (_HasMulticells(fCursorY, fCursorX, fCursorX + n - 1)) {
			// text sizing blocks in the way: one by one, by their rules
			_PutCharacter(codePoints[0], 1);
			codePoints++;
			count--;
			continue;
		}

		TerminalCell* cells = _Cells(fCursorY);
		_FixWideCharacters(fCursorY, fCursorX, fCursorX + n - 1);
		CellStyle style = fStyle;
		style.flags &= ~(CELL_WIDE | CELL_WIDE_TAIL);
		for (int32 j = 0; j < n; j++) {
			TerminalCell& cell = cells[fCursorX + j];
			cell.character = codePoints[j];
			cell.hyperlink = fHyperLink;
			cell.style = style;
		}
		if (fGraphics != NULL)
			fGraphics->CellsChanged(fCursorY, fCursorX, fCursorX + n - 1);
		_Touch(fCursorY);

		fLastCharacter = codePoints[n - 1];
		fLastRow = fCursorY;
		fLastColumn = fCursorX + n - 1;
		fLastValid = true;

		fCursorX += n;
		codePoints += n;
		count -= n;
		if (fCursorX > right) {
			fCursorX = right;
			fPendingWrap = fModes.autoWrap;
			if (!fModes.autoWrap && count > 0) {
				// the rest all lands in the last cell
				TerminalCell& cell = cells[right];
				cell.character = codePoints[count - 1];
				fLastCharacter = cell.character;
				return;
			}
		}
	}
}


uint32
Terminal::_MapCharset(uint32 c)
{
	int32 set = fCharsets.singleShift >= 0 ? fCharsets.singleShift
		: fCharsets.gl;
	fCharsets.singleShift = -1;

	switch (fCharsets.designations[set]) {
		case '0':
			if (c >= 0x5f && c <= 0x7e)
				return kDECSpecialGraphics[c - 0x5f];
			break;
		case 'A':
			if (c == '#')
				return 0x00a3;
			break;
	}
	return c;
}


void
Terminal::_PrintCodePoint(uint32 c)
{
	if (c < 0x80)
		c = _MapCharset(c);

	int width = unicode_width(c);
	int breakProperty = unicode_grapheme_break(c);

	// zero width format characters that do not combine (U+200B)
	if (width == 0 && breakProperty == GRAPHEME_CONTROL)
		return;

	if (fLastValid && _AppendToPrevious(c, width))
		return;

	if (width == 0) {
		// a combining character with nothing to combine with: show it on
		// its own, like a space with the mark
		width = 1;
	}

	_PutCharacter(c, width);
}


/*!	Decides whether \a c continues the grapheme cluster printed last
	(UAX #29) and if so adds it to that cell. Returns whether it did.
*/
bool
Terminal::_AppendToPrevious(uint32 c, int width)
{
	if (fLastRow < 0 || fLastRow >= fRows || fLastColumn < 0
		|| fLastColumn >= fColumns) {
		return false;
	}

	TerminalCell& cell = _Cells(fLastRow)[fLastColumn];
	if (cell.character != fLastCharacter || cell.character == 0)
		return false;

	// most characters start a cluster of their own: decide that without
	// looking at the whole cluster before
	uint32 previous = cell.character;
	if ((previous & kClusterBit) != 0) {
		uint32 index = previous & ~kClusterBit;
		if (index >= fClusters.size() || fClusters[index].empty())
			return false;
		previous = fClusters[index].back();
	}
	int previousBreak = unicode_grapheme_break(previous);
	int currentBreak = unicode_grapheme_break(c);
	if (currentBreak == GRAPHEME_OTHER && previousBreak != GRAPHEME_PREPEND
		&& previousBreak != GRAPHEME_ZWJ
		&& unicode_indic_conjunct_break(c) != INCB_CONSONANT) {
		return false;
	}

	std::vector<uint32> cluster;
	_ClusterCodePoints(cell.character, cluster);
	if (cluster.empty())
		return false;

	bool join = false;
	if (currentBreak == GRAPHEME_EXTEND || currentBreak == GRAPHEME_ZWJ
		|| currentBreak == GRAPHEME_SPACING_MARK) {
		join = true;	// GB9, GB9a
	} else if (previousBreak == GRAPHEME_PREPEND) {
		join = true;	// GB9b
	} else if (previousBreak == GRAPHEME_L
		&& (currentBreak == GRAPHEME_L || currentBreak == GRAPHEME_V
			|| currentBreak == GRAPHEME_LV || currentBreak == GRAPHEME_LVT)) {
		join = true;	// GB6
	} else if ((previousBreak == GRAPHEME_LV || previousBreak == GRAPHEME_V)
		&& (currentBreak == GRAPHEME_V || currentBreak == GRAPHEME_T)) {
		join = true;	// GB7
	} else if ((previousBreak == GRAPHEME_LVT || previousBreak == GRAPHEME_T)
		&& currentBreak == GRAPHEME_T) {
		join = true;	// GB8
	} else if (previousBreak == GRAPHEME_ZWJ
		&& unicode_is_extended_pictographic(c)) {
		// GB11: ExtPict Extend* ZWJ x ExtPict
		for (int32 i = (int32)cluster.size() - 2; i >= 0; i--) {
			if (unicode_is_extended_pictographic(cluster[i])) {
				join = true;
				break;
			}
			if (unicode_grapheme_break(cluster[i]) != GRAPHEME_EXTEND)
				break;
		}
	} else if (previousBreak == GRAPHEME_REGIONAL_INDICATOR
		&& currentBreak == GRAPHEME_REGIONAL_INDICATOR) {
		// GB12, GB13: pairs of regional indicators
		int32 count = 0;
		for (int32 i = (int32)cluster.size() - 1; i >= 0
			&& unicode_grapheme_break(cluster[i])
				== GRAPHEME_REGIONAL_INDICATOR; i--) {
			count++;
		}
		join = (count % 2) == 1;
	} else if (unicode_indic_conjunct_break(c) == INCB_CONSONANT) {
		// GB9c: Consonant [Extend Linker]* Linker [Extend Linker]* x
		// Consonant
		bool linker = false;
		for (int32 i = (int32)cluster.size() - 1; i >= 0; i--) {
			int incb = unicode_indic_conjunct_break(cluster[i]);
			if (incb == INCB_LINKER)
				linker = true;
			else if (incb == INCB_CONSONANT) {
				join = linker;
				break;
			} else if (incb != INCB_EXTEND)
				break;
		}
	}

	if (!join)
		return false;

	if (cluster.size() >= 32)
		return true;	// drop absurdly long clusters' tails

	cluster.push_back(c);
	uint32 character = _InternCluster(cluster.data(), cluster.size());
	cell.character = character;
	fLastCharacter = character;

	// VS16 asks for the emoji presentation: two cells (also for the
	// keycaps' digits, number sign and asterisk)
	uint32 base = cluster[0];
	if (c == 0xfe0f && !cell.IsWide()
		&& (unicode_is_extended_pictographic(base) || base == '#'
			|| base == '*' || (base >= '0' && base <= '9'))) {
		if (fLastColumn + 1 < fColumns) {
			TerminalCell& next = _Cells(fLastRow)[fLastColumn + 1];
			if (fCursorX == fLastColumn + 1 && !fPendingWrap) {
				_FixWideCharacters(fLastRow, fLastColumn + 1,
					fLastColumn + 1);
				next.character = 0;
				next.hyperlink = cell.hyperlink;
				next.style = cell.style;
				next.style.flags |= CELL_WIDE_TAIL;
				cell.style.flags |= CELL_WIDE;
				fCursorX++;
				if (fCursorX > fRight) {
					fCursorX = fRight;
					fPendingWrap = fModes.autoWrap;
				}
			}
		}
	}

	_Touch(fLastRow);
	return true;
}


void
Terminal::_ClusterCodePoints(uint32 character,
	std::vector<uint32>& codePoints) const
{
	codePoints.clear();
	if (character == 0)
		return;
	if ((character & kClusterBit) == 0) {
		codePoints.push_back(character);
		return;
	}
	uint32 index = character & ~kClusterBit;
	if (index < fClusters.size())
		codePoints = fClusters[index];
}


uint32
Terminal::_InternCluster(const uint32* codePoints, int32 count)
{
	std::string key((const char*)codePoints, count * sizeof(uint32));
	std::unordered_map<std::string, uint32>::iterator found
		= fClusterIndex.find(key);
	if (found != fClusterIndex.end())
		return found->second | kClusterBit;

	uint32 index = fClusters.size();
	if (index >= 0x7fffffff)
		return codePoints[0];
	fClusters.push_back(std::vector<uint32>(codePoints, codePoints + count));
	fClusterIndex[key] = index;
	return index | kClusterBit;
}


void
Terminal::_PutCharacter(uint32 character, int width)
{
	if (fPendingWrap && fModes.autoWrap) {
		_Line(fCursorY).flags |= LINE_WRAPPED;
		_CarriageReturn();
		_LineFeed();
	}

	int32 left = fCursorX >= fLeft ? fLeft : 0;
	int32 right = fCursorX <= fRight ? fRight : fColumns - 1;
	if (right - left + 1 < width) {
		// no room at all for a wide character
		width = 1;
		character = 0xfffd;
	}

	if (fCursorX + width - 1 > right) {
		if (fModes.autoWrap) {
			// doesn't fit: continue on the next line
			_ClearLine(fCursorY, fCursorX, right);
			_Line(fCursorY).flags |= LINE_WRAPPED;
			_CarriageReturn();
			_LineFeed();
			right = fCursorX <= fRight ? fRight : fColumns - 1;
		} else
			fCursorX = right - width + 1;
	}

	if (fModes.insert) {
		// shift the rest of the line (to the margin) right
		TerminalCell* cells = _Cells(fCursorY);
		int32 count = right - fCursorX + 1 - width;
		_FixWideCharacters(fCursorY, fCursorX, right);
		if (count > 0) {
			memmove(cells + fCursorX + width, cells + fCursorX,
				count * sizeof(TerminalCell));
		}
		if (cells[right].IsWide())
			_ClearCells(cells + right, 1);
	}

	if (_HasMulticells(fCursorY, fCursorX, fCursorX + width - 1)) {
		_SkipMulticells(width);
		right = fCursorX <= fRight ? fRight : fColumns - 1;
		_EraseMulticells(fCursorY, fCursorY, fCursorX, fCursorX + width - 1);
	}

	TerminalCell* cells = _Cells(fCursorY);
	_FixWideCharacters(fCursorY, fCursorX, fCursorX + width - 1);

	TerminalCell& cell = cells[fCursorX];
	cell.character = character;
	cell.hyperlink = fHyperLink;
	cell.style = fStyle;
	cell.style.flags &= ~(CELL_WIDE | CELL_WIDE_TAIL);
	if (width == 2) {
		cell.style.flags |= CELL_WIDE;
		TerminalCell& tail = cells[fCursorX + 1];
		tail.character = 0;
		tail.hyperlink = fHyperLink;
		tail.style = fStyle;
		tail.style.flags &= ~CELL_WIDE;
		tail.style.flags |= CELL_WIDE_TAIL;
	}

	if (character == 0x10eeee)
		_Line(fCursorY).flags |= LINE_PLACEHOLDERS;
	if (fGraphics != NULL)
		fGraphics->CellsChanged(fCursorY, fCursorX, fCursorX + width - 1);
	_Touch(fCursorY);

	fLastCharacter = character;
	fLastRow = fCursorY;
	fLastColumn = fCursorX;
	fLastValid = true;

	fCursorX += width;
	if (fCursorX > right) {
		fCursorX = right;
		fPendingWrap = fModes.autoWrap;
	}
}


void
Terminal::_RepeatCharacter(int32 count)
{
	if (!fLastValid || fLastCharacter == 0)
		return;

	uint32 character = fLastCharacter;
	int width = 1;
	if ((character & kClusterBit) == 0)
		width = std::max(1, unicode_width(character));
	else {
		std::vector<uint32> cluster;
		_ClusterCodePoints(character, cluster);
		if (!cluster.empty())
			width = std::max(1, unicode_width(cluster[0]));
	}

	count = std::min(count, fColumns * fRows);
	for (int32 i = 0; i < count; i++)
		_PutCharacter(character, width);
}


// #pragma mark - cursor movement


void
Terminal::_SetCursor(int32 x, int32 y)
{
	fCursorX = std::max((int32)0, std::min(x, fColumns - 1));
	fCursorY = std::max((int32)0, std::min(y, fRows - 1));
	fPendingWrap = false;
	_Changed();
}


/*!	Moves the cursor as CUP does: relative to the margins in origin mode.
*/
void
Terminal::_MoveCursorTo(int32 x, int32 y)
{
	if (fModes.origin) {
		x += fLeft;
		y += fTop;
		x = std::max(fLeft, std::min(x, fRight));
		y = std::max(fTop, std::min(y, fBottom));
	}
	_SetCursor(x, y);
}


void
Terminal::_CarriageReturn()
{
	if (fCursorX >= fLeft)
		fCursorX = fLeft;
	else
		fCursorX = 0;
	fPendingWrap = false;
	_Changed();
}


void
Terminal::_LineFeed()
{
	fPendingWrap = false;
	if (fCursorY == fBottom) {
		if (fCursorX >= fLeft && fCursorX <= fRight)
			_ScrollUp(1);
	} else if (fCursorY < fRows - 1)
		fCursorY++;
	_Changed();
}


void
Terminal::_ReverseIndex()
{
	fPendingWrap = false;
	if (fCursorY == fTop) {
		if (fCursorX >= fLeft && fCursorX <= fRight)
			_ScrollDown(1);
	} else if (fCursorY > 0)
		fCursorY--;
	_Changed();
}


void
Terminal::_Backspace()
{
	if (fPendingWrap) {
		fPendingWrap = false;
		_Changed();
		return;
	}

	if (fCursorX > (fCursorX >= fLeft ? fLeft : 0)) {
		fCursorX--;
	} else if (fModes.reverseWrap && fModes.autoWrap) {
		// back over the previous line's end (DECSET 45)
		if (fCursorY > fTop) {
			fCursorY--;
			fCursorX = fRight;
		} else if (fCursorY == fTop) {
			fCursorY = fBottom;
			fCursorX = fRight;
		}
	}
	_Changed();
}


void
Terminal::_Tab(int32 count)
{
	int32 right = fCursorX <= fRight ? fRight : fColumns - 1;
	while (count-- > 0 && fCursorX < right) {
		fCursorX++;
		while (fCursorX < right && !fTabStops[fCursorX])
			fCursorX++;
	}
	fPendingWrap = false;
	_Changed();
}


void
Terminal::_BackTab(int32 count)
{
	int32 left = fCursorX >= fLeft ? fLeft : 0;
	while (count-- > 0 && fCursorX > left) {
		fCursorX--;
		while (fCursorX > left && !fTabStops[fCursorX])
			fCursorX--;
	}
	fPendingWrap = false;
	_Changed();
}


// #pragma mark - scrolling


bool
Terminal::_FullWidthMargins() const
{
	return fLeft == 0 && fRight == fColumns - 1;
}


void
Terminal::_PushLineToHistory(TerminalLine& line)
{
	if (fHistory.Capacity() == 0) {
		fLinesScrolled++;
		return;
	}
	fHistory.AddLine(line, fColumns);
	fLinesScrolled++;
}


//! Scrolls the scroll region up by \a count lines (SU, LF at the bottom).
void
Terminal::_ScrollUp(int32 count)
{
	_ScrollRegionUp(fTop, fBottom, count, fTop == 0 && fScreen == &fMain);
}


void
Terminal::_ScrollDown(int32 count)
{
	_ScrollRegionDown(fTop, fBottom, count);
}


void
Terminal::_ScrollRegionUp(int32 top, int32 bottom, int32 count,
	bool toHistory)
{
	int32 height = bottom - top + 1;
	if (count <= 0 || height <= 0)
		return;
	count = std::min(count, height);
	fLastValid = false;

	if (!_FullWidthMargins()) {
		// only the cells between the left and right margin move
		int32 width = fRight - fLeft + 1;
		for (int32 row = top; row <= bottom - count; row++) {
			memcpy(_Cells(row) + fLeft, _Cells(row + count) + fLeft,
				width * sizeof(TerminalCell));
			_FixWideCharacters(row, fLeft, fRight);
		}
		for (int32 row = bottom - count + 1; row <= bottom; row++)
			_ClearCells(_Cells(row) + fLeft, width);
		_TouchRange(top, bottom);
		if (fGraphics != NULL)
			fGraphics->RegionScrolled(top, bottom, fLeft, fRight, -count);
		return;
	}

	if (top == 0 && bottom == fRows - 1) {
		// the whole screen: rotate the ring
		for (int32 i = 0; i < count; i++) {
			TerminalLine& line = _Line(0);
			if (toHistory)
				_PushLineToHistory(line);
			_ClearCells(line.cells, fColumns);
			line.flags = 0;
			line.version = ++fVersionCounter;
			fScreen->firstLine = (fScreen->firstLine + 1) % fRows;
		}
		fGeneration++;
		if (fGraphics != NULL)
			fGraphics->Scrolled(count, toHistory);
		return;
	}

	for (int32 i = 0; i < count; i++) {
		TerminalLine first = _Line(top);
		if (toHistory)
			_PushLineToHistory(first);
		for (int32 row = top; row < bottom; row++)
			_Line(row) = _Line(row + 1);
		_ClearCells(first.cells, fColumns);
		first.flags = 0;
		_Line(bottom) = first;
	}
	_TouchRange(top, bottom);
	if (fGraphics != NULL) {
		if (toHistory)
			fGraphics->Scrolled(count, true, bottom);
		else
			fGraphics->RegionScrolled(top, bottom, 0, fColumns - 1, -count);
	}
}


void
Terminal::_ScrollRegionDown(int32 top, int32 bottom, int32 count)
{
	int32 height = bottom - top + 1;
	if (count <= 0 || height <= 0)
		return;
	count = std::min(count, height);
	fLastValid = false;

	if (!_FullWidthMargins()) {
		int32 width = fRight - fLeft + 1;
		for (int32 row = bottom; row >= top + count; row--) {
			memcpy(_Cells(row) + fLeft, _Cells(row - count) + fLeft,
				width * sizeof(TerminalCell));
			_FixWideCharacters(row, fLeft, fRight);
		}
		for (int32 row = top; row < top + count; row++)
			_ClearCells(_Cells(row) + fLeft, width);
		_TouchRange(top, bottom);
		if (fGraphics != NULL)
			fGraphics->RegionScrolled(top, bottom, fLeft, fRight, count);
		return;
	}

	for (int32 i = 0; i < count; i++) {
		TerminalLine last = _Line(bottom);
		for (int32 row = bottom; row > top; row--)
			_Line(row) = _Line(row - 1);
		_ClearCells(last.cells, fColumns);
		last.flags = 0;
		_Line(top) = last;
	}
	_TouchRange(top, bottom);
	if (fGraphics != NULL)
		fGraphics->RegionScrolled(top, bottom, 0, fColumns - 1, count);
}


//!	Scrolls the region left (SL): columns move towards the left margin.
void
Terminal::_ScrollLeft(int32 count)
{
	int32 width = fRight - fLeft + 1;
	count = std::min(count, width);
	if (count <= 0)
		return;
	for (int32 row = fTop; row <= fBottom; row++) {
		TerminalCell* cells = _Cells(row);
		_FixWideCharacters(row, fLeft, fRight);
		memmove(cells + fLeft, cells + fLeft + count,
			(width - count) * sizeof(TerminalCell));
		_ClearCells(cells + fRight - count + 1, count);
		_FixWideCharacters(row, fLeft, fRight);
	}
	_TouchRange(fTop, fBottom);
}


void
Terminal::_ScrollRight(int32 count)
{
	int32 width = fRight - fLeft + 1;
	count = std::min(count, width);
	if (count <= 0)
		return;
	for (int32 row = fTop; row <= fBottom; row++) {
		TerminalCell* cells = _Cells(row);
		_FixWideCharacters(row, fLeft, fRight);
		memmove(cells + fLeft + count, cells + fLeft,
			(width - count) * sizeof(TerminalCell));
		_ClearCells(cells + fLeft, count);
		_FixWideCharacters(row, fLeft, fRight);
	}
	_TouchRange(fTop, fBottom);
}


/*!	kitty's unscroll (CSI + T): scrolls down, filling the top lines from
	the history where there is one.
*/
void
Terminal::_Unscroll(int32 count)
{
	if (count <= 0)
		return;
	count = std::min(count, fBottom - fTop + 1);
	_ScrollRegionDown(fTop, fBottom, count);

	if (fScreen != &fMain || fTop != 0 || !_FullWidthMargins())
		return;

	for (int32 row = count - 1; row >= 0; row--) {
		uint16 flags = 0;
		uint64 version = 0;
		if (!fHistory.RemoveLastLine(_Cells(row), fColumns, &flags,
				&version)) {
			break;
		}
		_Line(row).flags = flags;
		if (fLinesScrolled > 0)
			fLinesScrolled--;
	}
	_TouchRange(0, count - 1);
}


// #pragma mark - editing


void
Terminal::_InsertCharacters(int32 count)
{
	if (fCursorX < fLeft || fCursorX > fRight)
		return;
	int32 room = fRight - fCursorX + 1;
	count = std::min(count, room);
	TerminalCell* cells = _Cells(fCursorY);
	// text sizing blocks that would be split, or are more than a line high
	for (int32 x = fCursorX; x <= fRight; x++) {
		uint32 character = cells[x].character;
		TermMulticell multicell;
		if (!is_multicell(character)
			|| !GetMulticell(multicell_index(character), multicell)) {
			continue;
		}
		int32 left = x - multicell_x(character);
		if (multicell.scale > 1 || left < fCursorX
			|| left + multicell.columns - 1 > fRight - count) {
			_EraseMulticell(fCursorY, x);
		}
	}
	_FixWideCharacters(fCursorY, fCursorX, fRight);
	memmove(cells + fCursorX + count, cells + fCursorX,
		(room - count) * sizeof(TerminalCell));
	_ClearCells(cells + fCursorX, count);
	_FixWideCharacters(fCursorY, fCursorX, fRight);
	if (fRight + 1 < fColumns && cells[fRight].IsWide())
		_ClearCells(cells + fRight, 1);
	if (fGraphics != NULL)
		fGraphics->CellsChanged(fCursorY, fCursorX, fRight);
	fPendingWrap = false;
	_Touch(fCursorY);
}


void
Terminal::_DeleteCharacters(int32 count)
{
	if (fCursorX < fLeft || fCursorX > fRight)
		return;
	int32 room = fRight - fCursorX + 1;
	count = std::min(count, room);
	TerminalCell* cells = _Cells(fCursorY);
	// text sizing blocks that lose cells, or are more than a line high
	for (int32 x = fCursorX; x <= fRight; x++) {
		uint32 character = cells[x].character;
		TermMulticell multicell;
		if (!is_multicell(character)
			|| !GetMulticell(multicell_index(character), multicell)) {
			continue;
		}
		int32 left = x - multicell_x(character);
		if (multicell.scale > 1 || left <= fCursorX + count - 1)
			_EraseMulticell(fCursorY, x);
	}
	_FixWideCharacters(fCursorY, fCursorX, fCursorX + count - 1);
	_FixWideCharacters(fCursorY, fRight, fRight);
	memmove(cells + fCursorX, cells + fCursorX + count,
		(room - count) * sizeof(TerminalCell));
	_ClearCells(cells + fRight - count + 1, count);
	if (fGraphics != NULL)
		fGraphics->CellsChanged(fCursorY, fCursorX, fRight);
	fPendingWrap = false;
	_Touch(fCursorY);
}


void
Terminal::_EraseCharacters(int32 count)
{
	count = std::min(count, fColumns - fCursorX);
	_ClearLine(fCursorY, fCursorX, fCursorX + count - 1);
	fPendingWrap = false;
}


void
Terminal::_InsertLines(int32 count)
{
	if (fCursorY < fTop || fCursorY > fBottom || fCursorX < fLeft
		|| fCursorX > fRight) {
		return;
	}
	// text sizing blocks cut at this line, or at the bottom
	count = std::min(count, fBottom - fCursorY + 1);
	for (int32 x = 0; x < fColumns; x++) {
		uint32 character = _Cells(fCursorY)[x].character;
		if (is_multicell(character) && multicell_y(character) > 0)
			_EraseMulticell(fCursorY, x);
	}
	_EraseMulticells(fBottom - count + 1, fBottom, 0, fColumns - 1);
	_ScrollRegionDown(fCursorY, fBottom, count);
	_CarriageReturn();
}


void
Terminal::_DeleteLines(int32 count)
{
	if (fCursorY < fTop || fCursorY > fBottom || fCursorX < fLeft
		|| fCursorX > fRight) {
		return;
	}
	// deleted lines don't go into the history; text sizing blocks on them
	// go altogether
	_EraseMulticells(fCursorY, std::min(fBottom, fCursorY + count - 1), 0,
		fColumns - 1);
	_ScrollRegionUp(fCursorY, fBottom, count, false);
	_CarriageReturn();
}


void
Terminal::_InsertColumns(int32 count)
{
	if (fCursorY < fTop || fCursorY > fBottom || fCursorX < fLeft
		|| fCursorX > fRight) {
		return;
	}
	int32 room = fRight - fCursorX + 1;
	count = std::min(count, room);
	for (int32 row = fTop; row <= fBottom; row++) {
		TerminalCell* cells = _Cells(row);
		_FixWideCharacters(row, fCursorX, fRight);
		memmove(cells + fCursorX + count, cells + fCursorX,
			(room - count) * sizeof(TerminalCell));
		_ClearCells(cells + fCursorX, count);
	}
	_TouchRange(fTop, fBottom);
}


void
Terminal::_DeleteColumns(int32 count)
{
	if (fCursorY < fTop || fCursorY > fBottom || fCursorX < fLeft
		|| fCursorX > fRight) {
		return;
	}
	int32 room = fRight - fCursorX + 1;
	count = std::min(count, room);
	for (int32 row = fTop; row <= fBottom; row++) {
		TerminalCell* cells = _Cells(row);
		_FixWideCharacters(row, fCursorX, fRight);
		memmove(cells + fCursorX, cells + fCursorX + count,
			(room - count) * sizeof(TerminalCell));
		_ClearCells(cells + fRight - count + 1, count);
	}
	_TouchRange(fTop, fBottom);
}


void
Terminal::_EraseInDisplay(int32 mode, bool selective)
{
	fPendingWrap = false;
	switch (mode) {
		case 0:
			_ClearLine(fCursorY, fCursorX, fColumns - 1, selective);
			if (fCursorX == 0 && !selective)
				_Line(fCursorY).flags = 0;
			if (fCursorY + 1 < fRows)
				_ClearRows(fCursorY + 1, fRows - 1, selective);
			if (fGraphics != NULL && !selective)
				fGraphics->RowsCleared(fCursorY, fRows - 1);
			break;
		case 1:
			if (fCursorY > 0)
				_ClearRows(0, fCursorY - 1, selective);
			_ClearLine(fCursorY, 0, fCursorX, selective);
			if (fGraphics != NULL && !selective)
				fGraphics->RowsCleared(0, fCursorY);
			break;
		case 2:
			_ClearRows(0, fRows - 1, selective);
			if (fGraphics != NULL && !selective)
				fGraphics->ScreenCleared();
			_ClearExtraCursors();
			break;
		case 3:
			// the history (xterm)
			_ClearExtraCursors();
			if (fScreen == &fMain) {
				fHistory.Clear();
				if (fGraphics != NULL)
					fGraphics->HistoryCleared();
			}
			_Changed();
			break;
		case 22:
		{
			// kitty: scroll the screen's contents into the history, then
			// clear it
			if (fScreen == &fMain) {
				int32 last = fRows - 1;
				while (last >= 0 && LineLength(last) == 0)
					last--;
				for (int32 row = 0; row <= last; row++)
					_PushLineToHistory(_Line(row));
			}
			_ClearRows(0, fRows - 1);
			if (fGraphics != NULL)
				fGraphics->ScreenCleared();
			_ClearExtraCursors();
			break;
		}
	}
}


void
Terminal::_EraseInLine(int32 mode, bool selective)
{
	fPendingWrap = false;
	switch (mode) {
		case 0:
			_ClearLine(fCursorY, fCursorX, fColumns - 1, selective);
			_Line(fCursorY).flags &= ~LINE_WRAPPED;
			break;
		case 1:
			_ClearLine(fCursorY, 0, fCursorX, selective);
			break;
		case 2:
			_ClearLine(fCursorY, 0, fColumns - 1, selective);
			_Line(fCursorY).flags &= ~LINE_WRAPPED;
			break;
	}
}


// #pragma mark - state


void
Terminal::_SaveCursor()
{
	SavedCursor& saved = fScreen->savedCursor;
	saved.x = fCursorX;
	saved.y = fCursorY;
	saved.pendingWrap = fPendingWrap;
	saved.style = fStyle;
	saved.hyperlink = fHyperLink;
	saved.charsets = fCharsets;
	saved.origin = fModes.origin;
	saved.autoWrap = fModes.autoWrap;
	saved.valid = true;
}


void
Terminal::_RestoreCursor()
{
	SavedCursor& saved = fScreen->savedCursor;
	if (!saved.valid) {
		// like a reset of the saved state
		fModes.origin = false;
		fStyle = CellStyle();
		fHyperLink = 0;
		memset(&fCharsets, 0, sizeof(fCharsets));
		for (int32 i = 0; i < 4; i++)
			fCharsets.designations[i] = 'B';
		fCharsets.gr = 1;
		fCharsets.singleShift = -1;
		_SetCursor(0, 0);
		return;
	}

	fStyle = saved.style;
	fHyperLink = saved.hyperlink;
	fCharsets = saved.charsets;
	fModes.origin = saved.origin;
	fModes.autoWrap = saved.autoWrap;
	_SetCursor(saved.x, saved.y);
	fPendingWrap = saved.pendingWrap && fCursorX == saved.x;
}


void
Terminal::_SoftReset()
{
	// DECSTR
	fModes.insert = false;
	fModes.origin = false;
	fModes.autoWrap = true;
	fModes.cursorVisible = true;
	fModes.cursorKeys = false;
	fModes.applicationKeypad = false;
	fModes.reverseWrap = false;
	fModes.leftRightMargins = false;
	fTop = 0;
	fBottom = fRows - 1;
	fLeft = 0;
	fRight = fColumns - 1;
	fStyle = CellStyle();
	fHyperLink = 0;
	memset(&fCharsets, 0, sizeof(fCharsets));
	for (int32 i = 0; i < 4; i++)
		fCharsets.designations[i] = 'B';
	fCharsets.gr = 1;
	fCharsets.singleShift = -1;
	fScreen->savedCursor.valid = false;
	fPendingWrap = false;
	fCursorShape = CURSOR_SHAPE_DEFAULT;
	fCursorBlinkSet = false;
	_Changed();
}


void
Terminal::FullReset()
{
	if (fScreen != &fMain)
		_SwitchScreen(false, false, false);

	fParser.Reset();
	memset(&fModes, 0, sizeof(fModes));
	fModes.autoWrap = true;
	fModes.autoRepeat = true;
	fModes.cursorVisible = true;
	fSavedModes.clear();
	_SoftReset();
	_ClearExtraCursors();
	memset(fExtraCursorColors, 0, sizeof(fExtraCursorColors));

	for (int32 i = 0; i < COLOR_COUNT; i++)
		fColors[i] = fDefaultColors[i];
	fColorsGeneration++;
	fColorStack.clear();
	fSGRStack.clear();

	fMain.keyboardStack.clear();
	fMain.keyboardFlags = 0;
	fAlternate.keyboardStack.clear();
	fAlternate.keyboardFlags = 0;
	fMain.pointerShapes.clear();
	fAlternate.pointerShapes.clear();
	fMain.savedCursor.valid = false;
	fAlternate.savedCursor.valid = false;

	_ResetTabStops();
	_ClearRows(0, fRows - 1);
	fHistory.Clear();
	if (fGraphics != NULL)
		fGraphics->Reset();
	_SetCursor(0, 0);
	fLastValid = false;
	fTitleStack.clear();
	fSynchronizedSince = 0;

	if (fListener != NULL) {
		fListener->TerminalColorsChanged();
		fListener->TerminalModesChanged();
		fListener->TerminalPointerShapeChanged("");
	}
}


void
Terminal::ClearHistory()
{
	fHistory.Clear();
	if (fGraphics != NULL)
		fGraphics->HistoryCleared();
	_Changed();
}


void
Terminal::ClearAll()
{
	// keep the cursor's line, at the top
	if (fCursorY > 0 && fScreen == &fMain) {
		TerminalLine& line = _Line(fCursorY);
		std::swap(_Line(0), line);
	}
	if (fRows > 1)
		_ClearRows(1, fRows - 1);
	_Touch(0);
	fCursorY = 0;
	fHistory.Clear();
	if (fGraphics != NULL)
		fGraphics->ScreenCleared();
	_Changed();
}


void
Terminal::_SwitchScreen(bool alternate, bool clear, bool saveCursor)
{
	Screen* target = alternate ? &fAlternate : &fMain;
	if (fScreen == target) {
		if (clear && alternate)
			_ClearRows(0, fRows - 1);
		return;
	}

	if (saveCursor && alternate)
		_SaveCursor();

	fScreen = target;
	fLastValid = false;
	_ClearExtraCursors();

	if (alternate && clear) {
		CellStyle style = fStyle;
		fStyle = CellStyle();
		_ClearRows(0, fRows - 1);
		fStyle = style;
	}

	if (saveCursor && !alternate)
		_RestoreCursor();

	_TouchRange(0, fRows - 1);
	if (fGraphics != NULL)
		fGraphics->ScreenSwitched(alternate);
	if (fListener != NULL) {
		fListener->TerminalModesChanged();
		fListener->TerminalPointerShapeChanged(PointerShape());
	}
}


void
Terminal::_ResetTabStops()
{
	fTabStops.assign(fColumns, false);
	for (int32 i = 8; i < fColumns; i += 8)
		fTabStops[i] = true;
}


void
Terminal::_SetTabStop()
{
	if (fCursorX < fColumns)
		fTabStops[fCursorX] = true;
}


void
Terminal::_ClearTabStops(int32 mode)
{
	if (mode == 0) {
		if (fCursorX < fColumns)
			fTabStops[fCursorX] = false;
	} else if (mode == 3)
		fTabStops.assign(fColumns, false);
}


void
Terminal::_SetScrollRegion(int32 top, int32 bottom)
{
	if (top < 0)
		top = 0;
	if (bottom < 0 || bottom >= fRows)
		bottom = fRows - 1;
	if (top >= bottom)
		return;
	fTop = top;
	fBottom = bottom;
	_MoveCursorTo(0, 0);
}


void
Terminal::_SetHorizontalMargins(int32 left, int32 right)
{
	if (left < 0)
		left = 0;
	if (right < 0 || right >= fColumns)
		right = fColumns - 1;
	if (left >= right)
		return;
	fLeft = left;
	fRight = right;
	_MoveCursorTo(0, 0);
}


// #pragma mark - resizing


namespace {

struct ReflowLine {
	std::vector<TerminalCell>	cells;
	uint16						flags;
};

}	// namespace


/*!	Changes the size of the screens. On the main screen, lines are wrapped
	anew at the new width (history included), keeping the cursor where its
	text is.
*/
status_t
Terminal::Resize(int32 columns, int32 rows)
{
	columns = std::max(1, std::min(columns, kMaxColumns));
	rows = std::max(1, std::min(rows, kMaxRows));
	if (columns == fColumns && rows == fRows)
		return B_OK;

	int32 oldColumns = fColumns;
	int32 oldRows = fRows;
	Screen* active = fScreen;
	_ClearExtraCursors();

	// --- the main screen: logical lines, history first
	std::vector<ReflowLine> logical;
	std::vector<int64> oldRowLine;		// for each old row: its logical line
	std::vector<int32> oldRowOffset;	// and where in it the row started
	int32 oldHistorySize = fHistory.Size();
	int64 cursorLine = -1;
	int32 cursorOffset = 0;
	int64 savedLine = -1;
	int32 savedOffset = 0;
	{
		fScreen = &fMain;
		TerminalCell* cells = new(std::nothrow) TerminalCell[oldColumns];
		if (cells == NULL) {
			fScreen = active;
			return B_NO_MEMORY;
		}

		int32 historySize = fHistory.Size();
		int32 lastRow = oldRows - 1;
		int32 mainCursorY = active == &fMain ? fCursorY
			: (fMain.savedCursor.valid ? fMain.savedCursor.y : 0);
		int32 mainCursorX = active == &fMain ? fCursorX
			: (fMain.savedCursor.valid ? fMain.savedCursor.x : 0);
		while (lastRow > mainCursorY && LineLength(lastRow) == 0)
			lastRow--;

		bool continuing = false;
		for (int32 row = -historySize; row <= lastRow; row++) {
			uint16 flags = 0;
			GetLine(row, cells, &flags);
			if (columns != oldColumns) {
				// text sizing blocks cannot be wrapped again
				_FlattenMulticells(cells, oldColumns);
			}
			if (!continuing) {
				logical.push_back(ReflowLine());
				logical.back().flags = flags & ~LINE_WRAPPED;
			}
			ReflowLine& line = logical.back();

			int32 length = oldColumns;
			if ((flags & LINE_WRAPPED) == 0) {
				while (length > 0 && cells[length - 1].IsEmpty()
					&& cells[length - 1].style == CellStyle()) {
					length--;
				}
			}
			if (row == mainCursorY) {
				cursorLine = logical.size() - 1;
				cursorOffset = line.cells.size() + mainCursorX;
			}
			if (fMain.savedCursor.valid && active == &fMain
				&& row == fMain.savedCursor.y) {
				savedLine = logical.size() - 1;
				savedOffset = line.cells.size() + fMain.savedCursor.x;
			}
			oldRowLine.push_back(logical.size() - 1);
			oldRowOffset.push_back(line.cells.size());
			line.cells.insert(line.cells.end(), cells, cells + length);
			continuing = (flags & LINE_WRAPPED) != 0;
		}
		delete[] cells;
	}

	// --- wrap the logical lines anew
	std::vector<TerminalLine> wrapped;
	std::vector<TerminalCell*> allocated;
	int64 newCursorRow = 0;
	int32 newCursorX = 0;
	int64 newSavedRow = -1;
	int32 newSavedX = 0;
	std::vector<std::vector<std::pair<int64, int32> > > wrappedStarts(
		logical.size());
	for (size_t i = 0; i < logical.size(); i++) {
		ReflowLine& line = logical[i];
		size_t position = 0;
		bool first = true;
		do {
			wrappedStarts[i].push_back(std::make_pair((int64)wrapped.size(),
				(int32)position));
			TerminalCell* cells = new(std::nothrow) TerminalCell[columns];
			if (cells == NULL)
				break;
			allocated.push_back(cells);
			for (int32 x = 0; x < columns; x++) {
				cells[x].character = 0;
				cells[x].hyperlink = 0;
				cells[x].style = CellStyle();
			}

			size_t start = position;
			int32 x = 0;
			while (position < line.cells.size() && x < columns) {
				const TerminalCell& cell = line.cells[position];
				if (cell.IsWide() && x == columns - 1)
					break;	// continues on the next line
				cells[x++] = cell;
				position++;
				if (cell.IsWide() && position < line.cells.size()) {
					cells[x++] = line.cells[position++];
				}
			}

			TerminalLine newLine;
			newLine.cells = cells;
			newLine.version = ++fVersionCounter;
			newLine.flags = first ? line.flags : 0;
			if (position < line.cells.size())
				newLine.flags |= LINE_WRAPPED;

			if ((int64)i == cursorLine && cursorOffset >= (int32)start
				&& (cursorOffset < (int32)position
					|| position >= line.cells.size())) {
				newCursorRow = wrapped.size();
				newCursorX = std::min(cursorOffset - (int32)start,
					columns - 1);
			}
			if ((int64)i == savedLine && savedOffset >= (int32)start
				&& (savedOffset < (int32)position
					|| position >= line.cells.size())) {
				newSavedRow = wrapped.size();
				newSavedX = std::min(savedOffset - (int32)start, columns - 1);
			}
			wrapped.push_back(newLine);
			first = false;
		} while (position < line.cells.size());
	}

	// --- allocate the new screens
	Screen oldMain = fMain;
	Screen oldAlternate = fAlternate;
	fMain.lines = NULL;
	fMain.storage = NULL;
	fAlternate.lines = NULL;
	fAlternate.storage = NULL;
	fColumns = columns;
	fRows = rows;
	status_t status = _InitScreen(fMain, columns, rows);
	if (status == B_OK)
		status = _InitScreen(fAlternate, columns, rows);
	if (status != B_OK) {
		for (size_t i = 0; i < allocated.size(); i++)
			delete[] allocated[i];
		_FreeScreen(fMain);
		_FreeScreen(fAlternate);
		fMain.lines = oldMain.lines;
		fMain.storage = oldMain.storage;
		fAlternate.lines = oldAlternate.lines;
		fAlternate.storage = oldAlternate.storage;
		fColumns = oldColumns;
		fRows = oldRows;
		fScreen = active;
		return status;
	}

	// main screen: the last lines, as long as the cursor's line is one
	int64 total = wrapped.size();
	int64 firstOnScreen = std::max((int64)0, total - rows);
	if (newCursorRow < firstOnScreen)
		firstOnScreen = newCursorRow;

	// where each old row went, for the images anchored to them
	std::vector<int64> lineMap(oldRowLine.size());
	for (size_t row = 0; row < oldRowLine.size(); row++) {
		const std::vector<std::pair<int64, int32> >& starts
			= wrappedStarts[oldRowLine[row]];
		int64 index = starts.empty() ? 0 : starts[0].first;
		for (size_t j = 0; j < starts.size(); j++) {
			if (starts[j].second <= oldRowOffset[row])
				index = starts[j].first;
		}
		lineMap[row] = index;
	}
	int64 oldBase = (int64)fLinesScrolled - oldHistorySize;
	int64 linesScrolled = std::max((int64)0, oldBase + firstOnScreen);
	int64 newBase = linesScrolled - firstOnScreen;

	fHistory.Clear();
	fScreen = &fMain;
	for (int64 i = 0; i < firstOnScreen; i++)
		fHistory.AddLine(wrapped[i], columns);
	fLinesScrolled = linesScrolled;
	for (int64 i = firstOnScreen; i < total && i - firstOnScreen < rows; i++) {
		TerminalLine& line = fMain.lines[i - firstOnScreen];
		memcpy(line.cells, wrapped[i].cells, sizeof(TerminalCell) * columns);
		line.flags = wrapped[i].flags;
	}
	for (size_t i = 0; i < allocated.size(); i++)
		delete[] allocated[i];

	// the alternate screen: cut or padded, not wrapped
	for (int32 row = 0; row < std::min(rows, oldRows); row++) {
		int32 index = (oldAlternate.firstLine + row) % oldRows;
		memcpy(fAlternate.lines[row].cells, oldAlternate.lines[index].cells,
			sizeof(TerminalCell) * std::min(columns, oldColumns));
		fAlternate.lines[row].flags = oldAlternate.lines[index].flags;
		if (columns != oldColumns) {
			_FlattenMulticells(fAlternate.lines[row].cells,
				std::min(columns, oldColumns));
		}
	}

	fMain.savedCursor = oldMain.savedCursor;
	fMain.keyboardStack = oldMain.keyboardStack;
	fMain.keyboardFlags = oldMain.keyboardFlags;
	fMain.pointerShapes = oldMain.pointerShapes;
	fAlternate.savedCursor = oldAlternate.savedCursor;
	fAlternate.keyboardStack = oldAlternate.keyboardStack;
	fAlternate.keyboardFlags = oldAlternate.keyboardFlags;
	fAlternate.pointerShapes = oldAlternate.pointerShapes;

	delete[] oldMain.lines;
	delete[] oldMain.storage;
	delete[] oldAlternate.lines;
	delete[] oldAlternate.storage;

	fScreen = active;

	// the cursor
	if (active == &fMain) {
		fCursorY = (int32)(newCursorRow - firstOnScreen);
		fCursorX = newCursorX;
		if (fMain.savedCursor.valid && newSavedRow >= 0) {
			fMain.savedCursor.y = std::max((int64)0,
				newSavedRow - firstOnScreen);
			fMain.savedCursor.x = newSavedX;
		}
	} else {
		if (fMain.savedCursor.valid) {
			fMain.savedCursor.y = (int32)(newCursorRow - firstOnScreen);
			fMain.savedCursor.x = newCursorX;
		}
	}
	fCursorX = std::max((int32)0, std::min(fCursorX, columns - 1));
	fCursorY = std::max((int32)0, std::min(fCursorY, rows - 1));
	fPendingWrap = false;
	for (Screen* screen = &fMain; screen != NULL;
			screen = screen == &fMain ? &fAlternate : NULL) {
		SavedCursor& saved = screen->savedCursor;
		saved.x = std::max((int32)0, std::min(saved.x, columns - 1));
		saved.y = std::max((int32)0, std::min(saved.y, rows - 1));
	}

	fTop = 0;
	fBottom = rows - 1;
	fLeft = 0;
	fRight = columns - 1;
	fLastValid = false;

	std::vector<bool> tabStops = fTabStops;
	_ResetTabStops();
	for (int32 i = 0; i < std::min(columns, oldColumns); i++)
		fTabStops[i] = tabStops[i];

	if (fGraphics != NULL) {
		fGraphics->Reflowed(lineMap, oldBase, newBase);
		fGraphics->Resized(oldColumns, oldRows);
	}

	fGeneration++;

	if (fModes.inBandResize) {
		_Reply("\x1b[48;%d;%d;%d;%dt", rows, columns, rows * fCellHeight,
			columns * fCellWidth);
	}
	return B_OK;
}
