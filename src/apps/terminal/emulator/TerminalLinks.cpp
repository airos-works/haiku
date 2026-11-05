/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Finding the link under the pointer.

	An OSC 8 hyperlink names its address; the cells around the pointer that
	carry the same address are the link. A plain address has to be found in
	the text, and a long one is rarely on one row:

	- The terminal wrapped it: the row is marked as continuing on the next.
	- The program broke it itself, the way Claude Code (Ink), codex
	  (ratatui) and most other full screen programs lay out their text: hard
	  line breaks, and the continuation indented like the rest of the
	  paragraph. Ink breaks a word too long for the line at the right edge;
	  codex breaks addresses after "://" and "-".
	- tmux drew it: a pane's rows end at the pane's border ("│") or at the
	  margin, and rows of a pane next to the left border start after it.
	  Tables (Claude Code draws markdown tables with "│") are the same.

	So an address that reaches the right edge of its column on one row
	continues on the next row of that column (between the same borders), at
	its indentation, if it starts with address characters there; and one
	that stops short of the edge after a character no address ends with
	("-", "=", "&", "://"...) continues there too, when what follows would
	not have fitted. The address then runs from a known scheme to the last
	address character, without punctuation ending a sentence and without
	closing brackets it did not open.
*/


#include "TerminalLinks.h"

#include <ctype.h>
#include <string.h>


// how many rows a link is followed over, at most
static const int32 kMaxLinkRows = 64;

static const uint32 kBlank = ' ';
static const uint32 kNoCharacter = 0xffffffff;
	// the second cell of a wide character


namespace {


struct Row {
	int32				row;
	std::vector<uint32>	characters;		// one per cell
	std::vector<uint32>	links;			// OSC 8 link id per cell
	bool				wrapped;
};


struct Run {
	int32	row;
	int32	start;
	int32	end;	// exclusive
};


bool
is_border(uint32 c)
{
	switch (c) {
		case '|':
		case 0x2502:	// │
		case 0x2503:	// ┃
		case 0x2506:	// ┆
		case 0x2507:	// ┇
		case 0x250a:	// ┊
		case 0x250b:	// ┋
		case 0x2551:	// ║
		case 0x2595:	// ▕
		case 0x258f:	// ▏
			return true;
	}
	return false;
}


bool
is_blank(uint32 c)
{
	return c == kBlank || c == 0 || c == '\t' || c == 0xa0;
}


/*!	Characters an address can contain. Outside ASCII: letters of the
	scripts used in domain names and paths, not the symbols programs draw
	around them (bullets, arrows, box drawing, quotation marks).
*/
bool
is_url_character(uint32 c)
{
	if (c < 0x80) {
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
			|| (c >= '0' && c <= '9'))
			return true;
		return c != 0 && strchr("-._~:/?#[]@!$&'()*+,;=%", (int)c) != NULL;
	}
	if (c < 0xc0)
		return false;
	if (c < 0x2000)
		return c != 0xd7 && c != 0xf7;
	if (c >= 0x3040 && c <= 0x9fff)
		return true;
	if (c >= 0xac00 && c <= 0xd7af)
		return true;
	if (c >= 0x10000 && c < 0x1f000)
		return true;
	return false;
}


bool
read_row(const Terminal& terminal, int32 row, Row& _row)
{
	if (row < -terminal.HistorySize() || row >= terminal.Rows())
		return false;

	int32 columns = terminal.Columns();
	_row.row = row;
	_row.characters.assign(columns, kBlank);
	_row.links.assign(columns, 0);
	_row.wrapped = (terminal.LineFlags(row) & LINE_WRAPPED) != 0;

	for (int32 x = 0; x < columns; x++) {
		TerminalCell cell;
		if (!terminal.GetCell(row, x, cell))
			continue;
		_row.links[x] = cell.hyperlink;
		if (cell.IsWideTail()) {
			_row.characters[x] = kNoCharacter;
			continue;
		}
		uint32 codePoints[4];
		if (cell.IsEmpty()
			|| terminal.GetCharacter(cell.character, codePoints, 4) < 1)
			continue;
		_row.characters[x] = codePoints[0];
	}
	// the second cell of a wide character goes with the first
	for (int32 x = 1; x < columns; x++) {
		if (_row.characters[x] == kNoCharacter)
			_row.links[x] = _row.links[x - 1];
	}
	return true;
}


bool
url_cell(const Row& row, int32 x)
{
	uint32 c = row.characters[x];
	if (c == kNoCharacter)
		return x > 0 && is_url_character(row.characters[x - 1]);
	return is_url_character(c);
}


