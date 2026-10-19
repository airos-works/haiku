/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


//!	Encoding keyboard, mouse and paste input for the program.


#include "Terminal.h"

#include <stdio.h>
#include <string.h>

#include <algorithm>


static void
append_utf8(std::string& output, uint32 c)
{
	if (c < 0x80) {
		output += (char)c;
	} else if (c < 0x800) {
		output += (char)(0xc0 | (c >> 6));
		output += (char)(0x80 | (c & 0x3f));
	} else if (c < 0x10000) {
		output += (char)(0xe0 | (c >> 12));
		output += (char)(0x80 | ((c >> 6) & 0x3f));
		output += (char)(0x80 | (c & 0x3f));
	} else {
		output += (char)(0xf0 | (c >> 18));
		output += (char)(0x80 | ((c >> 12) & 0x3f));
		output += (char)(0x80 | ((c >> 6) & 0x3f));
		output += (char)(0x80 | (c & 0x3f));
	}
}


static bool
decode_utf8(const char* text, std::vector<uint32>& codePoints)
{
	codePoints.clear();
	const uint8* bytes = (const uint8*)text;
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
		} else if (c >= 0x80)
			return false;
		while (extra-- > 0) {
			if ((*bytes & 0xc0) != 0x80)
				return false;
			c = (c << 6) | (*bytes++ & 0x3f);
		}
		codePoints.push_back(c);
	}
	return true;
}


//! Whether the text has nothing that is not printable.
static bool
is_printable_text(const char* text)
{
	if (text == NULL || text[0] == '\0')
		return false;
	std::vector<uint32> codePoints;
	if (!decode_utf8(text, codePoints))
		return false;
	for (size_t i = 0; i < codePoints.size(); i++) {
		uint32 c = codePoints[i];
		if (c < 0x20 || (c >= 0x7f && c < 0xa0))
			return false;
	}
	return true;
}


/*!	How a key without a character is sent: CSI number ~, CSI 1 letter (or
	SS3 letter) or CSI number u.
*/
struct FunctionalKey {
	uint32	key;
	uint32	number;
	char	final;
};


static const FunctionalKey kFunctionalKeys[] = {
	{ KEY_ESCAPE, 27, 'u' },
	{ KEY_ENTER, 13, 'u' },
	{ KEY_TAB, 9, 'u' },
	{ KEY_BACKSPACE, 127, 'u' },
	{ KEY_INSERT, 2, '~' },
	{ KEY_DELETE, 3, '~' },
	{ KEY_LEFT, 1, 'D' },
	{ KEY_RIGHT, 1, 'C' },
	{ KEY_UP, 1, 'A' },
	{ KEY_DOWN, 1, 'B' },
	{ KEY_PAGE_UP, 5, '~' },
	{ KEY_PAGE_DOWN, 6, '~' },
	{ KEY_HOME, 1, 'H' },
	{ KEY_END, 1, 'F' },
	{ KEY_F1, 1, 'P' },
	{ KEY_F1 + 1, 1, 'Q' },
	{ KEY_F1 + 2, 13, '~' },
	{ KEY_F1 + 3, 1, 'S' },
	{ KEY_F1 + 4, 15, '~' },
	{ KEY_F1 + 5, 17, '~' },
	{ KEY_F1 + 6, 18, '~' },
	{ KEY_F1 + 7, 19, '~' },
	{ KEY_F1 + 8, 20, '~' },
	{ KEY_F1 + 9, 21, '~' },
	{ KEY_F1 + 10, 23, '~' },
	{ KEY_F1 + 11, 24, '~' },
	{ KEY_KP_BEGIN, 1, 'E' },
	{ 0, 0, 0 }
};


static const FunctionalKey*
find_functional_key(uint32 key)
{
	for (const FunctionalKey* entry = kFunctionalKeys; entry->key != 0;
			entry++) {
		if (entry->key == key)
			return entry;
	}
	return NULL;
}


