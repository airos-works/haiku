/*
 * Copyright 2001-2026, Haiku, Inc.
 * Copyright 2003-2004 Kian Duffy, myob@users.sourceforge.net
 * Parts Copyright 1998-1999 Kazuho Okui and Takashi Murai.
 * All rights reserved. Distributed under the terms of the MIT license.
 *
 * Authors:
 *		Stefano Ceccherini, stefano.ceccherini@gmail.com
 *		Kian Duffy, myob@users.sourceforge.net
 *		Y.Hayakawa, hida@sawada.riec.tohoku.ac.jp
 *		Simon South, simon@simonsouth.net
 *		Ingo Weinhold, ingo_weinhold@gmx.de
 *		Clemens Zeidler, haiku@Clemens-Zeidler.de
 *		Siarzhuk Zharski, zharik@gmx.li
 */


#include "TermViewStates.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include <Catalog.h>
#include <Clipboard.h>
#include <Cursor.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <Path.h>
#include <PopUpMenu.h>
#include <ScrollBar.h>
#include <UTF8.h>
#include <Window.h>

#include <Array.h>

#include "ActiveProcessInfo.h"
#include "Shell.h"
#include "TermConst.h"
#include "TermDragAndDrop.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Terminal TermView"


// selection granularity
enum {
	SELECT_CHARS,
	SELECT_WORDS,
	SELECT_LINES
};

static const uint32 kAutoScroll = 'AScr';

static const uint32 kMessageOpenLink = 'OLnk';
static const uint32 kMessageCopyLink = 'CLnk';
static const uint32 kMessageCopyAbsolutePath = 'CAbs';
static const uint32 kMessageMenuClosed = 'MClo';


static const char* const kKnownURLProtocols = "http:https:ftp:mailto";

// how many lines a link is followed over, at most
static const int32 kMaxLinkLines = 64;


static int32
mouse_button(int32 buttons)
{
	if ((buttons & B_PRIMARY_MOUSE_BUTTON) != 0)
		return MOUSE_BUTTON_LEFT;
	if ((buttons & B_TERTIARY_MOUSE_BUTTON) != 0)
		return MOUSE_BUTTON_MIDDLE;
	if ((buttons & B_SECONDARY_MOUSE_BUTTON) != 0)
		return MOUSE_BUTTON_RIGHT;
	if ((buttons & B_MOUSE_BUTTON(4)) != 0)
		return MOUSE_BUTTON_BACK;
	if ((buttons & B_MOUSE_BUTTON(5)) != 0)
		return MOUSE_BUTTON_FORWARD;
	return MOUSE_BUTTON_NONE;
}


// #pragma mark - State


TermView::State::State(TermView* view)
	:
	fView(view)
{
}


TermView::State::~State()
{
}


void
TermView::State::Entered()
{
}


void
TermView::State::Exited()
{
}


bool
TermView::State::MessageReceived(BMessage* message)
{
	return false;
}


void
TermView::State::ModifiersChanged(int32 oldModifiers, int32 modifiers)
{
}


void
TermView::State::KeyDown(const char* bytes, int32 numBytes)
{
}


void
TermView::State::MouseDown(BPoint where, int32 buttons, int32 modifiers)
{
}


void
TermView::State::MouseMoved(BPoint where, uint32 transit,
	const BMessage* message, int32 modifiers)
{
}


void
TermView::State::MouseUp(BPoint where, int32 buttons)
{
}


void
TermView::State::WindowActivated(bool active)
{
}


// #pragma mark - StandardBaseState


TermView::StandardBaseState::StandardBaseState(TermView* view)
	:
	State(view)
{
}


bool
TermView::StandardBaseState::_StandardMouseMoved(BPoint where, int32 modifiers)
{
	Terminal* terminal = fView->fTerminal;
	terminal->Lock();
	int32 tracking = terminal->MouseTracking();
	bool pixels = terminal->MouseEncoding() == MOUSE_ENCODING_SGR_PIXELS;
	terminal->Unlock();

	if (tracking != MOUSE_TRACKING_BUTTON && tracking != MOUSE_TRACKING_ANY)
		return false;
	if ((modifiers & B_SHIFT_KEY) != 0 && fView->fMouseButtons != 0)
		return false;

	// motion is reported when it reaches another cell (or pixel)
	TermPoint position = fView->_ConvertToTerminal(where);
	if (pixels || position != fView->fPrevPos) {
		fView->_SendMouseEvent(MOUSE_MOTION,
			mouse_button(fView->fMouseButtons), where, modifiers);
	}
	fView->fPrevPos = position;
	return true;
}


