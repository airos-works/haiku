/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef VT_PARSER_H
#define VT_PARSER_H


#include <SupportDefs.h>


static const int32 kVTMaxParameters = 32;
static const int32 kVTMaxIntermediates = 4;


/*!	A control sequence as the parser collected it: the private marker
	(one of "<=>?" right after CSI or DCS, 0 if there is none), up to four
	intermediate bytes, the parameters and the final byte.

	Parameters separated by ':' are sub-parameters of the one before them
	(SGR 38:2::255:0:0, 4:3); IsSubParameter() tells them apart. A parameter
	that was left empty has the value -1.
*/
struct VTSequence {
			char				marker;
			char				intermediates[kVTMaxIntermediates + 1];
			int32				intermediateCount;
			uint8				final;

			int32				parameters[kVTMaxParameters];
			uint32				subParameterMask;
			int32				parameterCount;

	inline	int32				Parameter(int32 index, int32 defaultValue)
									const;
	inline	bool				IsSubParameter(int32 index) const;
	inline	int32				SubParameterCount(int32 index) const;
	inline	bool				HasIntermediate(char c) const;
	inline	bool				Is(char marker, const char* intermediates,
									uint8 final) const;
};


class VTParser {
public:
	class Handler {
	public:
		virtual					~Handler();

		//! Printable text, decoded to code points.
		virtual	void			Print(const uint32* codePoints,
									int32 count) = 0;
		//! A C0 control (or DEL) to carry out.
		virtual	void			Execute(uint8 control) = 0;
		virtual	void			EscDispatch(const VTSequence& sequence) = 0;
		virtual	void			CsiDispatch(const VTSequence& sequence) = 0;
		//! An operating system command without its introducer and
		//! terminator. Not NUL terminated.
		virtual	void			OscDispatch(const char* data,
									size_t length) = 0;
		//! A device control string: \a sequence holds what came before the
		//! data (DCS's own parameters, intermediates and final byte).
		virtual	void			DcsDispatch(const VTSequence& sequence,
									const char* data, size_t length) = 0;
		//! An application program command (kitty's graphics protocol).
		virtual	void			ApcDispatch(const char* data,
									size_t length) = 0;
	};

public:
								VTParser(Handler* handler);
								~VTParser();

			void				Reset();

			void				Feed(const uint8* data, size_t length);

			//! Whether the string being dispatched was ended by BEL rather
			//! than ST (answers use the same terminator).
			bool				StringEndedWithBell() const
									{ return fStringEndedWithBell; }

			//! Whether the parser is between sequences, in its ground state.
			bool				IsInGround() const
									{ return fState == STATE_GROUND
										&& fUTF8Remaining == 0; }

private:
			enum State {
				STATE_GROUND,
				STATE_ESCAPE,
				STATE_ESCAPE_INTERMEDIATE,
				STATE_CSI_ENTRY,
				STATE_CSI_PARAM,
				STATE_CSI_INTERMEDIATE,
				STATE_CSI_IGNORE,
				STATE_DCS_ENTRY,
				STATE_DCS_PARAM,
				STATE_DCS_INTERMEDIATE,
				STATE_DCS_PASSTHROUGH,
				STATE_DCS_IGNORE,
				STATE_OSC_STRING,
				STATE_APC_STRING,
				STATE_SOS_PM_STRING,
				STATE_STRING_ESCAPE
			};

			const uint8*		_Ground(const uint8* data, const uint8* end);
			void				_FlushPrint();
			void				_PrintCodePoint(uint32 codePoint);

			void				_Clear();
			void				_Collect(uint8 c);
			void				_Param(uint8 c);
			void				_FinishParameter();
			void				_EnterString(State state);
			void				_StringPut(uint8 c);
			void				_DispatchString();
			void				_Execute(uint8 c);

private:
			Handler*			fHandler;
			State				fState;
			State				fStringState;
				// the string state STATE_STRING_ESCAPE returns to

			VTSequence			fSequence;
			int32				fCurrentParameter;
			bool				fParameterStarted;
			bool				fParameterOverflow;
			bool				fIgnoreSequence;

			// UTF-8 decoder
			uint32				fUTF8CodePoint;
			int32				fUTF8Remaining;
			uint32				fUTF8Minimum;

			// printable run being collected
			uint32*				fPrintBuffer;
			int32				fPrintCount;

			// OSC, DCS and APC data
			char*				fString;
			size_t				fStringLength;
			size_t				fStringCapacity;
			size_t				fStringLimit;
			bool				fStringOverflow;
			bool				fStringEndedWithBell;
};


// #pragma mark - inline methods


int32
VTSequence::Parameter(int32 index, int32 defaultValue) const
{
	if (index < 0 || index >= parameterCount || parameters[index] < 0)
		return defaultValue;
	return parameters[index];
}


bool
VTSequence::IsSubParameter(int32 index) const
{
	return index > 0 && index < kVTMaxParameters
		&& (subParameterMask & (1u << index)) != 0;
}


//! Number of sub-parameters that follow parameter \a index.
int32
VTSequence::SubParameterCount(int32 index) const
{
	int32 count = 0;
	while (IsSubParameter(index + 1 + count))
		count++;
	return count;
}


bool
VTSequence::HasIntermediate(char c) const
{
	for (int32 i = 0; i < intermediateCount; i++) {
		if (intermediates[i] == c)
			return true;
	}
	return false;
}


/*!	Whether the sequence has exactly this marker, these intermediates and
	this final byte. \a intermediates may be NULL for none.
*/
bool
VTSequence::Is(char marker, const char* intermediates, uint8 final) const
{
	if (this->final != final || this->marker != marker)
		return false;
	int32 count = 0;
	if (intermediates != NULL) {
		for (; intermediates[count] != '\0'; count++) {
			if (count >= intermediateCount
				|| this->intermediates[count] != intermediates[count]) {
				return false;
			}
		}
	}
	return count == intermediateCount;
}


#endif	// VT_PARSER_H
