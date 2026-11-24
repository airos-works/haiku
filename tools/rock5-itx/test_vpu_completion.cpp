/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
// rk3588_vpu's completion interrupt logic (#182, VpuCompletion.h) against a
// fake codec block whose level-triggered line a fake interrupt controller
// thread keeps delivering while it is asserted, in real time.

#include "../../src/add-ons/kernel/drivers/video/rk3588_vpu/VpuCompletion.h"

#include <assert.h>
#include <stdio.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace RK3588Vpu;
using Clock = std::chrono::steady_clock;

enum LineMode {
	kLineWorks,		// asserted while an end bit is set
	kLineDead,		// never asserted
	kLineStuck		// asserted, and the acknowledgement does not clear it
};

struct FakeBlock {
	const CompletionSpec& spec;
	std::mutex lock;				// the interrupt lock (a spinlock)
	std::condition_variable changed;
	uint64_t notifications = 0;
	CompletionState state = {};
	std::atomic<uint32_t> words[4096];
	std::atomic<unsigned> reads{0}, writes{0}, calls{0}, unhandled{0};
	std::atomic<bool> vectorOff{false}, stop{false};
	LineMode mode = kLineWorks;
	Clock::time_point origin = Clock::now();
	std::thread controller;
	std::mutex lineLock;
	std::condition_variable lineChanged;
	uint64_t raised = 0;

	explicit FakeBlock(const CompletionSpec& value) : spec(value)
	{
		for (auto& word : words) word = 0;
		state.installed = true;
		controller = std::thread([this] { Deliver(); });
	}
	~FakeBlock() { stop = true; Raise(); controller.join(); }
	// The block changed its status word: the line may have gone up. The
	// interrupt is taken at once, as on another CPU; the controller thread
	// delivers again for as long as the line stays up.
	void Raise()
	{
		if (!stop && !vectorOff && Asserted()) DeliverOnce();
		{ std::lock_guard<std::mutex> guard(lineLock); raised++; }
		lineChanged.notify_all();
	}
	std::mutex deliverLock;		// one CPU takes the vector at a time
	unsigned row = 0;
	void DeliverOnce()
	{
		std::lock_guard<std::mutex> one(deliverLock);
		if (vectorOff) return;
		calls++;
		bool handled;
		{
			std::lock_guard<std::mutex> guard(lock);
			Registers registers = {*this};
			handled = HandleCompletionInterrupt(state, spec, registers)
				== kCompletionHandled;
		}
		if (handled) {
			{ std::lock_guard<std::mutex> guard(lock); notifications++; }
			changed.notify_all();
			row = 0;
		} else if (++row >= 1000) {
			vectorOff = true;
			unhandled += row;
		}
	}

	// The block's job registers, as the handler sees them.
	struct Registers {
		FakeBlock& block;
		uint32_t Read(uint32_t index) { block.reads++; return block.words[index]; }
		void Write(uint32_t index, uint32_t value)
		{
			block.writes++;
			assert(index == block.spec.ackIndex);
			if (block.mode == kLineStuck) return;
			if (block.spec.ackWithStatus)	// a clear register: write 1 to clear
				block.words[block.spec.statusIndex] &= ~value;
			else
				block.words[index] = value;
		}
	};
	bool Asserted()
	{
		return mode != kLineDead && (words[spec.statusIndex] & spec.doneMask) != 0;
	}
	// The interrupt controller: delivers while the line is asserted; switches
	// the vector off after 1000 unhandled calls in a row, as the kernel does
	// after 99% of 10000.
	void Deliver()
	{
		uint64_t seen = 0;
		while (!stop) {
			if (vectorOff || !Asserted()) {
				std::unique_lock<std::mutex> guard(lineLock);
				lineChanged.wait_for(guard, std::chrono::milliseconds(1),
					[&] { return raised != seen || stop; });
				seen = raised;
				continue;
			}
			DeliverOnce();
		}
	}
	// The waiter's platform.
	int64_t Now()
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(
			Clock::now() - origin).count();
	}
	uint32_t Read(uint32_t index) { return words[index]; }
	void Snapshot(uint32_t& sequence, uint32_t& status)
	{
		std::lock_guard<std::mutex> guard(lock);
		sequence = state.sequence; status = state.status;
	}
	bool Sleep(uint32_t seen, int64_t micros)
	{
		std::unique_lock<std::mutex> guard(lock);
		uint64_t added = notifications;
		if (state.sequence != seen) return true;
		return changed.wait_for(guard, std::chrono::microseconds(micros),
			[&] { return notifications != added; });
	}
	bool Arm() { std::lock_guard<std::mutex> guard(lock); return ArmCompletion(state); }
	void Disarm() { std::lock_guard<std::mutex> guard(lock); DisarmCompletion(state); }
};