// #pragma mark - DefaultState


TermView::DefaultState::DefaultState(TermView* view)
	:
	StandardBaseState(view)
{
}


void
TermView::DefaultState::ModifiersChanged(int32 oldModifiers, int32 modifiers)
{
	_CheckEnterHyperLinkState(modifiers);
}


void
TermView::DefaultState::KeyDown(const char* bytes, int32 numBytes)
{
	BMessage* currentMessage = fView->Looper()->CurrentMessage();
	if (currentMessage == NULL)
		return;

	int32 key = 0;
	int32 mod = 0;
	int32 rawChar = 0;
	currentMessage->FindInt32("modifiers", &mod);
	currentMessage->FindInt32("key", &key);
	currentMessage->FindInt32("raw_char", &rawChar);

	fView->_ActivateCursor(true);

	// Terminal's own keys: scrolling the history, and kitty's jumps from
	// prompt to prompt
	Terminal* terminal = fView->fTerminal;
	terminal->Lock();
	bool alternate = terminal->IsAlternateScreen();
	terminal->Unlock();
	if (numBytes == 1) {
		bool controlShift = (mod & B_CONTROL_KEY) != 0
			&& (mod & B_SHIFT_KEY) != 0;
		switch (bytes[0]) {
			case B_UP_ARROW:
				if (controlShift) {
					fView->_ScrollToRows(fView->fScrollRows + 1);
					return;
				}
				break;
			case B_DOWN_ARROW:
				if (controlShift) {
					fView->_ScrollToRows(fView->fScrollRows - 1);
					return;
				}
				break;
			case B_PAGE_UP:
				if ((mod & B_SHIFT_KEY) != 0 && !alternate) {
					fView->_ScrollToRows(fView->fScrollRows + fView->fRows);
					return;
				}
				break;
			case B_PAGE_DOWN:
				if ((mod & B_SHIFT_KEY) != 0 && !alternate) {
					fView->_ScrollToRows(fView->fScrollRows - fView->fRows);
					return;
				}
				break;
			case B_HOME:
				if ((mod & B_SHIFT_KEY) != 0 && !alternate) {
					fView->_ScrollToRows(INT32_MAX);
					return;
				}
				break;
			case B_END:
				if ((mod & B_SHIFT_KEY) != 0 && !alternate) {
					fView->_ScrollToBottom();
					return;
				}
				break;
		}
		if (controlShift && !alternate && (rawChar == 'z' || rawChar == 'x')) {
			fView->_ScrollToPrompt(rawChar == 'x');
			return;
		}
	}

	TermKeyEvent event;
	BString text;
	if (!fView->_TranslateKey(currentMessage, false, event, text)) {
		// nothing the encoder knows: send what the keymap gives
		fView->_ScrollToBottom();
		fView->_WritePTY(bytes, numBytes);
		return;
	}
	event.text = text.String();

	fView->_ScrollToBottom();
	fView->_SendKey(event);
}


void
TermView::DefaultState::MouseDown(BPoint where, int32 buttons, int32 modifiers)
{
	// A program offering drags (kitty's drag and drop) gets the drag
	// gesture, rather than a selection starting.
	TermDragAndDrop* dragAndDrop = fView->fDragAndDrop;
	bool gesture = dragAndDrop != NULL && dragAndDrop->IsOfferingDrags()
		&& buttons == B_PRIMARY_MOUSE_BUTTON && (modifiers & B_SHIFT_KEY) == 0;
	if (gesture) {
		dragAndDrop->MouseDown(where, (int32)(where.x / fView->fFontWidth),
			(int32)(where.y / fView->fFontHeight),
			(int32)(where.x * fView->fScale), (int32)(where.y * fView->fScale));
		fView->fDragGesture = true;
		fView->SetMouseEventMask(B_POINTER_EVENTS, B_NO_POINTER_HISTORY);
	}

	// with mouse reporting the program gets the clicks, unless shift is
	// held to select text
	if (fView->_ReportsMouse() && (modifiers & B_SHIFT_KEY) == 0) {
		fView->_SendMouseEvent(MOUSE_PRESS, mouse_button(buttons), where,
			modifiers);
		fView->fPrevPos = fView->_ConvertToTerminal(where);
		return;
	}

	// paste button
	if ((buttons & (B_SECONDARY_MOUSE_BUTTON | B_TERTIARY_MOUSE_BUTTON)) != 0) {
		fView->Paste(fView->fMouseClipboard);
		return;
	}

	// select region
	if (buttons == B_PRIMARY_MOUSE_BUTTON && !gesture) {
		fView->fSelectState->Prepare(where, modifiers);
		fView->_NextState(fView->fSelectState);
	}
}


