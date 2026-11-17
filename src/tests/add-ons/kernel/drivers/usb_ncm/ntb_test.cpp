/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Tests the NTB code of usb_ncm: blocks built by NTBWriter parse back to the
	same datagrams, in both formats and with the alignments functions ask for,
	and parse_ntb() rejects or skips what is broken without reading outside
	the block. Builds for Haiku (jam ntb_test) and on any host:
	  g++ -std=c++17 -g -fsanitize=address,undefined -I<usb_ncm dir> \
	      ntb_test.cpp <usb_ncm dir>/NTB.cpp -o ntb_test && ./ntb_test
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "NTB.h"


using namespace ncm;


static int sFailures = 0;
static int sChecks = 0;

#define CHECK(condition) \
	do { \
		sChecks++; \
		if (!(condition)) { \
			sFailures++; \
			fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, \
				#condition); \
		} \
	} while (0)


static const uint32_t kMaxFrame = 1514;


static void
fill_frame(uint8_t* data, size_t length, uint32_t seed)
{
	for (size_t i = 0; i < length; i++)
		data[i] = (uint8_t)(seed * 31 + i * 7);
}


static bool
frame_matches(const uint8_t* data, size_t length, uint32_t seed)
{
	for (size_t i = 0; i < length; i++) {
		if (data[i] != (uint8_t)(seed * 31 + i * 7))
			return false;
	}
	return true;
}


/*!	Builds an NTB of the given frames, parses it back and compares. */
static void
round_trip(bool ntb32, size_t capacity, uint16_t divisor, uint16_t remainder,
	uint16_t alignment, uint32_t maxDatagrams,
	const std::vector<size_t>& lengths, uint16_t maxPacketSize,
	uint32_t deviceMaxSize, size_t* _transferLength = NULL)
{
	std::vector<uint8_t> buffer(capacity + 1, 0xee);
	NTBWriter writer;
	CHECK(writer.Init(ntb32, capacity, divisor, remainder, alignment,
		maxDatagrams));
	writer.Start(buffer.data());

	uint32_t added = 0;
	for (size_t i = 0; i < lengths.size(); i++) {
		uint8_t* target = writer.Add(lengths[i]);
		if (target == NULL)
			break;
		fill_frame(target, lengths[i], (uint32_t)i);
		added++;
	}
	CHECK(added == writer.Count());

	size_t length = writer.Finish(7, maxPacketSize, deviceMaxSize);
	CHECK(length <= capacity + 1);
	if (maxPacketSize != 0 && length < deviceMaxSize)
		CHECK(length % maxPacketSize != 0);
	if (_transferLength != NULL)
		*_transferLength = length;

	datagram datagrams[64];
	uint32_t count = 0;
	parse_stats stats = {};
	CHECK(parse_ntb(buffer.data(), length, kMaxFrame, datagrams, 64, &count,
		&stats) == PARSE_OK);
	CHECK(count == added);
	CHECK(stats.ndps == 1);
	CHECK(stats.bad_datagrams == 0);

	// the sequence and the block length as written
	CHECK(read16(buffer.data() + 6) == 7);
	if (ntb32)
		CHECK(read32(buffer.data() + 8) == length);
	else
		CHECK(read16(buffer.data() + 8) == length);

	uint16_t effectiveDivisor = divisor;
	if (divisor < 4 || (divisor & (divisor - 1)) != 0)
		effectiveDivisor = 4;
	uint16_t effectiveRemainder
		= remainder < effectiveDivisor ? remainder : 0;

	for (uint32_t i = 0; i < count && i < added; i++) {
		CHECK(datagrams[i].length == lengths[i]);
		CHECK(datagrams[i].offset % effectiveDivisor == effectiveRemainder);
		CHECK(frame_matches(buffer.data() + datagrams[i].offset,
			datagrams[i].length, i));
	}
}


