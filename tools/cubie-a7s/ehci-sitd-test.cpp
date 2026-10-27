/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Host test (Linux) of the EHCI driver's split isochronous arithmetic
	(src/add-ons/kernel/busses/usb/ehci_split_iso.h): the siTD layout and word
	encodings, the S-/C-masks, OUT T-count and TP, bus times and the TT /
	high-speed bandwidth budget; and of usb_audio's packet sizes
	(src/add-ons/kernel/drivers/audio/usb/IsoPacketSizes.h).

	Build and run with tools/cubie-a7s/ehci-sitd-test.sh.
*/

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// what ehci_hardware.h needs from Haiku's SupportDefs.h
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
#define _PACKED __attribute__((packed))
#if __SIZEOF_POINTER__ == 8
#	define B_HAIKU_64_BIT
#endif

#include "ehci_hardware.h"
#include "ehci_split_iso.h"
#include "IsoPacketSizes.h"


using namespace ehci_split;


static int sChecks;
static int sFailures;


#define CHECK(condition) \
	do { \
		sChecks++; \
		if (!(condition)) { \
			sFailures++; \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
		} \
	} while (0)

#define CHECK_EQUAL(value, expected) \
	do { \
		sChecks++; \
		unsigned long long _value = (unsigned long long)(value); \
		unsigned long long _expected = (unsigned long long)(expected); \
		if (_value != _expected) { \
			sFailures++; \
			printf("FAIL %s:%d: %s = %#llx (%llu), expected %#llx (%llu)\n", \
				__FILE__, __LINE__, #value, _value, _value, _expected, \
				_expected); \
		} \
	} while (0)


static void
test_layout()
{
	// EHCI 1.0 §3.3: nine dwords (seven plus the 64-bit extension)
	CHECK_EQUAL(offsetof(ehci_sitd, next_phy), 0);
	CHECK_EQUAL(offsetof(ehci_sitd, endpoint), 4);
	CHECK_EQUAL(offsetof(ehci_sitd, schedule), 8);
	CHECK_EQUAL(offsetof(ehci_sitd, transfer), 12);
	CHECK_EQUAL(offsetof(ehci_sitd, buffer_phy), 16);
	CHECK_EQUAL(offsetof(ehci_sitd, back_phy), 24);
	CHECK_EQUAL(offsetof(ehci_sitd, ext_buffer_phy), 28);
	CHECK_EQUAL(offsetof(ehci_sitd, this_phy), 36);
	CHECK(sizeof(ehci_sitd) <= 64);
	// the frame list's siTDs must stay 32-byte aligned
	CHECK_EQUAL(sizeof(sitd_entry), 64);
}


static void
test_words()
{
	// dword 1: IN, port 3, hub 2, endpoint 1, device 5
	CHECK_EQUAL(sitd_endpoint(true, 3, 2, 1, 5), 0x83020105);
	CHECK_EQUAL(sitd_endpoint(false, 1, 9, 2, 10), 0x0109020a);
	CHECK_EQUAL(sitd_endpoint(false, 127, 127, 15, 127), 0x7f7f0f7f);

	// dword 2: C-mask in bits 15:8, S-mask in 7:0
	CHECK_EQUAL(sitd_schedule(0x04, 0x70), 0x7004);

	// dword 3: IOC, total bytes, Active
	CHECK_EQUAL(sitd_transfer(192, true), 0x80c00080);
	CHECK_EQUAL(sitd_transfer(1023, false), 0x03ff0080);
	CHECK_EQUAL(sitd_remaining(0x00100000), 16);
	CHECK_EQUAL(sitd_remaining(0x80c00080), 192);

	// dwords 4 and 5: page 0 with the offset, page 1 where the packet ends;
	// OUT adds TP and T-count
	CHECK_EQUAL(sitd_buffer0(0x12345f80), 0x12345f80);
	CHECK_EQUAL(sitd_buffer1(0x12345f80, 192, false), 0x1234600a);
	CHECK_EQUAL(sitd_buffer1(0x12345f80, 192, true), 0x12346000);
	CHECK_EQUAL(sitd_buffer1(0x10000000, 176, false), 0x10000001);
	CHECK_EQUAL(sitd_buffer1(0x10000000, 0, false), 0x10000001);
	CHECK_EQUAL(sitd_buffer1(0x10000c00, 1023, false), 0x1000000e);
	CHECK_EQUAL(sitd_buffer1(0x10000e00, 1023, false), 0x1000100e);
}