void
TermView::DefaultState::MouseMoved(BPoint where, uint32 transit,
	const BMessage* dragMessage, int32 modifiers)
{
	TermDragAndDrop* dragAndDrop = fView->fDragAndDrop;
	if (dragMessage != NULL && dragAndDrop != NULL
		&& dragAndDrop->IsAcceptingDrops()) {
		// a drag over a program that takes drops
		if (transit == B_EXITED_VIEW || transit == B_OUTSIDE_VIEW)
			dragAndDrop->DragExited();
		else {
			dragAndDrop->DragMoved((int32)(where.x / fView->fFontWidth),
				(int32)(where.y / fView->fFontHeight),
				(int32)(where.x * fView->fScale),
				(int32)(where.y * fView->fScale), dragMessage);
		}
		return;
	}
	if (fView->fDragGesture && dragAndDrop != NULL)
		dragAndDrop->MouseMoved(where);

	if (_CheckEnterHyperLinkState(modifiers))
		return;

	if (transit == B_EXITED_VIEW) {
		// SGR pixel mouse reporting tells the program
		fView->_SendMouseEvent(MOUSE_LEAVE, MOUSE_BUTTON_NONE, where,
			modifiers);
		return;
	}

	_StandardMouseMoved(where, modifiers);
}


void
TermView::DefaultState::MouseUp(BPoint where, int32 buttons)
{
	if (fView->fDragGesture) {
		fView->fDragGesture = false;
		if (fView->fDragAndDrop != NULL)
			fView->fDragAndDrop->MouseUp();
	}

	if (fView->_ReportsMouse()) {
		// the button that went up
		int32 released = fView->fMouseButtons & ~buttons;
		fView->_SendMouseEvent(MOUSE_RELEASE, mouse_button(released), where,
			modifiers());
	}
}


void
TermView::DefaultState::WindowActivated(bool active)
{
	if (active)
		_CheckEnterHyperLinkState(fView->fModifiers);
}


bool
TermView::DefaultState::_CheckEnterHyperLinkState(int32 modifiers)
{
	if ((modifiers & B_COMMAND_KEY) != 0 && fView->Window()->IsActive()) {
		fView->_NextState(fView->fHyperLinkState);
		return true;
	}

	return false;
}


// #pragma mark - SelectState


TermView::SelectState::SelectState(TermView* view)
	:
	StandardBaseState(view),
	fSelectGranularity(SELECT_CHARS),
	fCheckMouseTracking(false),
	fMouseTracking(false)
{
}