//!	The column the content around \a x ends at: a border, or the margin.
int32
right_bound(const Row& row, int32 x)
{
	int32 columns = row.characters.size();
	for (; x < columns; x++) {
		if (is_border(row.characters[x]))
			return x;
	}
	return columns;
}


//!	The first column of the content around \a x: after a border, or 0.
int32
left_bound(const Row& row, int32 x)
{
	for (x--; x >= 0; x--) {
		if (is_border(row.characters[x]))
			return x + 1;
	}
	return 0;
}


bool
same_borders(const Row& a, const Row& b, int32 left, int32 right)
{
	int32 columns = a.characters.size();
	if (left > 0 && a.characters[left - 1] != b.characters[left - 1])
		return false;
	if (right < columns && a.characters[right] != b.characters[right])
		return false;
	return true;
}


bool
blank_between(const Row& row, int32 from, int32 to)
{
	for (int32 x = from; x < to; x++) {
		if (!is_blank(row.characters[x]))
			return false;
	}
	return true;
}


//!	Characters after which a program breaks an address that is too long
//!	for the rest of the row, and that no complete address ends with.
bool
is_break_character(uint32 c)
{
	return c != 0 && strchr("-=&?#:_%+~", (int)c) != NULL;
}


/*!	The run of address characters a link continues with on the row after
	\a run, if it does (see the comment at the top).
*/
bool
continues_on_next_row(const Row& a, const Run& run, const Row& b, Run& _next)
{
	int32 columns = a.characters.size();

	// wrapped by the terminal
	if (a.wrapped && run.end == columns) {
		if (!url_cell(b, 0))
			return false;
		_next.row = b.row;
		_next.start = 0;
		_next.end = 1;
		while (_next.end < columns && url_cell(b, _next.end))
			_next.end++;
		return true;
	}

	// broken by the program, or by tmux at a pane's edge
	int32 left = left_bound(a, run.start);
	int32 right = right_bound(a, run.end);
	if (!blank_between(a, run.end, right) || !same_borders(a, b, left, right))
		return false;

	int32 start = left;
	while (start < right && is_blank(b.characters[start]))
		start++;
	if (start >= right || start > run.start || !url_cell(b, start))
		return false;
	int32 end = start + 1;
	while (end < right && url_cell(b, end))
		end++;

	// Ink keeps the last column of a row free for the cursor in its text
	// fields (the prompt), and so breaks the words in them one short
	if (run.end < right && !(right == columns && run.end == right - 1)) {
		// It stops short of the edge: only where a program breaks
		// addresses, and only if the rest would not have fitted.
		uint32 last = a.characters[run.end - 1];
		bool scheme = run.end - run.start >= 3
			&& a.characters[run.end - 1] == '/'
			&& a.characters[run.end - 2] == '/'
			&& a.characters[run.end - 3] == ':';
		if (!scheme && !is_break_character(last))
			return false;
		int32 word = start;
		while (word < right && !is_blank(b.characters[word]))
			word++;
		if (word - start <= right - run.end)
			return false;
	}

	_next.row = b.row;
	_next.start = start;
	_next.end = end;
	return true;
}


//!	The run of address characters that ends the content of \a row's column
//!	from \a left on.
bool
last_run(const Row& row, int32 left, Run& _run)
{
	int32 right = right_bound(row, left);
	int32 end = right;
	while (end > left && is_blank(row.characters[end - 1]))
		end--;
	if (end <= left || !url_cell(row, end - 1))
		return false;
	int32 start = end - 1;
	while (start > left && url_cell(row, start - 1))
		start--;
	_run.row = row.row;
	_run.start = start;
	_run.end = end;
	return true;
}


struct Scheme {
	const char*	name;
	bool		slashes;
};


const Scheme kSchemes[] = {
	{ "https", true },
	{ "http", true },
	{ "ftp", true },
	{ "ftps", true },
	{ "sftp", true },
	{ "ssh", true },
	{ "file", true },
	{ "git", true },
	{ "mailto", false },
};


