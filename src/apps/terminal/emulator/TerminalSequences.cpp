/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


//!	Controls, escape sequences, control sequences and device control strings.


#include "Terminal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include <OS.h>

#include "TerminalGraphics.h"


static const char* const kTerminalName = "airos-terminal";
static const char* const kTerminalVersion = "2.0";

static const size_t kMaxKeyboardStackSize = 32;
static const size_t kMaxSGRStackSize = 10;
static const size_t kMaxTitleStackSize = 16;


// #pragma mark - controls


void
Terminal::Execute(uint8 control)
{
	switch (control) {
		case 0x07:	// BEL
			if (fListener != NULL)
				fListener->TerminalBell();
			break;
		case 0x08:	// BS
			_Backspace();
			break;
		case 0x09:	// HT
			_Tab(1);
			break;
		case 0x0a:	// LF
		case 0x0b:	// VT
		case 0x0c:	// FF
			_LineFeed();
			if (fModes.lineFeedNewLine)
				_CarriageReturn();
			break;
		case 0x0d:	// CR
			_CarriageReturn();
			break;
		case 0x0e:	// SO: G1 into GL
			fCharsets.gl = 1;
			break;
		case 0x0f:	// SI: G0 into GL
			fCharsets.gl = 0;
			break;
		default:
			break;
	}
}


// #pragma mark - escape sequences


void
Terminal::EscDispatch(const VTSequence& sequence)
{
	if (sequence.intermediateCount == 0) {
		switch (sequence.final) {
			case '7':	// DECSC
				_SaveCursor();
				break;
			case '8':	// DECRC
				_RestoreCursor();
				break;
			case 'D':	// IND
				_LineFeed();
				break;
			case 'E':	// NEL
				_LineFeed();
				_CarriageReturn();
				break;
			case 'H':	// HTS
				_SetTabStop();
				break;
			case 'M':	// RI
				_ReverseIndex();
				break;
			case 'N':	// SS2
				fCharsets.singleShift = 2;
				break;
			case 'O':	// SS3
				fCharsets.singleShift = 3;
				break;
			case 'c':	// RIS
				FullReset();
				break;
			case '=':	// DECKPAM
				fModes.applicationKeypad = true;
				break;
			case '>':	// DECKPNM
				fModes.applicationKeypad = false;
				break;
			case '6':	// DECBI: back index
				if (fCursorX == fLeft)
					_ScrollRight(1);
				else if (fCursorX > 0) {
					fCursorX--;
					fPendingWrap = false;
				}
				break;
			case '9':	// DECFI: forward index
				if (fCursorX == fRight)
					_ScrollLeft(1);
				else if (fCursorX < fColumns - 1) {
					fCursorX++;
					fPendingWrap = false;
				}
				break;
			case 'n':	// LS2
				fCharsets.gl = 2;
				break;
			case 'o':	// LS3
				fCharsets.gl = 3;
				break;
			case '|':	// LS3R
				fCharsets.gr = 3;
				break;
			case '}':	// LS2R
				fCharsets.gr = 2;
				break;
			case '~':	// LS1R
				fCharsets.gr = 1;
				break;
			case '\\':	// ST on its own
				break;
		}
		_Changed();
		return;
	}

	char intermediate = sequence.intermediates[0];
	switch (intermediate) {
		case '(':
		case ')':
		case '*':
		case '+':
		{
			// designate G0 - G3
			int32 set = intermediate - '(';
			uint8 designation = sequence.final;
			if (designation != '0' && designation != 'A')
				designation = 'B';
			fCharsets.designations[set] = designation;
			break;
		}
		case '#':
			if (sequence.final == '8') {
				// DECALN: fill the screen with Es
				fTop = 0;
				fBottom = fRows - 1;
				fLeft = 0;
				fRight = fColumns - 1;
				for (int32 row = 0; row < fRows; row++) {
					TerminalCell* cells = _Cells(row);
					for (int32 x = 0; x < fColumns; x++) {
						cells[x].character = 'E';
						cells[x].hyperlink = 0;
						cells[x].style = CellStyle();
					}
					_Line(row).flags = 0;
				}
				_TouchRange(0, fRows - 1);
				_SetCursor(0, 0);
			}
			break;
		case ' ':
			// S7C1T, S8C1T, ANSI conformance levels: nothing to do
			break;
	}
}


// #pragma mark - control sequences


