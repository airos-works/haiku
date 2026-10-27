/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef EHCI_SPLIT_ISO_H
#define EHCI_SPLIT_ISO_H

/*	The arithmetic of split isochronous transfers: a full-speed isochronous
	endpoint behind a high-speed hub, which the EHCI controller reaches with
	siTDs through the hub's transaction translator (TT). EHCI 1.0 §3.3 (the
	siTD), §4.12.3 (split isochronous), USB 2.0 §11.18 (TT scheduling) and
	§5.11.3 (bus time).

	No kernel dependencies: tools/cubie-a7s/ehci-sitd-test.cpp builds this
	header for the host and checks the masks, T-counts, word encodings and the
	bandwidth budget that ehci.cpp uses.

	The model is the one Linux's ehci-sched.c uses (CONFIG_USB_EHCI_TT_NEWSCHED):
	- OUT: one start-split per 188 bytes in consecutive microframes from Y,
	  no complete-splits; the last start-split must be before microframe 7.
	- IN: one start-split in Y, complete-splits in Y+2 .. Y+2+n+1 for n
	  start-split-sized pieces, all inside the frame, so the back pointer is
	  never needed (it is set to Terminate).
	- The TT's full-speed time is booked per start microframe with carry-over
	  into the next microframes; at most 900 µs per frame, 125 µs per
	  microframe, 30 µs in microframe 6 and nothing in 7.
	- Each start- and complete-split microframe keeps the high-speed periodic
	  limit of 100 µs.
*/

#include <stddef.h>
#include <stdint.h>