//!	The length of the scheme starting at \a index, including its ":" (and
//!	"//"), or 0.
size_t
scheme_at(const std::vector<uint32>& text, size_t index)
{
	if (index > 0 && text[index - 1] < 0x80
		&& (isalnum((int)text[index - 1]) || text[index - 1] == '+'
			|| text[index - 1] == '.' || text[index - 1] == '-'))
		return 0;

	for (size_t i = 0; i < sizeof(kSchemes) / sizeof(kSchemes[0]); i++) {
		const char* name = kSchemes[i].name;
		size_t length = strlen(name);
		std::string tail = kSchemes[i].slashes ? "://" : ":";
		if (index + length + tail.size() >= text.size())
			continue;
		bool match = true;
		for (size_t j = 0; j < length && match; j++) {
			uint32 c = text[index + j];
			if (c >= 'A' && c <= 'Z')
				c += 'a' - 'A';
			match = c == (uint32)name[j];
		}
		for (size_t j = 0; j < tail.size() && match; j++)
			match = text[index + length + j] == (uint32)tail[j];
		if (match)
			return length + tail.size();
	}
	return 0;
}


/*!	The end of the address starting at \a start: before punctuation that
	ends a sentence, and before closing brackets the address did not open.
*/
size_t
trim_url(const std::vector<uint32>& text, size_t start, size_t end)
{
	while (end > start) {
		uint32 last = text[end - 1];
		if (last < 0x80 && strchr(".,;:!?'\"*", (int)last) != NULL) {
			end--;
			continue;
		}
		uint32 open = 0;
		if (last == ')')
			open = '(';
		else if (last == ']')
			open = '[';
		if (open == 0)
			break;
		int32 depth = 0;
		for (size_t i = start; i < end; i++) {
			if (text[i] == open)
				depth++;
			else if (text[i] == last)
				depth--;
		}
		if (depth >= 0)
			break;
		end--;
	}
	return end;
}