void
Terminal::CsiDispatch(const VTSequence& sequence)
{
	const VTSequence& s = sequence;
	int32 p0 = s.Parameter(0, 1);
	if (p0 == 0)
		p0 = 1;

	if (s.marker == 0 && s.intermediateCount == 0) {
		switch (s.final) {
			case '@':	// ICH
				_InsertCharacters(p0);
				return;
			case 'A':	// CUU
			{
				int32 top = fCursorY >= fTop ? fTop : 0;
				_SetCursor(fCursorX, std::max(top, fCursorY - p0));
				return;
			}
			case 'B':	// CUD
			case 'e':	// VPR
			{
				int32 bottom = fCursorY <= fBottom ? fBottom : fRows - 1;
				_SetCursor(fCursorX, std::min(bottom, fCursorY + p0));
				return;
			}
			case 'C':	// CUF
			case 'a':	// HPR
			{
				int32 right = fCursorX <= fRight ? fRight : fColumns - 1;
				_SetCursor(std::min(right, fCursorX + p0), fCursorY);
				return;
			}
			case 'D':	// CUB
			{
				int32 left = fCursorX >= fLeft ? fLeft : 0;
				int32 x = fCursorX;
				if (fPendingWrap && fModes.reverseWrap)
					p0--;
				_SetCursor(std::max(left, x - p0), fCursorY);
				return;
			}
			case 'E':	// CNL
			{
				int32 bottom = fCursorY <= fBottom ? fBottom : fRows - 1;
				_SetCursor(fCursorX >= fLeft ? fLeft : 0,
					std::min(bottom, fCursorY + p0));
				return;
			}
			case 'F':	// CPL
			{
				int32 top = fCursorY >= fTop ? fTop : 0;
				_SetCursor(fCursorX >= fLeft ? fLeft : 0,
					std::max(top, fCursorY - p0));
				return;
			}
			case 'G':	// CHA
			case '`':	// HPA
				if (fModes.origin)
					_SetCursor(std::min(fLeft + p0 - 1, fRight), fCursorY);
				else
					_SetCursor(p0 - 1, fCursorY);
				return;
			case 'H':	// CUP
			case 'f':	// HVP
			{
				int32 column = s.Parameter(1, 1);
				if (column == 0)
					column = 1;
				_MoveCursorTo(column - 1, p0 - 1);
				return;
			}
			case 'I':	// CHT
				_Tab(p0);
				return;
			case 'J':	// ED
				_EraseInDisplay(s.Parameter(0, 0), false);
				return;
			case 'K':	// EL
				_EraseInLine(s.Parameter(0, 0), false);
				return;
			case 'L':	// IL
				_InsertLines(p0);
				return;
			case 'M':	// DL
				_DeleteLines(p0);
				return;
			case 'P':	// DCH
				_DeleteCharacters(p0);
				return;
			case 'S':	// SU
				_ScrollUp(p0);
				return;
			case 'T':	// SD
				if (s.parameterCount <= 1)
					_ScrollDown(p0);
				// with more parameters: mouse highlight tracking, ignored
				return;
			case 'X':	// ECH
				_EraseCharacters(p0);
				return;
			case 'Z':	// CBT
				_BackTab(p0);
				return;
			case 'b':	// REP
				_RepeatCharacter(p0);
				return;
			case 'c':	// DA1
				if (s.Parameter(0, 0) == 0)
					_Reply("\x1b[?62;22;52c");
				return;
			case 'd':	// VPA
				if (fModes.origin)
					_SetCursor(fCursorX, std::min(fTop + p0 - 1, fBottom));
				else
					_SetCursor(fCursorX, p0 - 1);
				return;
			case 'g':	// TBC
				_ClearTabStops(s.Parameter(0, 0));
				return;
			case 'h':	// SM
			case 'l':	// RM
				for (int32 i = 0; i < s.parameterCount; i++)
					_SetMode(s.Parameter(i, 0), false, s.final == 'h');
				return;
			case 'm':	// SGR
				_SelectGraphicRendition(s);
				return;
			case 'n':	// DSR
				_DeviceStatusReport(s);
				return;
			case 'r':	// DECSTBM
				_SetScrollRegion(s.Parameter(0, 1) - 1,
					s.Parameter(1, fRows) - 1);
				return;
			case 's':
				if (fModes.leftRightMargins) {
					// DECSLRM
					_SetHorizontalMargins(s.Parameter(0, 1) - 1,
						s.Parameter(1, fColumns) - 1);
				} else
					_SaveCursor();	// SCOSC
				return;
			case 't':	// XTWINOPS
				_WindowOperation(s);
				return;
			case 'u':	// SCORC
				_RestoreCursor();
				return;
			case 'x':	// DECREQTPARM
			{
				int32 request = s.Parameter(0, 0);
				if (request <= 1)
					_Reply("\x1b[%d;1;1;128;128;1;0x", request + 2);
				return;
			}
		}
		return;
	}

	if (s.marker == '?' && s.intermediateCount == 0) {
		switch (s.final) {
			case 'h':	// DECSET
			case 'l':	// DECRST
				for (int32 i = 0; i < s.parameterCount; i++)
					_SetMode(s.Parameter(i, 0), true, s.final == 'h');
				return;
			case 'J':	// DECSED
				_EraseInDisplay(s.Parameter(0, 0), true);
				return;
			case 'K':	// DECSEL
				_EraseInLine(s.Parameter(0, 0), true);
				return;
			case 'n':	// DSR, DEC style
				_DeviceStatusReport(s);
				return;
			case 's':	// XTSAVE
				if (s.parameterCount == 0) {
					// kitty: all modes without side effects
					static const int32 kModes[] = { 1, 5, 7, 8, 25, 1000,
						1002, 1003, 1004, 1005, 1006, 1015, 1016, 2004, 2031,
						2048, 5522 };
					for (size_t i = 0; i < sizeof(kModes) / sizeof(kModes[0]);
							i++) {
						fSavedModes[kModes[i]]
							= _ModeState(kModes[i], true) == 1;
					}
					fSavedModes[-4] = fModes.insert;
					fSavedModes[-20] = fModes.lineFeedNewLine;
				} else {
					for (int32 i = 0; i < s.parameterCount; i++) {
						int32 mode = s.Parameter(i, 0);
						fSavedModes[mode] = _ModeState(mode, true) == 1;
					}
				}
				return;
			case 'r':	// XTRESTORE
				if (s.parameterCount == 0) {
					std::map<int32, bool> saved = fSavedModes;
					for (std::map<int32, bool>::iterator it = saved.begin();
							it != saved.end(); it++) {
						if (it->first < 0)
							_SetMode(-it->first, false, it->second);
						else if (it->first != 6 && it->first != 3)
							_SetMode(it->first, true, it->second);
					}
				} else {
					for (int32 i = 0; i < s.parameterCount; i++) {
						int32 mode = s.Parameter(i, 0);
						std::map<int32, bool>::iterator found
							= fSavedModes.find(mode);
						if (found != fSavedModes.end())
							_SetMode(mode, true, found->second);
					}
				}
				return;
			case 'u':	// kitty keyboard protocol: query
				_KeyboardProtocol(s);
				return;
			case 'W':	// DECST8C: tab stops every 8 columns
				if (s.Parameter(0, 0) == 5)
					_ResetTabStops();
				return;
		}
		return;
	}

	if (s.marker == '?' && s.intermediateCount == 1
		&& s.intermediates[0] == '$' && s.final == 'p') {
		_RequestMode(s);	// DECRQM, private
		return;
	}

	if (s.marker == '>') {
		if (s.intermediateCount == 0) {
			switch (s.final) {
				case 'c':	// DA2: VT220, our version, no ROM cartridge
					if (s.Parameter(0, 0) == 0)
						_Reply("\x1b[>1;4000;1c");
					return;
				case 'm':	// XTMODKEYS
				{
					int32 resource = s.Parameter(0, -1);
					if (resource == 4) {
						fModes.modifyOtherKeys = s.parameterCount > 1
							? s.Parameter(1, 0) : 0;
					} else if (resource < 0)
						fModes.modifyOtherKeys = 0;
					return;
				}
				case 'n':	// XTMODKEYS reset
					if (s.Parameter(0, -1) == 4)
						fModes.modifyOtherKeys = 0;
					return;
				case 'q':	// XTVERSION
					if (s.Parameter(0, 0) == 0) {
						_Reply("\x1bP>|%s(%s)\x1b\\", kTerminalName,
							kTerminalVersion);
					}
					return;
				case 'u':	// kitty keyboard protocol: push
					_KeyboardProtocol(s);
					return;
				case 't':	// XTSMTITLE: title modes, hex titles unsupported
				case 'T':	// XTRMTITLE
				case 'p':	// XTSMPOINTER
					return;
			}
		}
		return;
	}

	if (s.marker == '<' && s.intermediateCount == 0 && s.final == 'u') {
		_KeyboardProtocol(s);	// kitty keyboard protocol: pop
		return;
	}

	if (s.marker == '=' && s.intermediateCount == 0) {
		switch (s.final) {
			case 'u':	// kitty keyboard protocol: set
				_KeyboardProtocol(s);
				return;
			case 'c':	// DA3: unit id
				if (s.Parameter(0, 0) == 0)
					_Reply("\x1bP!|00000000\x1b\\");
				return;
		}
		return;
	}

	if (s.marker != 0 || s.intermediateCount != 1)
		return;

	switch (s.intermediates[0]) {
		case ' ':
			switch (s.final) {
				case 'q':	// DECSCUSR
					_SetCursorStyle(s.Parameter(0, 0));
					return;
				case '@':	// SL
					_ScrollLeft(p0);
					return;
				case 'A':	// SR
					_ScrollRight(p0);
					return;
			}
			return;

		case '!':
			if (s.final == 'p')	// DECSTR
				_SoftReset();
			return;

		case '"':
			if (s.final == 'q') {
				// DECSCA
				int32 value = s.Parameter(0, 0);
				if (value == 1)
					fStyle.flags |= CELL_PROTECTED;
				else
					fStyle.flags &= ~CELL_PROTECTED;
			}
			return;

		case '$':
			switch (s.final) {
				case 'p':	// DECRQM, ANSI
					_RequestMode(s);
					return;
				case 'r':	// DECCARA
					_ChangeAttributesInArea(s, false);
					return;
				case 't':	// DECRARA
					_ChangeAttributesInArea(s, true);
					return;
				case 'v':	// DECCRA
					_CopyRectangle(s);
					return;
				case 'x':	// DECFRA
					_FillRectangle(s);
					return;
				case 'z':	// DECERA
					_EraseRectangle(s, false);
					return;
				case '{':	// DECSERA
					_EraseRectangle(s, true);
					return;
			}
			return;

		case '*':
			if (s.final == 'x')	// DECSACE
				fRectangularAttributes = s.Parameter(0, 0) == 2;
			return;

		case '+':
			if (s.final == 'T')	// kitty: unscroll
				_Unscroll(s.Parameter(0, 1));
			return;

		case '\'':
			switch (s.final) {
				case '}':	// DECIC
					_InsertColumns(p0);
					return;
				case '~':	// DECDC
					_DeleteColumns(p0);
					return;
			}
			return;

		case '#':
			switch (s.final) {
				case 'P':	// XTPUSHCOLORS
					_PushColors(s.Parameter(0, 0));
					return;
				case 'Q':	// XTPOPCOLORS
					_PopColors(s.Parameter(0, 0));
					return;
				case 'R':	// XTREPORTCOLORS
					_Reply("\x1b[%d;%d#Q", (int)fColorStack.size(),
						(int)fColorStack.size());
					return;
				case '{':	// XTPUSHSGR
				case 'p':
					_PushSGR(s);
					return;
				case '}':	// XTPOPSGR
				case 'q':
					_PopSGR();
					return;
			}
			return;
	}
}