static void
test_tcount()
{
	struct {
		uint32_t	length;
		uint32_t	tcount;
		uint32_t	tp;
	} cases[] = {
		{ 0, 1, kSitdTPAll },		// a zero-length packet: one start-split
		{ 1, 1, kSitdTPAll },
		{ 176, 1, kSitdTPAll },		// 44.1 kHz stereo 16 bit, 44 samples
		{ 188, 1, kSitdTPAll },
		{ 189, 2, kSitdTPBegin },
		{ 192, 2, kSitdTPBegin },	// 48 kHz stereo 16 bit
		{ 376, 2, kSitdTPBegin },
		{ 377, 3, kSitdTPBegin },
		{ 576, 4, kSitdTPBegin },	// 96 kHz stereo 24 bit in 3 bytes
		{ 1023, 6, kSitdTPBegin },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		CHECK_EQUAL(out_tcount(cases[i].length), cases[i].tcount);
		CHECK_EQUAL(out_tp(cases[i].length), cases[i].tp);
	}
	CHECK_EQUAL(kSitdTPBegin, 0x08);
}


static void
expect_masks(bool in, uint32_t maxPacket, uint32_t start, bool fits,
	uint8_t sMask, uint8_t cMask, int line)
{
	uint8_t s = 0xaa;
	uint8_t c = 0xaa;
	bool result = masks_at(in, maxPacket, start, s, c);
	sChecks++;
	if (result != fits || (fits && (s != sMask || c != cMask))) {
		sFailures++;
		printf("FAIL line %d: %s %u bytes from microframe %u: %s S %#x C %#x, "
			"expected %s S %#x C %#x\n", line, in ? "IN" : "OUT", maxPacket,
			start, result ? "fits" : "does not fit", s, c,
			fits ? "fits" : "does not fit", sMask, cMask);
	}
}

#define EXPECT_MASKS(in, size, start, fits, s, c) \
	expect_masks(in, size, start, fits, s, c, __LINE__)


static void
test_masks()
{
	// OUT: a start-split per 188 bytes in consecutive microframes, none in
	// microframe 7, no complete-splits
	EXPECT_MASKS(false, 192, 0, true, 0x03, 0x00);
	EXPECT_MASKS(false, 192, 1, true, 0x06, 0x00);
	EXPECT_MASKS(false, 192, 5, true, 0x60, 0x00);
	EXPECT_MASKS(false, 192, 6, false, 0, 0);
	EXPECT_MASKS(false, 176, 6, true, 0x40, 0x00);
	EXPECT_MASKS(false, 176, 7, false, 0, 0);
	EXPECT_MASKS(false, 1023, 0, true, 0x3f, 0x00);
	EXPECT_MASKS(false, 1023, 1, true, 0x7e, 0x00);
	EXPECT_MASKS(false, 1023, 2, false, 0, 0);

	// IN: one start-split in Y, complete-splits in Y+2 .. Y+n+3, all in
	// the frame (no back pointer)
	EXPECT_MASKS(true, 96, 0, true, 0x01, 0x1c);
	EXPECT_MASKS(true, 96, 2, true, 0x04, 0x70);
	EXPECT_MASKS(true, 96, 3, true, 0x08, 0xe0);
	EXPECT_MASKS(true, 96, 4, false, 0, 0);
	EXPECT_MASKS(true, 192, 0, true, 0x01, 0x3c);
	EXPECT_MASKS(true, 192, 2, true, 0x04, 0xf0);
	EXPECT_MASKS(true, 192, 3, false, 0, 0);
	EXPECT_MASKS(true, 752, 0, true, 0x01, 0xfc);
	EXPECT_MASKS(true, 752, 1, false, 0, 0);
	EXPECT_MASKS(true, 753, 0, false, 0, 0);

	// beyond full speed
	EXPECT_MASKS(false, 1024, 0, false, 0, 0);
	EXPECT_MASKS(true, 1024, 0, false, 0, 0);
	EXPECT_MASKS(false, 64, 8, false, 0, 0);

	// every mask that fits: S and C never overlap, C follows S by two
	for (uint32_t size = 0; size <= kMaxFullSpeedIsoPacket; size++) {
		for (uint32_t start = 0; start < 8; start++) {
			for (int in = 0; in < 2; in++) {
				uint8_t s, c;
				if (!masks_at(in, size, start, s, c))
					continue;
				CHECK((s & c) == 0);
				CHECK((s & (1u << start)) != 0);
				CHECK((s & 0x80) == 0);
				if (in) {
					CHECK_EQUAL(s, 1u << start);
					CHECK((c & ((1u << (start + 2)) - 1)) == 0);
					CHECK_EQUAL(__builtin_popcount(c),
						split_transactions(size) + 2);
				} else {
					CHECK_EQUAL(c, 0);
					CHECK_EQUAL(__builtin_popcount(s), split_transactions(size));
				}
			}
		}
	}
}


