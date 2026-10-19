/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "Base64.h"


static const char kAlphabet[]
	= "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";


static inline int
decode_character(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+' || c == '-')
		return 62;
	if (c == '/' || c == '_')
		return 63;
	return -1;
}


Base64Decoder::Base64Decoder()
{
	Reset();
}


void
Base64Decoder::Reset()
{
	fBits = 0;
	fBitCount = 0;
}


bool
Base64Decoder::Decode(const char* input, size_t length, std::string& output)
{
	for (size_t i = 0; i < length; i++) {
		char c = input[i];
		if (c == '=') {
			// padding ends the current quantum
			fBits = 0;
			fBitCount = 0;
			continue;
		}

		int value = decode_character(c);
		if (value < 0)
			return false;

		fBits = (fBits << 6) | value;
		fBitCount += 6;
		if (fBitCount >= 8) {
			fBitCount -= 8;
			output += (char)((fBits >> fBitCount) & 0xff);
		}
	}
	return true;
}


bool
base64_decode(const char* input, size_t length, std::string& output)
{
	Base64Decoder decoder;
	return decoder.Decode(input, length, output);
}


void
base64_encode(const void* data, size_t length, std::string& output,
	bool padding)
{
	const uint8* bytes = (const uint8*)data;
	output.reserve(output.size() + (length + 2) / 3 * 4);

	size_t i = 0;
	for (; i + 2 < length; i += 3) {
		uint32 value = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
		output += kAlphabet[(value >> 18) & 0x3f];
		output += kAlphabet[(value >> 12) & 0x3f];
		output += kAlphabet[(value >> 6) & 0x3f];
		output += kAlphabet[value & 0x3f];
	}

	if (i < length) {
		uint32 value = bytes[i] << 16;
		if (i + 1 < length)
			value |= bytes[i + 1] << 8;
		output += kAlphabet[(value >> 18) & 0x3f];
		output += kAlphabet[(value >> 12) & 0x3f];
		if (i + 1 < length)
			output += kAlphabet[(value >> 6) & 0x3f];
		else if (padding)
			output += '=';
		if (padding)
			output += '=';
	}
}