// #pragma mark - modes


void
Terminal::_SetMode(int32 mode, bool privateMode, bool set)
{
	if (!privateMode) {
		switch (mode) {
			case 4:
				fModes.insert = set;
				break;
			case 20:
				fModes.lineFeedNewLine = set;
				break;
		}
		return;
	}

	bool modesChanged = false;
	switch (mode) {
		case 1:
			fModes.cursorKeys = set;
			break;
		case 3:
			// DECCOLM: only if allowed (40); the window keeps its size
			if (fModes.allow132) {
				fModes.column132 = set;
				fTop = 0;
				fBottom = fRows - 1;
				fLeft = 0;
				fRight = fColumns - 1;
				_ClearRows(0, fRows - 1);
				_SetCursor(0, 0);
			}
			break;
		case 5:
			if (fModes.reverseVideo != set) {
				fModes.reverseVideo = set;
				_TouchRange(0, fRows - 1);
				fColorsGeneration++;
			}
			break;
		case 6:
			fModes.origin = set;
			_MoveCursorTo(0, 0);
			break;
		case 7:
			fModes.autoWrap = set;
			if (!set)
				fPendingWrap = false;
			break;
		case 8:
			fModes.autoRepeat = set;
			break;
		case 12:
			fModes.cursorBlink = set;
			fCursorBlinkSet = true;
			_Changed();
			break;
		case 25:
			fModes.cursorVisible = set;
			_Changed();
			break;
		case 40:
			fModes.allow132 = set;
			break;
		case 45:
			fModes.reverseWrap = set;
			break;
		case 66:
			fModes.applicationKeypad = set;
			break;
		case 67:
			fModes.backarrowSendsBS = set;
			break;
		case 69:
			fModes.leftRightMargins = set;
			if (!set) {
				fLeft = 0;
				fRight = fColumns - 1;
			}
			break;
		case 9:
		case 1000:
		case 1002:
		case 1003:
			fModes.mouseTracking = set ? mode : MOUSE_TRACKING_NONE;
			modesChanged = true;
			break;
		case 1005:
		case 1006:
		case 1015:
		case 1016:
			if (set)
				fModes.mouseEncoding = mode;
			else if (fModes.mouseEncoding == mode)
				fModes.mouseEncoding = MOUSE_ENCODING_DEFAULT;
			modesChanged = true;
			break;
		case 1004:
			fModes.focusReporting = set;
			if (set)
				_Reply(fFocused ? "\x1b[I" : "\x1b[O");
			break;
		case 1007:
			fModes.alternateScroll = set;
			break;
		case 1034:
			fModes.interpretMeta = set;
			break;
		case 1036:
		case 1039:
			fModes.metaSendsEscape = set;
			break;
		case 47:
			_SwitchScreen(set, false, false);
			break;
		case 1047:
			if (!set && fScreen == &fAlternate) {
				// cleared when leaving it
				_ClearRows(0, fRows - 1);
			}
			_SwitchScreen(set, false, false);
			break;
		case 1048:
			if (set)
				_SaveCursor();
			else
				_RestoreCursor();
			break;
		case 1049:
			if (set) {
				if (fScreen != &fAlternate) {
					_SaveCursor();
					_SwitchScreen(true, true, false);
				}
			} else if (fScreen == &fAlternate) {
				_SwitchScreen(false, false, false);
				_RestoreCursor();
			}
			break;
		case 2004:
			fModes.bracketedPaste = set;
			break;
		case 2026:
			fModes.synchronizedOutput = set;
			if (set)
				fSynchronizedSince = system_time();
			_Changed();
			break;
		case 2027:
			fModes.graphemeClusters = set;
			break;
		case 2031:
			fModes.colorSchemeReports = set;
			break;
		case 2048:
			fModes.inBandResize = set;
			if (set) {
				_Reply("\x1b[48;%d;%d;%d;%dt", fRows, fColumns,
					fRows * fCellHeight, fColumns * fCellWidth);
			}
			break;
		case 5522:
			fModes.pasteEvents = set;
			break;
	}

	if (modesChanged && fListener != NULL)
		fListener->TerminalModesChanged();
}


