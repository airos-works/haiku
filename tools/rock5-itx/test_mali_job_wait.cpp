/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
// The mali_csf job wait (#182, CsfJobWait.h) against a fake GPU thread that
// completes jobs and raises (or loses) the job interrupt, in real time.
#include <assert.h>
#include <stdio.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "CsfJobWait.h"

using namespace MaliCSF;
using Clock = std::chrono::steady_clock;

enum InterruptMode { kInterruptWorks, kInterruptDead, kInterruptSilent, kInterruptEarly };

struct FakeIO {
	Clock::time_point origin = Clock::now();
	bool frozen = false;
	std::mutex lock;
	std::condition_variable changed;
	uint32_t count = 0;				// what the handler counts
	uint64_t notifications = 0;		// what wakes sleepers
	std::atomic<bool> done{false};
	std::atomic<unsigned> pauses{0}, sleeps{0};

	int64_t Now()
	{
		if (frozen) return 1000;
		return std::chrono::duration_cast<std::chrono::microseconds>(
			Clock::now() - origin).count();
	}
	void PauseReset() { pauses++; std::this_thread::sleep_for(std::chrono::microseconds(100)); }
	uint32_t JobInterrupts() { std::lock_guard<std::mutex> guard(lock); return count; }
	bool WaitJobInterrupt(uint32_t seen, int64_t micros)
	{
		sleeps++;
		std::unique_lock<std::mutex> guard(lock);
		uint64_t added = notifications;		// ConditionVariable::Add()
		if (count != seen) return true;
		return changed.wait_for(guard, std::chrono::microseconds(micros),
			[&] { return notifications != added; });
	}
	void Interrupt(bool notify)
	{
		{
			std::lock_guard<std::mutex> guard(lock);
			count++;
			if (notify) notifications++;
		}
		changed.notify_all();
	}
};

struct JobResult { bool completed; int64_t lateMicros; unsigned sleeps, pauses; };

// One job of \a micros; the GPU thread completes it and raises its
// interrupt as \a mode says. Returns how long after completion the wait saw it.
static JobResult
RunJob(FakeIO& io, JobWaitPolicy& policy, int64_t micros, InterruptMode mode,
	int64_t timeout = 1000000)
{
	io.done = false;
	unsigned sleeps = io.sleeps, pauses = io.pauses;
	std::atomic<int64_t> completedAt{-1};
	std::thread gpu([&] {
		std::this_thread::sleep_for(std::chrono::microseconds(micros));
		if (mode == kInterruptEarly) {
			// The interrupt comes before the completion shows.
			io.Interrupt(true);
			std::this_thread::sleep_for(std::chrono::microseconds(1000));
		}
		completedAt = io.Now();
		io.done = true;
		if (mode == kInterruptWorks) io.Interrupt(true);
		else if (mode == kInterruptSilent) io.Interrupt(false);
	});
	bool completed = WaitJob(io, policy, timeout, [&] { return io.done.load(); });
	int64_t seen = io.Now();
	gpu.join();
	return {completed, completed ? seen - completedAt : -1,
		io.sleeps - sleeps, io.pauses - pauses};
}