static void
test_round_trips()
{
	std::vector<size_t> full(12, 1514);
	std::vector<size_t> mixed = { 60, 1514, 66, 54, 1000, 14, 1513, 590 };
	std::vector<size_t> small(40, 60);

	// Linux' f_ncm: 16 KiB out blocks, divisor 4, alignment 4
	round_trip(false, 16384, 4, 0, 4, 32, full, 512, 16384);
	round_trip(false, 16384, 4, 0, 4, 32, mixed, 512, 16384);
	round_trip(false, 16384, 4, 0, 4, 32, small, 512, 16384);
	// payload alignment as some functions ask for it
	round_trip(false, 32768, 8, 2, 8, 32, mixed, 1024, 32768);
	round_trip(false, 32768, 32, 14, 16, 16, mixed, 1024, 65536);
	// nonsense alignments fall back to 4
	round_trip(false, 16384, 3, 9, 0, 32, mixed, 512, 16384);
	// 32 bit
	round_trip(true, 65536, 4, 0, 8, 32, full, 1024, 65536);
	round_trip(true, 131072, 8, 0, 8, 64, mixed, 1024, 131072);

	// only as many as fit by count and by size
	{
		std::vector<uint8_t> buffer(16385);
		NTBWriter writer;
		CHECK(writer.Init(false, 16384, 4, 0, 4, 4));
		writer.Start(buffer.data());
		for (int i = 0; i < 4; i++)
			CHECK(writer.Add(60) != NULL);
		CHECK(!writer.CanAdd(60));
		CHECK(writer.Add(60) == NULL);
		CHECK(writer.Count() == 4);

		CHECK(writer.Init(false, 4096, 4, 0, 4, 32));
		writer.Start(buffer.data());
		CHECK(writer.Add(1514) != NULL);
		CHECK(writer.Add(1514) != NULL);
		CHECK(!writer.CanAdd(1514));
		CHECK(writer.CanAdd(800));
	}

	// a 16 bit block is never longer than 64 KiB
	{
		std::vector<uint8_t> buffer(131073);
		NTBWriter writer;
		CHECK(writer.Init(false, 131072, 4, 0, 4, 64));
		CHECK(writer.Capacity() == 0xffff);
	}

	// parameters that leave no room fail
	{
		NTBWriter writer;
		CHECK(!writer.Init(false, 64, 4, 0, 4, 32));
		CHECK(!writer.Init(false, 16384, 4, 0, 4, 0));
	}
}


static void
test_padding()
{
	// A second frame that makes the block end on a 512 byte boundary.
	NTBWriter writer;
	std::vector<uint8_t> buffer(16385);
	CHECK(writer.Init(false, 16384, 4, 0, 4, 32));
	writer.Start(buffer.data());
	writer.Add(1500);
	size_t used = writer.Used();
	writer.Start(buffer.data());
	writer.Add(1500);
	writer.Add(2048 - ((used + 3) & ~(size_t)3));
	CHECK(writer.Used() == 2048);
	size_t length = writer.Finish(1, 512, 16384);
	CHECK(length == 2049);
	CHECK(buffer[2048] == 0);

	// at the function's maximum, no padding: it knows where that ends
	writer.Start(buffer.data());
	writer.Add(1500);
	writer.Add(2048 - ((used + 3) & ~(size_t)3));
	length = writer.Finish(1, 512, 2048);
	CHECK(length == 2048);

	// the padded block still parses
	writer.Start(buffer.data());
	writer.Add(1500);
	writer.Add(2048 - ((used + 3) & ~(size_t)3));
	length = writer.Finish(1, 512, 16384);
	datagram datagrams[8];
	uint32_t count;
	CHECK(parse_ntb(buffer.data(), length, 4096, datagrams, 8, &count, NULL)
		== PARSE_OK);
	CHECK(count == 2);
}


/*!	A hand-made NTB16: header, datagrams, then NDPs at the given places. */
struct Builder {
	std::vector<uint8_t> data;

	Builder(size_t size) : data(size, 0) {}

	void Header(uint16_t blockLength, uint16_t ndpIndex,
		uint32_t signature = kNTH16Signature, uint16_t headerLength = 12)
	{
		write32(&data[0], signature);
		write16(&data[4], headerLength);
		write16(&data[6], 1);
		write16(&data[8], blockLength);
		write16(&data[10], ndpIndex);
	}

	void NDP(size_t at, const std::vector<std::pair<uint16_t, uint16_t>>& entries,
		uint16_t next = 0, uint32_t signature = kNDP16Signature,
		int lengthAdjust = 0)
	{
		write32(&data[at], signature);
		write16(&data[at + 4],
			(uint16_t)(8 + 4 * (entries.size() + 1) + lengthAdjust));
		write16(&data[at + 6], next);
		size_t offset = at + 8;
		for (auto& entry : entries) {
			write16(&data[offset], entry.first);
			write16(&data[offset + 2], entry.second);
			offset += 4;
		}
		write16(&data[offset], 0);
		write16(&data[offset + 2], 0);
	}

	parse_result Parse(uint32_t* count, parse_stats* stats,
		datagram* datagrams = NULL, uint32_t max = 32, size_t length = 0)
	{
		datagram local[32];
		*stats = parse_stats();
		return parse_ntb(data.data(), length != 0 ? length : data.size(),
			kMaxFrame, datagrams != NULL ? datagrams : local, max, count,
			stats);
	}
};


