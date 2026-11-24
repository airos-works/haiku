/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_VPU_COMPLETION_H
#define RK3588_VPU_COMPLETION_H

#include <stdint.h>

namespace RK3588Vpu {

/*!	Waiting for a codec block's completion interrupt instead of polling its
	status word every 50 us (#182). Off unless "completion_interrupts true"
	is in the rk3588_vpu driver settings; then the polled loops in driver.cpp
	are not used for a block whose handler is installed.

	The interrupt never decides more than the poll did: the handler only
	acts on the bits the poll takes as the end of a job (doneMask), records
	the status word, acknowledges it as the job's own code does afterwards,
	and wakes the waiter, once per job. It touches the block only while a
	job is armed: between jobs the domain may be powered off, and reading it
	then could stall the bus. Calls it declines (no job, no end bit, or the
	end already taken) go back to the kernel as unhandled, which switches a
	vector off after 99% of 10,000 calls went unhandled; more than
	kCompletionStormLimit of them in one job mark the block broken, and it
	is polled from then on. No logging in the
	handler (dprintf there stalled the ROCK 5 once: the GICv3 ITS trace).

	The waiter sleeps on the interrupt, but only trusts it once it has
	ended kCompletionTrustAfter sleeps in a row with the job done: until
	then (and again after a trusted sleep timed out on a finished job) each
	sleep lasts at most the old 50 us, so a missing interrupt costs nothing
	over the poll; trusted, at most kCompletionSafetyMicros. After an
	interrupt of this job came without the end bits being visible, the rest
	of the wait goes in 50 us steps. The deadline is the poll's. */

enum CompletionBlock {
	kCompletionRkvdec0,
	kCompletionAv1,
	kCompletionVdpu2,
	kCompletionRkvenc0,
	kCompletionBlocks
};

// The end-of-job bits each polled loop in driver.cpp waits for.
static const uint32_t kRkvdecTerminal = (1u << 2) | (1u << 4) | (1u << 5)
	| (1u << 9);
static const uint32_t kAv1Terminal = (1u << 11) | (1u << 12) | (1u << 13)
	| (1u << 14) | (1u << 16) | (1u << 18) | (1u << 21) | (1u << 23);
static const uint32_t kVdpu2Done = 1u << 0;
static const uint32_t kRkvencTerminal = (1u << 0) | 0x1f0;

struct CompletionSpec {
	const char* name;
	uint32_t vector;		// GIC SPI + 32
	uint32_t statusIndex;	// word index in the block's job registers
	uint32_t doneMask;
	uint32_t ackIndex;
	bool ackWithStatus;		// write the status back (else zero)
};

// Interrupt lines from Rockchip's rk3588s.dtsi (BSP kernel develop-5.10):
// irq_rkvdec0 SPI 95, irq_av1d SPI 108, irq_vdpu SPI 119 (also in mainline
// and in the board's FDT for VPU121), irq_rkvenc0 SPI 101. The EDK2 FDT has
// no RKVDEC or RKVENC node; these are level-high SPIs.
static const CompletionSpec kCompletionSpecs[kCompletionBlocks] = {
	// registers at 0xfdc38100; mpp_rkvdec2.c clears 0x380 (word 224)
	{"RKVDEC0", 95 + 32, 224, kRkvdecTerminal, 224, false},
	// VPU981 swreg 1, as rk3588_vpu981_irq() clears it
	{"AV1", 108 + 32, 1, kAv1Terminal, 1, false},
	// VPU121's decoder at 0xfdb50400, swreg 55 (rockchip_vpu2_vdpu_irq())
	{"VDPU2", 119 + 32, 55, kVdpu2Done, 55, false},
	// VEPU580 interrupt status 0x2c, cleared through 0x28 as RunRkvencJob()
	{"RKVENC0", 101 + 32, 11, kRkvencTerminal, 10, true},
};

static const int64_t kCompletionPollMicros = 50;
static const int64_t kCompletionSafetyMicros = 5000;
static const uint32_t kCompletionTrustAfter = 4;
static const uint32_t kCompletionStormLimit = 64;

struct CompletionState {
	// under the block's interrupt lock
	bool installed;
	bool armed;
	bool broken;
	uint32_t calls;			// calls declined in this job
	uint32_t status;		// end-of-job status the handler acknowledged
	uint32_t sequence;		// interrupts acted on
	uint64_t interrupts;
	uint64_t strays;		// calls while no job was armed
	// the waiter's, under the block's job lock
	bool trusted;
	uint32_t streak;
	uint64_t jobs;
	uint64_t notified;
	uint64_t timedOut;
	uint64_t missed;
	uint64_t trusts;
};

enum CompletionAction { kCompletionIgnored, kCompletionHandled };


/*!	The interrupt side, with the block's interrupt lock held. Registers
	supplies Read(index) and Write(index, value) on the job registers. */
template<class Registers>
CompletionAction
HandleCompletionInterrupt(CompletionState& state, const CompletionSpec& spec,
	Registers& registers)
{
	if (!state.armed) {
		state.strays++;
		return kCompletionIgnored;
	}
	if (state.broken)
		return kCompletionIgnored;
	// One end per job: a line still up after it was acknowledged, or up
	// without an end bit, is declined, so that the kernel switches it off.
	if ((state.status & spec.doneMask) != 0) {
		if (++state.calls > kCompletionStormLimit)
			state.broken = true;
		return kCompletionIgnored;
	}
	uint32_t status = registers.Read(spec.statusIndex);
	if ((status & spec.doneMask) == 0) {
		if (++state.calls > kCompletionStormLimit)
			state.broken = true;
		return kCompletionIgnored;
	}
	registers.Write(spec.ackIndex, spec.ackWithStatus ? status : 0);
	state.status |= status;
	state.sequence++;
	state.interrupts++;
	return kCompletionHandled;
}


//! Before the job's start word; false: poll this job.
inline bool
ArmCompletion(CompletionState& state)
{
	if (!state.installed || state.broken)
		return false;
	state.armed = true;
	state.calls = 0;
	state.status = 0;
	return true;
}


//! After the job's own acknowledgement (and reset, if any).
inline void
DisarmCompletion(CompletionState& state)
{
	state.armed = false;
	state.status = 0;
}


inline void
RecordCompletionSleep(CompletionState& state, bool notified, bool done,
	bool longSleep, bool interruptCame)
{
	if (notified) {
		state.notified++;
		if (done && !state.trusted
			&& ++state.streak >= kCompletionTrustAfter) {
			state.trusted = true;
			state.trusts++;
		}
		return;
	}
	state.timedOut++;
	if (!done)
		return;
	// The job ended without waking the waiter. A trusted sleep is long
	// enough that it was not a race: the interrupt, or its wake-up, is
	// missing. A short one may just have run out as the interrupt came.
	if (longSleep) {
		state.missed++;
		state.trusted = false;
		state.streak = 0;
	} else if (!interruptCame)
		state.streak = 0;
}


/*!	Waits for an armed job's end, at most \a timeout microseconds, and
	returns the status word as the polled loop would have read it last.
	Platform supplies Now(), Read(index) (the live word, after a read
	barrier), Snapshot(sequence, status) (under the interrupt lock) and
	Sleep(seen, micros): sleep unless the sequence moved past \a seen, true
	if the interrupt ended it. */
template<class Platform>
uint32_t
WaitCompletion(Platform& platform, CompletionState& state,
	const CompletionSpec& spec, int64_t timeout)
{
	state.jobs++;
	int64_t start = platform.Now();
	uint32_t first, status = 0;
	platform.Snapshot(first, status);
	bool slept = false, notified = false, longSleep = false, came = false;
	uint64_t rounds = 2 * (uint64_t(timeout) / kCompletionPollMicros + 2);
	for (uint64_t round = 0; round < rounds; round++) {
		uint32_t seen, saved;
		platform.Snapshot(seen, saved);
		// The handler may have acknowledged the word already.
		status = (saved & spec.doneMask) != 0
			? saved : platform.Read(spec.statusIndex);
		bool done = (status & spec.doneMask) != 0;
		if (slept)
			RecordCompletionSleep(state, notified, done, longSleep,
				came || seen != first);
		if (done)
			return status;
		int64_t now = platform.Now();
		if (now < start || now - start >= timeout)
			return status;
		came = seen != first;
		int64_t slice = state.trusted && !came
			? kCompletionSafetyMicros : kCompletionPollMicros;
		if (slice > timeout - (now - start))
			slice = timeout - (now - start);
		longSleep = slice > kCompletionPollMicros;
		notified = platform.Sleep(seen, slice);
		slept = true;
	}
	return status;
}

} // namespace RK3588Vpu

#endif
