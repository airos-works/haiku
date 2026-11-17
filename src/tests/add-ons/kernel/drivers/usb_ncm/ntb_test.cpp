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
		// the remainder is that of the payload after the Ethernet header
		CHECK((datagrams[i].offset + kEthernetHeaderSize) % effectiveDivisor
			== effectiveRemainder);
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
	// where the second datagram goes: its payload on a 4 byte boundary
	size_t second = used;
	while ((second + kEthernetHeaderSize) % 4 != 0)
		second++;
	writer.Start(buffer.data());
	writer.Add(1500);
	writer.Add(2048 - second);
	CHECK(writer.Used() == 2048);
	size_t length = writer.Finish(1, 512, 16384);
	CHECK(length == 2049);
	CHECK(buffer[2048] == 0);

	// at the function's maximum, no padding: it knows where that ends
	writer.Start(buffer.data());
	writer.Add(1500);
	writer.Add(2048 - second);
	length = writer.Finish(1, 512, 2048);
	CHECK(length == 2048);

	// the padded block still parses
	writer.Start(buffer.data());
	writer.Add(1500);
	writer.Add(2048 - second);
	length = writer.Finish(1, 512, 16384);
	datagram datagrams[8];
	uint32_t count;
	CHECK(parse_ntb(buffer.data(), length, 4096, datagrams, 8, &count, NULL)
		== PARSE_OK);
	CHECK(count == 2);
}


struct layout {
	uint16_t	divisor;
	uint16_t	remainder;
	uint16_t	alignment;
	// what the writer has to use: nonsense falls back to 4/0/4
	uint16_t	effectiveDivisor;
	uint16_t	effectivePayloadRemainder;
	uint16_t	effectiveAlignment;
};