int
main()
{
	setvbuf(stdout, NULL, _IONBF, 0);
	// A loaded build host stretches sleeps; the bounds here allow for it.
	const int64_t slack = 20000;

	{	// Off: the old poll, and nothing of the interrupt path.
		FakeIO io;
		JobWaitPolicy policy;
		JobResult result = RunJob(io, policy, 3000, kInterruptWorks);
		printf("off: late %lld us, %u pauses, %u sleeps\n",
			(long long)result.lateMicros, result.pauses, result.sleeps);
		assert(result.completed && result.sleeps == 0 && result.pauses >= 5);
		assert(policy.Stats().notified + policy.Stats().timedOut == 0);
	}

	FakeIO io;
	JobWaitPolicy policy;
	policy.SetEnabled(true);
	// Untrusted, sleeps are 100 us at most: the poll's interval. Trust takes
	// four wake-ups in a row; on a busy host a sleep can run out just before
	// the fake interrupt comes, which starts the count over.
	for (unsigned job = 0; job < 40 && !policy.Trusted(); job++) {
		JobResult result = RunJob(io, policy, 3000, kInterruptWorks);
		printf("untrusted job %u: late %lld us, %u sleeps\n", job,
			(long long)result.lateMicros, result.sleeps);
		assert(result.completed && result.pauses == 0 && result.sleeps >= 5);
		assert(result.lateMicros < slack);
	}
	assert(policy.Trusted() && policy.Stats().trusted == 1);

	// Trusted: one sleep a job, ended by the interrupt.
	unsigned oneSleep = 0;
	for (unsigned job = 0; job < 20; job++) {
		JobResult result = RunJob(io, policy, 3000, kInterruptWorks);
		assert(result.completed && result.lateMicros < slack && result.sleeps <= 2);
		oneSleep += result.sleeps == 1;
	}
	printf("trusted: %u of 20 jobs took one sleep\n", oneSleep);
	assert(oneSleep >= 15 && policy.Trusted() && policy.Stats().missed == 0);

	// A long job: trusted sleeps end at their bound, and that is no miss.
	JobResult longJob = RunJob(io, policy, 12000, kInterruptWorks);
	printf("12 ms job: %u sleeps, late %lld us\n", longJob.sleeps,
		(long long)longJob.lateMicros);
	assert(longJob.completed && longJob.sleeps >= 2 && longJob.sleeps <= 6);
	assert(policy.Trusted() && policy.Stats().missed == 0);

	// The interrupt comes before the completion is visible: the rest of the
	// wait polls, so it costs no more than the poll would.
	JobResult early = RunJob(io, policy, 2000, kInterruptEarly);
	printf("early interrupt: late %lld us, %u sleeps\n",
		(long long)early.lateMicros, early.sleeps);
	assert(early.completed && early.lateMicros < slack && early.sleeps >= 3);
	assert(policy.Trusted() && policy.Stats().missed == 0);

	// The interrupt stops: the first job waits out one safety bound, the
	// trust goes, and the next jobs are polled again.
	JobResult dead = RunJob(io, policy, 3000, kInterruptDead);
	printf("dead interrupt: late %lld us, %u sleeps\n",
		(long long)dead.lateMicros, dead.sleeps);
	assert(dead.completed && dead.lateMicros <= kJobWaitSafetyMicros + slack);
	assert(!policy.Trusted() && policy.Stats().missed == 1);
	for (unsigned job = 0; job < 6; job++) {
		JobResult result = RunJob(io, policy, 3000, kInterruptDead);
		assert(result.completed && result.lateMicros < slack && result.sleeps >= 5);
	}
	assert(!policy.Trusted() && policy.Stats().missed == 1);

	// Counted but never notified (a lost wake-up): the same.
	for (unsigned job = 0; job < 6; job++) {
		JobResult result = RunJob(io, policy, 3000, kInterruptSilent);
		assert(result.completed && result.lateMicros < slack);
	}
	assert(!policy.Trusted() && policy.Stats().missed == 1);

	// It comes back: trust is earned again.
	for (unsigned job = 0; job < 40 && !policy.Trusted(); job++)
		assert(RunJob(io, policy, 2000, kInterruptWorks).completed);
	assert(policy.Trusted() && policy.Stats().trusted == 2);

	// A job that never completes fails at the timeout, as before.
	io.done = false;
	int64_t start = io.Now();
	assert(!WaitJob(io, policy, 20000, [] { return false; }));
	int64_t spent = io.Now() - start;
	printf("timeout 20000 us: gave up after %lld us\n", (long long)spent);
	assert(spent >= 20000 && spent < 20000 + kJobWaitSafetyMicros + slack);

	// A clock that stands still cannot keep the wait for good.
	io.frozen = true;
	unsigned sleeps = io.sleeps;
	assert(!WaitJob(io, policy, 1000, [] { return false; }));
	printf("frozen clock: gave up after %u sleeps\n", io.sleeps - sleeps);
	assert(io.sleeps - sleeps <= 2 * (1000 / kJobWaitPollMicros + 2));

	const JobWaitStats& stats = policy.Stats();
	printf("totals: %llu woken, %llu timed out, %llu missed, trusted %llu times\n",
		(unsigned long long)stats.notified, (unsigned long long)stats.timedOut,
		(unsigned long long)stats.missed, (unsigned long long)stats.trusted);
	puts("MALI_CSF_JOB_WAIT_TEST_PASS");
	return 0;
}