/*!	The state DECRQM reports: 0 not recognized, 1 set, 2 reset,
	3 permanently set, 4 permanently reset.
*/
int32
Terminal::_ModeState(int32 mode, bool privateMode) const
{
#define STATE(x) ((x) ? 1 : 2)
	if (!privateMode) {
		switch (mode) {
			case 2:		// KAM
				return 4;
			case 4:
				return STATE(fModes.insert);
			case 12:	// SRM
				return 3;
			case 20:
				return STATE(fModes.lineFeedNewLine);
		}
		return 0;
	}

	switch (mode) {
		case 1:
			return STATE(fModes.cursorKeys);
		case 2:		// DECANM
			return 3;
		case 3:
			return STATE(fModes.column132);
		case 4:		// DECSCLM
			return 4;
		case 5:
			return STATE(fModes.reverseVideo);
		case 6:
			return STATE(fModes.origin);
		case 7:
			return STATE(fModes.autoWrap);
		case 8:
			return STATE(fModes.autoRepeat);
		case 12:
			return STATE(fCursorBlinkSet ? fModes.cursorBlink
				: fDefaultCursorBlinking);
		case 25:
			return STATE(fModes.cursorVisible);
		case 40:
			return STATE(fModes.allow132);
		case 45:
			return STATE(fModes.reverseWrap);
		case 66:
			return STATE(fModes.applicationKeypad);
		case 67:
			return STATE(fModes.backarrowSendsBS);
		case 69:
			return STATE(fModes.leftRightMargins);
		case 9:
		case 1000:
		case 1002:
		case 1003:
			return STATE(fModes.mouseTracking == mode);
		case 1005:
		case 1006:
		case 1015:
		case 1016:
			return STATE(fModes.mouseEncoding == mode);
		case 1004:
			return STATE(fModes.focusReporting);
		case 1007:
			return STATE(fModes.alternateScroll);
		case 1034:
			return STATE(fModes.interpretMeta);
		case 1036:
		case 1039:
			return STATE(fModes.metaSendsEscape);
		case 47:
		case 1047:
		case 1049:
			return STATE(fScreen == &fAlternate);
		case 1048:
			return 2;
		case 2004:
			return STATE(fModes.bracketedPaste);
		case 2026:
			return STATE(fModes.synchronizedOutput);
		case 2027:
			return STATE(fModes.graphemeClusters);
		case 2031:
			return STATE(fModes.colorSchemeReports);
		case 2048:
			return STATE(fModes.inBandResize);
		case 5522:
			return STATE(fModes.pasteEvents);
	}
	return 0;
#undef STATE
}


void
Terminal::_RequestMode(const VTSequence& sequence)
{
	bool privateMode = sequence.marker == '?';
	int32 mode = sequence.Parameter(0, 0);
	_Reply("\x1b[%s%d;%d$y", privateMode ? "?" : "", mode,
		_ModeState(mode, privateMode));
}


void
Terminal::_SetCursorStyle(int32 style)
{
	// 0, 1 blinking block, 2 steady block, 3 blinking underline,
	// 4 steady underline, 5 blinking bar, 6 steady bar
	switch (style) {
		case 0:
			fCursorShape = CURSOR_SHAPE_DEFAULT;
			fCursorBlinkSet = false;
			break;
		case 1:
		case 2:
			fCursorShape = CURSOR_SHAPE_BLOCK;
			break;
		case 3:
		case 4:
			fCursorShape = CURSOR_SHAPE_UNDERLINE;
			break;
		case 5:
		case 6:
			fCursorShape = CURSOR_SHAPE_BEAM;
			break;
		default:
			return;
	}
	if (style != 0) {
		fCursorBlinkSet = true;
		fModes.cursorBlink = (style % 2) == 1;
	}
	_Changed();
}


void
Terminal::_ReportCursorStyle(std::string& reply) const
{
	CursorInfo info = Cursor();
	int32 style = 1;
	switch (info.shape) {
		case CURSOR_SHAPE_UNDERLINE:
			style = 3;
			break;
		case CURSOR_SHAPE_BEAM:
			style = 5;
			break;
		default:
			style = 1;
			break;
	}
	if (!info.blinking)
		style++;
	char buffer[16];
	snprintf(buffer, sizeof(buffer), "%d q", (int)style);
	reply = buffer;
}


// #pragma mark - kitty keyboard protocol


