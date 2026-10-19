/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef TERMINAL_BASE64_H
#define TERMINAL_BASE64_H


#include <string>

#include <SupportDefs.h>


/*!	A base64 decoder that can be fed in pieces: leftover characters are
	carried over, and padding may end a quantum anywhere (data that was cut
	into pieces before it was encoded).
*/
class Base64Decoder {
public:
								Base64Decoder();

			void				Reset();
			//! Decodes \a length characters, appending to \a output. Returns
			//! false on a character outside the alphabet.
			bool				Decode(const char* input, size_t length,
									std::string& output);
			//! Whether the data so far ended on a complete quantum.
			bool				IsComplete() const
									{ return fBitCount < 6; }

private:
			uint32				fBits;
			int32				fBitCount;
};


bool base64_decode(const char* input, size_t length, std::string& output);
void base64_encode(const void* data, size_t length, std::string& output,
	bool padding = true);


#endif	// TERMINAL_BASE64_H
