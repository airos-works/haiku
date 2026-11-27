/*
 * Copyright 2010, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SYSTEM_USER_MUTEX_DEFS_H
#define _SYSTEM_USER_MUTEX_DEFS_H


// flags passed to _kern_mutex_{un}block
// (same uint32 also used for B_TIMEOUT, etc.)
#define B_USER_MUTEX_SHARED			0x40000000
	// Mutex is in shared memory.
#define B_USER_MUTEX_UNBLOCK_ALL	0x80000000
	// All threads currently waiting on the mutex will be unblocked. The mutex
	// state will be locked.
#define B_USER_MUTEX_UNBLOCK_RELEASED	0x20000000
	// The caller has released the mutex: one waiter is woken to compete for
	// it with running threads instead of being handed the mutex (which would
	// separate every two critical sections by a wake-up). Without the flag
	// the waiter is handed the mutex (condition variables rely on that).


// mutex value flags
#define B_USER_MUTEX_LOCKED		0x01
#define B_USER_MUTEX_WAITING	0x02
#define B_USER_MUTEX_DISABLED	0x04


#endif	/* _SYSTEM_USER_MUTEX_DEFS_H */
