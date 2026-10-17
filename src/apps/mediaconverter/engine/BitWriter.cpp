/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "BitWriter.h"


BitWriter::BitWriter()
	:
	fBitCount(0)
{
}


void
BitWriter::Bits(uint32 value, int count)
{
	for (int i = count - 1; i >= 0; i--) {
		if ((fBitCount & 7) == 0)
			fData.push_back(0);
		if ((value >> i) & 1)
			fData.back() |= 0x80 >> (fBitCount & 7);
		fBitCount++;
	}
}


void
BitWriter::UnsignedExpGolomb(uint32 value)
{
	uint64 coded = (uint64)value + 1;
	int length = 0;
	while ((coded >> length) > 1)
		length++;
	Bits(0, length);
	// the leading one and the rest, at most 33 bits
	if (length >= 32) {
		Bits((uint32)(coded >> 32), length + 1 - 32);
		Bits((uint32)coded, 32);
	} else
		Bits((uint32)coded, length + 1);
}


void
BitWriter::SignedExpGolomb(int32 value)
{
	if (value > 0)
		UnsignedExpGolomb((uint32)value * 2 - 1);
	else
		UnsignedExpGolomb((uint32)(-(int64)value) * 2);
}


void
BitWriter::TrailingBits()
{
	Bit(true);
	ByteAlign();
}


void
BitWriter::ByteAlign(bool one)
{
	while ((fBitCount & 7) != 0)
		Bit(one);
}


void
BitWriter::Bytes(const uint8* data, size_t size)
{
	for (size_t i = 0; i < size; i++)
		Bits(data[i], 8);
}


size_t
append_annexb_nal(std::vector<uint8>& out, const uint8* header,
	size_t headerSize, const BitWriter& rbsp)
{
	size_t start = out.size();
	static const uint8 kStartCode[4] = { 0, 0, 0, 1 };
	out.insert(out.end(), kStartCode, kStartCode + 4);
	out.insert(out.end(), header, header + headerSize);

	const std::vector<uint8>& data = rbsp.Data();
	int zeros = 0;
	for (size_t i = 0; i < data.size(); i++) {
		if (zeros >= 2 && data[i] <= 3) {
			out.push_back(3);
			zeros = 0;
		}
		out.push_back(data[i]);
		zeros = data[i] == 0 ? zeros + 1 : 0;
	}

	size_t bits = (out.size() - start) * 8;
	size_t spare = (8 - (rbsp.BitCount() & 7)) & 7;
	return bits - spare;
}