void
Terminal::_KeyboardProtocol(const VTSequence& sequence)
{
	Screen* screen = fScreen;
	switch (sequence.marker) {
		case '?':
			_Reply("\x1b[?%uu", (unsigned)screen->keyboardFlags);
			break;
		case '>':
		{
			uint32 flags = sequence.Parameter(0, 0) & KEYBOARD_ALL_FLAGS;
			if (screen->keyboardStack.size() >= kMaxKeyboardStackSize) {
				screen->keyboardStack.erase(
					screen->keyboardStack.begin());
			}
			screen->keyboardStack.push_back(screen->keyboardFlags);
			screen->keyboardFlags = flags;
			break;
		}
		case '<':
		{
			int32 count = sequence.Parameter(0, 1);
			if (count == 0)
				count = 1;
			for (int32 i = 0; i < count; i++) {
				if (screen->keyboardStack.empty()) {
					// popping more than was pushed resets the flags
					screen->keyboardFlags = 0;
					break;
				}
				screen->keyboardFlags = screen->keyboardStack.back();
				screen->keyboardStack.pop_back();
			}
			break;
		}
		case '=':
		{
			uint32 flags = sequence.Parameter(0, 0) & KEYBOARD_ALL_FLAGS;
			switch (sequence.Parameter(1, 1)) {
				case 1:
					screen->keyboardFlags = flags;
					break;
				case 2:
					screen->keyboardFlags |= flags;
					break;
				case 3:
					screen->keyboardFlags &= ~flags;
					break;
			}
			break;
		}
	}
	if (fListener != NULL)
		fListener->TerminalModesChanged();
}


// #pragma mark - SGR


/*!	Parses an extended color (38, 48, 58) starting at parameter \a index
	(the 38 itself). Returns the index of the last parameter it used, or -1
	if the sequence is not a valid color.
*/
int32
Terminal::_ParseSGRColor(const VTSequence& s, int32 index, TermColor& color)
{
	if (s.IsSubParameter(index + 1)) {
		// colon form: 38:5:n, 38:2:r:g:b, 38:2:cs:r:g:b
		int32 count = s.SubParameterCount(index);
		int32 type = s.Parameter(index + 1, -1);
		if (type == 5 && count >= 2) {
			color = indexed_color(s.Parameter(index + 2, 0) & 0xff);
		} else if (type == 2 && count >= 4) {
			int32 first = count >= 5 ? index + 3 : index + 2;
			color = direct_color(s.Parameter(first, 0) & 0xff,
				s.Parameter(first + 1, 0) & 0xff,
				s.Parameter(first + 2, 0) & 0xff);
		} else
			return -1;
		return index + count;
	}

	// semicolon form: 38;5;n, 38;2;r;g;b
	int32 type = s.Parameter(index + 1, -1);
	if (type == 5) {
		if (index + 2 >= s.parameterCount)
			return -1;
		color = indexed_color(s.Parameter(index + 2, 0) & 0xff);
		return index + 2;
	}
	if (type == 2) {
		if (index + 4 >= s.parameterCount)
			return -1;
		color = direct_color(s.Parameter(index + 2, 0) & 0xff,
			s.Parameter(index + 3, 0) & 0xff,
			s.Parameter(index + 4, 0) & 0xff);
		return index + 4;
	}
	return -1;
}


/*!	Applies the SGR parameters \a start to \a end (exclusive) of \a s to
	\a style. With \a reverseOnly (DECRARA) the attributes are toggled
	instead of set.
*/
void
Terminal::_ApplySGR(const VTSequence& s, int32 start, int32 end,
	CellStyle& style, bool reverseOnly)
{
	if (start >= end) {
		if (!reverseOnly) {
			style.foreground = style.background = style.underlineColor
				= kDefaultColor;
			style.flags &= ~CELL_SGR_FLAGS;
			style.underline = UNDERLINE_NONE;
		}
		return;
	}

	for (int32 i = start; i < end; i++) {
		if (s.IsSubParameter(i))
			continue;	// belongs to a parameter that ignored it

		int32 value = s.Parameter(i, 0);

		if (reverseOnly) {
			uint16 flag = 0;
			switch (value) {
				case 0:
					flag = CELL_BOLD | CELL_BLINK | CELL_INVERSE;
					if (style.underline == UNDERLINE_NONE)
						style.underline = UNDERLINE_SINGLE;
					else
						style.underline = UNDERLINE_NONE;
					break;
				case 1: flag = CELL_BOLD; break;
				case 4:
					style.underline = style.underline == UNDERLINE_NONE
						? UNDERLINE_SINGLE : UNDERLINE_NONE;
					break;
				case 5: flag = CELL_BLINK; break;
				case 7: flag = CELL_INVERSE; break;
			}
			style.flags ^= flag;
			continue;
		}

		switch (value) {
			case 0:
				style.foreground = style.background = style.underlineColor
					= kDefaultColor;
				style.flags &= ~CELL_SGR_FLAGS;
				style.underline = UNDERLINE_NONE;
				break;
			case 1:
				style.flags |= CELL_BOLD;
				break;
			case 2:
				style.flags |= CELL_FAINT;
				break;
			case 3:
				style.flags |= CELL_ITALIC;
				break;
			case 4:
				if (s.IsSubParameter(i + 1)) {
					int32 underline = s.Parameter(i + 1, 1);
					style.underline = std::min(underline,
						(int32)UNDERLINE_DASHED);
				} else
					style.underline = UNDERLINE_SINGLE;
				break;
			case 5:
			case 6:
				style.flags |= CELL_BLINK;
				break;
			case 7:
				style.flags |= CELL_INVERSE;
				break;
			case 8:
				style.flags |= CELL_INVISIBLE;
				break;
			case 9:
				style.flags |= CELL_STRIKE;
				break;
			case 21:
				style.underline = UNDERLINE_DOUBLE;
				break;
			case 22:
				style.flags &= ~(CELL_BOLD | CELL_FAINT);
				break;
			case 23:
				style.flags &= ~CELL_ITALIC;
				break;
			case 24:
				style.underline = UNDERLINE_NONE;
				break;
			case 25:
				style.flags &= ~CELL_BLINK;
				break;
			case 27:
				style.flags &= ~CELL_INVERSE;
				break;
			case 28:
				style.flags &= ~CELL_INVISIBLE;
				break;
			case 29:
				style.flags &= ~CELL_STRIKE;
				break;
			case 38:
			case 48:
			case 58:
			{
				TermColor color;
				int32 last = _ParseSGRColor(s, i, color);
				if (last < 0) {
					// an invalid color ends the sequence
					return;
				}
				if (value == 38)
					style.foreground = color;
				else if (value == 48)
					style.background = color;
				else
					style.underlineColor = color;
				i = last;
				break;
			}
			case 39:
				style.foreground = kDefaultColor;
				break;
			case 49:
				style.background = kDefaultColor;
				break;
			case 53:
				style.flags |= CELL_OVERLINE;
				break;
			case 55:
				style.flags &= ~CELL_OVERLINE;
				break;
			case 59:
				style.underlineColor = kDefaultColor;
				break;
			case 221:
				style.flags &= ~CELL_BOLD;
				break;
			case 222:
				style.flags &= ~CELL_FAINT;
				break;
			default:
				if (value >= 30 && value <= 37)
					style.foreground = indexed_color(value - 30);
				else if (value >= 40 && value <= 47)
					style.background = indexed_color(value - 40);
				else if (value >= 90 && value <= 97)
					style.foreground = indexed_color(value - 90 + 8);
				else if (value >= 100 && value <= 107)
					style.background = indexed_color(value - 100 + 8);
				break;
		}
	}
}