static void
test_parse_good()
{
	// two NDPs chained, the second before the first
	{
		Builder b(4096);
		b.Header(4096, 2048);
		b.NDP(2048, { {16, 100}, {116, 60} }, 1024);
		b.NDP(1024, { {200, 1514} });
		uint32_t count;
		parse_stats stats;
		datagram datagrams[32];
		CHECK(b.Parse(&count, &stats, datagrams) == PARSE_OK);
		CHECK(count == 3);
		CHECK(stats.ndps == 2);
		CHECK(datagrams[2].offset == 200 && datagrams[2].length == 1514);
	}

	// block length 0: ends with the transfer
	{
		Builder b(2048);
		b.Header(0, 12);
		b.NDP(12, { {64, 1500} });
		uint32_t count;
		parse_stats stats;
		CHECK(b.Parse(&count, &stats) == PARSE_OK);
		CHECK(count == 1);
	}

	// CRC NDP: four bytes less each
	{
		Builder b(2048);
		b.Header(2048, 12);
		b.NDP(12, { {64, 1518} }, 0, 0x314d434e);
		uint32_t count;
		parse_stats stats;
		datagram datagrams[32];
		CHECK(b.Parse(&count, &stats, datagrams) == PARSE_OK);
		CHECK(count == 1 && datagrams[0].length == 1514);
	}

	// more datagrams than room: counted, not returned
	{
		Builder b(4096);
		b.Header(4096, 12);
		std::vector<std::pair<uint16_t, uint16_t>> entries;
		for (int i = 0; i < 10; i++)
			entries.push_back({ (uint16_t)(256 + i * 64), 60 });
		b.NDP(12, entries);
		uint32_t count;
		parse_stats stats;
		datagram datagrams[4];
		CHECK(b.Parse(&count, &stats, datagrams, 4) == PARSE_OK);
		CHECK(count == 4);
		CHECK(stats.truncated == 6);
	}

	// the table ends at the first zero entry
	{
		Builder b(4096);
		b.Header(4096, 12);
		b.NDP(12, { {256, 60}, {0, 0}, {512, 60} });
		uint32_t count;
		parse_stats stats;
		CHECK(b.Parse(&count, &stats) == PARSE_OK);
		CHECK(count == 1);
	}
}


static void
test_parse_bad()
{
	uint32_t count;
	parse_stats stats;

	// too short for any header
	{
		Builder b(12);
		b.Header(12, 0);
		CHECK(b.Parse(&count, &stats, NULL, 32, 3) == PARSE_SHORT);
		CHECK(b.Parse(&count, &stats, NULL, 32, 8) == PARSE_SHORT);
	}
	// wrong signature
	{
		Builder b(2048);
		b.Header(2048, 12, 0x12345678);
		CHECK(b.Parse(&count, &stats) == PARSE_BAD_SIGNATURE);
	}
	// wrong header length
	{
		Builder b(2048);
		b.Header(2048, 16, kNTH16Signature, 16);
		CHECK(b.Parse(&count, &stats) == PARSE_BAD_HEADER);
	}
	// block length shorter than the header
	{
		Builder b(2048);
		b.Header(8, 12);
		CHECK(b.Parse(&count, &stats) == PARSE_BAD_HEADER);
	}
	// NDP index inside the header, past the block, at its very end
	{
		Builder b(2048);
		b.Header(2048, 4);
		CHECK(b.Parse(&count, &stats) == PARSE_BAD_NDP);
		b.Header(2048, 4000);
		CHECK(b.Parse(&count, &stats) == PARSE_BAD_NDP);
		b.Header(2048, 2044);
		CHECK(b.Parse(&count, &stats) == PARSE_BAD_NDP);
		b.Header(0xffff, 0xfffc);
		CHECK(b.Parse(&count, &stats) == PARSE_BAD_NDP);
	}
	// NDP signature of the other format, or garbage
	{
		Builder b(2048);
		b.Header(2048, 12);
		b.NDP(12, { {64, 60} }, 0, kNDP32Signature);
		CHECK(b.Parse(&count, &stats) == PARSE_BAD_NDP);
		b.NDP(12, { {64, 60} }, 0, 0x324d434e);
		CHECK(b.Parse(&count, &stats) == PARSE_BAD_NDP);
	}
	// NDP length: too small, not a multiple of 4, past the block
	{
		Builder b(2048);
		b.Header(2048, 12);
		b.NDP(12, { }, 0, kNDP16Signature, 0);
			// 12 bytes: no room for an entry and the terminator
		CHECK(b.Parse(&count, &stats) == PARSE_BAD_NDP);
		b.NDP(12, { {64, 60} }, 0, kNDP16Signature, 2);
		CHECK(b.Parse(&count, &stats) == PARSE_BAD_NDP);
		b.NDP(2000, { {64, 60}, {128, 60}, {192, 60} }, 0, kNDP16Signature,
			32);
		b.Header(2048, 2000);
		CHECK(b.Parse(&count, &stats) == PARSE_BAD_NDP);
	}
	// datagrams outside the block, in the header, too short, too long
	{
		Builder b(2048);
		b.Header(2048, 12);
		b.NDP(12, { {2000, 60}, {1988, 60}, {4, 60}, {64, 13}, {64, 1515},
			{0xfff0, 0x20}, {128, 60} });
		datagram datagrams[32];
		CHECK(b.Parse(&count, &stats, datagrams) == PARSE_OK);
		CHECK(count == 2);
		CHECK(stats.bad_datagrams == 5);
		CHECK(datagrams[0].offset == 1988 && datagrams[1].offset == 128);
	}
	// a block length that cuts datagrams off
	{
		Builder b(2048);
		b.Header(1024, 12);
		b.NDP(12, { {64, 60}, {1000, 60} });
		CHECK(b.Parse(&count, &stats) == PARSE_OK);
		CHECK(count == 1 && stats.bad_datagrams == 1);
	}
	// a block length beyond the transfer: the transfer limits
	{
		Builder b(2048);
		b.Header(4000, 12);
		b.NDP(12, { {64, 60}, {1000, 60} });
		CHECK(b.Parse(&count, &stats, NULL, 32, 1024) == PARSE_OK);
		CHECK(count == 1 && stats.bad_datagrams == 1);
	}
	// NDP loops: to itself, between two; the datagrams count once
	{
		Builder b(4096);
		b.Header(4096, 12);
		b.NDP(12, { {512, 60} }, 12);
		CHECK(b.Parse(&count, &stats) == PARSE_BAD_NDP);
		CHECK(count == 1);
		b.NDP(12, { {512, 60} }, 1024);
		b.NDP(1024, { {600, 60} }, 12);
		CHECK(b.Parse(&count, &stats) == PARSE_BAD_NDP);
		CHECK(count == 2);
	}
	// a chain longer than kMaxNDPs
	{
		Builder b(8192);
		b.Header(8192, 12);
		size_t at = 12;
		for (int i = 0; i < 20; i++) {
			size_t next = at + 64;
			b.NDP(at, { {4096, 60} }, i == 19 ? 0 : (uint16_t)next);
			at = next;
		}
		CHECK(b.Parse(&count, &stats) == PARSE_BAD_NDP);
		CHECK(count == kMaxNDPs);
	}
}


