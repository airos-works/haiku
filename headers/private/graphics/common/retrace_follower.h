/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RETRACE_FOLLOWER_H
#define _RETRACE_FOLLOWER_H


/*!	For a display driver that has no vertical blank interrupt it can use, but
	can read a frame counter of the display hardware, one that advances once
	a frame at the blank: a kernel thread that follows the counter and
	releases the retrace semaphores when it advances.

	It sleeps for most of a frame and looks closely (every 200 µs) only
	around the time the counter is due. A semaphore is released only when
	the advance was seen within a millisecond of it happening, so a wait
	never ends in the middle of a frame; when the thread was late, it lets
	that blank go and the waiters get the next one. Nobody waiting for two
	seconds puts the thread to sleep; it looks for waiters every 5 ms then,
	and finds the counter's rhythm again before it releases anything.

	While the display shows nothing (FrameCount() negative, or the counter
	stands still), the semaphores are released at the last known rate, so
	that waiting for a blank never waits long.
*/


#include <KernelExport.h>
#include <OS.h>

#include "retrace_semaphores.h"


class RetraceFollower {
public:
	RetraceFollower(RetraceSemaphores& semaphores, uint32 counterMask)
		:
		fSemaphores(semaphores),
		fCounterMask(counterMask),
		fThread(-1),
		fStopping(0)
	{
	}

	virtual ~RetraceFollower()
	{
		Stop();
	}

	status_t Start(const char* name)
	{
		if (fThread >= 0)
			return B_OK;
		atomic_set(&fStopping, 0);
		fThread = spawn_kernel_thread(&_Thread, name,
			B_REAL_TIME_DISPLAY_PRIORITY, this);
		if (fThread < 0)
			return fThread;
		return resume_thread(fThread);
	}

	void Stop()
	{
		if (fThread < 0)
			return;
		atomic_set(&fStopping, 1);
		status_t result;
		wait_for_thread(fThread, &result);
		fThread = -1;
	}

protected:
	// The display's frame counter, or a negative value while it shows
	// nothing. Called from the follower's thread only.
	virtual int64 FrameCount() = 0;

private:
	static status_t _Thread(void* self)
	{
		((RetraceFollower*)self)->_Run();
		return B_OK;
	}

	void _Run()
	{
		const bigtime_t kPoll = 200;
		bigtime_t period = 16667;
		int64 count = -1;
		bigtime_t lastLook = 0;
		bigtime_t lastChange = 0;
		bigtime_t lastWaiter = system_time();
		bool asleep = false;

		while (atomic_get(&fStopping) == 0) {
			if (asleep) {
				if (!fSemaphores.HasWaiters()) {
					snooze(5000);
					continue;
				}
				asleep = false;
				count = -1;
				lastWaiter = system_time();
			}

			int64 current = FrameCount();
			bigtime_t now = system_time();
			if (current < 0) {
				// nothing is shown; waits end anyway
				fSemaphores.Release();
				count = -1;
				snooze(period);
				continue;
			}

			if (count < 0) {
				// the counter's rhythm, before anything is released
				count = current;
				lastLook = now;
				lastChange = 0;
				snooze(kPoll);
				continue;
			}

			if (current != count) {
				bool precise = now - lastLook <= 1000;
				bool oneFrame = ((current - count) & fCounterMask) == 1;
				if (precise && oneFrame && lastChange != 0) {
					bigtime_t measured = now - lastChange;
					if (measured >= 4000 && measured <= 100000)
						period = (period * 7 + measured) / 8;
				}
				count = current;
				lastLook = now;
				if (!precise) {
					// looked too late to tell when it happened
					lastChange = 0;
					snooze(kPoll);
					continue;
				}
				lastChange = now;

				if (fSemaphores.HasWaiters())
					lastWaiter = now;
				fSemaphores.Release();
				if (now - lastWaiter > 2000000) {
					asleep = true;
					continue;
				}

				snooze_until(lastChange + period - 700, B_SYSTEM_TIMEBASE);
				continue;
			}

			lastLook = now;
			if (lastChange != 0 && now - lastChange > 3 * period) {
				// the counter stands still: the display went dark
				fSemaphores.Release();
				snooze(period);
				continue;
			}
			snooze(kPoll);
		}
	}

private:
	RetraceSemaphores&	fSemaphores;
	uint32				fCounterMask;
	thread_id			fThread;
	int32				fStopping;
};


#endif	/* _RETRACE_FOLLOWER_H */
