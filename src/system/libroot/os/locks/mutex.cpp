/*
 * Copyright 2009, Michael Lotz, mmlr@mlotz.ch.
 * Copyright 2008-2010, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2002-2009, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


#include <locks.h>

#include <stdlib.h>
#include <string.h>

#include <syscalls.h>
#include <user_mutex_defs.h>
#include <user_mutex_spin.h>


// #pragma mark - mutex


void
__mutex_init(mutex *lock, const char *name)
{
	lock->name = name;
	lock->lock = 0;
	lock->flags = 0;
}


void
__mutex_init_etc(mutex *lock, const char *name, uint32 flags)
{
	lock->name = (flags & MUTEX_FLAG_CLONE_NAME) != 0 ? strdup(name) : name;
	lock->lock = 0;
	lock->flags = flags;
		// MUTEX_FLAG_ADAPTIVE is accepted, but makes no difference: all
		// mutexes spin a while before sleeping when there are several CPUs.
}


void
__mutex_destroy(mutex *lock)
{
	if ((lock->flags & MUTEX_FLAG_CLONE_NAME) != 0)
		free(const_cast<char*>(lock->name));
}


status_t
__mutex_lock(mutex *lock)
{
	const int32 oldValue = atomic_test_and_set(&lock->lock,
		B_USER_MUTEX_LOCKED, 0);
	if (oldValue == 0 || (oldValue & B_USER_MUTEX_DISABLED) != 0)
		return B_OK;

	if (__user_mutex_try_lock(&lock->lock, true))
		return B_OK;

	// we have to call the kernel
	status_t error;
	do {
		error = _kern_mutex_lock(&lock->lock, lock->name, 0, 0);
	} while (error == B_INTERRUPTED);

	return error;
}


void
__mutex_unlock(mutex *lock)
{
	// clear the locked flag
	int32 oldValue = atomic_and(&lock->lock, ~(int32)B_USER_MUTEX_LOCKED);
	if ((oldValue & B_USER_MUTEX_WAITING) != 0
			&& (oldValue & B_USER_MUTEX_DISABLED) == 0) {
		_kern_mutex_unblock(&lock->lock, B_USER_MUTEX_UNBLOCK_RELEASED);
	}

	if ((oldValue & B_USER_MUTEX_LOCKED) == 0)
		debugger("mutex was not actually locked!");
}