//! The non-keypad key a keypad key stands for in the legacy encoding.
static uint32
keypad_equivalent(uint32 key, const char* text)
{
	switch (key) {
		case KEY_KP_ENTER:
			return KEY_ENTER;
		case KEY_KP_LEFT:
			return KEY_LEFT;
		case KEY_KP_RIGHT:
			return KEY_RIGHT;
		case KEY_KP_UP:
			return KEY_UP;
		case KEY_KP_DOWN:
			return KEY_DOWN;
		case KEY_KP_PAGE_UP:
			return KEY_PAGE_UP;
		case KEY_KP_PAGE_DOWN:
			return KEY_PAGE_DOWN;
		case KEY_KP_HOME:
			return KEY_HOME;
		case KEY_KP_END:
			return KEY_END;
		case KEY_KP_INSERT:
			return KEY_INSERT;
		case KEY_KP_DELETE:
			return KEY_DELETE;
		case KEY_KP_BEGIN:
			return KEY_KP_BEGIN;
	}
	if (key >= KEY_KP_0 && key <= KEY_KP_9)
		return '0' + key - KEY_KP_0;
	switch (key) {
		case KEY_KP_DECIMAL:
			return '.';
		case KEY_KP_DIVIDE:
			return '/';
		case KEY_KP_MULTIPLY:
			return '*';
		case KEY_KP_SUBTRACT:
			return '-';
		case KEY_KP_ADD:
			return '+';
		case KEY_KP_EQUAL:
			return '=';
		case KEY_KP_SEPARATOR:
			return ',';
	}
	return key;
}


static bool
is_keypad_key(uint32 key)
{
	return key >= KEY_KP_0 && key <= KEY_KP_BEGIN;
}


static bool
is_modifier_key(uint32 key)
{
	return (key >= KEY_LEFT_SHIFT && key <= KEY_ISO_LEVEL5_SHIFT)
		|| key == KEY_CAPS_LOCK || key == KEY_NUM_LOCK
		|| key == KEY_SCROLL_LOCK;
}


//! The legacy ctrl mapping of ASCII keys (kitty's table).
static int
legacy_control(uint32 c)
{
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 1;
	switch (c) {
		case ' ':
		case '2':
		case '@':
			return 0;
		case '/':
		case '7':
		case '_':
			return 31;
		case '3':
		case '[':
			return 27;
		case '4':
		case '\\':
			return 28;
		case '5':
		case ']':
			return 29;
		case '6':
		case '^':
		case '~':
			return 30;
		case '8':
		case '?':
			return 127;
	}
	return -1;
}


static bool
is_legacy_text_key(uint32 c)
{
	if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
		return true;
	return c != 0 && strchr("`-=[]\\;',./", (int)c) != NULL;
}