void
TermView::SelectState::Prepare(BPoint where, int32 modifiers)
{
	int32 clicks;
	fView->Window()->CurrentMessage()->FindInt32("clicks", &clicks);

	if (fView->_HasSelection()) {
		TermPoint inPos = fView->_ConvertToTerminal(where);
		if (inPos >= fView->fSelection.start
			&& inPos < fView->fSelection.end) {
			if (modifiers & B_CONTROL_KEY) {
				BPoint p;
				uint32 bt;
				do {
					fView->GetMouse(&p, &bt);

					if (bt == 0) {
						fView->_Deselect();
						return;
					}

					snooze(40000);

				} while (abs((int)(where.x - p.x)) < 4
					&& abs((int)(where.y - p.y)) < 4);

				fView->InitiateDrag();
				return;
			}
		}
	}

	// If mouse has moved too much, disable double/triple click.
	if (fView->_MouseDistanceSinceLastClick(where) > 8)
		clicks = 1;

	fView->SetMouseEventMask(B_POINTER_EVENTS | B_KEYBOARD_EVENTS,
		B_NO_POINTER_HISTORY | B_LOCK_WINDOW_FOCUS);

	TermPoint clickPos = fView->_ConvertToTerminal(where);

	if (modifiers & B_SHIFT_KEY) {
		fView->fInitialSelectionStart = clickPos;
		fView->fInitialSelectionEnd = clickPos;
		fView->_ExtendSelection(fView->fInitialSelectionStart, true, false);
	} else {
		fView->_Deselect();
		fView->fInitialSelectionStart = clickPos;
		fView->fInitialSelectionEnd = clickPos;
	}

	// If clicks larger than 3, reset mouse click counter.
	clicks = (clicks - 1) % 3 + 1;

	switch (clicks) {
		case 1:
			fCheckMouseTracking = true;
			fSelectGranularity = SELECT_CHARS;
			break;

		case 2:
			fView->_SelectWord(where, (modifiers & B_SHIFT_KEY) != 0, false);
			fMouseTracking = true;
			fSelectGranularity = SELECT_WORDS;
			break;

		case 3:
			fView->_SelectLine(where, (modifiers & B_SHIFT_KEY) != 0, false);
			fMouseTracking = true;
			fSelectGranularity = SELECT_LINES;
			break;
	}
}


bool
TermView::SelectState::MessageReceived(BMessage* message)
{
	if (message->what == kAutoScroll) {
		_AutoScrollUpdate();
		return true;
	}

	return false;
}


void
TermView::SelectState::MouseMoved(BPoint where, uint32 transit,
	const BMessage* message, int32 modifiers)
{
	if (fCheckMouseTracking) {
		if (fView->_MouseDistanceSinceLastClick(where) > 9)
			fMouseTracking = true;
	}
	if (!fMouseTracking)
		return;

	bool doAutoScroll = false;

	if (where.y < 0) {
		doAutoScroll = true;
		fView->fAutoScrollSpeed = where.y;
		where.x = 0;
		where.y = 0;
	}

	BRect bounds(fView->Bounds());
	if (where.y > bounds.bottom) {
		doAutoScroll = true;
		fView->fAutoScrollSpeed = where.y - bounds.bottom;
		where.x = bounds.right;
		where.y = bounds.bottom;
	}

	if (doAutoScroll) {
		if (fView->fAutoScrollRunner == NULL) {
			BMessage message(kAutoScroll);
			fView->fAutoScrollRunner = new (std::nothrow) BMessageRunner(
				BMessenger(fView), &message, 10000);
		}
	} else {
		delete fView->fAutoScrollRunner;
		fView->fAutoScrollRunner = NULL;
	}

	switch (fSelectGranularity) {
		case SELECT_CHARS:
		{
			// If we just start selecting, we first select the initially
			// hit char, so that we get a proper initial selection -- the char
			// in question, which will thus always be selected, regardless of
			// whether selecting forward or backward.
			if (fView->fInitialSelectionStart == fView->fInitialSelectionEnd) {
				fView->_Select(fView->fInitialSelectionStart,
					fView->fInitialSelectionEnd, true, true);
			}

			fView->_ExtendSelection(fView->_ConvertToTerminal(where), true,
				true);
			break;
		}
		case SELECT_WORDS:
			fView->_SelectWord(where, true, true);
			break;
		case SELECT_LINES:
			fView->_SelectLine(where, true, true);
			break;
	}
}


void
TermView::SelectState::MouseUp(BPoint where, int32 buttons)
{
	fCheckMouseTracking = false;
	fMouseTracking = false;

	if (fView->fAutoScrollRunner != NULL) {
		delete fView->fAutoScrollRunner;
		fView->fAutoScrollRunner = NULL;
	}

	// When releasing the first mouse button, we copy the selected text to the
	// clipboard.
	if ((buttons & B_PRIMARY_MOUSE_BUTTON) == 0
		&& (fView->fMouseButtons & B_PRIMARY_MOUSE_BUTTON) != 0) {
		fView->Copy(fView->fMouseClipboard);
	}

	fView->_NextState(fView->fDefaultState);
}


