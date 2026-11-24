/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_JOB_WAIT_H
#define MALI_CSF_JOB_WAIT_H

#include "CsfRun.h"

namespace MaliCSF {

/*!	How the hardware worker waits for a job to complete (#182).

	Off (the default) it looks every 100 us, through WaitFirmware(), as it
	always has. On ("job_interrupt_wait true" in the mali_csf driver
	settings) it sleeps until the job interrupt wakes it. The interrupt has
	to earn that: until it has ended kJobWaitTrustAfter sleeps in a row with
	the job complete, and again after a sleep timed out on a job that was
	then found complete, each sleep is bounded by the old 100 us, so a
	missing interrupt costs nothing over the poll. Once trusted, a sleep is
	bounded by kJobWaitSafetyMicros, which is what one lost interrupt costs
	at most; it also takes the trust away. After an interrupt of this wait
	has come and the job was still not complete, the rest of the wait goes
	in 100 us steps again: the interrupt it waited for may have been that
	one.

	The IO supplies JobInterrupts(), a count the job interrupt handler
	raises, and WaitJobInterrupt(seen, micros): sleep at most \a micros
	unless the count differs from \a seen; true if the interrupt woke it.
	Neither is called while the wait is off. */
static const int64_t kJobWaitPollMicros = 100;
static const int64_t kJobWaitSafetyMicros = 5000;
static const uint32_t kJobWaitTrustAfter = 4;

struct JobWaitStats {
	uint64_t notified;		// sleeps the interrupt ended
	uint64_t timedOut;		// sleeps that ran to their bound
	uint64_t missed;		// trusted sleeps that timed out on a complete job
	uint64_t trusted;		// times the interrupt earned trust
};

class JobWaitPolicy {
public:
	void SetEnabled(bool enabled)
	{
		fEnabled = enabled;
		fTrusted = false;
		fStreak = 0;
	}
	bool Enabled() const { return fEnabled; }
	bool Trusted() const { return fTrusted; }
	const JobWaitStats& Stats() const { return fStats; }

	//! How long the next sleep may last.
	int64_t SliceMicros(bool interruptCame) const
	{
		return fTrusted && !interruptCame
			? kJobWaitSafetyMicros : kJobWaitPollMicros;
	}

	/*!	One sleep ended; \a done: the job was complete when it was looked
		at; \a interruptCame: an interrupt of this wait had come by then. */
	void Record(bool notified, bool done, bool longSleep, bool interruptCame)
	{
		if (notified) {
			fStats.notified++;
			if (done && !fTrusted && ++fStreak >= kJobWaitTrustAfter) {
				fTrusted = true;
				fStats.trusted++;
			}
			return;
		}
		fStats.timedOut++;
		if (!done)
			return;
		// The job completed without waking the waiter. A trusted sleep is
		// long enough that it was not a race: the interrupt, or its
		// wake-up, is missing.
		if (longSleep) {
			fStats.missed++;
			fTrusted = false;
			fStreak = 0;
			return;
		}
		// A short sleep may just have run out as the interrupt came.
		if (!interruptCame)
			fStreak = 0;
	}

private:
	bool fEnabled = false;
	bool fTrusted = false;
	uint32_t fStreak = 0;
	JobWaitStats fStats = {};
};


/*!	Waits for \a condition, as WaitFirmware() does, at most \a timeout
	microseconds. With the policy off, it is WaitFirmware() itself. */
template<typename IO, typename Condition>
bool
WaitJob(IO& io, JobWaitPolicy& policy, int64_t timeout, Condition condition)
{
	if (!policy.Enabled())
		return WaitFirmware(io, timeout, condition);

	int64_t start = io.Now();
	if (start < 0)
		return false;
	uint32_t first = io.JobInterrupts();
	bool slept = false, notified = false, longSleep = false, came = false;
	// A frozen clock must not keep the worker here for good; WaitFirmware()
	// bounds its loop for the same reason. Interrupts may end sleeps early,
	// hence the margin.
	uint64_t rounds = 2 * (uint64_t(timeout) / kJobWaitPollMicros + 2);
	for (uint64_t round = 0; round < rounds; round++) {
		// Read the count before looking: an interrupt that comes while
		// condition() runs then ends the sleep at once.
		uint32_t seen = io.JobInterrupts();
		bool done = condition();
		if (slept)
			policy.Record(notified, done, longSleep, came || seen != first);
		if (done)
			return true;
		int64_t now = io.Now();
		if (now < start || now - start > timeout)
			return false;
		came = seen != first;
		int64_t slice = policy.SliceMicros(came);
		int64_t left = timeout - (now - start) + 1;
		if (slice > left)
			slice = left;
		longSleep = slice > kJobWaitPollMicros;
		notified = io.WaitJobInterrupt(seen, slice);
		slept = true;
	}
	return false;
}

} // namespace MaliCSF

#endif