bool
Terminal::EncodeKey(const TermKeyEvent& event, std::string& output) const
{
	uint32 flags = fScreen->keyboardFlags;
	uint32 key = event.key;
	int32 action = event.action;
	bool hasText = is_printable_text(event.text);

	// what goes on the wire: no lock keys, except with "report all keys"
	uint32 modifiers = event.modifiers & 0x3f;

	if (flags == 0) {
		// --- legacy encoding
		if (action == KEY_ACTION_RELEASE || is_modifier_key(key))
			return false;

		if (is_keypad_key(key))
			key = keypad_equivalent(key, event.text);

		bool shift = (modifiers & KEY_MODIFIER_SHIFT) != 0;
		bool alt = (modifiers & KEY_MODIFIER_ALT) != 0;
		bool control = (modifiers & KEY_MODIFIER_CONTROL) != 0;
		bool other = (modifiers & ~(KEY_MODIFIER_SHIFT | KEY_MODIFIER_ALT
			| KEY_MODIFIER_CONTROL)) != 0;
		char buffer[64];

		switch (key) {
			case KEY_ENTER:
				if (other)
					break;
				if (alt)
					output += "\x1b";
				output += fModes.lineFeedNewLine ? "\r\n" : "\r";
				return true;
			case KEY_ESCAPE:
				if (other)
					break;
				output += alt ? "\x1b\x1b" : "\x1b";
				return true;
			case KEY_BACKSPACE:
			{
				if (other)
					break;
				if (alt)
					output += "\x1b";
				bool backspace = fModes.backarrowSendsBS ? !control : control;
				output += backspace ? "\x08" : "\x7f";
				return true;
			}
			case KEY_TAB:
				if (other)
					break;
				if (shift) {
					if (control)
						break;	// CSI u
					output += alt ? "\x1b\x1b[Z" : "\x1b[Z";
				} else
					output += alt ? "\x1b\t" : "\t";
				return true;
		}

		const FunctionalKey* functional = find_functional_key(key);
		if (functional != NULL && functional->final != 'u') {
			if (modifiers == 0) {
				if (functional->number == 1) {
					// arrows, Home, End: SS3 in cursor key mode; F1-F4
					// always
					bool ss3 = (functional->final >= 'P'
							&& functional->final <= 'S')
						|| fModes.cursorKeys;
					snprintf(buffer, sizeof(buffer), "\x1b%c%c",
						ss3 ? 'O' : '[', functional->final);
				} else {
					snprintf(buffer, sizeof(buffer), "\x1b[%u~",
						(unsigned)functional->number);
				}
			} else if (functional->number == 1) {
				snprintf(buffer, sizeof(buffer), "\x1b[1;%u%c",
					(unsigned)modifiers + 1, functional->final);
			} else {
				snprintf(buffer, sizeof(buffer), "\x1b[%u;%u~",
					(unsigned)functional->number, (unsigned)modifiers + 1);
			}
			output += buffer;
			return true;
		}

		if (key < KEY_ESCAPE) {
			// a key with a character
			uint32 c = key;
			if (c == ' ' && !other) {
				if (alt)
					output += "\x1b";
				output += control ? std::string(1, '\0') : std::string(" ");
				return true;
			}

			if (fModes.modifyOtherKeys >= 2 && (control || alt)
				&& !(alt && !control && !hasText)) {
				// xterm's modifyOtherKeys
				uint32 character = shift && event.shiftedKey != 0
					? event.shiftedKey : c;
				snprintf(buffer, sizeof(buffer), "\x1b[27;%u;%u~",
					(unsigned)modifiers + 1, (unsigned)character);
				output += buffer;
				return true;
			}

			if (!other && !(control && shift) && is_legacy_text_key(c)) {
				if (alt)
					output += "\x1b";
				if (control) {
					int mapped = legacy_control(c);
					if (mapped >= 0)
						output += (char)mapped;
					else
						append_utf8(output, c);
				} else if (shift && hasText)
					output += event.text;
				else if (shift && event.shiftedKey != 0)
					append_utf8(output, event.shiftedKey);
				else if (hasText)
					output += event.text;
				else
					append_utf8(output, c);
				return true;
			}

			if (!other && !control && hasText) {
				// text, perhaps with alt
				if (alt)
					output += "\x1b";
				output += event.text;
				return true;
			}

			if (control && !other && !shift && hasText && !alt) {
				// a control character the keymap made
				output += event.text;
				return true;
			}
		} else if (hasText && modifiers == 0) {
			output += event.text;
			return true;
		}

		// anything else, keys legacy terminals don't send (media keys and
		// the like) included: CSI u
		uint32 number = key;
		if (functional != NULL)
			number = functional->number;
		if (modifiers == 0)
			snprintf(buffer, sizeof(buffer), "\x1b[%uu", (unsigned)number);
		else {
			snprintf(buffer, sizeof(buffer), "\x1b[%u;%uu", (unsigned)number,
				(unsigned)modifiers + 1);
		}
		output += buffer;
		return true;
	}

	// --- kitty's progressive enhancement
	bool reportEvents = (flags & KEYBOARD_REPORT_EVENTS) != 0;
	bool reportAll = (flags & KEYBOARD_REPORT_ALL_KEYS) != 0;
	bool reportAlternates = (flags & KEYBOARD_REPORT_ALTERNATES) != 0;
	bool reportText = (flags & KEYBOARD_REPORT_TEXT) != 0 && reportAll;

	if (action == KEY_ACTION_REPEAT && !reportEvents)
		action = KEY_ACTION_PRESS;
	if (action == KEY_ACTION_RELEASE && !reportEvents)
		return false;
	if (is_modifier_key(key) && !reportAll)
		return false;

	if (reportAll)
		modifiers = event.modifiers;

	// Enter, Tab and Backspace stay legacy unless all keys are reported
	if (!reportAll && (key == KEY_ENTER || key == KEY_TAB
			|| key == KEY_BACKSPACE)) {
		if (action == KEY_ACTION_RELEASE)
			return false;
		uint32 legacyModifiers = modifiers;
		if ((legacyModifiers & ~(KEY_MODIFIER_SHIFT | KEY_MODIFIER_ALT
				| KEY_MODIFIER_CONTROL)) == 0) {
			bool alt = (legacyModifiers & KEY_MODIFIER_ALT) != 0;
			bool control = (legacyModifiers & KEY_MODIFIER_CONTROL) != 0;
			bool shift = (legacyModifiers & KEY_MODIFIER_SHIFT) != 0;
			if (key == KEY_ENTER && !control) {
				output += alt ? "\x1b\r" : "\r";
				return true;
			}
			if (key == KEY_BACKSPACE && !shift) {
				if (alt)
					output += "\x1b";
				output += control ? "\x08" : "\x7f";
				return true;
			}
			if (key == KEY_TAB && !control) {
				if (shift)
					output += alt ? "\x1b\x1b[Z" : "\x1b[Z";
				else
					output += alt ? "\x1b\t" : "\t";
				return true;
			}
		}
	}

	// text keys without modifiers (or with shift) are sent as text
	bool textModifiersOnly = (modifiers
		& ~(KEY_MODIFIER_SHIFT | KEY_MODIFIER_CAPS_LOCK
			| KEY_MODIFIER_NUM_LOCK)) == 0;
	if (!reportAll && hasText && textModifiersOnly
		&& key != KEY_ESCAPE) {
		if (action == KEY_ACTION_RELEASE)
			return false;
		output += event.text;
		return true;
	}

	if (!reportAll && hasText)
		modifiers &= ~(KEY_MODIFIER_CAPS_LOCK | KEY_MODIFIER_NUM_LOCK);

	const FunctionalKey* functional = find_functional_key(key);
	uint32 number = key;
	char final = 'u';
	if (functional != NULL) {
		number = functional->number;
		final = functional->final;
	}
	if (key == KEY_KP_BEGIN && !reportAll) {
		number = 1;
		final = 'E';
	}

	// the key number is the unshifted key
	if (functional == NULL && number < KEY_ESCAPE && number >= 'A'
		&& number <= 'Z') {
		number += 'a' - 'A';
	}

	std::string sequence = "\x1b[";
	char buffer[32];
	snprintf(buffer, sizeof(buffer), "%u", (unsigned)number);

	bool needAlternates = false;
	std::string alternates;
	if (reportAlternates && final == 'u') {
		uint32 shifted = (modifiers & KEY_MODIFIER_SHIFT) != 0
			? event.shiftedKey : 0;
		if (shifted == number)
			shifted = 0;
		uint32 base = event.baseKey != number ? event.baseKey : 0;
		if (shifted != 0 || base != 0) {
			needAlternates = true;
			char alternateBuffer[32];
			if (shifted != 0)
				snprintf(alternateBuffer, sizeof(alternateBuffer), ":%u",
					(unsigned)shifted);
			else
				alternateBuffer[0] = ':', alternateBuffer[1] = '\0';
			alternates = alternateBuffer;
			if (base != 0) {
				snprintf(alternateBuffer, sizeof(alternateBuffer), ":%u",
					(unsigned)base);
				alternates += alternateBuffer;
			}
		}
	}

	std::string text;
	if (reportText && hasText) {
		std::vector<uint32> codePoints;
		decode_utf8(event.text, codePoints);
		for (size_t i = 0; i < codePoints.size(); i++) {
			snprintf(buffer + 0, sizeof(buffer), i == 0 ? "%u" : ":%u",
				(unsigned)codePoints[i]);
			text += buffer;
		}
	}

	bool needEvent = reportEvents && action != KEY_ACTION_PRESS;
	bool needModifiers = modifiers != 0 || needEvent || !text.empty();

	if (final != 'u' && final != '~' && !needModifiers && !needAlternates) {
		// CSI letter
		sequence += final;
		output += sequence;
		return true;
	}

	snprintf(buffer, sizeof(buffer), "%u", (unsigned)number);
	sequence += buffer;
	sequence += alternates;
	if (needModifiers) {
		snprintf(buffer, sizeof(buffer), ";%u", (unsigned)modifiers + 1);
		sequence += buffer;
		if (needEvent) {
			snprintf(buffer, sizeof(buffer), ":%d", (int)action);
			sequence += buffer;
		}
		if (!text.empty())
			sequence += ";" + text;
	}
	sequence += final;
	output += sequence;
	return true;
}