/*!	Builds an NTB as usb_ncm sends it and checks every rule of NCM 1.0 (and
	of what Linux' cdc_ncm and FreeBSD's cdce send) straight from the bytes,
	without parse_ntb():
	- NTH: signature, wHeaderLength, wSequence, wBlockLength equal to the
	  transfer length and within the function's dwNtbOutMaxSize, wNdpIndex on
	  wNdpOutAlignment;
	- NDP: signature without CRC ("NCM0"/"ncm0"), wLength counting the used
	  entries and the terminating zero entry, wNextNdpIndex 0, room for
	  maxDatagrams entries before the first datagram;
	- datagrams: in order, not overlapping each other or the NDP, within the
	  block, each one's payload after the Ethernet header at
	  wNdpOutPayloadRemainder modulo wNdpOutDivisor;
	- no transfer that ends on a packet boundary below dwNtbOutMaxSize (the
	  function would wait for more), the padding byte zero.
*/
static void
check_layout(bool ntb32, size_t capacity, const layout& l,
	uint32_t maxDatagrams, const std::vector<size_t>& lengths,
	uint16_t maxPacketSize, uint32_t deviceMaxSize)
{
	std::vector<uint8_t> buffer(capacity + 1, 0xee);
	NTBWriter writer;
	CHECK(writer.Init(ntb32, capacity, l.divisor, l.remainder, l.alignment,
		maxDatagrams));
	CHECK(writer.Divisor() == l.effectiveDivisor);
	CHECK(writer.PayloadRemainder() == l.effectivePayloadRemainder);
	CHECK(writer.NDPAlignment() == l.effectiveAlignment);
	writer.Start(buffer.data());

	std::vector<size_t> added;
	for (size_t i = 0; i < lengths.size(); i++) {
		uint8_t* target = writer.Add(lengths[i]);
		if (target == NULL)
			break;
		fill_frame(target, lengths[i], (uint32_t)i);
		added.push_back(lengths[i]);
	}
	CHECK(!added.empty());
	const uint16_t sequence = 0x1234;
	size_t length = writer.Finish(sequence, maxPacketSize, deviceMaxSize);
	const uint8_t* data = buffer.data();

	// NTH
	const size_t headerSize = ntb32 ? kNTH32Size : kNTH16Size;
	CHECK(read32(data) == (ntb32 ? kNTH32Signature : kNTH16Signature));
	CHECK(read16(data + 4) == headerSize);
	CHECK(read16(data + 6) == sequence);
	size_t blockLength = ntb32 ? read32(data + 8) : read16(data + 8);
	size_t ndpIndex = ntb32 ? read32(data + 12) : read16(data + 10);
	CHECK(blockLength == length);
	CHECK(length <= capacity);
	CHECK(length <= deviceMaxSize);
	CHECK(ndpIndex >= headerSize);
	CHECK(ndpIndex % l.effectiveAlignment == 0);
	// right after the header, as Linux and FreeBSD put it
	CHECK(ndpIndex < headerSize + l.effectiveAlignment);

	// NDP
	const size_t ndpHeaderSize = ntb32 ? kNDP32HeaderSize : kNDP16HeaderSize;
	const size_t entrySize = ntb32 ? kNDP32EntrySize : kNDP16EntrySize;
	const uint8_t* ndp = data + ndpIndex;
	CHECK(read32(ndp) == (ntb32 ? kNDP32Signature : kNDP16Signature));
	size_t ndpLength = read16(ndp + 4);
	CHECK(ndpLength == ndpHeaderSize + (added.size() + 1) * entrySize);
	CHECK(ndpLength % 4 == 0);
	if (ntb32) {
		CHECK(read16(ndp + 6) == 0);
		CHECK(read32(ndp + 8) == 0);
		CHECK(read32(ndp + 12) == 0);
	} else
		CHECK(read16(ndp + 6) == 0);
	const size_t ndpEnd = ndpIndex + ndpHeaderSize
		+ (maxDatagrams + 1) * entrySize;

	size_t previousEnd = ndpEnd;
	for (size_t i = 0; i <= added.size(); i++) {
		const uint8_t* entry = ndp + ndpHeaderSize + i * entrySize;
		size_t offset = ntb32 ? read32(entry) : read16(entry);
		size_t datagramLength = ntb32 ? read32(entry + 4) : read16(entry + 2);
		if (i == added.size()) {
			// the terminating entry, and the unused ones after it
			for (size_t j = i; j <= maxDatagrams; j++) {
				const uint8_t* unused = ndp + ndpHeaderSize + j * entrySize;
				for (size_t k = 0; k < entrySize; k++)
					CHECK(unused[k] == 0);
			}
			break;
		}

		CHECK(datagramLength == added[i]);
		CHECK(offset >= previousEnd);
		CHECK(offset - previousEnd < l.effectiveDivisor);
		CHECK((offset + kEthernetHeaderSize) % l.effectiveDivisor
			== l.effectivePayloadRemainder);
		CHECK(offset + datagramLength <= length);
		CHECK(frame_matches(data + offset, datagramLength, (uint32_t)i));
		// the gap before it is zeroed
		for (size_t k = previousEnd; k < offset; k++)
			CHECK(data[k] == 0);
		previousEnd = offset + datagramLength;
	}

	// ends with the last datagram, or one byte after it
	CHECK(length == previousEnd || length == previousEnd + 1);
	if (length == previousEnd + 1) {
		CHECK(data[previousEnd] == 0);
		CHECK(previousEnd % maxPacketSize == 0);
	}
	if (maxPacketSize != 0 && length < deviceMaxSize)
		CHECK(length % maxPacketSize != 0);

	// and the parser agrees
	datagram datagrams[300];
	uint32_t count = 0;
	parse_stats stats = {};
	CHECK(parse_ntb(data, length, kMaxFrame, datagrams, 300, &count, &stats)
		== PARSE_OK);
	CHECK(count == added.size());
	CHECK(stats.bad_datagrams == 0);
}


