/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "HevcStream.h"
#include "UvdHevc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

#define CHECK(value) do { if (!(value)) { \
	fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); abort(); } } while (0)

static void Test(const char* path)
{
	FILE* file = fopen(path, "rb");
	CHECK(file && fseek(file, 0, SEEK_END) == 0);
	long size = ftell(file); rewind(file);
	CHECK(size > 0 && size < 128 * 1024 * 1024);
	std::vector<uint8_t> bytes(size);
	CHECK(fread(bytes.data(), 1, size, file) == (size_t)size); fclose(file);
	std::vector<std::vector<uint8_t>> units;
	std::vector<uint8_t> current;
	bool picture = false;
	for (size_t pos = 0; pos + 3 < bytes.size();) {
		if (bytes[pos] || bytes[pos + 1] || bytes[pos + 2] != 1) { pos++; continue; }
		size_t start = pos + 3, end = start;
		while (end + 3 <= bytes.size() && !(bytes[end] == 0 && bytes[end + 1] == 0 && bytes[end + 2] == 1)) end++;
		if (end + 3 > bytes.size()) end = bytes.size();
		pos = end;
		CHECK(end - start >= 3);
		unsigned type = bytes[start] >> 1 & 63;
		bool vcl = type < 32, first = vcl && (bytes[start + 2] & 0x80);
		if (picture && (first || type == 32 || type == 33 || type == 34 || type == 35 || type == 39)) {
			units.push_back(current); current.clear(); picture = false;
		}
		current.insert(current.end(), {0, 0, 1});
		current.insert(current.end(), bytes.begin() + start, bytes.begin() + end);
		picture |= vcl;
	}
	if (picture) units.push_back(current);
	CHECK(!units.empty());
	auto stream = new HevcStream();
	uint32_t slots = 0;
	unsigned frames = 0, skipped = 0;
	std::vector<std::pair<uint32_t, int32_t>> order;
	std::vector<uint8_t> firstSlices;
	for (const auto& unit : units) {
		if (!stream->Prepare(unit.data(), unit.size())) {
			fprintf(stderr, "%s frame %u: %s\n", path, frames, stream->Error()); abort();
		}
		if (stream->skipPicture) skipped++;
		else {
			if (!frames) firstSlices = stream->bitstream;
			CHECK(amdgpu::UvdHevcValidate(stream->config, stream->picture, stream->bitstream.size()));
			CHECK(amdgpu::UvdHevcBitstream(stream->bitstream.data(), stream->bitstream.size(), stream->picture.nal_type));
			uint32_t next;
			CHECK(amdgpu::UvdHevcNextReferences(stream->picture, slots, !frames, next));
			slots = next;
			uint8_t message[amdgpu::kUvdMessageBytes];
			CHECK(amdgpu::UvdHevcMessage(message, 1, 1, stream->config, &stream->picture, stream->bitstream.size(), frames + 1));
			printf("frame %u epoch %u POC %d slot %u refs %04x depth %u\n",
				frames, stream->sequence, stream->poc, stream->picture.current_slot, slots, stream->config.bit_depth);
			order.emplace_back(stream->sequence, stream->poc);
			frames++;
		}
		CHECK(!stream->Prepare(unit.data(), unit.size())); // pending submission cannot be overwritten
		CHECK(stream->Commit());
		CHECK(!stream->Commit());
	}
	// Closed-GOP fixtures start each output epoch at zero, including after
	// IDR resets and POC-LSB wrap within a long sequence.
	std::sort(order.begin(), order.end());
	uint32_t epoch = 0;
	int32_t expectedPoc = 0;
	for (const auto& entry : order) {
		if (entry.first != epoch) { CHECK(entry.first == epoch + 1); epoch = entry.first; expectedPoc = 0; }
		CHECK(entry.second == expectedPoc++);
	}
	stream->Reset();
	CHECK(stream->Prepare(units[0].data(), units[0].size()));
	CHECK(stream->picture.current_slot == 0 && stream->poc == 0);
	CHECK(stream->Commit());
	stream->Reset();
	CHECK(stream->Prepare(firstSlices.data(), firstSlices.size()) && stream->Commit());
	stream->Clear();
	CHECK(!stream->Prepare(firstSlices.data(), firstSlices.size()));
	CHECK(stream->Prepare(units[0].data(), units[0].size()) && stream->sequence == 1 && stream->Commit());
	if (units.size() > 1) {
		stream->Reset();
		CHECK(!stream->Prepare(units[1].data(), units[1].size()));
	}
	// Mutate a real random-access access unit; no firmware submission occurs.
	uint32_t random = 0x71324589;
	for (unsigned n = 0; n < 2000; n++) {
		auto next = [&]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
		auto unit = units[0];
		for (unsigned i = 0, count = next() % 8 + 1; i < count; i++) unit[next() % unit.size()] ^= uint8_t(next());
		stream->Reset();
		if (stream->Prepare(unit.data(), unit.size())) CHECK(stream->Commit());
	}
	delete stream;
	printf("PASS: %s: %u pictures, %u skipped, POC order, kernel metadata, DPB slots, reset and 2000 mutations\n", path, frames, skipped);
}

int main(int argc, char** argv)
{
	CHECK(argc > 1);
	for (int i = 1; i < argc; i++) Test(argv[i]);
}