struct Job { uint32_t status; int64_t lateMicros; uint64_t sleeps; };

// One job of \a micros finishing with \a endStatus (0: never), waited for
// as driver.cpp does: arm, start, wait, the job's own acknowledgement, disarm.
static Job
RunJob(FakeBlock& block, int64_t micros, uint32_t endStatus,
	int64_t timeout = 500000)
{
	assert(block.Arm());
	uint64_t sleeps = block.state.notified + block.state.timedOut;
	std::atomic<int64_t> finishedAt{-1};
	std::thread hardware([&] {
		std::this_thread::sleep_for(std::chrono::microseconds(micros));
		if (endStatus == 0) return;
		finishedAt = block.Now();
		block.words[block.spec.statusIndex] = endStatus;
		block.Raise();
	});
	uint32_t status = WaitCompletion(block, block.state, block.spec, timeout);
	int64_t seen = block.Now();
	hardware.join();
	// the job's own acknowledgement, as the polled path does it
	if (block.mode != kLineStuck) {
		if (block.spec.ackWithStatus)
			block.words[block.spec.statusIndex] &= ~status;
		else
			block.words[block.spec.statusIndex] = 0;
	}
	block.Disarm();
	return {status, finishedAt >= 0 ? seen - finishedAt : -1,
		block.state.notified + block.state.timedOut - sleeps};
}

