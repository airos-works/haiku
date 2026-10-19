/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


//!	Tests of Terminal's emulator: feed it output, check screen and answers.


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include <zlib.h>

#include "Base64.h"
#include "Terminal.h"
#include "TerminalGraphics.h"


static int sFailures = 0;
static int sChecks = 0;


#define CHECK(condition) \
	do { \
		sChecks++; \
		if (!(condition)) { \
			sFailures++; \
			fprintf(stderr, "%s:%d: FAILED: %s\n", __FILE__, __LINE__, \
				#condition); \
		} \
	} while (false)

#define CHECK_EQUAL(actual, expected) \
	do { \
		sChecks++; \
		std::string a_ = (actual); \
		std::string e_ = (expected); \
		if (a_ != e_) { \
			sFailures++; \
			fprintf(stderr, "%s:%d: FAILED: %s\n  expected \"%s\"\n  got      " \
				"\"%s\"\n", __FILE__, __LINE__, #actual, \
				Escape(e_).c_str(), Escape(a_).c_str()); \
		} \
	} while (false)


static std::string
Escape(const std::string& text)
{
	std::string result;
	for (size_t i = 0; i < text.size(); i++) {
		unsigned char c = text[i];
		if (c == 0x1b)
			result += "\\e";
		else if (c < 0x20 || c == 0x7f) {
			char buffer[8];
			snprintf(buffer, sizeof(buffer), "\\x%02x", c);
			result += buffer;
		} else
			result += c;
	}
	return result;
}


struct Recorder : Terminal::Listener {
	std::string title;
	int bells = 0;
	std::string clipboard;
	std::vector<TermNotification> notifications;
	std::string pointer;
	std::string directory;

	virtual void TerminalTitleChanged(const char* text) { title = text; }
	virtual void TerminalBell() { bells++; }
	virtual void TerminalSetClipboard(const char* selections,
		const void* data, size_t length)
	{
		clipboard.assign((const char*)data, length);
	}
	virtual void TerminalNotify(const TermNotification& notification)
	{
		notifications.push_back(notification);
	}
	virtual void TerminalPointerShapeChanged(const char* shape)
	{
		pointer = shape;
	}
	virtual void TerminalWorkingDirectoryChanged(const char* url)
	{
		directory = url;
	}
};


struct Fixture {
	Terminal	terminal;
	Recorder	recorder;

	Fixture(int32 columns = 20, int32 rows = 5, int32 history = 100)
	{
		terminal.Init(columns, rows, history);
		terminal.SetListener(&recorder);
		terminal.SetCellSize(10, 20);
		terminal.SetDefaultColor(COLOR_FOREGROUND, 0x000000);
		terminal.SetDefaultColor(COLOR_BACKGROUND, 0xffffff);
		for (int i = 0; i < 16; i++)
			terminal.SetDefaultColor(i, 0x010101 * i);
	}

	void Feed(const std::string& text)
	{
		terminal.Lock();
		terminal.Feed((const uint8*)text.data(), text.size());
		terminal.Unlock();
	}

	std::string Reply()
	{
		std::string output;
		terminal.TakeOutput(output);
		return output;
	}

	std::string Row(int32 row)
	{
		std::string text;
		terminal.GetText(TermPoint(0, row), TermPoint(terminal.Columns(), row),
			text);
		return text;
	}

	TerminalCell Cell(int32 row, int32 column)
	{
		TerminalCell cell;
		terminal.GetCell(row, column, cell);
		return cell;
	}

	TermPoint Cursor()
	{
		Terminal::CursorInfo info = terminal.Cursor();
		return TermPoint(info.x, info.y);
	}
};


static void
TestPrinting()
{
	Fixture f;
	f.Feed("hello");
	CHECK_EQUAL(f.Row(0), "hello");
	CHECK(f.Cursor() == TermPoint(5, 0));

	f.Feed("\r\nworld");
	CHECK_EQUAL(f.Row(1), "world");

	// autowrap
	Fixture w(10, 3);
	w.Feed("0123456789");
	CHECK(w.Cursor() == TermPoint(9, 0));
	w.Feed("ab");
	CHECK_EQUAL(w.Row(0), "0123456789");
	CHECK_EQUAL(w.Row(1), "ab");
	CHECK((w.terminal.LineFlags(0) & LINE_WRAPPED) != 0);
	std::string text;
	w.terminal.GetText(TermPoint(0, 0), TermPoint(0, 2), text);
	CHECK_EQUAL(text, "0123456789ab\n");

	// no autowrap
	Fixture n(10, 3);
	n.Feed("\x1b[?7l0123456789abc");
	CHECK_EQUAL(n.Row(0), "012345678c");
	CHECK_EQUAL(n.Row(1), "");

	// scrolling into the history
	Fixture s(10, 3, 100);
	s.Feed("1\r\n2\r\n3\r\n4\r\n5");
	CHECK_EQUAL(s.Row(0), "3");
	CHECK_EQUAL(s.Row(2), "5");
	CHECK(s.terminal.HistorySize() == 2);
	CHECK_EQUAL(s.Row(-1), "2");
	CHECK_EQUAL(s.Row(-2), "1");

	// history capacity
	Fixture c(10, 2, 5);
	for (int i = 0; i < 20; i++) {
		char line[16];
		snprintf(line, sizeof(line), "%d\r\n", i);
		c.Feed(line);
	}
	CHECK(c.terminal.HistorySize() == 5);
	CHECK_EQUAL(c.Row(-1), "18");
	CHECK_EQUAL(c.Row(-5), "14");

	// tabs
	Fixture t;
	t.Feed("a\tb");
	CHECK(t.Cell(0, 8).character == 'b');
	t.Feed("\x1b[3g\r\x1b[4C\x1bH\r\tc");
	CHECK(t.Cell(0, 4).character == 'c');

	// DEC special graphics
	Fixture g;
	g.Feed("\x1b(0qx\x1b(Bq");
	CHECK(g.Cell(0, 0).character == 0x2500);
	CHECK(g.Cell(0, 1).character == 0x2502);
	CHECK(g.Cell(0, 2).character == 'q');
}


static void
TestUnicode()
{
	// double width
	Fixture f(10, 3);
	f.Feed("\xe6\xbc\xa2\xe5\xad\x97");	// 漢字
	CHECK(f.Cursor() == TermPoint(4, 0));
	CHECK(f.Cell(0, 0).IsWide());
	CHECK(f.Cell(0, 1).IsWideTail());
	CHECK_EQUAL(f.Row(0), "\xe6\xbc\xa2\xe5\xad\x97");

	// overwriting half of one clears the other half
	f.Feed("\r\x1b[1Cx");
	CHECK(f.Cell(0, 0).character == 0);
	CHECK(!f.Cell(0, 0).IsWide());
	CHECK(f.Cell(0, 1).character == 'x');

	// a wide character at the margin wraps
	Fixture m(5, 3);
	m.Feed("abcd\xe6\xbc\xa2");
	CHECK_EQUAL(m.Row(0), "abcd");
	CHECK_EQUAL(m.Row(1), "\xe6\xbc\xa2");

	// combining characters join the character before them
	Fixture c;
	c.Feed("e\xcc\x81x");
	CHECK(c.Cursor() == TermPoint(2, 0));
	uint32 codePoints[4];
	CHECK(c.terminal.GetCharacter(c.Cell(0, 0).character, codePoints, 4)
		== 2);
	CHECK(codePoints[1] == 0x301);
	CHECK_EQUAL(c.Row(0), "e\xcc\x81x");

	// an emoji ZWJ sequence is one character, two cells wide
	Fixture z;
	z.Feed("\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d"
		"\xf0\x9f\x91\xa7!");	// 👨‍👩‍👧!
	CHECK(z.Cursor() == TermPoint(3, 0));
	CHECK(z.terminal.GetCharacter(z.Cell(0, 0).character, codePoints, 4)
		== 4);
	CHECK(z.Cell(0, 2).character == '!');

	// flags: pairs of regional indicators
	Fixture r;
	r.Feed("\xf0\x9f\x87\xa6\xf0\x9f\x87\xba\xf0\x9f\x87\xb3\xf0\x9f\x87\xbf");
	CHECK(r.Cursor() == TermPoint(4, 0));

	// VS16 makes a text-style symbol wide
	Fixture v;
	v.Feed("\xe2\x9d\xa4\xef\xb8\x8fx");	// ❤️x
	CHECK(v.Cell(0, 0).IsWide());
	CHECK(v.Cell(0, 2).character == 'x');

	// invalid UTF-8
	Fixture i;
	i.Feed("a\xff" "b\xe2\x82z");
	CHECK(i.Cell(0, 1).character == 0xfffd);
	CHECK(i.Cell(0, 3).character == 0xfffd);
	CHECK(i.Cell(0, 4).character == 'z');
}


static void
TestEditing()
{
	Fixture f(10, 5);
	f.Feed("abcdefghij");
	f.Feed("\x1b[1;3H\x1b[2@");	// ICH
	CHECK_EQUAL(f.Row(0), "ab  cdefgh");
	f.Feed("\x1b[3P");	// DCH
	CHECK_EQUAL(f.Row(0), "abdefgh");
	f.Feed("\x1b[2X");	// ECH
	CHECK_EQUAL(f.Row(0), "ab  fgh");
	f.Feed("\x1b[K");
	CHECK_EQUAL(f.Row(0), "ab");
	f.Feed("\x1b[1K");
	CHECK_EQUAL(f.Row(0), "");

	f.Feed("\x1b[H1\r\n2\r\n3\r\n4\r\n5");
	f.Feed("\x1b[2;1H\x1b[L");	// IL
	CHECK_EQUAL(f.Row(1), "");
	CHECK_EQUAL(f.Row(2), "2");
	CHECK_EQUAL(f.Row(4), "4");
	f.Feed("\x1b[2M");	// DL
	CHECK_EQUAL(f.Row(1), "3");
	CHECK_EQUAL(f.Row(2), "4");
	CHECK(f.terminal.HistorySize() == 0);

	f.Feed("\x1b[2J\x1b[Hx\x1b[3b");	// REP
	CHECK_EQUAL(f.Row(0), "xxxx");

	// erase keeps the background color
	f.Feed("\x1b[44m\x1b[2K\x1b[0m");
	CHECK(f.Cell(0, 5).style.background == palette_color(4));

	// insert mode
	Fixture i(10, 2);
	i.Feed("abc\r\x1b[4hX\x1b[4l");
	CHECK_EQUAL(i.Row(0), "Xabc");

	// DECALN
	Fixture a(3, 2);
	a.Feed("\x1b#8");
	CHECK_EQUAL(a.Row(1), "EEE");
}


static void
TestCursorAndRegions()
{
	Fixture f(10, 6);
	f.Feed("\x1b[3;4H");
	CHECK(f.Cursor() == TermPoint(3, 2));
	f.Feed("\x1b[A\x1b[2D");
	CHECK(f.Cursor() == TermPoint(1, 1));
	f.Feed("\x1b[10B");
	CHECK(f.Cursor() == TermPoint(1, 5));

	// scroll region
	f.Feed("\x1b[H\x1b[2J");
	for (int i = 0; i < 6; i++) {
		char line[16];
		snprintf(line, sizeof(line), "\x1b[%d;1H%d", i + 1, i);
		f.Feed(line);
	}
	f.Feed("\x1b[2;4r\x1b[4;1H\n");
	CHECK_EQUAL(f.Row(0), "0");
	CHECK_EQUAL(f.Row(1), "2");
	CHECK_EQUAL(f.Row(3), "");
	CHECK_EQUAL(f.Row(4), "4");
	CHECK(f.terminal.HistorySize() == 0);
	f.Feed("\x1b[2;1H\x1bM");	// RI at the top of the region
	CHECK_EQUAL(f.Row(1), "");
	CHECK_EQUAL(f.Row(2), "2");

	// origin mode
	f.Feed("\x1b[?6h\x1b[H");
	CHECK(f.Cursor() == TermPoint(0, 1));
	f.Feed("\x1b[?6l\x1b[r");

	// left and right margins
	Fixture m(10, 3);
	m.Feed("0123456789\r\nabcdefghij");
	m.Feed("\x1b[?69h\x1b[3;6s\x1b[1;3H\x1b[2@");
	CHECK_EQUAL(m.Row(0), "01  236789");

	// save and restore the cursor
	Fixture s;
	s.Feed("\x1b[2;3H\x1b[1m\x1b" "7\x1b[H\x1b[0m\x1b" "8x");
	CHECK(s.Cell(1, 2).character == 'x');
	CHECK((s.Cell(1, 2).style.flags & CELL_BOLD) != 0);

	// unscroll
	Fixture u(10, 3, 100);
	u.Feed("1\r\n2\r\n3\r\n4\r\n5");
	CHECK_EQUAL(u.Row(0), "3");
	u.Feed("\x1b[2+T");
	CHECK_EQUAL(u.Row(0), "1");
	CHECK_EQUAL(u.Row(1), "2");
	CHECK_EQUAL(u.Row(2), "3");
	CHECK(u.terminal.HistorySize() == 0);
}


static void
TestStyles()
{
	Fixture f;
	f.Feed("\x1b[1;3;4;38;5;196;48;2;1;2;3mA");
	TerminalCell cell = f.Cell(0, 0);
	CHECK((cell.style.flags & CELL_BOLD) != 0);
	CHECK((cell.style.flags & CELL_ITALIC) != 0);
	CHECK(cell.style.underline == UNDERLINE_SINGLE);
	CHECK(cell.style.foreground == palette_color(196));
	CHECK(cell.style.background == rgb_color_value(1, 2, 3));

	f.Feed("\x1b[0;4:3;58:2::10:20:30;38:2::5:6:7mB");
	cell = f.Cell(0, 1);
	CHECK(cell.style.underline == UNDERLINE_CURLY);
	CHECK(cell.style.underlineColor == rgb_color_value(10, 20, 30));
	CHECK(cell.style.foreground == rgb_color_value(5, 6, 7));
	CHECK((cell.style.flags & CELL_BOLD) == 0);

	f.Feed("\x1b[0;1;2;221mC\x1b[222mD\x1b[53;9mE");
	CHECK((f.Cell(0, 2).style.flags & (CELL_BOLD | CELL_FAINT))
		== CELL_FAINT);
	CHECK((f.Cell(0, 3).style.flags & (CELL_BOLD | CELL_FAINT)) == 0);
	CHECK((f.Cell(0, 4).style.flags & (CELL_OVERLINE | CELL_STRIKE))
		== (CELL_OVERLINE | CELL_STRIKE));

	f.Feed("\x1b[0;91;102mF");
	CHECK(f.Cell(0, 5).style.foreground == palette_color(9));
	CHECK(f.Cell(0, 5).style.background == palette_color(10));

	// 4;3 is underline and italic, not curly
	f.Feed("\x1b[0;4;3mG");
	CHECK(f.Cell(0, 6).style.underline == UNDERLINE_SINGLE);
	CHECK((f.Cell(0, 6).style.flags & CELL_ITALIC) != 0);

	// DECCARA, rectangle
	Fixture d(10, 4);
	d.Feed("aaaaa\r\nbbbbb\r\nccccc");
	d.Feed("\x1b[2*x\x1b[1;2;2;3;44$r\x1b[*x");
	CHECK(d.Cell(0, 1).style.background == palette_color(4));
	CHECK(d.Cell(1, 2).style.background == palette_color(4));
	CHECK(d.Cell(0, 3).style.background == kDefaultColor);
	CHECK(d.Cell(2, 1).style.background == kDefaultColor);

	// XTPUSHSGR / XTPOPSGR
	Fixture p;
	p.Feed("\x1b[31m\x1b[#{\x1b[32m\x1b[#}x");
	CHECK(p.Cell(0, 0).style.foreground == palette_color(1));
}


static void
TestScreens()
{
	Fixture f(10, 3);
	f.Feed("main\x1b[?1049h");
	CHECK(f.terminal.IsAlternateScreen());
	CHECK_EQUAL(f.Row(0), "");
	f.Feed("alt");
	f.Feed("\x1b[?1049l");
	CHECK(!f.terminal.IsAlternateScreen());
	CHECK_EQUAL(f.Row(0), "main");
	CHECK(f.Cursor() == TermPoint(4, 0));

	// RIS
	f.Feed("\x1b[1;31m\x1b" "c");
	CHECK_EQUAL(f.Row(0), "");
	f.Feed("x");
	CHECK(f.Cell(0, 0).style.foreground == kDefaultColor);
}


static void
TestReplies()
{
	Fixture f(80, 24);
	f.Feed("\x1b[c");
	CHECK_EQUAL(f.Reply(), "\x1b[?62;22;52c");
	f.Feed("\x1b[>c");
	CHECK_EQUAL(f.Reply(), "\x1b[>1;4000;1c");
	f.Feed("\x1b[5;7H\x1b[6n");
	CHECK_EQUAL(f.Reply(), "\x1b[5;7R");
	f.Feed("\x1b[?2004h\x1b[?2004$p\x1b[?1$p\x1b[4$p\x1b[?9999$p");
	CHECK_EQUAL(f.Reply(), "\x1b[?2004;1$y\x1b[?1;2$y\x1b[4;2$y"
		"\x1b[?9999;0$y");
	// grapheme clusters cannot be turned off
	f.Feed("\x1b[?2027l\x1b[?2027$p");
	CHECK_EQUAL(f.Reply(), "\x1b[?2027;3$y");
	f.Feed("\x1b[>q");
	CHECK_EQUAL(f.Reply(), "\x1bP>|airos-terminal(2.0)\x1b\\");
	f.Feed("\x1b[14t\x1b[16t\x1b[18t");
	CHECK_EQUAL(f.Reply(), "\x1b[4;480;800t\x1b[6;20;10t\x1b[8;24;80t");
	f.Feed("\x1b[1;31m\x1bP$qm\x1b\\");
	CHECK_EQUAL(f.Reply(), "\x1bP1$r0;1;31m\x1b\\");
	f.Feed("\x1b[3;10r\x1bP$qr\x1b\\\x1b[r");
	CHECK_EQUAL(f.Reply(), "\x1bP1$r3;10r\x1b\\");
	f.Feed("\x1b[5 q\x1bP$q q\x1b\\");
	CHECK_EQUAL(f.Reply(), "\x1bP1$r5 q\x1b\\");
	// XTGETTCAP: "TN" and "Co", and an unknown one
	f.Feed("\x1bP+q544e;436f;7a7a\x1b\\");
	CHECK_EQUAL(f.Reply(), "\x1bP1+r544e=787465726D2D323536636F6C6F72\x1b\\"
		"\x1bP1+r436f=323536\x1b\\\x1bP0+r7a7a\x1b\\");
	// focus reporting
	f.Feed("\x1b[?1004h");
	CHECK_EQUAL(f.Reply(), "\x1b[I");
	f.terminal.SetFocused(false);
	CHECK_EQUAL(f.Reply(), "\x1b[O");
	// in-band resize notifications
	f.Feed("\x1b[?2048h");
	CHECK_EQUAL(f.Reply(), "\x1b[48;24;80;480;800t");
	f.terminal.Resize(100, 30);
	CHECK_EQUAL(f.Reply(), "\x1b[48;30;100;600;1000t");
	// color scheme
	f.Feed("\x1b[?996n");
	CHECK_EQUAL(f.Reply(), "\x1b[?997;2n");
}


static void
TestKeyboardProtocol()
{
	Fixture f;
	f.Feed("\x1b[?u");
	CHECK_EQUAL(f.Reply(), "\x1b[?0u");
	f.Feed("\x1b[>1u\x1b[?u");
	CHECK_EQUAL(f.Reply(), "\x1b[?1u");
	f.Feed("\x1b[>11u\x1b[?u");
	CHECK_EQUAL(f.Reply(), "\x1b[?11u");
	f.Feed("\x1b[<u\x1b[?u");
	CHECK_EQUAL(f.Reply(), "\x1b[?1u");
	f.Feed("\x1b[=4;2u\x1b[?u");
	CHECK_EQUAL(f.Reply(), "\x1b[?5u");
	f.Feed("\x1b[=1;3u\x1b[?u");
	CHECK_EQUAL(f.Reply(), "\x1b[?4u");
	f.Feed("\x1b[<5u\x1b[?u");
	CHECK_EQUAL(f.Reply(), "\x1b[?0u");
	// the answer itself, echoed back, is no query
	f.Feed("\x1b[?0u");
	CHECK_EQUAL(f.Reply(), "");

	// separate flags for the alternate screen
	f.Feed("\x1b[>1u\x1b[?1049h\x1b[?u");
	CHECK_EQUAL(f.Reply(), "\x1b[?0u");
	f.Feed("\x1b[?1049l\x1b[?u");
	CHECK_EQUAL(f.Reply(), "\x1b[?1u");

	struct KeyTest {
		uint32		flags;
		uint32		key;
		uint32		shifted;
		uint32		modifiers;
		int32		action;
		const char*	text;
		const char*	expected;
	};
	static const KeyTest kTests[] = {
		// legacy
		{ 0, 'a', 'A', 0, 1, "a", "a" },
		{ 0, 'a', 'A', KEY_MODIFIER_SHIFT, 1, "A", "A" },
		{ 0, 'a', 'A', KEY_MODIFIER_CONTROL, 1, "\x01", "\x01" },
		{ 0, 'a', 'A', KEY_MODIFIER_ALT, 1, "a", "\x1b" "a" },
		{ 0, 'a', 'A', KEY_MODIFIER_CONTROL | KEY_MODIFIER_SHIFT, 1, "",
			"\x1b[97;6u" },
		{ 0, 'i', 'I', KEY_MODIFIER_CONTROL | KEY_MODIFIER_ALT, 1, "",
			"\x1b\t" },
		{ 0, '3', '#', KEY_MODIFIER_CONTROL, 1, "", "\x1b" },
		{ 0, ' ', ' ', KEY_MODIFIER_CONTROL, 1, "", "" },
		{ 0, KEY_UP, 0, 0, 1, "", "\x1b[A" },
		{ 0, KEY_UP, 0, KEY_MODIFIER_CONTROL, 1, "", "\x1b[1;5A" },
		{ 0, KEY_F1, 0, 0, 1, "", "\x1bOP" },
		{ 0, KEY_F1, 0, KEY_MODIFIER_SHIFT, 1, "", "\x1b[1;2P" },
		{ 0, KEY_F1 + 4, 0, KEY_MODIFIER_CONTROL, 1, "", "\x1b[15;5~" },
		{ 0, KEY_PAGE_UP, 0, KEY_MODIFIER_ALT, 1, "", "\x1b[5;3~" },
		{ 0, KEY_ENTER, 0, 0, 1, "\r", "\r" },
		{ 0, KEY_ENTER, 0, KEY_MODIFIER_ALT, 1, "", "\x1b\r" },
		{ 0, KEY_BACKSPACE, 0, 0, 1, "", "\x7f" },
		{ 0, KEY_BACKSPACE, 0, KEY_MODIFIER_CONTROL, 1, "", "\x08" },
		{ 0, KEY_TAB, 0, KEY_MODIFIER_SHIFT, 1, "", "\x1b[Z" },
		{ 0, KEY_ESCAPE, 0, 0, 1, "", "\x1b" },
		{ 0, KEY_ENTER, 0, KEY_MODIFIER_SUPER, 1, "", "\x1b[13;9u" },
		{ 0, 'a', 'A', 0, 3, "a", "" },
		{ 0, KEY_KP_0 + 5, 0, 0, 1, "5", "5" },
		{ 0, 0xe9, 0xc9, 0, 1, "\xc3\xa9", "\xc3\xa9" },
		{ 0, KEY_MEDIA_PLAY, 0, 0, 1, "", "\x1b[57428u" },
		// disambiguate
		{ 1, 'a', 'A', 0, 1, "a", "a" },
		{ 1, 'a', 'A', KEY_MODIFIER_SHIFT, 1, "A", "A" },
		{ 1, 'a', 'A', KEY_MODIFIER_CONTROL, 1, "\x01", "\x1b[97;5u" },
		{ 1, 'a', 'A', KEY_MODIFIER_ALT, 1, "a", "\x1b[97;3u" },
		{ 1, KEY_ESCAPE, 0, 0, 1, "", "\x1b[27u" },
		{ 1, KEY_ENTER, 0, 0, 1, "\r", "\r" },
		{ 1, KEY_UP, 0, 0, 1, "", "\x1b[A" },
		{ 1, KEY_F1 + 2, 0, 0, 1, "", "\x1b[13~" },
		{ 1, KEY_KP_0 + 5, 0, 0, 1, "5", "5" },
		{ 1, KEY_KP_ENTER, 0, 0, 1, "", "\x1b[57414u" },
		{ 1, 'a', 'A', 0, 3, "a", "" },
		// event types
		{ 3, 'a', 'A', KEY_MODIFIER_CONTROL, 3, "", "\x1b[97;5:3u" },
		{ 3, KEY_UP, 0, 0, 3, "", "\x1b[1;1:3A" },
		{ 3, KEY_UP, 0, 0, 2, "", "\x1b[1;1:2A" },
		{ 3, KEY_ENTER, 0, 0, 3, "", "" },
		// all keys as escape codes
		{ 8, 'a', 'A', 0, 1, "a", "\x1b[97u" },
		{ 8, KEY_ENTER, 0, 0, 1, "\r", "\x1b[13u" },
		{ 8, KEY_LEFT_SHIFT, 0, KEY_MODIFIER_SHIFT, 1, "", "\x1b[57441;2u" },
		{ 10, KEY_ENTER, 0, 0, 3, "", "\x1b[13;1:3u" },
		// text
		{ 24, 'a', 'A', KEY_MODIFIER_SHIFT, 1, "A", "\x1b[97;2;65u" },
		// alternates
		{ 13, 'a', 'A', KEY_MODIFIER_SHIFT | KEY_MODIFIER_CONTROL, 1, "",
			"\x1b[97:65;6u" },
	};

	for (size_t i = 0; i < sizeof(kTests) / sizeof(kTests[0]); i++) {
		const KeyTest& test = kTests[i];
		Fixture k;
		char setup[48];
		snprintf(setup, sizeof(setup), "\x1b[=%uu", (unsigned)test.flags);
		k.Feed(setup);
		TermKeyEvent event;
		event.key = test.key;
		event.shiftedKey = test.shifted;
		event.baseKey = 0;
		event.modifiers = test.modifiers;
		event.action = test.action;
		event.text = test.text;
		std::string output;
		k.terminal.EncodeKey(event, output);
		std::string expected = test.expected;
		if (test.key == ' ' && test.flags == 0
			&& test.modifiers == KEY_MODIFIER_CONTROL) {
			expected = std::string(1, '\0');
		}
		if (output != expected) {
			fprintf(stderr, "key test %zu (flags %u, key %u, mods %u, "
				"action %d):\n", i, (unsigned)test.flags,
				(unsigned)test.key, (unsigned)test.modifiers,
				(int)test.action);
		}
		CHECK_EQUAL(output, expected);
	}

	// cursor key mode
	Fixture c;
	c.Feed("\x1b[?1h");
	TermKeyEvent event = { KEY_UP, 0, 0, 0, 1, "" };
	std::string output;
	c.terminal.EncodeKey(event, output);
	CHECK_EQUAL(output, "\x1bOA");
}


static void
TestMouse()
{
	Fixture f(80, 24);
	std::string output;
	CHECK(!f.terminal.EncodeMouse(MOUSE_PRESS, MOUSE_BUTTON_LEFT, 0, 1, 2,
		10, 20, output));

	f.Feed("\x1b[?1000h");
	f.terminal.EncodeMouse(MOUSE_PRESS, MOUSE_BUTTON_LEFT, 0, 1, 2, 10, 20,
		output);
	CHECK_EQUAL(output, "\x1b[M \"#");
	output.clear();
	f.terminal.EncodeMouse(MOUSE_RELEASE, MOUSE_BUTTON_LEFT, 0, 1, 2, 10, 20,
		output);
	CHECK_EQUAL(output, "\x1b[M#\"#");
	output.clear();
	CHECK(!f.terminal.EncodeMouse(MOUSE_MOTION, MOUSE_BUTTON_LEFT, 0, 1, 2,
		10, 20, output));

	f.Feed("\x1b[?1002h\x1b[?1006h");
	f.terminal.EncodeMouse(MOUSE_MOTION, MOUSE_BUTTON_LEFT,
		KEY_MODIFIER_CONTROL, 4, 5, 10, 20, output);
	CHECK_EQUAL(output, "\x1b[<48;5;6M");
	output.clear();
	f.terminal.EncodeMouse(MOUSE_RELEASE, MOUSE_BUTTON_LEFT, 0, 4, 5, 10, 20,
		output);
	CHECK_EQUAL(output, "\x1b[<0;5;6m");
	output.clear();
	f.terminal.EncodeMouse(MOUSE_PRESS, MOUSE_BUTTON_WHEEL_UP, 0, 4, 5, 10,
		20, output);
	CHECK_EQUAL(output, "\x1b[<64;5;6M");
	output.clear();

	f.Feed("\x1b[?1016h");
	f.terminal.EncodeMouse(MOUSE_PRESS, MOUSE_BUTTON_RIGHT, 0, 4, 5, 123, 45,
		output);
	CHECK_EQUAL(output, "\x1b[<2;124;46M");
	output.clear();
	f.terminal.EncodeMouse(MOUSE_LEAVE, MOUSE_BUTTON_NONE, 0, 4, 5, 123, 45,
		output);
	CHECK_EQUAL(output, "\x1b[<288;124;46M");

	// paste
	Fixture p;
	output.clear();
	p.terminal.EncodePaste("a\nb\r\nc", 6, output);
	CHECK_EQUAL(output, "a\rb\rc");
	p.Feed("\x1b[?2004h");
	output.clear();
	p.terminal.EncodePaste("x\x1b[201~y", 9, output);
	CHECK_EQUAL(output, "\x1b[200~x[201~y\x1b[201~");
}


static void
TestOperatingSystemCommands()
{
	Fixture f;
	f.Feed("\x1b]2;Hello\x07");
	CHECK_EQUAL(f.recorder.title, "Hello");
	f.Feed("\x1b]0;World\x1b\\");
	CHECK_EQUAL(f.recorder.title, "World");

	// palette
	f.Feed("\x1b]4;1;rgb:ff/80/00\x1b\\\x1b]4;1;?\x07");
	CHECK_EQUAL(f.Reply(), "\x1b]4;1;rgb:ffff/8080/0000\x07");
	f.Feed("\x1b]104;1\x1b\\\x1b]4;1;?\x1b\\");
	CHECK_EQUAL(f.Reply(), "\x1b]4;1;rgb:0101/0101/0101\x1b\\");
	f.Feed("\x1b]11;#102030\x1b\\\x1b]11;?\x1b\\");
	CHECK_EQUAL(f.Reply(), "\x1b]11;rgb:1010/2020/3030\x1b\\");
	f.Feed("\x1b]10;red\x1b\\\x1b]10;?\x1b\\");
	CHECK_EQUAL(f.Reply(), "\x1b]10;rgb:ffff/0000/0000\x1b\\");
	CHECK(f.terminal.Color(COLOR_FOREGROUND) == 0xff0000);
	f.Feed("\x1b]110\x1b\\");
	CHECK(f.terminal.Color(COLOR_FOREGROUND) == 0x000000);

	// kitty's OSC 21
	f.Feed("\x1b]21;foreground=white;foreground=?;cursor=?;nonsense=?\x1b\\");
	CHECK_EQUAL(f.Reply(), "\x1b]21;foreground=rgb:ff/ff/ff;cursor="
		";unknown=bm9uc2Vuc2U\x1b\\");
	f.Feed("\x1b]21;foreground=rgb:ff/ff/ff;cursor=;unknown=bm9uc2Vuc2U\x1b\\");
	CHECK_EQUAL(f.Reply(), "");

	// color stack
	f.Feed("\x1b]30001\x1b\\\x1b]10;blue\x1b\\\x1b]30101\x1b\\");
	CHECK(f.terminal.Color(COLOR_FOREGROUND) == 0xffffff);

	// hyperlinks
	f.Feed("\x1b]8;id=x;http://example.com\x1b\\link\x1b]8;;\x1b\\ no");
	TerminalCell cell = f.Cell(0, 0);
	CHECK(cell.hyperlink != 0);
	std::string uri, identifier;
	CHECK(f.terminal.GetHyperLink(cell.hyperlink, uri, identifier));
	CHECK_EQUAL(uri, "http://example.com");
	CHECK_EQUAL(identifier, "x");
	CHECK(f.Cell(0, 5).hyperlink == 0);

	// clipboard
	f.Feed("\x1b]52;c;aGVsbG8=\x07");
	CHECK_EQUAL(f.recorder.clipboard, "hello");

	// pointer shapes
	f.Feed("\x1b]22;pointer\x1b\\");
	CHECK_EQUAL(f.recorder.pointer, "pointer");
	f.Feed("\x1b]22;>wait,hand2\x1b\\\x1b]22;?__current__,pointer,bogus\x1b\\");
	CHECK_EQUAL(f.Reply(), "\x1b]22;pointer,1,0\x1b\\");
	f.Feed("\x1b]22;<\x1b\\\x1b]22;?__current__\x1b\\");
	CHECK_EQUAL(f.Reply(), "\x1b]22;wait\x1b\\");

	// working directory
	f.Feed("\x1b]7;file://host/boot/home\x1b\\");
	CHECK_EQUAL(f.recorder.directory, "file://host/boot/home");

	// shell integration marks
	Fixture s;
	s.Feed("\x1b]133;A\x07$ ls\r\n\x1b]133;C\x07out\r\n\x1b]133;A\x07$ ");
	CHECK((s.terminal.LineFlags(0) & LINE_PROMPT) != 0);
	CHECK((s.terminal.LineFlags(1) & LINE_OUTPUT) != 0);
	int32 row;
	CHECK(s.terminal.FindPrompt(2, false, row) && row == 0);
	CHECK(s.terminal.FindPrompt(0, true, row) && row == 2);

	// notifications
	Fixture n;
	n.Feed("\x1b]99;;Hello world\x1b\\");
	CHECK(n.recorder.notifications.size() == 1);
	CHECK_EQUAL(n.recorder.notifications[0].title, "Hello world");
	n.Feed("\x1b]99;i=1:d=0;Title\x1b\\\x1b]99;i=1:p=body;Body\x1b\\");
	CHECK(n.recorder.notifications.size() == 2);
	CHECK_EQUAL(n.recorder.notifications[1].title, "Title");
	CHECK_EQUAL(n.recorder.notifications[1].body, "Body");
	n.Feed("\x1b]99;i=q:p=?;\x1b\\");
	std::string answer = n.Reply();
	CHECK(answer.find("\x1b]99;i=q:p=?;o=always") == 0);
	n.Feed(answer);
	CHECK_EQUAL(n.Reply(), "");
	n.Feed("\x1b]9;iTerm says hi\x07");
	CHECK(n.recorder.notifications.size() == 3);
	n.Feed("\x1b]777;notify;Title;Body text\x07");
	CHECK(n.recorder.notifications.size() == 4);
	CHECK_EQUAL(n.recorder.notifications[3].body, "Body text");
	// base64 payload with a reply on activation
	n.Feed("\x1b]99;i=abc:e=1:a=report;SGk=\x1b\\");
	CHECK_EQUAL(n.recorder.notifications.back().title, "Hi");
	n.terminal.ReportNotificationActivated("abc", 0);
	CHECK_EQUAL(n.Reply(), "\x1b]99;i=abc;\x1b\\");

	// text sizing falls back to normal text
	Fixture t;
	t.Feed("\x1b]66;s=2;Hi\x1b\\!");
	CHECK_EQUAL(t.Row(0), "Hi  !");
}


static void
TestReflow()
{
	Fixture f(10, 4, 100);
	f.Feed("0123456789abcdefghij\r\nshort");
	CHECK_EQUAL(f.Row(0), "0123456789");
	CHECK_EQUAL(f.Row(1), "abcdefghij");
	CHECK(f.Cursor() == TermPoint(5, 2));

	f.terminal.Resize(20, 4);
	CHECK_EQUAL(f.Row(0), "0123456789abcdefghij");
	CHECK_EQUAL(f.Row(1), "short");
	CHECK(f.Cursor() == TermPoint(5, 1));

	f.terminal.Resize(7, 4);
	CHECK_EQUAL(f.Row(0), "0123456");
	CHECK_EQUAL(f.Row(1), "789abcd");
	CHECK_EQUAL(f.Row(2), "efghij");
	CHECK_EQUAL(f.Row(3), "short");
	CHECK(f.Cursor() == TermPoint(5, 3));

	// fewer rows: the top goes into the history
	f.terminal.Resize(7, 2);
	CHECK_EQUAL(f.Row(0), "efghij");
	CHECK_EQUAL(f.Row(1), "short");
	CHECK(f.terminal.HistorySize() == 2);
	CHECK(f.Cursor() == TermPoint(5, 1));

	// more rows: back from the history
	f.terminal.Resize(10, 5);
	CHECK_EQUAL(f.Row(0), "0123456789");
	CHECK_EQUAL(f.Row(2), "short");
	CHECK(f.terminal.HistorySize() == 0);
	CHECK(f.Cursor() == TermPoint(5, 2));

	// wide characters are not split
	Fixture w(6, 3);
	w.Feed("ab\xe6\xbc\xa2\xe5\xad\x97");
	w.terminal.Resize(5, 3);
	CHECK_EQUAL(w.Row(0), "ab\xe6\xbc\xa2");
	CHECK_EQUAL(w.Row(1), "\xe5\xad\x97");

	// the alternate screen is cut, not wrapped
	Fixture a(10, 3);
	a.Feed("\x1b[?1049h0123456789");
	a.terminal.Resize(5, 3);
	CHECK_EQUAL(a.Row(0), "01234");
	a.Feed("\x1b[?1049l");
}


static void
TestFind()
{
	Fixture f(10, 4, 100);
	f.Feed("hello world\r\nfoo bar");
	TermPoint start, end;
	CHECK(f.terminal.FindWord(TermPoint(1, 0),
		[](uint32 c, void*) { return c != ' '; }, NULL, false, start, end));
	CHECK(start == TermPoint(0, 0));
	CHECK(end == TermPoint(5, 0));
	// a word over a soft wrap
	CHECK(f.terminal.FindWord(TermPoint(8, 0),
		[](uint32 c, void*) { return c != ' '; }, NULL, false, start, end));
	CHECK(start == TermPoint(6, 0));
	CHECK(end == TermPoint(1, 1));

	CHECK(f.terminal.Find("bar", TermPoint(0, 0), true, true, false, start,
		end));
	CHECK(start == TermPoint(4, 2));
	CHECK(end == TermPoint(7, 2));
	CHECK(f.terminal.Find("WORLD", TermPoint(0, 0), true, false, false,
		start, end));
	CHECK(start == TermPoint(6, 0));
	CHECK(end == TermPoint(1, 1));
	CHECK(!f.terminal.Find("WORLD", TermPoint(0, 0), true, true, false,
		start, end));
	CHECK(f.terminal.Find("o", TermPoint(0, 3), false, true, false, start,
		end));
	CHECK(start == TermPoint(2, 2));
}


static bool
InflateWithZlib(const uint8* data, size_t length, size_t expected,
	std::vector<uint8>& output)
{
	output.resize(expected);
	uLongf size = expected;
	return uncompress(output.data(), &size, data, length) == Z_OK
		&& size == expected;
}


static void
TestGraphics()
{
	TerminalGraphics::SetCodecs(NULL, InflateWithZlib);

	Fixture f(20, 10, 100);
	// query: one RGB pixel
	f.Feed("\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\\x1b[c");
	CHECK_EQUAL(f.Reply(), "\x1b_Gi=31;OK\x1b\\\x1b[?62;22;52c");

	// transmit and display a 25x30 RGBA image: 3 columns, 2 rows
	std::string pixels(25 * 30 * 4, '\xff');
	std::string encoded;
	base64_encode(pixels.data(), pixels.size(), encoded);
	f.Feed("\x1b[2;3H\x1b_Ga=T,i=5,s=25,v=30;" + encoded + "\x1b\\");
	CHECK_EQUAL(f.Reply(), "\x1b_Gi=5;OK\x1b\\");
	CHECK(f.terminal.Graphics()->HasPlacements());
	CHECK(f.Cursor() == TermPoint(5, 2));
	std::vector<ImageDraw> draws;
	f.terminal.Graphics()->CollectDraws(0, 10, draws);
	CHECK(draws.size() == 1);
	CHECK(draws[0].x == 20 && draws[0].y == 20);
	CHECK(draws[0].width == 25 && draws[0].height == 30);

	// it scrolls with the text, into the history
	f.Feed("\x1b[10;1H\n");
	draws.clear();
	f.terminal.Graphics()->CollectDraws(0, 10, draws);
	CHECK(draws.size() == 1 && draws[0].y == 0);
	f.Feed("\n\n");
	draws.clear();
	f.terminal.Graphics()->CollectDraws(0, 10, draws);
	CHECK(draws.empty());
	draws.clear();
	f.terminal.Graphics()->CollectDraws(-3, 10, draws);
	CHECK(draws.size() == 1 && draws[0].y == 20);
	f.Feed("\x1b[9;1H\x1b[2+T");	// unscroll it back

	// chunked, compressed, scaled into 4 columns
	std::string rgb(10 * 10 * 3, '\x80');
	std::vector<uint8> compressed(compressBound(rgb.size()));
	uLongf compressedSize = compressed.size();
	compress(compressed.data(), &compressedSize, (const Bytef*)rgb.data(),
		rgb.size());
	std::string chunked;
	base64_encode(compressed.data(), compressedSize, chunked);
	std::string first = chunked.substr(0, 8);
	std::string rest = chunked.substr(8);
	f.Feed("\x1b[H\x1b_Ga=T,i=6,f=24,s=10,v=10,o=z,c=4,m=1;" + first
		+ "\x1b\\");
	CHECK_EQUAL(f.Reply(), "");
	f.Feed("\x1b_Gm=0;" + rest + "\x1b\\");
	CHECK_EQUAL(f.Reply(), "\x1b_Gi=6;OK\x1b\\");
	draws.clear();
	f.terminal.Graphics()->CollectDraws(0, 10, draws);
	CHECK(draws.size() == 2);
	bool found = false;
	for (size_t i = 0; i < draws.size(); i++) {
		if (draws[i].imageId == 6) {
			found = true;
			CHECK(draws[i].width == 40 && draws[i].height == 40);
		}
	}
	CHECK(found);

	// our echoed answers are no commands
	f.Feed("\x1b_Gi=31;OK\x1b\\\x1b_Gi=99;ENOENT:No such image\x1b\\");
	CHECK_EQUAL(f.Reply(), "");

	// put by id, missing image
	f.Feed("\x1b_Ga=p,i=99\x1b\\");
	CHECK(f.Reply().find("\x1b_Gi=99;ENOENT") == 0);

	// both i and I
	f.Feed("\x1b_Ga=p,i=1,I=2\x1b\\");
	CHECK(f.Reply().find("EINVAL") != std::string::npos);

	// image numbers get fresh ids
	f.Feed("\x1b_Ga=t,I=13,f=24,s=1,v=1;AAAA\x1b\\");
	std::string reply = f.Reply();
	CHECK(reply.find(",I=13;OK") != std::string::npos);

	// deleting
	f.Feed("\x1b_Ga=d,d=i,i=6\x1b\\");
	draws.clear();
	f.terminal.Graphics()->CollectDraws(-10, 20, draws);
	CHECK(draws.size() == 1);
	CHECK(draws[0].imageId == 5 && draws[0].y == 10 * 20 + 0);
	f.Feed("\x1b[2J");
	draws.clear();
	f.terminal.Graphics()->CollectDraws(-10, 20, draws);
	CHECK(draws.size() == 0);

	// clearing the screen keeps the images in the history, ED 3 not
	Fixture h(20, 4, 100);
	h.Feed("\x1b_Ga=T,f=24,s=1,v=1;AAAA\x1b\\\r\n\n\n\n\n\x1b[2J");
	draws.clear();
	h.terminal.Graphics()->CollectDraws(-10, 14, draws);
	CHECK(draws.size() == 1);
	h.Feed("\x1b[3J");
	draws.clear();
	h.terminal.Graphics()->CollectDraws(-10, 14, draws);
	CHECK(draws.size() == 0);

	// Unicode placeholders: a 2x2 virtual placement of image 42
	Fixture u(20, 5);
	std::string small(10 * 40 * 4, '\x40');
	std::string smallEncoded;
	base64_encode(small.data(), small.size(), smallEncoded);
	u.Feed("\x1b_Ga=T,U=1,i=42,s=10,v=40,c=2,r=2,q=2;" + smallEncoded
		+ "\x1b\\");
	CHECK_EQUAL(u.Reply(), "");
	CHECK(u.Cursor() == TermPoint(0, 0));
	u.Feed("\x1b[38;5;42m\xf4\x8e\xbb\xae\xcc\x85\xcc\x85"
		"\xf4\x8e\xbb\xae\x1b[39m\r\n");
	u.Feed("\x1b[38;5;42m\xf4\x8e\xbb\xae\xcc\x8d\xf4\x8e\xbb\xae\x1b[39m");
	TerminalCell cells[20];
	u.terminal.GetLine(0, cells);
	draws.clear();
	u.terminal.Graphics()->CollectPlaceholderDraws(cells, 20, 0, draws);
	CHECK(draws.size() == 1);
	if (draws.size() == 1) {
		// the 10x40 image fitted into 20x40 pixels, centered
		CHECK(draws[0].x == 5);
		CHECK(draws[0].width == 10);
		CHECK(draws[0].height == 20);
		CHECK(draws[0].sourceY == 0);
	}
	u.terminal.GetLine(1, cells);
	draws.clear();
	u.terminal.Graphics()->CollectPlaceholderDraws(cells, 20, 1, draws);
	CHECK(draws.size() == 1 && draws[0].sourceY == 20
		&& draws[0].sourceHeight == 20);

	// animation frames
	Fixture a(20, 5);
	a.Feed("\x1b_Ga=T,i=7,f=24,s=1,v=1;AAAA\x1b\\");
	a.Reply();
	a.Feed("\x1b_Ga=f,i=7,f=24,s=1,v=1,z=10;/wAA\x1b\\");
	CHECK_EQUAL(a.Reply(), "\x1b_Gi=7;OK\x1b\\");
	a.Feed("\x1b_Ga=a,i=7,s=3\x1b\\");
	bool changed = false;
	a.terminal.Graphics()->Animate(system_time() + 1000000, changed);
	CHECK(changed);
}


int
main()
{
	TestPrinting();
	TestUnicode();
	TestEditing();
	TestCursorAndRegions();
	TestStyles();
	TestScreens();
	TestReplies();
	TestKeyboardProtocol();
	TestMouse();
	TestOperatingSystemCommands();
	TestReflow();
	TestFind();
	TestGraphics();

	printf("%d checks, %d failed\n", sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}
