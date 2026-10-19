/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


//!	DEC style escape sequence parser (after Paul Williams' state diagram).


#include "VTParser.h"

#include <stdlib.h>
#include <string.h>

#include <new>


static const int32 kPrintBufferSize = 4096;

// The longest strings acted on. A hyperlink (OSC 8) carries a whole address
// and a clipboard command (OSC 52) a whole text; kitty's graphics commands
// are sent in chunks of 4 KiB, but a program may send an image in one piece.
static const size_t kMaxOSCLength = 1024 * 1024;
static const size_t kMaxDCSLength = 1024 * 1024;
static const size_t kMaxAPCLength = 64 * 1024 * 1024;

static const uint32 kReplacementCharacter = 0xfffd;


VTParser::Handler::~Handler()
{
}


VTParser::VTParser(Handler* handler)
	:
	fHandler(handler),
	fState(STATE_GROUND),
	fStringState(STATE_GROUND),
	fCurrentParameter(-1),
	fParameterStarted(false),
	fParameterOverflow(false),
	fIgnoreSequence(false),
	fUTF8CodePoint(0),
	fUTF8Remaining(0),
	fUTF8Minimum(0),
	fPrintBuffer(new uint32[kPrintBufferSize]),
	fPrintCount(0),
	fString(NULL),
	fStringLength(0),
	fStringCapacity(0),
	fStringLimit(0),
	fStringOverflow(false),
	fStringEndedWithBell(false)
{
	_Clear();
}


VTParser::~VTParser()
{
	delete[] fPrintBuffer;
	free(fString);
}


void
VTParser::Reset()
{
	fState = STATE_GROUND;
	fUTF8Remaining = 0;
	fPrintCount = 0;
	fStringLength = 0;
	_Clear();
}


void
VTParser::Feed(const uint8* data, size_t length)
{
	const uint8* end = data + length;
	while (data < end) {
		if (fState == STATE_GROUND) {
			data = _Ground(data, end);
			continue;
		}

		uint8 c = *data++;

		// CAN and SUB cancel any sequence; ESC starts a new one, except
		// in strings, where it may be the start of the terminator.
		if (c == 0x18 || c == 0x1a) {
			fState = STATE_GROUND;
			fStringLength = 0;
			_Execute(c);
			continue;
		}

		switch (fState) {
			case STATE_GROUND:
				break;

			case STATE_ESCAPE:
				if (c == 0x1b) {
					_Clear();
				} else if (c < 0x20) {
					_Execute(c);
				} else if (c < 0x30) {
					_Collect(c);
					fState = STATE_ESCAPE_INTERMEDIATE;
				} else if (c == '[') {
					_Clear();
					fState = STATE_CSI_ENTRY;
				} else if (c == ']') {
					_EnterString(STATE_OSC_STRING);
				} else if (c == 'P') {
					_Clear();
					fState = STATE_DCS_ENTRY;
				} else if (c == '_') {
					_EnterString(STATE_APC_STRING);
				} else if (c == '^' || c == 'X') {
					_EnterString(STATE_SOS_PM_STRING);
				} else if (c < 0x7f) {
					fSequence.final = c;
					fState = STATE_GROUND;
					fHandler->EscDispatch(fSequence);
				} else if (c >= 0x80) {
					// not a sequence after all
					fState = STATE_GROUND;
					data--;
				}
				break;

			case STATE_ESCAPE_INTERMEDIATE:
				if (c == 0x1b) {
					_Clear();
					fState = STATE_ESCAPE;
				} else if (c < 0x20) {
					_Execute(c);
				} else if (c < 0x30) {
					_Collect(c);
				} else if (c < 0x7f) {
					fSequence.final = c;
					fState = STATE_GROUND;
					fHandler->EscDispatch(fSequence);
				} else if (c >= 0x80) {
					fState = STATE_GROUND;
					data--;
				}
				break;

			case STATE_CSI_ENTRY:
			case STATE_CSI_PARAM:
			case STATE_CSI_INTERMEDIATE:
			case STATE_CSI_IGNORE:
			case STATE_DCS_ENTRY:
			case STATE_DCS_PARAM:
			case STATE_DCS_INTERMEDIATE:
			case STATE_DCS_IGNORE:
			{
				bool isDCS = fState >= STATE_DCS_ENTRY;
				State entry = isDCS ? STATE_DCS_ENTRY : STATE_CSI_ENTRY;
				State param = isDCS ? STATE_DCS_PARAM : STATE_CSI_PARAM;
				State intermediate = isDCS
					? STATE_DCS_INTERMEDIATE : STATE_CSI_INTERMEDIATE;
				State ignore = isDCS ? STATE_DCS_IGNORE : STATE_CSI_IGNORE;

				if (c == 0x1b) {
					_Clear();
					fState = STATE_ESCAPE;
					break;
				}
				if (c < 0x20) {
					// C0 controls are carried out within CSI, and are data
					// that is ignored in a DCS header.
					if (!isDCS)
						_Execute(c);
					break;
				}
				if (c == 0x7f)
					break;
				if (c >= 0x80) {
					// In UTF-8 a byte with the high bit set is not part of
					// any sequence: drop the sequence and print it.
					fState = STATE_GROUND;
					data--;
					break;
				}

				if (fState == ignore) {
					if (c >= 0x40) {
						if (isDCS) {
							// swallow the string as well
							_EnterString(STATE_SOS_PM_STRING);
						} else
							fState = STATE_GROUND;
					}
					break;
				}

				if (c < 0x30) {
					if (fState == entry || fState == param
						|| fState == intermediate) {
						_FinishParameter();
						_Collect(c);
						fState = intermediate;
					}
				} else if (c < 0x40) {
					if (fState == intermediate) {
						fState = ignore;
					} else if (c >= 0x3c) {
						// private marker, only allowed first
						if (fState == entry) {
							fSequence.marker = c;
							fState = param;
						} else
							fState = ignore;
					} else {
						_Param(c);
						fState = param;
					}
				} else {
					_FinishParameter();
					fSequence.final = c;
					if (isDCS) {
						_EnterString(STATE_DCS_PASSTHROUGH);
					} else {
						fState = STATE_GROUND;
						if (!fIgnoreSequence)
							fHandler->CsiDispatch(fSequence);
					}
				}
				break;
			}

			case STATE_DCS_PASSTHROUGH:
			case STATE_OSC_STRING:
			case STATE_APC_STRING:
			case STATE_SOS_PM_STRING:
				if (c == 0x1b) {
					fStringState = fState;
					fState = STATE_STRING_ESCAPE;
				} else if (c == 0x07 && fState != STATE_DCS_PASSTHROUGH) {
					// xterm accepts BEL as the terminator of an OSC
					fStringEndedWithBell = true;
					_DispatchString();
					fStringEndedWithBell = false;
					fState = STATE_GROUND;
				} else if (c < 0x20 && fState != STATE_DCS_PASSTHROUGH) {
					// other controls are ignored in strings
				} else if (c != 0x7f)
					_StringPut(c);
				break;

			case STATE_STRING_ESCAPE:
				if (c == '\\') {
					fState = fStringState;
					_DispatchString();
					fState = STATE_GROUND;
				} else {
					// An escape sequence that cuts an operating system
					// command short ends it; other strings are dropped.
					fState = fStringState;
					if (fStringState == STATE_OSC_STRING)
						_DispatchString();
					fStringLength = 0;
					_Clear();
					fState = STATE_ESCAPE;
					data--;
				}
				break;
		}
	}

	_FlushPrint();
}


