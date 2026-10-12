/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
// Metadata-only syntax fixtures. These deliberately contain no entropy-coded
// picture and must never be submitted to the hardware.
#include "H264Stream.h"
#include <assert.h>
#include <stdio.h>

struct Bits {
	std::vector<uint8_t> bytes;
	unsigned count = 0;
	Bits& Put(uint32_t value, unsigned size = 1) {
		for (unsigned i = size; i > 0; i--) {
			if ((count & 7) == 0) bytes.push_back(0);
			bytes.back() |= ((value >> (i - 1)) & 1) << (7 - (count++ & 7));
		}
		return *this;
	}
	Bits& UE(unsigned value) {
		unsigned size = 0; for (unsigned n = value + 1; n > 1; n >>= 1) size++;
		return Put(0, size).Put(value + 1, size + 1);
	}
};
static void Nal(std::vector<uint8_t>& unit, uint8_t type, Bits bits)
{
	bits.Put(1); // rbsp stop bit
	unit.insert(unit.end(), {0,0,1,type});
	unsigned zeros = 0;
	for (uint8_t byte : bits.bytes) {
		if (zeros == 2 && byte <= 3) { unit.push_back(3); zeros = 0; }
		unit.push_back(byte); zeros = byte == 0 ? zeros + 1 : 0;
	}
}
static void Picture(H264Stream& stream, bool idr, int frame, int poc,
	bool mmco5 = false, bool discard = false, bool sets = false)
{
	std::vector<uint8_t> unit;
	if (sets) {
		Bits s;
		s.Put(66,8).Put(0,8).Put(30,8).UE(0).UE(0).UE(0).UE(0).UE(2).Put(0);
		s.UE(1).UE(1).Put(1).Put(1).Put(0).Put(0);
		Nal(unit, 0x67, s);
		Bits p;
		p.UE(0).UE(0).Put(0).Put(0).UE(0).UE(0).UE(0).Put(0).Put(0,2);
		p.UE(0).UE(0).UE(0).Put(1).Put(0).Put(0);
		Nal(unit, 0x68, p);
	}
	Bits slice;
	slice.UE(0).UE(2).UE(0).Put(frame,4); // I slice, PPS 0
	if (idr) slice.UE(0);
	slice.Put(poc,4);
	if (idr) slice.Put(discard).Put(0);
	else { slice.Put(mmco5); if (mmco5) slice.UE(5).UE(0); }
	Nal(unit, idr ? 0x65 : 0x61, slice);
	assert(stream.Prepare(unit.data(), unit.size()));
}
int main()
{
	H264Stream stream;
	Picture(stream, true, 0, 0, false, false, true);
	assert(stream.sequence == 1 && stream.poc == 0 && !stream.discardPrior);
	assert(stream.Commit());
	Picture(stream, false, 1, 6);
	assert(stream.sequence == 1 && stream.poc == 6 && stream.Commit());
	Picture(stream, false, 2, 8, true);
	assert(stream.sequence == 2 && stream.poc == 0 && stream.picture.field_order_count[0] == 8);
	assert(stream.Commit());
	Picture(stream, false, 1, 2);
	assert(stream.sequence == 2 && stream.poc == 2
		&& stream.picture.reference_frame_num[0] == 0
		&& stream.picture.reference_field_order_count[0][0] == 0);
	assert(stream.Commit());
	Picture(stream, true, 0, 0, false, true);
	assert(stream.sequence == 3 && stream.discardPrior && stream.Commit());
	stream.Reset();
	Picture(stream, true, 0, 0);
	assert(stream.sequence == 4 && !stream.discardPrior && stream.Commit());
	puts("PASS: IDR/discard, MMCO5 output epoch and reference renumbering, retained parameter sets after reset (metadata only)");
}