static void
test_bus_time()
{
	// USB 2.0 §5.11.3 (as Linux's usb_calc_bus_time())
	CHECK_EQUAL(full_speed_iso_nanoseconds(false, 192), 157227);
	CHECK_EQUAL(full_speed_iso_nanoseconds(true, 96), 83378);
	CHECK_EQUAL(high_speed_iso_nanoseconds(192), 4377);
	CHECK_EQUAL(high_speed_iso_nanoseconds(1), 663);
	CHECK_EQUAL(microseconds(4377), 5);
	CHECK_EQUAL(microseconds(1000), 1);

	budget_entry out;
	CHECK(budget_for(1, false, 192, 1, out));
	CHECK_EQUAL(out.ttMicroseconds, 160);
	CHECK_EQUAL(out.hsStartMicroseconds, 5);
	CHECK_EQUAL(out.hsCompleteMicroseconds, 0);

	budget_entry in;
	CHECK(budget_for(1, true, 96, 1, in));
	CHECK_EQUAL(in.ttMicroseconds, 87);
	CHECK_EQUAL(in.hsStartMicroseconds, 1);
	CHECK_EQUAL(in.hsCompleteMicroseconds, 3);

	budget_entry bad;
	CHECK(!budget_for(0, false, 192, 1, bad));
	CHECK(!budget_for(1, false, 192, 3, bad));
	CHECK(!budget_for(1, false, 1024, 1, bad));
}


// The microframe-0 interrupt start-splits the driver keeps free.
static const uint16_t kReserve = 125;


static void
test_audio_card_slots()
{
	budget_entry entries[16];
	memset(entries, 0, sizeof(entries));

	// playback, 48 kHz stereo 16 bit: microframe 0 is taken by the
	// interrupt splits, so microframes 1 and 2
	budget_entry playback;
	CHECK(budget_for(7, false, 192, 1, playback));
	CHECK(find_slot(entries, 16, kReserve, false, 192, playback));
	CHECK_EQUAL(playback.phase, 0);
	CHECK_EQUAL(playback.startMicroframe, 1);
	CHECK_EQUAL(playback.sMask, 0x06);
	CHECK_EQUAL(playback.cMask, 0x00);
	entries[0] = playback;

	// capture, 48 kHz mono 16 bit, on the same TT: after the playback's
	// carry-over into microframe 2
	budget_entry capture;
	CHECK(budget_for(7, true, 96, 1, capture));
	CHECK(find_slot(entries, 16, kReserve, true, 96, capture));
	CHECK_EQUAL(capture.startMicroframe, 2);
	CHECK_EQUAL(capture.sMask, 0x04);
	CHECK_EQUAL(capture.cMask, 0x70);
	entries[1] = capture;

	uint16_t table[kBudgetFrames][8];
	uint16_t total[kBudgetFrames];
	tt_budget(entries, 16, 7, kReserve, table, total);
	CHECK_EQUAL(total[0], 125 + 160 + 87);
	CHECK_EQUAL(table[0][0], 125);
	CHECK_EQUAL(table[0][1], 125);
	CHECK_EQUAL(table[0][2], 35 + 87);
	CHECK_EQUAL(table[0][3], 0);
	CHECK_EQUAL(total[31], total[0]);

	uint16_t hs[kBudgetFrames][8];
	hs_budget(entries, 16, hs);
	CHECK_EQUAL(hs[0][1], 5);
	CHECK_EQUAL(hs[0][2], 5 + 1);
	CHECK_EQUAL(hs[0][4], 3);
	CHECK_EQUAL(hs[0][6], 3);
	CHECK_EQUAL(hs[0][7], 0);
}