/*!	Handles text in the ground state, until the first control or the end of
	the data, and returns where it stopped.
*/
const uint8*
VTParser::_Ground(const uint8* data, const uint8* end)
{
	while (data < end) {
		uint8 c = *data;

		if (fUTF8Remaining > 0) {
			if ((c & 0xc0) != 0x80) {
				// truncated sequence: replace it, then look at this byte again
				fUTF8Remaining = 0;
				_PrintCodePoint(kReplacementCharacter);
				continue;
			}
			data++;
			fUTF8CodePoint = (fUTF8CodePoint << 6) | (c & 0x3f);
			if (--fUTF8Remaining == 0) {
				uint32 codePoint = fUTF8CodePoint;
				if (codePoint < fUTF8Minimum || codePoint > 0x10ffff
					|| (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
					codePoint = kReplacementCharacter;
				}
				_PrintCodePoint(codePoint);
			}
			continue;
		}

		if (c >= 0x20 && c < 0x7f) {
			// fast path for runs of ASCII
			const uint8* runEnd = data + 1;
			while (runEnd < end && *runEnd >= 0x20 && *runEnd < 0x7f)
				runEnd++;
			while (data < runEnd) {
				int32 count = runEnd - data;
				int32 room = kPrintBufferSize - fPrintCount;
				if (count > room)
					count = room;
				uint32* buffer = fPrintBuffer + fPrintCount;
				for (int32 i = 0; i < count; i++)
					buffer[i] = data[i];
				fPrintCount += count;
				data += count;
				if (fPrintCount == kPrintBufferSize)
					_FlushPrint();
			}
			continue;
		}

		data++;

		if (c < 0x20) {
			_FlushPrint();
			if (c == 0x1b) {
				_Clear();
				fState = STATE_ESCAPE;
				return data;
			}
			fHandler->Execute(c);
			continue;
		}

		if (c == 0x7f)
			continue;

		// start of a UTF-8 sequence
		if ((c & 0xe0) == 0xc0) {
			fUTF8CodePoint = c & 0x1f;
			fUTF8Remaining = 1;
			fUTF8Minimum = 0x80;
		} else if ((c & 0xf0) == 0xe0) {
			fUTF8CodePoint = c & 0x0f;
			fUTF8Remaining = 2;
			fUTF8Minimum = 0x800;
		} else if ((c & 0xf8) == 0xf0) {
			fUTF8CodePoint = c & 0x07;
			fUTF8Remaining = 3;
			fUTF8Minimum = 0x10000;
		} else
			_PrintCodePoint(kReplacementCharacter);
	}

	return data;
}


void
VTParser::_PrintCodePoint(uint32 codePoint)
{
	fPrintBuffer[fPrintCount++] = codePoint;
	if (fPrintCount == kPrintBufferSize)
		_FlushPrint();
}


void
VTParser::_FlushPrint()
{
	if (fPrintCount > 0) {
		int32 count = fPrintCount;
		fPrintCount = 0;
		fHandler->Print(fPrintBuffer, count);
	}
}


void
VTParser::_Execute(uint8 c)
{
	_FlushPrint();
	fHandler->Execute(c);
}


void
VTParser::_Clear()
{
	fSequence.marker = 0;
	fSequence.intermediates[0] = '\0';
	fSequence.intermediateCount = 0;
	fSequence.final = 0;
	fSequence.subParameterMask = 0;
	fSequence.parameterCount = 0;
	fCurrentParameter = -1;
	fParameterStarted = false;
	fParameterOverflow = false;
	fIgnoreSequence = false;
}


void
VTParser::_Collect(uint8 c)
{
	if (fSequence.intermediateCount >= kVTMaxIntermediates) {
		fIgnoreSequence = true;
		return;
	}
	fSequence.intermediates[fSequence.intermediateCount++] = c;
	fSequence.intermediates[fSequence.intermediateCount] = '\0';
}


void
VTParser::_Param(uint8 c)
{
	if (c == ';' || c == ':') {
		int32 index = fSequence.parameterCount;
		if (index < kVTMaxParameters) {
			fSequence.parameters[index] = fCurrentParameter;
			fSequence.parameterCount++;
		} else
			fParameterOverflow = true;
		if (c == ':' && index + 1 < kVTMaxParameters)
			fSequence.subParameterMask |= 1u << (index + 1);
		fCurrentParameter = -1;
		fParameterStarted = true;
		return;
	}

	// a digit
	fParameterStarted = true;
	if (fCurrentParameter < 0)
		fCurrentParameter = 0;
	if (fCurrentParameter < 100000000)
		fCurrentParameter = fCurrentParameter * 10 + (c - '0');
}


void
VTParser::_FinishParameter()
{
	if (!fParameterStarted)
		return;
	if (fSequence.parameterCount < kVTMaxParameters) {
		fSequence.parameters[fSequence.parameterCount++] = fCurrentParameter;
	}
	fCurrentParameter = -1;
	fParameterStarted = false;
}


void
VTParser::_EnterString(State state)
{
	fState = state;
	fStringLength = 0;
	fStringOverflow = false;
	switch (state) {
		case STATE_OSC_STRING:
			fStringLimit = kMaxOSCLength;
			break;
		case STATE_DCS_PASSTHROUGH:
			fStringLimit = kMaxDCSLength;
			break;
		case STATE_APC_STRING:
			fStringLimit = kMaxAPCLength;
			break;
		default:
			fStringLimit = 0;
			break;
	}
}


void
VTParser::_StringPut(uint8 c)
{
	if (fStringLength >= fStringLimit) {
		fStringOverflow = true;
		return;
	}

	if (fStringLength == fStringCapacity) {
		size_t capacity = fStringCapacity == 0 ? 256 : fStringCapacity * 2;
		if (capacity > fStringLimit)
			capacity = fStringLimit;
		char* string = (char*)realloc(fString, capacity + 1);
		if (string == NULL) {
			fStringOverflow = true;
			return;
		}
		fString = string;
		fStringCapacity = capacity;
	}

	fString[fStringLength++] = c;
}


void
VTParser::_DispatchString()
{
	if (fStringOverflow || fString == NULL) {
		// too long to be acted on (or nothing at all)
		if (!fStringOverflow && fState == STATE_OSC_STRING)
			fHandler->OscDispatch("", 0);
		else if (!fStringOverflow && fState == STATE_DCS_PASSTHROUGH)
			fHandler->DcsDispatch(fSequence, "", 0);
		else if (!fStringOverflow && fState == STATE_APC_STRING)
			fHandler->ApcDispatch("", 0);
		fStringLength = 0;
		return;
	}

	fString[fStringLength] = '\0';
	switch (fState) {
		case STATE_OSC_STRING:
			fHandler->OscDispatch(fString, fStringLength);
			break;
		case STATE_DCS_PASSTHROUGH:
			fHandler->DcsDispatch(fSequence, fString, fStringLength);
			break;
		case STATE_APC_STRING:
			fHandler->ApcDispatch(fString, fStringLength);
			break;
		default:
			break;
	}
	fStringLength = 0;

	// don't keep the memory of a huge string around
	if (fStringCapacity > 64 * 1024) {
		free(fString);
		fString = NULL;
		fStringCapacity = 0;
	}
}