void
Terminal::_SelectGraphicRendition(const VTSequence& sequence)
{
	_ApplySGR(sequence, 0, sequence.parameterCount, fStyle, false);
}


void
Terminal::_PushSGR(const VTSequence& sequence)
{
	if (fSGRStack.size() >= kMaxSGRStackSize)
		fSGRStack.erase(fSGRStack.begin());
	fSGRStack.push_back(fStyle);
}


void
Terminal::_PopSGR()
{
	if (fSGRStack.empty())
		return;
	uint16 protectedFlag = fStyle.flags & CELL_PROTECTED;
	fStyle = fSGRStack.back();
	fStyle.flags = (fStyle.flags & ~CELL_PROTECTED) | protectedFlag;
	fSGRStack.pop_back();
}


// #pragma mark - reports


void
Terminal::_DeviceStatusReport(const VTSequence& sequence)
{
	int32 request = sequence.Parameter(0, 0);
	bool dec = sequence.marker == '?';
	switch (request) {
		case 5:
			if (!dec)
				_Reply("\x1b[0n");
			break;
		case 6:
		{
			// CPR, relative to the margins in origin mode
			int32 x = fCursorX;
			int32 y = fCursorY;
			if (fModes.origin) {
				x -= fLeft;
				y -= fTop;
			}
			if (dec)
				_Reply("\x1b[?%d;%d;1R", y + 1, x + 1);
			else
				_Reply("\x1b[%d;%dR", y + 1, x + 1);
			break;
		}
		case 15:	// printer
			if (dec)
				_Reply("\x1b[?13n");
			break;
		case 25:	// user defined keys
			if (dec)
				_Reply("\x1b[?21n");
			break;
		case 26:	// keyboard: North American
			if (dec)
				_Reply("\x1b[?27;1;0;0n");
			break;
		case 996:	// color scheme (contour, kitty)
			if (dec)
				_Reply("\x1b[?997;%dn", fDarkColorScheme ? 1 : 2);
			break;
	}
}


void
Terminal::_WindowOperation(const VTSequence& sequence)
{
	switch (sequence.Parameter(0, 0)) {
		case 14:	// text area size in pixels
			if (sequence.Parameter(1, 0) == 2) {
				// the window: we report the text area as well
			}
			_Reply("\x1b[4;%d;%dt", fRows * fCellHeight,
				fColumns * fCellWidth);
			break;
		case 16:	// cell size in pixels
			_Reply("\x1b[6;%d;%dt", fCellHeight, fCellWidth);
			break;
		case 18:	// text area size in characters
			_Reply("\x1b[8;%d;%dt", fRows, fColumns);
			break;
		case 19:	// screen size in characters
			_Reply("\x1b[9;%d;%dt", fRows, fColumns);
			break;
		case 22:	// push title
			if (fTitleStack.size() >= kMaxTitleStackSize)
				fTitleStack.erase(fTitleStack.begin());
			fTitleStack.push_back(fTitle);
			break;
		case 23:	// pop title
			if (!fTitleStack.empty()) {
				std::string title = fTitleStack.back();
				fTitleStack.pop_back();
				_SetTitle(title.c_str(), true, true);
			}
			break;
	}
}


// #pragma mark - rectangular areas


/*!	Reads a rectangle Pt;Pl;Pb;Pr from the parameters starting at \a first,
	in screen coordinates (inclusive), clipped to the screen.
*/
bool
Terminal::_RectangleFromParameters(const VTSequence& s, int32 first,
	int32& top, int32& left, int32& bottom, int32& right) const
{
	top = s.Parameter(first, 1);
	left = s.Parameter(first + 1, 1);
	bottom = s.Parameter(first + 2, 0);
	right = s.Parameter(first + 3, 0);
	if (top == 0)
		top = 1;
	if (left == 0)
		left = 1;
	if (bottom == 0)
		bottom = fModes.origin ? fBottom - fTop + 1 : fRows;
	if (right == 0)
		right = fModes.origin ? fRight - fLeft + 1 : fColumns;

	top--;
	left--;
	bottom--;
	right--;
	if (fModes.origin) {
		top += fTop;
		bottom += fTop;
		left += fLeft;
		right += fLeft;
		bottom = std::min(bottom, fBottom);
		right = std::min(right, fRight);
	}
	bottom = std::min(bottom, fRows - 1);
	right = std::min(right, fColumns - 1);
	return top <= bottom && left <= right;
}


void
Terminal::_ChangeAttributesInArea(const VTSequence& s, bool reverse)
{
	int32 top, left, bottom, right;
	if (!_RectangleFromParameters(s, 0, top, left, bottom, right))
		return;

	int32 sgrStart = std::min((int32)4, s.parameterCount);
	for (int32 row = top; row <= bottom; row++) {
		int32 from = left;
		int32 to = right;
		if (!fRectangularAttributes && top != bottom) {
			// stream: from the start to the end, wrapping
			from = row == top ? left : 0;
			to = row == bottom ? right : fColumns - 1;
		}
		TerminalCell* cells = _Cells(row);
		for (int32 x = from; x <= to; x++) {
			CellStyle style = cells[x].style;
			uint16 keep = style.flags & (CELL_WIDE | CELL_WIDE_TAIL
				| CELL_PROTECTED);
			_ApplySGR(s, sgrStart, s.parameterCount, style, reverse);
			style.flags = (style.flags & ~(CELL_WIDE | CELL_WIDE_TAIL
				| CELL_PROTECTED)) | keep;
			cells[x].style = style;
		}
		_Touch(row);
	}
}