static void
test_tt_full()
{
	budget_entry entries[16];
	memset(entries, 0, sizeof(entries));

	// 192-byte OUT streams (160 µs each) on one TT: microframes 1-2 and 3-4;
	// a third would run past microframe 6
	int accepted = 0;
	for (int i = 0; i < 16; i++) {
		budget_entry candidate;
		CHECK(budget_for(1, false, 192, 1, candidate));
		if (!find_slot(entries, 16, kReserve, false, 192, candidate))
			break;
		entries[i] = candidate;
		accepted++;

		uint16_t table[kBudgetFrames][8];
		uint16_t total[kBudgetFrames];
		tt_budget(entries, 16, 1, kReserve, table, total);
		for (uint32_t frame = 0; frame < kBudgetFrames; frame++) {
			CHECK(total[frame] <= kTTFrameMicroseconds);
			for (uint32_t microframe = 0; microframe < 8; microframe++)
				CHECK(table[frame][microframe] <= 125);
		}
	}
	CHECK_EQUAL(accepted, 2);
	CHECK_EQUAL(entries[0].startMicroframe, 1);
	CHECK_EQUAL(entries[1].startMicroframe, 3);
	CHECK_EQUAL(entries[1].sMask, 0x18);

	// another hub's TT is not affected
	budget_entry other;
	CHECK(budget_for(2, false, 192, 1, other));
	CHECK(find_slot(entries, 16, kReserve, false, 192, other));
	CHECK_EQUAL(other.startMicroframe, 1);

	// a small IN stream (24 µs) still fits after the first one's carry-over
	budget_entry small;
	CHECK(budget_for(1, true, 16, 1, small));
	CHECK_EQUAL(small.ttMicroseconds, 24);
	CHECK(find_slot(entries, 16, kReserve, true, 16, small));
	CHECK_EQUAL(small.startMicroframe, 2);
	CHECK_EQUAL(small.sMask, 0x04);
	CHECK_EQUAL(small.cMask, 0x70);

	// without the reserve, microframe 0 is used
	budget_entry none[16];
	memset(none, 0, sizeof(none));
	budget_entry first;
	CHECK(budget_for(1, false, 192, 1, first));
	CHECK(find_slot(none, 16, 0, false, 192, first));
	CHECK_EQUAL(first.startMicroframe, 0);
	CHECK_EQUAL(first.sMask, 0x03);
}


static void
test_periods()
{
	budget_entry entries[16];
	memset(entries, 0, sizeof(entries));

	// 600 bytes every 8 frames: 479 µs, four start-splits
	budget_entry first;
	CHECK(budget_for(1, false, 600, 8, first));
	CHECK_EQUAL(first.ttMicroseconds, 479);
	CHECK(find_slot(entries, 16, kReserve, false, 600, first));
	CHECK_EQUAL(first.phase, 0);
	CHECK_EQUAL(first.startMicroframe, 1);
	CHECK_EQUAL(first.sMask, 0x1e);
	entries[0] = first;

	// the second one no longer fits in frame 0 (mod 8): frame 1
	budget_entry second;
	CHECK(budget_for(1, false, 600, 8, second));
	CHECK(find_slot(entries, 16, kReserve, false, 600, second));
	CHECK_EQUAL(second.phase, 1);
	CHECK_EQUAL(second.startMicroframe, 1);
	entries[1] = second;

	uint16_t table[kBudgetFrames][8];
	uint16_t total[kBudgetFrames];
	tt_budget(entries, 16, 1, kReserve, table, total);
	CHECK_EQUAL(total[0], 125 + 479);
	CHECK_EQUAL(total[1], 125 + 479);
	CHECK_EQUAL(total[2], 125);
	CHECK_EQUAL(total[8], 125 + 479);
	CHECK_EQUAL(table[0][4], 479 - 3 * 125);

	// a period-1 stream must fit in every frame, including the busy ones
	budget_entry every;
	CHECK(budget_for(1, false, 192, 1, every));
	CHECK(!find_slot(entries, 16, kReserve, false, 192, every));

	// periods past the table are booked as kBudgetFrames
	budget_entry rare;
	CHECK(budget_for(3, false, 64, 128, rare));
	CHECK_EQUAL(budget_period(rare), kBudgetFrames);
	CHECK(find_slot(entries, 16, kReserve, false, 64, rare));
	CHECK(rare.phase < kBudgetFrames);
}