void
append_utf8(std::string& text, uint32 c)
{
	if (c < 0x80)
		text += (char)c;
	else if (c < 0x800) {
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
set_range(TermLinkRange& link, const std::vector<Run>& runs)
{
	link.segments.clear();
	for (size_t i = 0; i < runs.size(); i++) {
		TermLinkSegment segment;
		segment.start = TermPoint(runs[i].start, runs[i].row);
		segment.end = TermPoint(runs[i].end, runs[i].row);
		link.segments.push_back(segment);
	}
	link.start = link.segments.front().start;
	link.end = link.segments.back().end;
}


// #pragma mark - OSC 8


bool
same_link(const Terminal& terminal, uint32 a, uint32 b)
{
	if (a == b)
		return a != 0;
	if (a == 0 || b == 0)
		return false;
	std::string uriA, idA, uriB, idB;
	return terminal.GetHyperLink(a, uriA, idA)
		&& terminal.GetHyperLink(b, uriB, idB) && uriA == uriB && idA == idB;
}


bool
find_hyperlink(const Terminal& terminal, const Row& row, int32 x,
	TermLinkRange& _link)
{
	uint32 id = row.links[x];
	if (id == 0 || !terminal.GetHyperLink(id, _link.uri, _link.identifier))
		return false;
	_link.hyperlink = id;

	int32 columns = row.characters.size();
	Run run = { row.row, x, x + 1 };
	while (run.start > 0 && same_link(terminal, row.links[run.start - 1], id))
		run.start--;
	while (run.end < columns && same_link(terminal, row.links[run.end], id))
		run.end++;

	std::vector<Run> runs(1, run);

	// The rows before and after, where the link goes on after blanks (the
	// indentation of a program that broke it into lines).
	Row current = row;
	for (int32 i = 0; i < kMaxLinkRows; i++) {
		Run first = runs.front();
		Row previous;
		if (!blank_between(current, left_bound(current, first.start),
				first.start)
			|| !read_row(terminal, current.row - 1, previous))
			break;
		int32 right = right_bound(previous, left_bound(previous, first.start));
		int32 end = right;
		while (end > 0 && is_blank(previous.characters[end - 1])
				&& previous.links[end - 1] == 0)
			end--;
		if (end == 0 || !same_link(terminal, previous.links[end - 1], id))
			break;
		Run before = { previous.row, end - 1, end };
		while (before.start > 0
			&& same_link(terminal, previous.links[before.start - 1], id))
			before.start--;
		runs.insert(runs.begin(), before);
		current = previous;
	}

	current = row;
	for (int32 i = 0; i < kMaxLinkRows; i++) {
		Run last = runs.back();
		Row next;
		if (!blank_between(current, last.end, right_bound(current, last.end))
			|| !read_row(terminal, current.row + 1, next))
			break;
		int32 start = left_bound(next, last.end > 0 ? last.end - 1 : 0);
		while (start < columns && is_blank(next.characters[start])
				&& next.links[start] == 0)
			start++;
		if (start >= columns || !same_link(terminal, next.links[start], id))
			break;
		Run after = { next.row, start, start + 1 };
		while (after.end < columns
			&& same_link(terminal, next.links[after.end], id))
			after.end++;
		runs.push_back(after);
		current = next;
	}

	set_range(_link, runs);
	return true;
}


}	// namespace


// #pragma mark -


bool
FindTerminalLink(const Terminal& terminal, TermPoint position,
	TermLinkRange& _link)
{
	Row row;
	if (position.x < 0 || position.x >= terminal.Columns()
		|| !read_row(terminal, position.y, row))
		return false;

	_link = TermLinkRange();
	if (row.links[position.x] != 0)
		return find_hyperlink(terminal, row, position.x, _link);

	if (!url_cell(row, position.x))
		return false;

	// the run of address characters under the pointer
	int32 columns = terminal.Columns();
	Run run = { row.row, position.x, position.x + 1 };
	while (run.start > 0 && url_cell(row, run.start - 1))
		run.start--;
	while (run.end < columns && url_cell(row, run.end))
		run.end++;

	std::vector<Run> runs(1, run);
	size_t pointerRun = 0;

	// the rows it continues from
	Row current = row;
	for (int32 i = 0; i < kMaxLinkRows; i++) {
		const Run first = runs.front();
		Row previous;
		if (!read_row(terminal, current.row - 1, previous))
			break;
		Run before;
		Run check;
		if (previous.wrapped && first.start == 0) {
			if (!last_run(previous, left_bound(previous, columns - 1), before)
				|| before.end != columns)
				break;
		} else {
			int32 left = left_bound(current, first.start);
			if (!blank_between(current, left, first.start)
				|| !last_run(previous, left, before))
				break;
		}
		if (!continues_on_next_row(previous, before, current, check)
			|| check.start != first.start)
			break;
		runs.insert(runs.begin(), before);
		pointerRun++;
		current = previous;
	}

	// and the rows it continues on
	current = row;
	for (int32 i = 0; i < kMaxLinkRows; i++) {
		Run last = runs.back();
		Row next;
		Run after;
		if (!read_row(terminal, current.row + 1, next)
			|| !continues_on_next_row(current, last, next, after))
			break;
		runs.push_back(after);
		current = next;
	}

	// the text, with where each character came from
	std::vector<uint32> text;
	std::vector<TermPoint> cells;
	std::vector<int32> widths;
	size_t pointer = 0;
	for (size_t i = 0; i < runs.size(); i++) {
		Row content;
		if (i == pointerRun)
			content = row;
		else
			read_row(terminal, runs[i].row, content);
		for (int32 x = runs[i].start; x < runs[i].end; x++) {
			uint32 c = content.characters[x];
			if (i == pointerRun && x == position.x)
				pointer = text.size();
			if (c == kNoCharacter) {
				if (i == pointerRun && x == position.x && pointer > 0)
					pointer--;
				continue;
			}
			text.push_back(c);
			cells.push_back(TermPoint(x, runs[i].row));
			widths.push_back(x + 1 < columns
				&& content.characters[x + 1] == kNoCharacter ? 2 : 1);
		}
	}

	// the address the pointer is in: from the last scheme before it, to
	// the next scheme or the end of the address characters
	size_t start = text.size();
	size_t schemeLength = 0;
	for (size_t i = 0; i <= pointer && i < text.size(); i++) {
		size_t length = scheme_at(text, i);
		if (length > 0) {
			start = i;
			schemeLength = length;
		}
	}
	if (start == text.size())
		return false;
	size_t end = text.size();
	for (size_t i = start + schemeLength; i < text.size(); i++) {
		if (scheme_at(text, i) > 0) {
			end = i;
			break;
		}
	}
	end = trim_url(text, start, end);
	if (end <= start + schemeLength || pointer >= end)
		return false;

	_link.hyperlink = 0;
	_link.uri.clear();
	for (size_t i = start; i < end; i++)
		append_utf8(_link.uri, text[i]);

	// the cells it covers, one run per row
	std::vector<Run> linkRuns;
	for (size_t i = start; i < end; i++) {
		const TermPoint& cell = cells[i];
		int32 cellEnd = cell.x + widths[i];
		if (!linkRuns.empty() && linkRuns.back().row == cell.y)
			linkRuns.back().end = cellEnd;
		else {
			Run segment = { cell.y, cell.x, cellEnd };
			linkRuns.push_back(segment);
		}
	}
	set_range(_link, linkRuns);
	return true;
}