void
Terminal::_CopyRectangle(const VTSequence& s)
{
	int32 top, left, bottom, right;
	if (!_RectangleFromParameters(s, 0, top, left, bottom, right))
		return;

	// destination: Pp (page) is parameter 4, then Ptd;Pld at 5, 6
	int32 destinationTop = s.Parameter(5, 1) - 1;
	int32 destinationLeft = s.Parameter(6, 1) - 1;
	if (fModes.origin) {
		destinationTop += fTop;
		destinationLeft += fLeft;
	}
	if (destinationTop >= fRows || destinationLeft >= fColumns)
		return;

	int32 height = std::min(bottom - top + 1, fRows - destinationTop);
	int32 width = std::min(right - left + 1, fColumns - destinationLeft);
	std::vector<TerminalCell> copy((size_t)height * width);
	for (int32 y = 0; y < height; y++) {
		memcpy(&copy[(size_t)y * width], _Cells(top + y) + left,
			width * sizeof(TerminalCell));
	}
	for (int32 y = 0; y < height; y++) {
		memcpy(_Cells(destinationTop + y) + destinationLeft,
			&copy[(size_t)y * width], width * sizeof(TerminalCell));
		_FixWideCharacters(destinationTop + y, destinationLeft,
			destinationLeft + width - 1);
		_Touch(destinationTop + y);
	}
}


void
Terminal::_FillRectangle(const VTSequence& s)
{
	int32 character = s.Parameter(0, 0);
	if (character < 32 || (character > 126 && character < 160))
		return;

	int32 top, left, bottom, right;
	if (!_RectangleFromParameters(s, 1, top, left, bottom, right))
		return;

	for (int32 row = top; row <= bottom; row++) {
		_FixWideCharacters(row, left, right);
		TerminalCell* cells = _Cells(row);
		for (int32 x = left; x <= right; x++) {
			cells[x].character = character;
			cells[x].hyperlink = 0;
			cells[x].style = fStyle;
			cells[x].style.flags &= ~(CELL_WIDE | CELL_WIDE_TAIL);
		}
		_Touch(row);
	}
}


void
Terminal::_EraseRectangle(const VTSequence& s, bool selective)
{
	int32 top, left, bottom, right;
	if (!_RectangleFromParameters(s, 0, top, left, bottom, right))
		return;

	for (int32 row = top; row <= bottom; row++) {
		_FixWideCharacters(row, left, right);
		TerminalCell* cells = _Cells(row);
		for (int32 x = left; x <= right; x++) {
			if (selective && (cells[x].style.flags & CELL_PROTECTED) != 0)
				continue;
			cells[x].character = 0;
			cells[x].hyperlink = 0;
			uint16 keep = cells[x].style.flags & CELL_PROTECTED;
			cells[x].style = CellStyle();
			cells[x].style.flags = keep;
		}
		_Touch(row);
	}
}


// #pragma mark - device control strings


void
Terminal::DcsDispatch(const VTSequence& sequence, const char* data,
	size_t length)
{
	std::string string(data, length);

	if (sequence.marker == 0 && sequence.intermediateCount == 1
		&& sequence.intermediates[0] == '$' && sequence.final == 'q') {
		_RequestStatusString(string.c_str());	// DECRQSS
		return;
	}

	if (sequence.marker == 0 && sequence.intermediateCount == 1
		&& sequence.intermediates[0] == '+' && sequence.final == 'q') {
		_RequestTermcap(string.c_str());	// XTGETTCAP
		return;
	}

	if (sequence.marker == '=' && sequence.final == 's') {
		// iTerm2's synchronized updates: DCS = 1 s / DCS = 2 s
		int32 value = sequence.Parameter(0, 0);
		if (value == 1 || value == 2)
			_SetMode(2026, true, value == 1);
		return;
	}

	// kitty's private DCS codes (remote control and kittens) are not
	// supported; anything else is ignored too.
}


void
Terminal::_RequestStatusString(const char* request)
{
	std::string reply;
	bool valid = true;

	if (strcmp(request, "m") == 0) {
		// the current SGR
		char buffer[128];
		reply = "0";
		const CellStyle& style = fStyle;
		if ((style.flags & CELL_BOLD) != 0) reply += ";1";
		if ((style.flags & CELL_FAINT) != 0) reply += ";2";
		if ((style.flags & CELL_ITALIC) != 0) reply += ";3";
		if (style.underline == UNDERLINE_SINGLE)
			reply += ";4";
		else if (style.underline != UNDERLINE_NONE) {
			snprintf(buffer, sizeof(buffer), ";4:%d", style.underline);
			reply += buffer;
		}
		if ((style.flags & CELL_BLINK) != 0) reply += ";5";
		if ((style.flags & CELL_INVERSE) != 0) reply += ";7";
		if ((style.flags & CELL_INVISIBLE) != 0) reply += ";8";
		if ((style.flags & CELL_STRIKE) != 0) reply += ";9";
		if ((style.flags & CELL_OVERLINE) != 0) reply += ";53";
		struct { TermColor color; int base; } colors[] = {
			{ style.foreground, 38 }, { style.background, 48 },
			{ style.underlineColor, 58 }
		};
		for (int i = 0; i < 3; i++) {
			TermColor color = colors[i].color;
			if (is_indexed_color(color)) {
				uint32 index = color & 0xff;
				if (colors[i].base != 58 && index < 8)
					snprintf(buffer, sizeof(buffer), ";%d",
						colors[i].base - 8 + (int)index);
				else if (colors[i].base != 58 && index < 16)
					snprintf(buffer, sizeof(buffer), ";%d",
						colors[i].base + 52 + (int)index - 8);
				else
					snprintf(buffer, sizeof(buffer), ";%d:5:%u",
						colors[i].base, (unsigned)index);
				reply += buffer;
			} else if (is_direct_color(color)) {
				snprintf(buffer, sizeof(buffer), ";%d:2::%u:%u:%u",
					colors[i].base, (unsigned)((color >> 16) & 0xff),
					(unsigned)((color >> 8) & 0xff),
					(unsigned)(color & 0xff));
				reply += buffer;
			}
		}
		reply += "m";
	} else if (strcmp(request, "r") == 0) {
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "%d;%dr", fTop + 1, fBottom + 1);
		reply = buffer;
	} else if (strcmp(request, "s") == 0) {
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "%d;%ds", fLeft + 1, fRight + 1);
		reply = buffer;
	} else if (strcmp(request, " q") == 0) {
		_ReportCursorStyle(reply);
	} else if (strcmp(request, "\"q") == 0) {
		reply = (fStyle.flags & CELL_PROTECTED) != 0 ? "1\"q" : "0\"q";
	} else if (strcmp(request, "\"p") == 0) {
		reply = "62;1\"p";
	} else if (strcmp(request, "*x") == 0) {
		reply = fRectangularAttributes ? "2*x" : "0*x";
	} else if (strcmp(request, "t") == 0) {
		char buffer[32];
		snprintf(buffer, sizeof(buffer), "%dt", fRows);
		reply = buffer;
	} else
		valid = false;

	if (valid)
		_Reply("\x1bP1$r%s\x1b\\", reply.c_str());
	else
		_Reply("\x1bP0$r\x1b\\");
}


