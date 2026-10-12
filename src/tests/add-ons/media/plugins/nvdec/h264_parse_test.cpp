/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "h264_parse.h"
#include "hevc_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static void Require(bool value, const char* message)
{
	if (!value) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

struct Bits {
	std::vector<uint8_t> bytes;
	unsigned count = 0;
	Bits& Put(uint32_t value, unsigned bits = 1)
	{
		for (unsigned i = bits; i > 0; i--) {
			if ((count & 7) == 0) bytes.push_back(0);
			bytes.back() |= ((value >> (i - 1)) & 1) << (7 - (count++ & 7));
		}
		return *this;
	}
	Bits& UE(uint32_t value)
	{
		uint64_t code = (uint64_t)value + 1;
		unsigned bits = 0;
		for (uint64_t n = code; n > 1; n >>= 1) bits++;
		Put(0, bits);
		Put(1);
		if (bits != 0) Put(value + 1, bits);
		return *this;
	}
	Bits& SE(int value) { return UE(value <= 0 ? -2 * value : 2 * value - 1); }
	Bits& Stop() { return Put(1); }
};

static Bits Sps(uint32_t id = 0)
{
	Bits b;
	b.Put(66, 8).Put(0, 8).Put(30, 8).UE(id);
	b.UE(0).UE(0).UE(0).UE(2).Put(0); // frame_num/POC/ref limits
	b.UE(39).UE(29).Put(1).Put(1).Put(0).Put(0).Stop();
	return b;
}

static Bits Pps()
{
	Bits b;
	b.UE(0).UE(0).Put(0).Put(0).UE(0).UE(0).UE(0);
	b.Put(0).Put(0, 2).SE(0).SE(0).SE(0).Put(1).Put(0).Put(0).Stop();
	return b;
}

static Bits SlicePrefix()
{
	Bits b;
	b.UE(0).UE(0).UE(0).Put(1, 4).Put(2, 4).Put(0);
	return b;
}

static void Regressions()
{
	H264Bits br;
	uint8_t zero[16] = {};
	h264BitsInit(&br, zero, sizeof(zero));
	Require(h264UE(&br) == 0 && br.failed, "32 leading zero bits rejected without invalid shift");
	h264BitsInit(&br, NULL, 0);
	Require(h264UE(&br) == 0 && br.failed, "empty Exp-Golomb rejected");
	h264BitsInit(&br, zero, 1);
	Require(h264Bits(&br, 9) == 0 && br.failed, "truncated fixed-width code rejected");
	h264BitsInit(&br, zero, sizeof(zero));
	Require(h264Bits(&br, 33) == 0 && br.failed, "oversized fixed-width code rejected");
	for (uint32_t value : {0u, 1u, 255u, 65535u, 0x7fffffffu, 0xfffffffeu}) {
		Bits bits; bits.UE(value);
		h264BitsInit(&br, bits.bytes.data(), bits.bytes.size());
		Require(h264UE(&br) == value && !br.failed, "full supported unsigned range");
	}
	Bits overflow; overflow.UE(0xffffffffu);
	h264BitsInit(&br, overflow.bytes.data(), overflow.bytes.size());
	Require(h264UE(&br) == 0 && br.failed, "unrepresentable Exp-Golomb rejected");

	H264ParamSets sets = {};
	H264Sps sps;
	H264Pps pps;
	H264Slice slice;
	Bits s = Sps(), p = Pps();
	Require(h264ParseSps(s.bytes.data(), s.bytes.size(), &sps), "baseline SPS");
	Require(sps.picWidthInMbs == 40 && sps.picHeightInMapUnits == 30, "SPS dimensions");
	Require(sps.matrixCoefficients == 2 && sps.fullRange == 0, "unspecified VUI colour defaults");
	Bits colour;
	colour.Put(66, 8).Put(0, 8).Put(30, 8).UE(0);
	colour.UE(0).UE(0).UE(0).UE(2).Put(0);
	colour.UE(39).UE(29).Put(1).Put(1).Put(0).Put(1); // VUI present
	colour.Put(0).Put(0).Put(1).Put(5, 3).Put(1).Put(1);
	colour.Put(1, 8).Put(14, 8).Put(6, 8); // primaries, transfer, matrix
	colour.Put(0).Put(0).Put(0).Put(0).Put(0).Put(0).Stop();
	H264Sps colourSps;
	Require(h264ParseSps(colour.bytes.data(), colour.bytes.size(), &colourSps)
		&& colourSps.fullRange == 1 && colourSps.matrixCoefficients == 6,
		"retain full range and matrix independently of primaries/transfer");
	sets.sps[0] = sps;
	Require(h264ParsePps(p.bytes.data(), p.bytes.size(), &sets, &pps), "baseline PPS");
	sets.pps[0] = pps;
	for (size_t n = 0; n < s.bytes.size(); n++)
		Require(!h264ParseSps(s.bytes.data(), n, &sps), "truncated SPS rejected");
	for (size_t n = 0; n < p.bytes.size(); n++)
		Require(!h264ParsePps(p.bytes.data(), n, &sets, &pps), "truncated PPS rejected");
	for (uint32_t id : {32u, 0x80000000u, 0xfffffffeu}) {
		Bits bad = Sps(id);
		Require(!h264ParseSps(bad.bytes.data(), bad.bytes.size(), &sps), "oversized SPS ID");
		Bits badPps; badPps.UE(id).UE(0);
		if (id >= 256)
			Require(!h264ParsePps(badPps.bytes.data(), badPps.bytes.size(), &sets, &pps),
				"oversized PPS ID");
	}
	for (unsigned count : {1u, 64u, 65u}) {
		Bits b = SlicePrefix(); b.Put(1);
		for (unsigned i = 0; i < count; i++) b.UE(0).UE(0);
		b.UE(3).Put(0).Stop();
		bool ok = h264ParseSliceHeader(b.bytes.data(), b.bytes.size(), &sets, 1, 2, &slice);
		Require(ok == (count <= H264_MAX_LIST_MODS), "reference-list bound");
		if (ok) Require(slice.listModCount[0] == (int)count, "complete reference list");
	}
	Bits unterminated = SlicePrefix(); unterminated.Put(1).UE(0).UE(0);
	Require(!h264ParseSliceHeader(unterminated.bytes.data(), unterminated.bytes.size(),
		&sets, 1, 2, &slice), "unterminated reference list returns failure");
	Bits invalid = SlicePrefix(); invalid.Put(1).UE(4).UE(0).Stop();
	Require(!h264ParseSliceHeader(invalid.bytes.data(), invalid.bytes.size(), &sets, 1, 2,
		&slice), "invalid reference modification rejected");
	for (unsigned count : {1u, 32u, 33u}) {
		Bits b = SlicePrefix(); b.Put(0).Put(1);
		for (unsigned i = 0; i < count; i++) b.UE(1).UE(0);
		b.UE(0).Stop();
		Require(h264ParseSliceHeader(b.bytes.data(), b.bytes.size(), &sets, 1, 2, &slice)
			== (count <= H264_MAX_MMCO), "MMCO bound");
	}
	// Reproducible malformed syntax coverage with valid parameter-set context.
	// Acceptance is allowed for mutations that still describe valid syntax.
	uint32_t random = 0x71324589;
	for (unsigned n = 0; n < 25000; n++) {
		auto next = [&]() { random ^= random << 13; random ^= random >> 17;
			random ^= random << 5; return random; };
		std::vector<uint8_t> data(next() % 256 + 1);
		for (auto& byte : data) byte = next();
		h264ParseSps(data.data(), data.size(), &sps);
		h264ParsePps(data.data(), data.size(), &sets, &pps);
		h264ParseSliceHeader(data.data(), data.size(), &sets, 1, 2, &slice);
	}
	puts("PASS: bit bounds, truncations, IDs, reference-list/MMCO limits and 25000 malformed inputs");
}

static void Stream(const char* path, bool hevc)
{
	FILE* file = fopen(path, "rb");
	Require(file != NULL && fseek(file, 0, SEEK_END) == 0, "open stream");
	long bytes = ftell(file); rewind(file);
	Require(bytes > 0 && bytes <= 128 * 1024 * 1024, "stream extent");
	std::vector<uint8_t> data(bytes), rbsp(bytes);
	Require(fread(data.data(), 1, bytes, file) == (size_t)bytes, "read stream");
	fclose(file);
	H264ParamSets h = {};
	HevcParamSets v = {};
	unsigned units = 0, slices = 0;
	for (size_t pos = 0; pos + 3 < data.size();) {
		if (data[pos] != 0 || data[pos + 1] != 0 || data[pos + 2] != 1) { pos++; continue; }
		size_t start = pos + 3, end = start;
		while (end + 3 <= data.size()
			&& !(data[end] == 0 && data[end + 1] == 0 && data[end + 2] == 1)) end++;
		if (end + 3 > data.size()) end = data.size();
		pos = end;
		while (end > start && data[end - 1] == 0) end--;
		Require(end > start, "nonempty NAL");
		if (hevc) {
			unsigned type = data[start] >> 1 & 63;
			size_t size = h264ToRbsp(&data[start], end - start, rbsp.data());
			if (type == HEVC_NAL_SPS) {
				HevcSps sps;
				Require(hevcParseSps(rbsp.data(), size, &sps), "HEVC SPS");
				v.sps[sps.id] = sps;
			} else if (type == HEVC_NAL_PPS) {
				HevcPps pps;
				Require(hevcParsePps(rbsp.data(), size, &v, &pps), "HEVC PPS");
				v.pps[pps.id] = pps;
			} else if (hevcIsVcl(type)) {
				HevcSlice slice;
				Require(hevcParseSliceHeader(rbsp.data(), size, &v, &slice), "HEVC slice header");
				slices++;
			}
		} else {
			unsigned type = data[start] & 31, ref = data[start] >> 5 & 3;
			size_t size = h264ToRbsp(&data[start + 1], end - start - 1, rbsp.data());
			if (type == 7) {
				H264Sps sps;
				Require(h264ParseSps(rbsp.data(), size, &sps), "H264 SPS");
				h.sps[sps.id] = sps;
			} else if (type == 8) {
				H264Pps pps;
				Require(h264ParsePps(rbsp.data(), size, &h, &pps), "H264 PPS");
				h.pps[pps.id] = pps;
			} else if (type == 1 || type == 5) {
				H264Slice slice;
				Require(h264ParseSliceHeader(rbsp.data(), size, &h, type, ref, &slice), "H264 slice header");
				slices++;
			}
		}
		units++;
	}
	Require(slices != 0, "stream has slices");
	printf("PASS: %s: %u NAL units, %u slice headers\n", path, units, slices);
}

int main(int argc, char** argv)
{
	Regressions();
	bool hevc = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--hevc") == 0) hevc = true;
		else Stream(argv[i], hevc);
	}
	return 0;
}