void
TermView::SelectState::_AutoScrollUpdate()
{
	if (fMouseTracking && fView->fAutoScrollRunner != NULL) {
		int32 rows = (int32)(fView->fAutoScrollSpeed / fView->fFontHeight);
		if (rows == 0)
			rows = fView->fAutoScrollSpeed < 0 ? -1 : 1;
		fView->_ScrollToRows(fView->fScrollRows - rows);
		if (fView->fAutoScrollSpeed < 0) {
			fView->_ExtendSelection(
				fView->_ConvertToTerminal(BPoint(0, 0)), true, true);
		} else {
			fView->_ExtendSelection(
				fView->_ConvertToTerminal(fView->Bounds().RightBottom()), true,
				true);
		}
	}
}


// #pragma mark - HyperLinkState


TermView::HyperLinkState::HyperLinkState(TermView* view)
	:
	State(view),
	fURLCharClassifier(kURLAdditionalWordCharacters),
	fPathComponentCharClassifier(
		BString(kDefaultAdditionalWordCharacters).RemoveFirst("/")),
	fCurrentDirectory(),
	fHighlightActive(false)
{
}


void
TermView::HyperLinkState::Entered()
{
	ActiveProcessInfo activeProcessInfo;
	if (fView->GetActiveProcessInfo(activeProcessInfo))
		fCurrentDirectory = activeProcessInfo.CurrentDirectory();
	else
		fCurrentDirectory.Truncate(0);

	_UpdateHighlight();
}


void
TermView::HyperLinkState::Exited()
{
	_DeactivateHighlight();
}


void
TermView::HyperLinkState::ModifiersChanged(int32 oldModifiers, int32 modifiers)
{
	if ((modifiers & B_COMMAND_KEY) == 0)
		fView->_NextState(fView->fDefaultState);
	else
		_UpdateHighlight();
}


void
TermView::HyperLinkState::MouseDown(BPoint where, int32 buttons,
	int32 modifiers)
{
	TermPoint start;
	TermPoint end;
	HyperLink link;

	bool pathPrefixOnly = (modifiers & B_SHIFT_KEY) != 0;
	if (!_GetHyperLinkAt(where, pathPrefixOnly, link, start, end))
		return;

	if ((buttons & B_PRIMARY_MOUSE_BUTTON) != 0) {
		link.Open();
	} else if ((buttons & B_SECONDARY_MOUSE_BUTTON) != 0) {
		fView->fHyperLinkMenuState->Prepare(where, link);
		fView->_NextState(fView->fHyperLinkMenuState);
	}
}


void
TermView::HyperLinkState::MouseMoved(BPoint where, uint32 transit,
	const BMessage* message, int32 modifiers)
{
	_UpdateHighlight(where, modifiers);
}


void
TermView::HyperLinkState::WindowActivated(bool active)
{
	if (!active)
		fView->_NextState(fView->fDefaultState);
}


