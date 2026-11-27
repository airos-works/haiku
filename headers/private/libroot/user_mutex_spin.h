/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LIBROOT_USER_MUTEX_SPIN_H
#define _LIBROOT_USER_MUTEX_SPIN_H


#include <OS.h>

#include <user_mutex_defs.h>


#ifdef __cplusplus
extern "C" {
#endif

extern int32 __gCPUCount;

#ifdef __cplusplus
}
#endif


// How long a locker polls a mutex held by another thread before it goes to
// sleep in the kernel, in microseconds. Sleeping and being woken up costs
// several microseconds, more than the typical critical section, which is over
// while the locker polls, if the holder is running on another CPU. A time, not
// a number of polls: the pause instruction takes from a few to over a hundred
// cycles depending on the processor.
#ifndef USER_MUTEX_SPIN_TIME
#	define USER_MUTEX_SPIN_TIME	4
#endif


static inline void
__user_mutex_cpu_pause()
{
#if defined(__x86_64__) || defined(__i386__)
	__builtin_ia32_pause();
#elif defined(__aarch64__)
	__asm__ __volatile__("yield" ::: "memory");
#else
	__asm__ __volatile__("" ::: "memory");
#endif
}


/*!	Takes the user mutex \a lock if it is not locked, keeping the other flags
	(B_USER_MUTEX_WAITING) as they are. Unlike a test-and-set against 0 this
	also takes a mutex that has been released while threads sleep on it: the
	kernel wakes one of them to compete for it (B_USER_MUTEX_UNBLOCK_RELEASED)
	and the woken thread sleeps again if it is taken.
	When \a spin is true, the mutex is held by another thread and there is more
	than one CPU, the function polls it for a while first.
	Returns whether it took the mutex.
*/
static inline bool
__user_mutex_try_lock(int32* lock, bool spin)
{
	bigtime_t until = 0;
	int32 polls = 0;
	int32 value = atomic_get(lock);
	while (true) {
		if ((value & B_USER_MUTEX_LOCKED) == 0) {
			const int32 oldValue = atomic_test_and_set(lock,
				value | B_USER_MUTEX_LOCKED, value);
			if (oldValue == value)
				return true;
			value = oldValue;
			continue;
		}

		if (!spin || __gCPUCount < 2)
			return false;

		// look at the clock every few polls only
		if ((polls++ & 15) == 0) {
			const bigtime_t now = system_time();
			if (until == 0)
				until = now + USER_MUTEX_SPIN_TIME;
			else if (now >= until)
				return false;
		}

		__user_mutex_cpu_pause();
		value = atomic_get(lock);
	}
}


#endif	// _LIBROOT_USER_MUTEX_SPIN_H