static void
hex_decode(const std::string& hex, std::string& text)
{
	text.clear();
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		char digits[3] = { hex[i], hex[i + 1], 0 };
		text += (char)strtol(digits, NULL, 16);
	}
}


static void
hex_encode(const std::string& text, std::string& hex)
{
	static const char kDigits[] = "0123456789ABCDEF";
	hex.clear();
	for (size_t i = 0; i < text.size(); i++) {
		hex += kDigits[((uint8)text[i]) >> 4];
		hex += kDigits[((uint8)text[i]) & 0xf];
	}
}


/*!	XTGETTCAP: answers termcap/terminfo capability queries, which programs
	(kitty's kittens, tmux, neovim) use to learn what the terminal does.
*/
void
Terminal::_RequestTermcap(const char* request)
{
	struct Capability {
		const char*	name;
		const char*	value;
	};
	static const Capability kCapabilities[] = {
		{ "TN", "xterm-256color" },
		{ "name", "xterm-256color" },
		{ "Co", "256" },
		{ "colors", "256" },
		{ "RGB", "8/8/8" },
		{ "Tc", "" },
		{ "Su", "" },
		{ "Smulx", "\x1b[4:%p1%dm" },
		{ "Setulc", "\x1b[58:2:%p1%{65536}%/%d:%p1%{256}%/%{255}%&%d"
			":%p1%{255}%&%dm" },
		{ "Ss", "\x1b[%p1%d q" },
		{ "Se", "\x1b[2 q" },
		{ "Ms", "\x1b]52;%p1%s;%p2%s\x07" },
		{ "Sync", "\x1b[?2026%?%p1%{1}%-%tl%eh%;" },
		{ "BD", "\x1b[?2004l" },
		{ "BE", "\x1b[?2004h" },
		{ "PS", "\x1b[200~" },
		{ "PE", "\x1b[201~" },
		{ "fe", "\x1b[?1004h" },
		{ "fd", "\x1b[?1004l" },
		{ "kxIN", "\x1b[I" },
		{ "kxOUT", "\x1b[O" },
		{ "XM", "\x1b[?1006;1000%?%p1%{1}%=%th%el%;" },
		{ "xm", "\x1b[<%i%p3%d;%p1%d;%p2%d;%?%p4%tM%em%;" },
		{ "Smol", "\x1b[53m" },
		{ "Rmol", "\x1b[55m" },
		{ "Rect", "" },
		{ "kbs", "\x7f" },
		{ "kdch1", "\x1b[3~" },
		{ "khome", "\x1bOH" },
		{ "kend", "\x1bOF" },
		{ "kpp", "\x1b[5~" },
		{ "knp", "\x1b[6~" },
		{ "kcuu1", "\x1bOA" },
		{ "kcud1", "\x1bOB" },
		{ "kcuf1", "\x1bOC" },
		{ "kcub1", "\x1bOD" },
		{ "kf1", "\x1bOP" },
		{ "kf2", "\x1bOQ" },
		{ "kf3", "\x1bOR" },
		{ "kf4", "\x1bOS" },
		{ "kf5", "\x1b[15~" },
		{ "kf6", "\x1b[17~" },
		{ "kf7", "\x1b[18~" },
		{ "kf8", "\x1b[19~" },
		{ "kf9", "\x1b[20~" },
		{ "kf10", "\x1b[21~" },
		{ "kf11", "\x1b[23~" },
		{ "kf12", "\x1b[24~" },
		{ NULL, NULL }
	};

	std::string names(request);
	size_t start = 0;
	while (start <= names.size()) {
		size_t end = names.find(';', start);
		if (end == std::string::npos)
			end = names.size();
		std::string hexName = names.substr(start, end - start);
		start = end + 1;
		if (hexName.empty())
			continue;

		std::string name;
		hex_decode(hexName, name);

		const Capability* found = NULL;
		for (const Capability* capability = kCapabilities;
				capability->name != NULL; capability++) {
			if (name == capability->name) {
				found = capability;
				break;
			}
		}

		if (found == NULL) {
			_Reply("\x1bP0+r%s\x1b\\", hexName.c_str());
			continue;
		}

		std::string hexValue;
		hex_encode(found->value, hexValue);
		if (hexValue.empty())
			_Reply("\x1bP1+r%s\x1b\\", hexName.c_str());
		else {
			fOutput += "\x1bP1+r";
			fOutput += hexName;
			fOutput += "=";
			fOutput += hexValue;
			fOutput += "\x1b\\";
		}
	}
}


// #pragma mark - application program commands


void
Terminal::ApcDispatch(const char* data, size_t length)
{
	if (length > 0 && data[0] == 'G' && fGraphics != NULL)
		fGraphics->HandleCommand(data + 1, length - 1);
}
