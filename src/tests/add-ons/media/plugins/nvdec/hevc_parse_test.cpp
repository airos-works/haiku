/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
// Syntax-only fixtures. Never submit these incomplete pictures to hardware.
#include "hevc_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <vector>

#define CHECK(value) do { if (!(value)) { \
	fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); abort(); } } while (0)

struct Bits {
	std::vector<uint8_t> data;
	unsigned count = 0, syntax = 0;
	int replace = -1;
	uint32_t replacement = 0;
	Bits& Put(uint32_t value, unsigned bits = 1) {
		for (unsigned i = bits; i; i--) {
			if (!(count & 7)) data.push_back(0);
			data.back() |= ((value >> (i - 1)) & 1) << (7 - (count++ & 7));
		}
		return *this;
	}
	Bits& UE(uint32_t value) {
		if (int(syntax++) == replace) value = replacement;
		uint64_t code = uint64_t(value) + 1;
		unsigned bits = 0;
		for (uint64_t n = code; n > 1; n >>= 1) bits++;
		Put(0, bits).Put(1);
		if (bits) Put(uint32_t(code), bits);
		return *this;
	}
};

static Bits Sps(int replace = -1, uint32_t value = 0)
{
	Bits b; b.replace = replace; b.replacement = value;
	b.Put(66, 8).Put(1, 8).Put(0, 4).Put(0, 3).Put(1); // SPS, one temporal layer
	b.Put(0, 2).Put(0).Put(1, 5).Put(0, 32).Put(0, 4).Put(0, 32).Put(0, 12).Put(120, 8);
	b.UE(0).UE(1).UE(128).UE(96).Put(1).UE(0).UE(0).UE(0).UE(0); // crop window
	b.UE(0).UE(0).UE(4).Put(0).UE(4).UE(2).UE(0); // depth, POC, DPB
	b.UE(0).UE(3).UE(0).UE(3).UE(0).UE(0); // coding/transform blocks
	b.Put(0).Put(1).Put(1).Put(0).UE(0).Put(0).Put(1).Put(1).Put(0).Put(0).Put(1);
	return b;
}

static Bits Pps(int replace = -1, uint32_t value = 0)
{
	Bits b; b.replace = replace; b.replacement = value;
	b.Put(68, 8).Put(1, 8).UE(0).UE(0).Put(0).Put(0).Put(0, 3).Put(1).Put(0);
	b.UE(0).UE(0).UE(0).Put(0).Put(0).Put(0).UE(0).UE(0);
	b.Put(0).Put(0).Put(0).Put(0).Put(1).Put(0); // tiles enabled
	b.UE(1).UE(1).Put(0).UE(0).UE(0).Put(1).Put(1).Put(0).Put(0);
	b.Put(0).UE(0).Put(0).Put(0).Put(1);
	return b;
}

static Bits Slice(int replace = -1, uint32_t value = 0)
{
	Bits b; b.replace = replace; b.replacement = value;
	b.Put(2, 8).Put(1, 8).Put(1).UE(0).UE(1).Put(2, 8).Put(0);
	b.UE(1).UE(0).UE(0).Put(1).Put(1); // one used negative delta, -1
	return b;
}