bool
Terminal::EncodeMouse(int32 type, int32 button, uint32 modifiers,
	int32 column, int32 row, int32 pixelX, int32 pixelY,
	std::string& output) const
{
	int32 tracking = fModes.mouseTracking;
	if (tracking == MOUSE_TRACKING_NONE)
		return false;

	bool wheel = button >= MOUSE_BUTTON_WHEEL_UP
		&& button <= MOUSE_BUTTON_WHEEL_RIGHT;

	switch (type) {
		case MOUSE_PRESS:
			break;
		case MOUSE_RELEASE:
			if (tracking == MOUSE_TRACKING_X10 || wheel)
				return false;
			break;
		case MOUSE_MOTION:
			if (tracking == MOUSE_TRACKING_ANY)
				break;
			if (tracking == MOUSE_TRACKING_BUTTON
				&& button != MOUSE_BUTTON_NONE) {
				break;
			}
			return false;
		case MOUSE_LEAVE:
			if (fModes.mouseEncoding != MOUSE_ENCODING_SGR_PIXELS)
				return false;
			break;
	}

	int32 code = button;
	if (type == MOUSE_RELEASE && fModes.mouseEncoding != MOUSE_ENCODING_SGR
		&& fModes.mouseEncoding != MOUSE_ENCODING_SGR_PIXELS) {
		code = 3;
	}
	if (type == MOUSE_MOTION)
		code += 32;
	if (type == MOUSE_LEAVE)
		code = 256 | 32;

	// xterm modifiers: shift 4, meta 8, control 16 (not for X10)
	if (tracking != MOUSE_TRACKING_X10) {
		if ((modifiers & KEY_MODIFIER_SHIFT) != 0)
			code |= 4;
		if ((modifiers & KEY_MODIFIER_ALT) != 0)
			code |= 8;
		if ((modifiers & KEY_MODIFIER_CONTROL) != 0)
			code |= 16;
	}

	int32 x = column + 1;
	int32 y = row + 1;
	char buffer[64];

	switch (fModes.mouseEncoding) {
		case MOUSE_ENCODING_SGR:
		case MOUSE_ENCODING_SGR_PIXELS:
		{
			if (fModes.mouseEncoding == MOUSE_ENCODING_SGR_PIXELS) {
				x = std::max((int32)0, pixelX) + 1;
				y = std::max((int32)0, pixelY) + 1;
			}
			snprintf(buffer, sizeof(buffer), "\x1b[<%d;%d;%d%c", (int)code,
				(int)x, (int)y, type == MOUSE_RELEASE ? 'm' : 'M');
			output += buffer;
			return true;
		}
		case MOUSE_ENCODING_URXVT:
			snprintf(buffer, sizeof(buffer), "\x1b[%d;%d;%dM",
				(int)code + 32, (int)x, (int)y);
			output += buffer;
			return true;
		case MOUSE_ENCODING_UTF8:
		{
			if (x > 2015 || y > 2015)
				return false;
			output += "\x1b[M";
			append_utf8(output, code + 32);
			append_utf8(output, x + 32);
			append_utf8(output, y + 32);
			return true;
		}
		default:
			if (x > 223 || y > 223)
				return false;
			output += "\x1b[M";
			output += (char)(code + 32);
			output += (char)(x + 32);
			output += (char)(y + 32);
			return true;
	}
}


/*!	Text pasted into the terminal: line breaks as carriage returns, and in
	brackets if the program asked for them. The end bracket and other
	escape sequences are taken out of the text, so a paste cannot end the
	paste or control the terminal.
*/
void
Terminal::EncodePaste(const char* text, size_t length,
	std::string& output) const
{
	bool bracketed = fModes.bracketedPaste;
	if (bracketed)
		output += "\x1b[200~";

	for (size_t i = 0; i < length; i++) {
		char c = text[i];
		if (c == '\r') {
			if (i + 1 < length && text[i + 1] == '\n')
				i++;
			output += '\r';
		} else if (c == '\n')
			output += '\r';
		else if (c == '\x1b' && bracketed) {
			// drop escapes: no "\e[201~" inside the brackets
		} else if (c == '\0')
			continue;
		else
			output += c;
	}

	if (bracketed)
		output += "\x1b[201~";
}