bool
TermView::HyperLinkState::_GetHyperLinkAt(BPoint where, bool pathPrefixOnly,
	HyperLink& _link, TermPoint& _start, TermPoint& _end)
{
	Terminal* terminal = fView->fTerminal;
	TermPoint pos = fView->_ConvertToTerminal(where);

	// an OSC 8 link
	terminal->Lock();
	TerminalCell cell;
	bool haveCell = terminal->GetCell(pos.y, pos.x, cell);
	if (haveCell && cell.IsWideTail() && pos.x > 0)
		haveCell = terminal->GetCell(pos.y, pos.x - 1, cell);
	if (haveCell && cell.hyperlink != 0) {
		std::string uri;
		std::string identifier;
		bool found = terminal->GetHyperLink(cell.hyperlink, uri, identifier);
		terminal->Unlock();
		if (!found)
			return false;
		_link = HyperLink(BString(uri.c_str()), cell.hyperlink,
			BString(identifier.c_str()));
		_GetLinkRange(pos, cell.hyperlink, _start, _end);
		return true;
	}
	terminal->Unlock();

	// try to get a URL first
	BString text;
	if (!fView->_FindWord(pos, &fURLCharClassifier, false, _start, _end))
		return false;

	_ExtendURLOverLineBreaks(_start, _end);

	text.Truncate(0);
	fView->_GetText(_start, _end, text);
	text.RemoveAll("\n");
	text.Trim();

	// We're only happy, if it has a protocol part which we know.
	int32 colonIndex = text.FindFirst(':');
	if (colonIndex >= 0) {
		BString protocol(text, colonIndex);
		if (strstr(kKnownURLProtocols, protocol) != NULL) {
			_link = HyperLink(text, HyperLink::TYPE_URL);
			return true;
		}
	}

	// no obvious URL -- try file name
	if (!fView->_FindWord(pos, fView->fCharClassifier, false, _start, _end))
		return false;

	// In path-prefix-only mode we determine the end position anew by omitting
	// the '/' in the allowed word chars.
	if (pathPrefixOnly) {
		TermPoint componentStart;
		TermPoint componentEnd;
		if (fView->_FindWord(pos, &fPathComponentCharClassifier, false,
				componentStart, componentEnd)) {
			_end = componentEnd;
		} else {
			// That means pos points to a '/'. We simply use the previous
			// position.
			_end = pos;
			if (_start == _end) {
				// Well, must be just "/". Advance to the next position.
				_end.x++;
			}
		}
	}

	text.Truncate(0);
	fView->_GetText(_start, _end, text);
	text.Trim();
	if (text.IsEmpty())
		return false;

	// Collect a list of colons in the string and their respective positions in
	// the text buffer. We do this up-front so we can unlock the text buffer
	// while we're doing all the entry existence tests.
	struct ColonPosition {
		int32		index;
		TermPoint	position;
	};
	std::vector<ColonPosition> colonPositions;
	TermPoint searchPos = _start;
	terminal->Lock();
	for (int32 index = 0; (index = text.FindFirst(':', index)) >= 0;) {
		TermPoint foundStart;
		TermPoint foundEnd;
		if (!terminal->Find(":", searchPos, true, true, false, foundStart,
				foundEnd)) {
			terminal->Unlock();
			return false;
		}

		ColonPosition colonPosition;
		colonPosition.index = index;
		colonPosition.position = foundStart;
		colonPositions.push_back(colonPosition);

		index++;
		searchPos = foundEnd;
	}
	terminal->Unlock();

	// Since we also want to consider ':' a potential path delimiter, in two
	// nested loops we chop off components from the beginning respective the
	// end.
	BString originalText = text;
	TermPoint originalStart = _start;
	TermPoint originalEnd = _end;

	int32 colonCount = colonPositions.size();
	for (int32 startColonIndex = -1; startColonIndex < colonCount;
			startColonIndex++) {
		int32 startIndex;
		if (startColonIndex < 0) {
			startIndex = 0;
			_start = originalStart;
		} else {
			startIndex = colonPositions[startColonIndex].index + 1;
			_start = colonPositions[startColonIndex].position;
			if (_start >= pos)
				break;
			_start.x++;
				// Note: This is potentially a non-normalized position (i.e.
				// the end of a soft-wrapped line). While not that nice, it
				// works anyway.
		}

		for (int32 endColonIndex = colonCount; endColonIndex > startColonIndex;
				endColonIndex--) {
			int32 endIndex;
			if (endColonIndex == colonCount) {
				endIndex = originalText.Length();
				_end = originalEnd;
			} else {
				endIndex = colonPositions[endColonIndex].index;
				_end = colonPositions[endColonIndex].position;
				if (_end <= pos)
					break;
			}

			originalText.CopyInto(text, startIndex, endIndex - startIndex);
			if (text.IsEmpty())
				continue;

			// check, whether the file exists
			BString actualPath;
			if (_EntryExists(text, actualPath)) {
				_link = HyperLink(text, actualPath, HyperLink::TYPE_PATH);
				return true;
			}

			// As such this isn't an existing path. We also want to recognize:
			// * "<path>:<line>"
			// * "<path>:<line>:<column>"

			BString path = text;

			for (int32 i = 0; i < 2; i++) {
				int32 colonIndex = path.FindLast(':');
				if (colonIndex <= 0 || colonIndex == path.Length() - 1)
					break;

				char* numberEnd;
				strtol(path.String() + colonIndex + 1, &numberEnd, 0);
				if (*numberEnd != '\0')
					break;

				path.Truncate(colonIndex);
				if (_EntryExists(path, actualPath)) {
					BString address = path == actualPath
						? text
						: BString(actualPath) << (text.String() + colonIndex);
					_link = HyperLink(text, address,
						i == 0
							? HyperLink::TYPE_PATH_WITH_LINE
							: HyperLink::TYPE_PATH_WITH_LINE_AND_COLUMN);
					return true;
				}
			}
		}
	}

	return false;
}