static void
test_high_speed_budget()
{
	uint16_t table[kBudgetFrames][8];
	memset(table, 0, sizeof(table));
	for (uint32_t frame = 0; frame < kBudgetFrames; frame++)
		table[frame][2] = 96;

	budget_entry candidate;
	memset(&candidate, 0, sizeof(candidate));
	candidate.tt = 1;
	candidate.period = 1;
	candidate.sMask = 0x04;
	candidate.hsStartMicroseconds = 4;
	CHECK(hs_fits(table, candidate));
	candidate.hsStartMicroseconds = 5;
	CHECK(!hs_fits(table, candidate));

	candidate.sMask = 0x01;
	candidate.cMask = 0x1c;
	candidate.hsStartMicroseconds = 1;
	candidate.hsCompleteMicroseconds = 4;
	CHECK(hs_fits(table, candidate));
	candidate.hsCompleteMicroseconds = 5;
	CHECK(!hs_fits(table, candidate));

	// a busy frame only matters to the periods that hit it
	memset(table, 0, sizeof(table));
	table[3][2] = 100;
	candidate.period = 4;
	candidate.phase = 3;
	CHECK(!hs_fits(table, candidate));
	candidate.phase = 1;
	CHECK(hs_fits(table, candidate));
}


static void
test_audio_packets()
{
	using namespace usb_audio_packets;

	// 44.1 kHz: 9 x 44 + 45 in every 10 ms
	CHECK_EQUAL(pattern_packets(44100, 1000), 10);
	uint32_t sum = 0;
	for (uint32_t i = 0; i < 10; i++) {
		CHECK_EQUAL(samples_in_packet(44100, 1000, i), i == 9 ? 45 : 44);
		sum += samples_in_packet(44100, 1000, i);
	}
	CHECK_EQUAL(sum, 441);
	CHECK_EQUAL(max_samples_in_packet(44100, 1000), 45);
	CHECK_EQUAL(max_samples_in_packet(48000, 1000), 48);

	struct {
		uint32_t	rate;
		uint32_t	packets;
		uint32_t	samples;
	} cases[] = {
		{ 8000, 48, 384 },
		{ 11025, 40, 441 },
		{ 16000, 48, 768 },
		{ 22050, 40, 882 },
		{ 32000, 48, 1536 },
		{ 44100, 40, 1764 },
		{ 48000, 42, 2016 },
		{ 88200, 20, 1764 },
		{ 96000, 21, 2016 },
		{ 192000, 10, 1920 },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint32_t rate = cases[i].rate;
		uint32_t packets = packets_per_buffer(rate, 1000, 2048, 48);
		CHECK_EQUAL(packets, cases[i].packets);
		uint32_t samples = samples_per_buffer(rate, 1000, packets);
		CHECK_EQUAL(samples, cases[i].samples);

		// the packets add up to the buffer, and every buffer is the same
		uint32_t total = 0;
		uint32_t largest = 0;
		for (uint32_t packet = 0; packet < packets; packet++) {
			uint32_t count = samples_in_packet(rate, 1000, packet);
			CHECK_EQUAL(samples_in_packet(rate, 1000, packet + packets), count);
			total += count;
			if (count > largest)
				largest = count;
		}
		CHECK_EQUAL(total, samples);
		CHECK_EQUAL(largest, max_samples_in_packet(rate, 1000));
		CHECK_EQUAL(packets % pattern_packets(rate, 1000), 0);

		// two buffers of one stream fit in EHCI's 128-frame schedule: the
		// driver starts up to (2 + 8 + 7) / 8 + 2 = 4 frames ahead and keeps
		// 8 frames of slack
		CHECK(4 + 2 * packets <= 120);
	}
}


int
main()
{
	test_layout();
	test_words();
	test_tcount();
	test_masks();
	test_bus_time();
	test_audio_card_slots();
	test_tt_full();
	test_periods();
	test_high_speed_budget();
	test_audio_packets();

	printf("%d checks, %d failed\n", sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}