int
main()
{
	setvbuf(stdout, NULL, _IONBF, 0);
	const int64_t slack = 20000;	// a loaded build host stretches sleeps

	// Every block: the handler takes exactly the end bits the poll waits for,
	// hands their status to the waiter and acknowledges as the job would.
	for (unsigned which = 0; which < kCompletionBlocks; which++) {
		const CompletionSpec& spec = kCompletionSpecs[which];
		FakeBlock block(spec);
		uint32_t end = spec.doneMask & (0u - spec.doneMask); // lowest end bit
		uint32_t extra = which == kCompletionRkvdec0 ? 1u << 1 : 0;	// IRQ raw
		// Trust needs four wake-ups in a row; on a busy host a sleep can run
		// out just before the fake interrupt is taken, which starts over.
		unsigned jobs = 0;
		while (jobs < 8 || (!block.state.trusted && jobs < 40)) {
			Job result = RunJob(block, 2000, end | extra);
			assert(result.status == (end | extra) && result.lateMicros < slack);
			assert(block.words[spec.statusIndex] == 0);
			jobs++;
		}
		printf("%s: vector %u, %llu interrupts, %llu woken, %llu timed out,"
			" trusted %d\n", spec.name, spec.vector,
			(unsigned long long)block.state.interrupts,
			(unsigned long long)block.state.notified,
			(unsigned long long)block.state.timedOut, block.state.trusted);
		assert(block.state.interrupts == jobs && block.state.trusted);
		assert(block.state.missed == 0 && !block.state.broken);
		assert(block.state.strays == 0);
	}

	FakeBlock block(kCompletionSpecs[kCompletionRkvdec0]);
	const uint32_t ready = 1u << 2;

	// Not armed: the handler neither reads nor writes the block (it may be
	// powered off), and declines.
	{
		std::lock_guard<std::mutex> guard(block.lock);
		FakeBlock::Registers registers = {block};
		unsigned reads = block.reads, writes = block.writes;
		assert(HandleCompletionInterrupt(block.state, block.spec, registers)
			== kCompletionIgnored);
		assert(block.reads == reads && block.writes == writes);
		assert(block.state.strays == 1);
		// Armed, but no end bit: declined, nothing acknowledged.
		assert(ArmCompletion(block.state));
		block.words[224] = 1u << 6;	// buffer empty: not what the poll ends on
		assert(HandleCompletionInterrupt(block.state, block.spec, registers)
			== kCompletionIgnored);
		assert(block.writes == writes && block.words[224] == (1u << 6));
		block.words[224] = 0;
		DisarmCompletion(block.state);
	}

	// Untrusted: sleeps of 50 us at most; then one sleep a job.
	for (unsigned job = 0; job < 40 && !block.state.trusted; job++) {
		Job result = RunJob(block, 3000, ready);
		assert(result.status == ready && result.sleeps >= 5);
	}
	assert(block.state.trusted);
	unsigned single = 0;
	for (unsigned job = 0; job < 20; job++) {
		Job result = RunJob(block, 3000, ready);
		assert(result.status == ready && result.lateMicros < slack);
		single += result.sleeps == 1;
	}
	printf("trusted: %u of 20 jobs took one sleep\n", single);
	assert(single >= 15 && block.state.missed == 0);

	// The line goes dead: one safety bound, the trust goes, polling again.
	block.mode = kLineDead;
	Job dead = RunJob(block, 3000, ready);
	printf("dead line: late %lld us\n", (long long)dead.lateMicros);
	assert(dead.status == ready);
	assert(dead.lateMicros <= kCompletionSafetyMicros + slack);
	assert(!block.state.trusted && block.state.missed == 1);
	for (unsigned job = 0; job < 5; job++) {
		Job result = RunJob(block, 3000, ready);
		assert(result.status == ready && result.lateMicros < slack
			&& result.sleeps >= 5);
	}
	block.mode = kLineWorks;

	// A job that never ends returns at the poll's deadline, without end bits.
	int64_t start = block.Now();
	Job hung = RunJob(block, 0, 0, 20000);
	int64_t spent = block.Now() - start;
	printf("hung job: returned %#x after %lld us\n", hung.status, (long long)spent);
	assert(hung.status == 0 && spent >= 20000
		&& spent < 20000 + kCompletionSafetyMicros + slack);

	// A line the acknowledgement does not quiet: the handler gives up after
	// kCompletionStormLimit calls, the vector goes off, the job still ends by
	// polling, and the block is polled from then on.
	FakeBlock stuck(kCompletionSpecs[kCompletionVdpu2]);
	stuck.mode = kLineStuck;
	Job storm = RunJob(stuck, 2000, kVdpu2Done);
	while (!stuck.vectorOff) std::this_thread::yield();
	printf("stuck line: %u handler calls, broken %d, vector off %d\n",
		(unsigned)stuck.calls, stuck.state.broken, (bool)stuck.vectorOff);
	assert(storm.status == kVdpu2Done && stuck.state.broken);
	assert(stuck.state.interrupts == 1);
	assert(stuck.writes == 1);	// acknowledged once, then left alone
	assert(!stuck.Arm());			// polled from now on

	printf("totals: %llu interrupts, %llu woken, %llu timed out, %llu missed,"
		" %llu stray, trusted %llu times\n",
		(unsigned long long)block.state.interrupts,
		(unsigned long long)block.state.notified,
		(unsigned long long)block.state.timedOut,
		(unsigned long long)block.state.missed,
		(unsigned long long)block.state.strays,
		(unsigned long long)block.state.trusts);
	puts("RK3588_VPU_COMPLETION_TEST_PASS");
	return 0;
}