/*!	Returns the range of the cells around \a pos that carry the OSC 8
	hyperlink \a link. A program that wraps the text of a link itself repeats
	the link on each line, so the range continues over a line filled to the
	right margin when the next one starts with the same link.
*/
void
TermView::HyperLinkState::_GetLinkRange(const TermPoint& pos, uint32 link,
	TermPoint& _start, TermPoint& _end) const
{
	int32 width = fView->fColumns;
	int32 maxCells = width * kMaxLinkLines;

	TermPoint start = pos;
	for (int32 i = 0; i < maxCells; i++) {
		TermPoint previous = start;
		if (--previous.x < 0) {
			previous.x = width - 1;
			previous.y--;
		}
		if (!_CellHasLink(previous, link))
			break;
		start = previous;
	}

	TermPoint end = pos;
	for (int32 i = 0; i < maxCells; i++) {
		TermPoint next = end;
		if (++next.x >= width) {
			next.x = 0;
			next.y++;
		}
		if (!_CellHasLink(next, link))
			break;
		end = next;
	}

	_start = start;
	_end = TermPoint(end.x + 1, end.y);
}


bool
TermView::HyperLinkState::_CellHasLink(const TermPoint& pos, uint32 link) const
{
	Terminal* terminal = fView->fTerminal;
	terminal->Lock();
	TerminalCell cell;
	bool result = terminal->GetCell(pos.y, pos.x, cell)
		&& cell.hyperlink == link;
	terminal->Unlock();
	return result;
}


/*!	Programs that lay out their screen themselves, Claude Code's sign-in for
	one, break a long address into lines on their own, each filled to the
	right margin and ended with a hard line break, where the terminal would
	have wrapped it softly. So an address continues over a full line that
	it ends, if the next line starts with address characters, and the same
	backwards.
*/
void
TermView::HyperLinkState::_ExtendURLOverLineBreaks(TermPoint& _start,
	TermPoint& _end)
{
	Terminal* terminal = fView->fTerminal;
	int32 width = fView->fColumns;

	for (int32 i = 0; i < kMaxLinkLines && _start.x == 0; i++) {
		int32 row = _start.y - 1;
		terminal->Lock();
		bool full = row >= -terminal->HistorySize()
			&& terminal->LineLength(row) == width;
		terminal->Unlock();
		if (!full)
			break;

		TermPoint start;
		TermPoint end;
		if (!fView->_FindWord(TermPoint(width - 1, row), &fURLCharClassifier,
				false, start, end)
			|| end != TermPoint(width, row)) {
			break;
		}
		_start = start;
	}

	for (int32 i = 0; i < kMaxLinkLines && _end.x == width; i++) {
		int32 row = _end.y + 1;
		if (row >= fView->fRows)
			break;

		TermPoint start;
		TermPoint end;
		if (!fView->_FindWord(TermPoint(0, row), &fURLCharClassifier, false,
				start, end)
			|| start != TermPoint(0, row)) {
			break;
		}
		_end = end;
	}
}


bool
TermView::HyperLinkState::_EntryExists(const BString& path,
	BString& _actualPath) const
{
	if (path.IsEmpty())
		return false;

	if (path[0] == '/' || fCurrentDirectory.IsEmpty()) {
		_actualPath = path;
	} else if (path == "~" || path.StartsWith("~/")) {
		// Replace '~' with the user's home directory. We don't handle "~user"
		// here yet.
		BPath homeDirectory;
		if (find_directory(B_USER_DIRECTORY, &homeDirectory) != B_OK)
			return false;
		_actualPath = homeDirectory.Path();
		_actualPath << path.String() + 1;
	} else {
		_actualPath.Truncate(0);
		_actualPath << fCurrentDirectory << '/' << path;
	}

	struct stat st;
	return lstat(_actualPath, &st) == 0;
}