static void
test_layouts()
{
	const layout layouts[] = {
		// the Realtek RTL8156: datagrams on 4 byte boundaries
		{ 4, 2, 4, 4, 2, 4 },
		// Linux' f_ncm
		{ 4, 0, 4, 4, 0, 4 },
		{ 4, 1, 4, 4, 1, 4 },
		{ 4, 3, 8, 4, 3, 8 },
		{ 8, 0, 8, 8, 0, 8 },
		{ 8, 2, 8, 8, 2, 8 },
		{ 8, 6, 16, 8, 6, 16 },
		{ 16, 14, 16, 16, 14, 16 },
		{ 32, 14, 16, 32, 14, 16 },
		{ 32, 0, 32, 32, 0, 32 },
		{ 64, 2, 64, 64, 2, 64 },
		// nonsense: divisor and alignment 4, remainder 0
		{ 3, 9, 0, 4, 0, 4 },
		{ 4, 4, 6, 4, 0, 4 },
		{ 0, 0, 2, 4, 0, 4 },
	};

	const std::vector<std::vector<size_t>> sets = {
		{ 42 },				// an ARP reply
		{ 342 },			// a DHCP discover
		{ 60, 1514, 66, 54, 1000, 14, 1513, 590 },
		std::vector<size_t>(12, 1514),
		std::vector<size_t>(40, 60),
		{ 15, 16, 17, 18, 19, 20, 21 },
	};

	for (const layout& l : layouts) {
		for (const auto& lengths : sets) {
			for (uint32_t maxDatagrams : { 1u, 2u, 3u, 32u, 40u }) {
				// what the RTL8156 asks for: 17404 out, 1024 byte packets
				check_layout(false, 17404, l, maxDatagrams, lengths, 1024,
					17404);
				check_layout(false, 16384, l, maxDatagrams, lengths, 512,
					16384);
				check_layout(false, 16384, l, maxDatagrams, lengths, 64,
					65536);
				check_layout(true, 65536, l, maxDatagrams, lengths, 1024,
					65536);
			}
		}
	}

	// Every length from an Ethernet header up, one and two at a time: the
	// padding byte comes and goes with the packet boundary.
	for (const layout& l : layouts) {
		for (size_t length = 14; length <= 1514; length++) {
			check_layout(false, 17404, l, 32, { length }, 1024, 17404);
			check_layout(false, 4096, l, 1, { length }, 64, 65536);
			check_layout(false, 17404, l, 32, { length, 1514 - length / 2 },
				512, 17404);
		}
	}
}


/*!	The exact bytes of an ARP reply sent to an RTL8156 (divisor 4, payload
	remainder 2, NDP alignment 4) with one datagram per NTB: what Linux'
	cdc_ncm sends with tx_max_datagrams 1.
*/
static void
test_rtl8156_bytes()
{
	std::vector<uint8_t> buffer(17405, 0xee);
	NTBWriter writer;
	CHECK(writer.Init(false, 17404, 4, 2, 4, 1));
	CHECK(writer.FrameRemainder() == 0);
	writer.Start(buffer.data());
	uint8_t* target = writer.Add(42);
	CHECK(target == buffer.data() + 28);
	fill_frame(target, 42, 0);
	size_t length = writer.Finish(5, 1024, 17404);
	CHECK(length == 70);

	const uint8_t expected[28] = {
		'N', 'C', 'M', 'H', 12, 0, 5, 0, 70, 0, 12, 0,
		'N', 'C', 'M', '0', 16, 0, 0, 0, 28, 0, 42, 0, 0, 0, 0, 0
	};
	CHECK(memcmp(buffer.data(), expected, sizeof(expected)) == 0);

	// with room for 32 entries: the NDP takes 8 + 33 * 4 bytes, the frame
	// follows at 152 (Linux, with its 40 entries, puts it at 184)
	CHECK(writer.Init(false, 17404, 4, 2, 4, 32));
	writer.Start(buffer.data());
	CHECK(writer.Add(342) == buffer.data() + 152);
	CHECK(writer.Add(42) == buffer.data() + 152 + 344);
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
	test_layouts();
	test_rtl8156_bytes();
	test_parse_good();
	test_parse_bad();
	test_fuzz();

	printf("ntb_test: %d checks, %d failed\n", sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}