namespace ehci_split {


// A start- or complete-split carries at most this much full-speed data per
// high-speed microframe (USB 2.0 §11.18.4, the TT's best case budget).
static const uint32_t kBytesPerMicroframe = 188;

// The bandwidth tables span this many frames. A longer period is booked as if
// it were this long, which only overestimates.
static const uint32_t kBudgetFrames = 32;

// Periodic full-speed time of one TT per frame (90% of 1 ms) and per start
// microframe. A transaction started late in the frame must still end in it.
static const uint32_t kTTFrameMicroseconds = 900;
static const uint16_t kTTMicroframeMicroseconds[8]
	= { 125, 125, 125, 125, 125, 125, 30, 0 };

// High-speed periodic time per microframe (80% of 125 µs, EHCI §4.12).
static const uint32_t kHSMicroframeMicroseconds = 100;

// The TT think time is in the hub descriptor; book the slowest (32 full-speed
// bit times, TTTT = 3).
static const uint32_t kTTThinkTimeNanoseconds = 2666;

// The longest full-speed isochronous packet (USB 2.0 §5.6.3).
static const uint32_t kMaxFullSpeedIsoPacket = 1023;


// #pragma mark - siTD fields (EHCI §3.3)


// Endpoint and transaction translator characteristics (dword 1)
static const uint32_t kSitdDirectionIn = 1u << 31;

inline uint32_t
sitd_endpoint(bool in, uint32_t hubPort, uint32_t hubAddress,
	uint32_t endpoint, uint32_t deviceAddress)
{
	return (in ? kSitdDirectionIn : 0) | ((hubPort & 0x7f) << 24)
		| ((hubAddress & 0x7f) << 16) | ((endpoint & 0x0f) << 8)
		| (deviceAddress & 0x7f);
}


// Microframe schedule control (dword 2)
inline uint32_t
sitd_schedule(uint8_t sMask, uint8_t cMask)
{
	return ((uint32_t)cMask << 8) | sMask;
}


// Transfer state (dword 3)
static const uint32_t kSitdIOC = 1u << 31;
static const uint32_t kSitdPageSelect = 1u << 30;
static const uint32_t kSitdStatusActive = 1u << 7;
static const uint32_t kSitdStatusERR = 1u << 6;
static const uint32_t kSitdStatusBuffer = 1u << 5;
static const uint32_t kSitdStatusBabble = 1u << 4;
static const uint32_t kSitdStatusTransaction = 1u << 3;
static const uint32_t kSitdStatusMissed = 1u << 2;
static const uint32_t kSitdStatusSplitState = 1u << 1;
static const uint32_t kSitdStatusErrors = kSitdStatusERR | kSitdStatusBuffer
	| kSitdStatusBabble | kSitdStatusTransaction | kSitdStatusMissed;

inline uint32_t
sitd_transfer(uint32_t length, bool interruptOnComplete)
{
	return (interruptOnComplete ? kSitdIOC : 0) | ((length & 0x3ff) << 16)
		| kSitdStatusActive;
}

inline uint32_t
sitd_remaining(uint32_t transfer)
{
	return (transfer >> 16) & 0x3ff;
}


// Buffer page pointers (dwords 4 and 5)
static const uint32_t kSitdPageMask = 0xfffff000;
static const uint32_t kSitdTPAll = 0u << 3;
static const uint32_t kSitdTPBegin = 1u << 3;


inline uint32_t
split_transactions(uint32_t length)
{
	uint32_t count = (length + kBytesPerMicroframe - 1) / kBytesPerMicroframe;
	return count == 0 ? 1 : count;
}


// OUT: the number of start-splits a packet takes; a zero-length packet still
// takes one.
inline uint32_t
out_tcount(uint32_t length)
{
	return split_transactions(length);
}


// OUT: the transaction position of the first start-split, "all" when one
// start-split carries the packet and "begin" when more follow.
inline uint32_t
out_tp(uint32_t length)
{
	return out_tcount(length) > 1 ? kSitdTPBegin : kSitdTPAll;
}


// Page 0 (with the offset of the packet) and page 1 for a packet at
// physical address bufferPhysical; page 1 is where the packet ends and
// carries TP and T-count for OUT.
inline uint32_t
sitd_buffer0(uint32_t bufferPhysical)
{
	return bufferPhysical;
}


inline uint32_t
sitd_buffer1(uint32_t bufferPhysical, uint32_t length, bool in)
{
	uint32_t page = (bufferPhysical + length) & kSitdPageMask;
	if (in)
		return page;
	return page | out_tp(length) | out_tcount(length);
}


// #pragma mark - masks


// S-mask and C-mask for a packet of up to maxPacketSize bytes whose
// start-split is in microframe startMicroframe. False when they do not fit
// in one frame: an OUT start-split in microframe 7, or IN complete-splits
// past microframe 7 (which would need the back pointer).
inline bool
masks_at(bool in, uint32_t maxPacketSize, uint32_t startMicroframe,
	uint8_t& sMask, uint8_t& cMask)
{
	if (maxPacketSize > kMaxFullSpeedIsoPacket || startMicroframe > 7)
		return false;

	uint32_t transactions = split_transactions(maxPacketSize);
	uint32_t start;
	uint32_t complete;
	if (in) {
		start = 1;
		complete = ((1u << (transactions + 2)) - 1) << 2;
	} else {
		start = (1u << transactions) - 1;
		complete = 0;
	}

	start <<= startMicroframe;
	complete <<= startMicroframe;
	if (start >= (1u << 7) || complete > 0xff)
		return false;

	sMask = (uint8_t)start;
	cMask = (uint8_t)complete;
	return true;
}


// #pragma mark - bus time (USB 2.0 §5.11.3)


inline uint32_t
bit_time(uint32_t bytes)
{
	// bit stuffing: 7 bits on the wire for 6 data bits in the worst case
	return 7 * 8 * bytes / 6;
}


inline uint32_t
full_speed_iso_nanoseconds(bool in, uint32_t bytes)
{
	return (in ? 7268 : 6265) + 1000
		+ 8354u * (31 + 10 * bit_time(bytes)) / 1000;
}


inline uint32_t
high_speed_iso_nanoseconds(uint32_t bytes)
{
	return (38 * 8 * 2083 + 2083 * (3 + bit_time(bytes))) / 1000 + 5;
}


inline uint16_t
microseconds(uint32_t nanoseconds)
{
	return (uint16_t)((nanoseconds + 999) / 1000);
}


// #pragma mark - bandwidth budget


struct budget_entry {
	uint32_t	tt;					// which TT (0: entry unused)
	uint16_t	period;				// frames, a power of two
	uint16_t	phase;				// first frame, below min(period, kBudgetFrames)
	uint8_t		startMicroframe;
	uint8_t		sMask;				// absolute microframes of the frame
	uint8_t		cMask;
	uint16_t	ttMicroseconds;		// full-speed time incl. think time
	uint16_t	hsStartMicroseconds;	// per start-split microframe
	uint16_t	hsCompleteMicroseconds;	// per complete-split microframe
};


inline uint32_t
budget_period(const budget_entry& entry)
{
	return entry.period < kBudgetFrames ? entry.period : kBudgetFrames;
}


// The stream's bandwidth for an endpoint: masks and phase are set by
// find_slot().
inline bool
budget_for(uint32_t tt, bool in, uint32_t maxPacketSize, uint32_t period,
	budget_entry& entry)
{
	if (tt == 0 || period == 0 || (period & (period - 1)) != 0
		|| maxPacketSize > kMaxFullSpeedIsoPacket) {
		return false;
	}

	entry.tt = tt;
	entry.period = (uint16_t)period;
	entry.phase = 0;
	entry.startMicroframe = 0;
	entry.sMask = 0;
	entry.cMask = 0;
	entry.ttMicroseconds = microseconds(kTTThinkTimeNanoseconds
		+ full_speed_iso_nanoseconds(in, maxPacketSize));
	if (in) {
		// the start-split carries no data, the complete-splits do
		entry.hsStartMicroseconds = microseconds(high_speed_iso_nanoseconds(1));
		entry.hsCompleteMicroseconds
			= microseconds(high_speed_iso_nanoseconds(maxPacketSize));
	} else {
		entry.hsStartMicroseconds
			= microseconds(high_speed_iso_nanoseconds(maxPacketSize));
		entry.hsCompleteMicroseconds = 0;
	}
	return true;
}


// The full-speed time of one TT per frame and start microframe, carrying
// what does not fit in a microframe into the next ones (as the TT simply
// runs its transactions back to back). reserveMicroframe0 books the
// microframe-0 start-splits of the full-/low-speed interrupt queue heads,
// which this driver puts there without booking them.
inline void
tt_budget(const budget_entry* entries, size_t count, uint32_t tt,
	uint16_t reserveMicroframe0, uint16_t table[kBudgetFrames][8],
	uint16_t frameTotal[kBudgetFrames])
{
	for (uint32_t frame = 0; frame < kBudgetFrames; frame++) {
		for (uint32_t microframe = 0; microframe < 8; microframe++)
			table[frame][microframe] = 0;
		table[frame][0] = reserveMicroframe0;
		frameTotal[frame] = reserveMicroframe0;
	}

	for (size_t i = 0; i < count; i++) {
		const budget_entry& entry = entries[i];
		if (entry.tt != tt)
			continue;

		uint32_t period = budget_period(entry);
		for (uint32_t frame = entry.phase; frame < kBudgetFrames;
				frame += period) {
			frameTotal[frame] += entry.ttMicroseconds;
			uint32_t time = entry.ttMicroseconds;
			for (uint32_t microframe = entry.startMicroframe; microframe < 8;
					microframe++) {
				time += table[frame][microframe];
				if (time <= 125) {
					table[frame][microframe] = (uint16_t)time;
					break;
				}
				table[frame][microframe] = 125;
				time -= 125;
			}
		}
	}
}


// Whether the candidate's full-speed transaction fits on the TT in every
// frame of its phase (Linux tt_available()).
inline bool
tt_fits(const uint16_t table[kBudgetFrames][8],
	const uint16_t frameTotal[kBudgetFrames], const budget_entry& candidate)
{
	uint32_t period = budget_period(candidate);
	uint32_t start = candidate.startMicroframe;
	uint32_t time = candidate.ttMicroseconds;
	if (start >= 7)
		return false;

	for (uint32_t frame = candidate.phase; frame < kBudgetFrames;
			frame += period) {
		if (frameTotal[frame] + time > kTTFrameMicroseconds)
			return false;

		uint32_t microframes[8];
		for (uint32_t i = 0; i < 8; i++)
			microframes[i] = table[frame][i];

		if (kTTMicroframeMicroseconds[start] <= microframes[start])
			return false;

		// A transaction longer than a microframe must not delay the ones
		// already booked: the microframes it fills have to be empty.
		if (time > 125) {
			for (uint32_t i = start; i < start + time / 125 && i < 8; i++) {
				if (microframes[i] > 0)
					return false;
			}
		}

		microframes[start] += time;
		for (uint32_t i = 0; i < 7; i++) {
			if (microframes[i] > kTTMicroframeMicroseconds[i]) {
				microframes[i + 1]
					+= microframes[i] - kTTMicroframeMicroseconds[i];
				microframes[i] = kTTMicroframeMicroseconds[i];
			}
		}
		if (microframes[7] > kTTMicroframeMicroseconds[7])
			return false;
	}

	return true;
}


// High-speed time the split streams take per frame and microframe.
inline void
hs_budget(const budget_entry* entries, size_t count,
	uint16_t table[kBudgetFrames][8])
{
	for (uint32_t frame = 0; frame < kBudgetFrames; frame++) {
		for (uint32_t microframe = 0; microframe < 8; microframe++)
			table[frame][microframe] = 0;
	}

	for (size_t i = 0; i < count; i++) {
		const budget_entry& entry = entries[i];
		if (entry.tt == 0)
			continue;

		uint32_t period = budget_period(entry);
		for (uint32_t frame = entry.phase; frame < kBudgetFrames;
				frame += period) {
			for (uint32_t microframe = 0; microframe < 8; microframe++) {
				if ((entry.sMask & (1u << microframe)) != 0)
					table[frame][microframe] += entry.hsStartMicroseconds;
				if ((entry.cMask & (1u << microframe)) != 0)
					table[frame][microframe] += entry.hsCompleteMicroseconds;
			}
		}
	}
}


inline bool
hs_fits(const uint16_t table[kBudgetFrames][8], const budget_entry& candidate)
{
	uint32_t period = budget_period(candidate);
	for (uint32_t frame = candidate.phase; frame < kBudgetFrames;
			frame += period) {
		for (uint32_t microframe = 0; microframe < 8; microframe++) {
			uint32_t time = table[frame][microframe];
			if ((candidate.sMask & (1u << microframe)) != 0)
				time += candidate.hsStartMicroseconds;
			if ((candidate.cMask & (1u << microframe)) != 0)
				time += candidate.hsCompleteMicroseconds;
			if (time > kHSMicroframeMicroseconds)
				return false;
		}
	}

	return true;
}


// Find the first phase and start microframe where the candidate (from
// budget_for()) fits next to the booked entries, and set its masks.
inline bool
find_slot(const budget_entry* entries, size_t count,
	uint16_t reserveMicroframe0, bool in, uint32_t maxPacketSize,
	budget_entry& candidate)
{
	uint16_t ttTable[kBudgetFrames][8];
	uint16_t ttTotal[kBudgetFrames];
	uint16_t hsTable[kBudgetFrames][8];
	tt_budget(entries, count, candidate.tt, reserveMicroframe0, ttTable,
		ttTotal);
	hs_budget(entries, count, hsTable);

	uint32_t period = budget_period(candidate);
	for (uint32_t phase = 0; phase < period; phase++) {
		for (uint32_t start = 0; start < 7; start++) {
			uint8_t sMask;
			uint8_t cMask;
			if (!masks_at(in, maxPacketSize, start, sMask, cMask))
				continue;

			candidate.phase = (uint16_t)phase;
			candidate.startMicroframe = (uint8_t)start;
			candidate.sMask = sMask;
			candidate.cMask = cMask;
			if (tt_fits(ttTable, ttTotal, candidate)
				&& hs_fits(hsTable, candidate)) {
				return true;
			}
		}
	}

	candidate.sMask = candidate.cMask = 0;
	return false;
}


}	// namespace ehci_split


#endif	// EHCI_SPLIT_ISO_H