void
TermView::HyperLinkState::_UpdateHighlight()
{
	BPoint where;
	uint32 buttons;
	fView->GetMouse(&where, &buttons, false);
	_UpdateHighlight(where, fView->fModifiers);
}


void
TermView::HyperLinkState::_UpdateHighlight(BPoint where, int32 modifiers)
{
	TermPoint start;
	TermPoint end;
	HyperLink link;

	bool pathPrefixOnly = (modifiers & B_SHIFT_KEY) != 0;
	if (_GetHyperLinkAt(where, pathPrefixOnly, link, start, end))
		_ActivateHighlight(start, end);
	else
		_DeactivateHighlight();
}


void
TermView::HyperLinkState::_ActivateHighlight(const TermPoint& start,
	const TermPoint& end)
{
	fView->_SetLinkHighlight(start, end);
	if (!fHighlightActive) {
		BCursor cursor(B_CURSOR_ID_FOLLOW_LINK);
		fView->SetViewCursor(&cursor);
		fHighlightActive = true;
	}
}


void
TermView::HyperLinkState::_DeactivateHighlight()
{
	if (fHighlightActive) {
		fView->_ClearLinkHighlight();
		fHighlightActive = false;
		BCursor cursor(fView->_ReportsMouse() ? B_CURSOR_ID_SYSTEM_DEFAULT
			: B_CURSOR_ID_I_BEAM);
		fView->SetViewCursor(&cursor);
	}
}


// #pragma mark - HyperLinkMenuState


class TermView::HyperLinkMenuState::PopUpMenu : public BPopUpMenu {
public:
	PopUpMenu(const BMessenger& messageTarget)
		:
		BPopUpMenu("open hyperlink"),
		fMessageTarget(messageTarget)
	{
		SetAsyncAutoDestruct(true);
	}

	~PopUpMenu()
	{
		fMessageTarget.SendMessage(kMessageMenuClosed);
	}

private:
	BMessenger	fMessageTarget;
};


TermView::HyperLinkMenuState::HyperLinkMenuState(TermView* view)
	:
	State(view),
	fLink()
{
}


void
TermView::HyperLinkMenuState::Prepare(BPoint point, const HyperLink& link)
{
	fLink = link;

	// open context menu
	PopUpMenu* menu = new PopUpMenu(fView);
	BLayoutBuilder::Menu<> menuBuilder(menu);
	switch (link.GetType()) {
		case HyperLink::TYPE_URL:
		case HyperLink::TYPE_OSC_URL:
			menuBuilder
				.AddItem(B_TRANSLATE("Open link"), kMessageOpenLink)
				.AddItem(B_TRANSLATE("Copy link location"), kMessageCopyLink);
			break;

		case HyperLink::TYPE_PATH:
		case HyperLink::TYPE_PATH_WITH_LINE:
		case HyperLink::TYPE_PATH_WITH_LINE_AND_COLUMN:
			menuBuilder.AddItem(B_TRANSLATE("Open path"), kMessageOpenLink);
			menuBuilder.AddItem(B_TRANSLATE("Copy path"), kMessageCopyLink);
			if (fLink.Text() != fLink.Address()) {
				menuBuilder.AddItem(B_TRANSLATE("Copy absolute path"),
					kMessageCopyAbsolutePath);
			}
			break;
	}
	menu->SetTargetForItems(fView);
	menu->Go(fView->ConvertToScreen(point), true, true, true);
}


void
TermView::HyperLinkMenuState::Exited()
{
	fLink = HyperLink();
}


bool
TermView::HyperLinkMenuState::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMessageOpenLink:
			if (fLink.IsValid())
				fLink.Open();
			return true;

		case kMessageCopyLink:
		case kMessageCopyAbsolutePath:
		{
			if (fLink.IsValid()) {
				BString toCopy = fLink.Address();
				fView->_PutOnClipboard(be_clipboard, toCopy.String(),
					toCopy.Length());
			}
			return true;
		}

		case kMessageMenuClosed:
			fView->_NextState(fView->fDefaultState);
			return true;
	}

	return false;
}