/*!	Random changes to good blocks: never a datagram outside the transfer.
	With -fsanitize=address, no read outside it either. */
static void
test_fuzz()
{
	srand(123);
	for (int round = 0; round < 200000; round++) {
		bool ntb32 = (round & 1) != 0;
		size_t capacity = 512 + rand() % 8192;
		std::vector<uint8_t> buffer(capacity + 1);
		NTBWriter writer;
		if (!writer.Init(ntb32, capacity, 4 << (rand() % 3), rand() % 8,
				4 << (rand() % 3), 1 + rand() % 40)) {
			continue;
		}
		writer.Start(buffer.data());
		int frames = rand() % 20;
		for (int i = 0; i < frames; i++) {
			size_t length = 14 + rand() % 1500;
			uint8_t* target = writer.Add(length);
			if (target == NULL)
				break;
			fill_frame(target, length, i);
		}
		if (writer.IsEmpty())
			continue;
		size_t length = writer.Finish(round, 512, capacity);

		int changes = 1 + rand() % 4;
		for (int i = 0; i < changes; i++) {
			size_t at = rand() % length;
			if (rand() % 3 == 0)
				buffer[at] = 0xff;
			else
				buffer[at] ^= 1 << (rand() % 8);
		}
		size_t parseLength = rand() % 4 == 0 ? rand() % (length + 1) : length;

		// a copy of exactly that size, so that ASan sees any overrun
		std::vector<uint8_t> exact(buffer.begin(),
			buffer.begin() + parseLength);
		datagram datagrams[64];
		uint32_t count;
		parse_stats stats = {};
		parse_ntb(exact.data(), exact.size(), kMaxFrame, datagrams, 64,
			&count, &stats);
		CHECK(count <= 64);
		for (uint32_t i = 0; i < count; i++) {
			CHECK(datagrams[i].length >= 14);
			CHECK(datagrams[i].length <= kMaxFrame);
			CHECK((size_t)datagrams[i].offset + datagrams[i].length
				<= parseLength);
		}
	}
}


int
main()
{
	test_round_trips();
	test_padding();
	test_parse_good();
	test_parse_bad();
	test_fuzz();

	printf("ntb_test: %d checks, %d failed\n", sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}