int main()
{
	auto sets = new HevcParamSets();
	Bits s = Sps(), p = Pps(), v = Slice();
	CHECK(hevcParseSps(s.data.data(), s.data.size(), &sets->sps[0]));
	CHECK(hevcParsePps(p.data.data(), p.data.size(), sets, &sets->pps[0]));
	HevcSlice slice;
	CHECK(hevcParseSliceHeader(v.data.data(), v.data.size(), sets, &slice));
	CHECK(slice.shortTerm.numNegative == 1 && slice.shortTerm.deltaPoc[0][0] == -1);
	// A predicted inline RPS carries its source-set size to the UVD message.
	sets->sps[0].numShortTermSets = 1;
	auto& reference = sets->sps[0].shortTerm[0];
	reference.numNegative = reference.numPositive = 1;
	reference.deltaPoc[0][0] = -1; reference.deltaPoc[1][0] = 2;
	Bits predicted;
	predicted.Put(2, 8).Put(1, 8).Put(1).UE(0).UE(1).Put(2, 8).Put(0);
	predicted.Put(1).UE(0).Put(0).UE(0).Put(1).Put(1).Put(1).Put(1);
	CHECK(hevcParseSliceHeader(predicted.data.data(), predicted.data.size(), sets, &slice));
	CHECK(slice.shortTerm.numDeltaPocsOfRefRpsIdx == 2 && slice.shortTerm.numNegative == 0
		&& slice.shortTerm.numPositive == 2 && slice.shortTerm.deltaPoc[1][0] == 1
		&& slice.shortTerm.deltaPoc[1][1] == 3);
	sets->sps[0].numShortTermSets = 0;
	// Long-term counts, SPS indexes and cumulative MSB cycles are independently bounded.
	sets->sps[0].longTermRefsPresent = 1;
	sets->sps[0].numLongTermRefsSps = 3;
	for (unsigned index : {2u, 3u}) {
		Bits lt;
		lt.Put(2, 8).Put(1, 8).Put(1).UE(0).UE(1).Put(2, 8).Put(0).UE(0).UE(0);
		lt.UE(1).UE(0).Put(index, 2).Put(0).Put(1);
		CHECK(hevcParseSliceHeader(lt.data.data(), lt.data.size(), sets, &slice) == (index == 2));
	}
	for (uint32_t last : {0u, 1u}) {
		Bits lt;
		lt.Put(2, 8).Put(1, 8).Put(1).UE(0).UE(1).Put(2, 8).Put(0).UE(0).UE(0);
		lt.UE(0).UE(2).Put(1, 8).Put(1).Put(1).UE(0x7fffffffu);
		lt.Put(2, 8).Put(1).Put(1).UE(last).Put(1);
		CHECK(hevcParseSliceHeader(lt.data.data(), lt.data.size(), sets, &slice) == (last == 0));
	}
	sets->sps[0].longTermRefsPresent = 0;
	sets->sps[0].numLongTermRefsSps = 0;
	HevcSps parsedSps;
	HevcPps parsedPps;
	// Target each encoded unsigned field with values that overflow an int or
	// become invalid when added, multiplied, shifted or used as an array index.
	for (uint32_t value : {0x7fffffffu, 0x80000000u, 0xfffffffeu}) {
		for (unsigned i = 0; i < s.syntax; i++) {
			auto bad = Sps(i, value);
			bool accepted = hevcParseSps(bad.data.data(), bad.data.size(), &parsedSps);
			CHECK(accepted == (i == 13 && value == 0x7fffffffu)); // latency is stored without arithmetic
		}
		for (unsigned i = 0; i < p.syntax; i++) {
			auto bad = Pps(i, value);
			CHECK(!hevcParsePps(bad.data.data(), bad.data.size(), sets, &parsedPps));
		}
		for (unsigned i = 0; i < v.syntax; i++) {
			auto bad = Slice(i, value);
			CHECK(!hevcParseSliceHeader(bad.data.data(), bad.data.size(), sets, &slice));
		}
	}
	for (size_t i = 0; i < s.data.size(); i++)
		CHECK(!hevcParseSps(s.data.data(), i, &parsedSps));
	for (size_t i = 0; i < p.data.size(); i++)
		CHECK(!hevcParsePps(p.data.data(), i, sets, &parsedPps));
	uint32_t random = 0x71324589;
	for (unsigned n = 0; n < 25000; n++) {
		auto next = [&]() { random ^= random << 13; random ^= random >> 17;
			random ^= random << 5; return random; };
		auto data = n % 3 == 0 ? s.data : n % 3 == 1 ? p.data : v.data;
		for (unsigned i = 0, count = next() % 5 + 1; i < count; i++)
			data[next() % data.size()] ^= uint8_t(next());
		hevcParseSps(data.data(), data.size(), &parsedSps);
		hevcParsePps(data.data(), data.size(), sets, &parsedPps);
		hevcParseSliceHeader(data.data(), data.size(), sets, &slice);
	}
	delete sets;
	puts("PASS: HEVC syntax bounds, predicted RPS size, long-term indexing/cycle bounds, truncation and 25000 mutations");
}
