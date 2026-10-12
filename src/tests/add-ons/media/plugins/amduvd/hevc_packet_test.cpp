/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "HevcPacket.h"
#include "HevcStream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
	fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); abort(); } } while (0)

static std::vector<uint8_t> Record(FILE* file)
{
	uint8_t size[4];
	size_t got = fread(size, 1, 4, file);
	if (!got) { CHECK(feof(file)); return {}; }
	CHECK(got == 4);
	uint32_t bytes = size[0] | uint32_t(size[1]) << 8 | uint32_t(size[2]) << 16 | uint32_t(size[3]) << 24;
	CHECK(bytes && bytes <= AMDGPU_VIDEO_MAX_BITSTREAM);
	std::vector<uint8_t> data(bytes);
	CHECK(fread(data.data(), 1, bytes, file) == bytes);
	return data;
}

int main(int argc, char** argv)
{
	CHECK(argc == 3);
	FILE* mp4 = fopen(argv[1], "rb");
	FILE* raw = fopen(argv[2], "rb");
	CHECK(mp4 && raw);
	auto extra = Record(mp4);
	HevcPacket packet;
	std::vector<uint8_t> output;
	CHECK(!packet.Convert(extra.data(), extra.size(), false, output));
	CHECK(packet.Configure(extra.data(), extra.size()));
	// All four encoded length sizes have the same bounded conversion contract.
	for (unsigned length = 1; length <= 4; length++) {
		auto empty = extra; empty.resize(23); empty[22] = 0;
		empty[21] = (empty[21] & ~3) | (length - 1);
		CHECK(packet.Configure(empty.data(), empty.size()));
		std::vector<uint8_t> nal(length, 0);
		nal.back() = 3; nal.insert(nal.end(), {38, 1, 0x80});
		CHECK(packet.Convert(nal.data(), nal.size(), true, output));
		CHECK(output == std::vector<uint8_t>({0, 0, 0, 1, 38, 1, 0x80}));
		CHECK(!packet.Convert(nal.data(), length - 1, false, output) && output.empty());
	}
	CHECK(packet.Configure(extra.data(), extra.size()));
	auto a = new HevcStream(), b = new HevcStream();
	unsigned count = 0;
	std::vector<uint8_t> first;
	for (;;) {
		auto encoded = Record(mp4), annex = Record(raw);
		if (encoded.empty()) { CHECK(annex.empty()); break; }
		CHECK(!annex.empty());
		if (!count) first = encoded;
		CHECK(packet.Convert(encoded.data(), encoded.size(), count == 0, output));
		CHECK(a->Prepare(output.data(), output.size()) && b->Prepare(annex.data(), annex.size()));
		CHECK(!memcmp(&a->config, &b->config, sizeof(a->config))
			&& !memcmp(&a->picture, &b->picture, sizeof(a->picture))
			&& a->bitstream == b->bitstream && a->poc == b->poc && a->sequence == b->sequence);
		CHECK(a->Commit() && b->Commit()); count++;
	}
	CHECK(count);
	delete a; delete b; fclose(mp4); fclose(raw);
	CHECK(packet.Convert(first.data(), first.size(), true, output));
	auto expected = output;
	std::vector<std::vector<uint8_t>> arrays;
	for (size_t at = 23; at < extra.size();) {
		size_t start = at;
		CHECK(extra.size() - at >= 3);
		unsigned nals = unsigned(extra[at + 1]) * 256 + extra[at + 2]; at += 3;
		for (unsigned i = 0; i < nals; i++) {
			CHECK(extra.size() - at >= 2);
			size_t size = size_t(extra[at]) * 256 + extra[at + 1]; at += 2;
			CHECK(size <= extra.size() - at); at += size;
		}
		arrays.emplace_back(extra.begin() + start, extra.begin() + at);
	}
	std::vector<uint8_t> reversed(extra.begin(), extra.begin() + 23);
	for (auto it = arrays.rbegin(); it != arrays.rend(); ++it) reversed.insert(reversed.end(), it->begin(), it->end());
	CHECK(packet.Configure(reversed.data(), reversed.size()));
	CHECK(packet.Convert(first.data(), first.size(), true, output) && output == expected);
	// Truncated configuration cannot leave an earlier converter enabled.
	for (size_t n = 1; n < extra.size(); n++) {
		CHECK(!packet.Configure(extra.data(), n));
		CHECK(!packet.Convert(first.data(), first.size(), true, output) && output.empty());
	}
	CHECK(packet.Configure(extra.data(), extra.size()));
	uint8_t invalid[] = {0xff, 0xff, 0xff, 0xff};
	CHECK(!packet.Convert(invalid, sizeof(invalid), true, output) && output.empty());
	uint32_t random = 0x71324589;
	for (unsigned n = 0; n < 10000; n++) {
		auto next = [&]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
		auto config = extra, data = first;
		config[next() % config.size()] ^= uint8_t(next());
		data[next() % data.size()] ^= uint8_t(next());
		if (packet.Configure(config.data(), config.size()))
			packet.Convert(data.data(), data.size(), true, output);
	}
	printf("PASS: %u hvcC packets exactly match raw HEVC metadata/slices; truncation, failed configuration and 10000 mutations\n", count);
}
