/*
 * Copyright 2008-2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2002-2009, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */

#include <lock.h>

#include <stdlib.h>
#include <string.h>

#include <cpu.h>
#include <debug.h>
#include <interrupts.h>
#include <kernel.h>
#include <listeners.h>
#include <scheduling_analysis.h>
#include <smp.h>
#include <thread.h>
#include <util/AutoLock.h>


struct mutex_waiter {
	Thread*			thread;
	mutex_waiter*	next;		// next in queue
	mutex_waiter*	last;		// last in queue (valid for the first in queue)
#if !KDEBUG
	bool			hand_off;	// the unlocker hands the mutex to this one
	bool			acquired;	// set by the unlocker when it did
	bool			woken;		// woken to compete, counted in mutex::woken
	uint8			lost;		// how often it was woken but the mutex taken
#endif
};

struct rw_lock_waiter {
	Thread*			thread;
	rw_lock_waiter*	next;		// next in queue
	rw_lock_waiter*	last;		// last in queue (valid for the first in queue)
	bool			writer;
};

#define MUTEX_FLAG_RELEASED		0x2


#if KDEBUG
#define KDEBUG_STATIC static
#define RECURSIVE_LOCK_HOLDER(recursiveLock)	((recursiveLock)->lock.holder)
static status_t _mutex_lock(struct mutex* lock, void* locker);
static void _mutex_unlock(struct mutex* lock);
#else
#define KDEBUG_STATIC
#define RECURSIVE_LOCK_HOLDER(recursiveLock)	((recursiveLock)->holder)
#endif


int32
recursive_lock_get_recursion(recursive_lock *lock)
{
	if (RECURSIVE_LOCK_HOLDER(lock) == thread_get_current_thread_id())
		return lock->recursion;

	return -1;
}


void
recursive_lock_init(recursive_lock *lock, const char *name)
{
	recursive_lock_init_etc(lock, name, 0);
}


void
recursive_lock_init_etc(recursive_lock *lock, const char *name, uint32 flags)
{
	mutex_init_etc(&lock->lock, name != NULL ? name : "recursive lock", flags);
#if !KDEBUG
	lock->holder = -1;
#endif
	lock->recursion = 0;
}


void
recursive_lock_destroy(recursive_lock *lock)
{
	if (lock == NULL)
		return;

	mutex_destroy(&lock->lock);
}


status_t
recursive_lock_lock(recursive_lock *lock)
{
#if KDEBUG
	if (!gKernelStartup && !are_interrupts_enabled()) {
		panic("recursive_lock_lock: called with interrupts disabled for lock "
			"%p (\"%s\")\n", lock, lock->lock.name);
	}
#endif

	thread_id thread = thread_get_current_thread_id();

	if (thread != RECURSIVE_LOCK_HOLDER(lock)) {
		mutex_lock(&lock->lock);
#if !KDEBUG
		lock->holder = thread;
#endif
	}

	lock->recursion++;
	return B_OK;
}


status_t
recursive_lock_trylock(recursive_lock *lock)
{
	thread_id thread = thread_get_current_thread_id();

#if KDEBUG
	if (!gKernelStartup && !are_interrupts_enabled()) {
		panic("recursive_lock_lock: called with interrupts disabled for lock "
			"%p (\"%s\")\n", lock, lock->lock.name);
	}
#endif

	if (thread != RECURSIVE_LOCK_HOLDER(lock)) {
		status_t status = mutex_trylock(&lock->lock);
		if (status != B_OK)
			return status;

#if !KDEBUG
		lock->holder = thread;
#endif
	}

	lock->recursion++;
	return B_OK;
}


void
recursive_lock_unlock(recursive_lock *lock)
{
	if (thread_get_current_thread_id() != RECURSIVE_LOCK_HOLDER(lock))
		panic("recursive_lock %p unlocked by non-holder thread!\n", lock);

	if (--lock->recursion == 0) {
#if !KDEBUG
		lock->holder = -1;
#endif
		mutex_unlock(&lock->lock);
	}
}


status_t
recursive_lock_switch_lock(recursive_lock* from, recursive_lock* to)
{
#if KDEBUG
	if (!gKernelStartup && !are_interrupts_enabled()) {
		panic("recursive_lock_switch_lock(): called with interrupts "
			"disabled for locks %p, %p", from, to);
	}
#endif

	if (--from->recursion > 0)
		return recursive_lock_lock(to);

#if !KDEBUG
	from->holder = -1;
#endif

	thread_id thread = thread_get_current_thread_id();

	if (thread == RECURSIVE_LOCK_HOLDER(to)) {
		to->recursion++;
		mutex_unlock(&from->lock);
		return B_OK;
	}

	status_t status = mutex_switch_lock(&from->lock, &to->lock);
	if (status != B_OK) {
		from->recursion++;
#if !KDEBUG
		from->holder = thread;
#endif
		return status;
	}

#if !KDEBUG
	to->holder = thread;
#endif
	to->recursion++;
	return B_OK;
}


status_t
recursive_lock_switch_from_mutex(mutex* from, recursive_lock* to)
{
#if KDEBUG
	if (!gKernelStartup && !are_interrupts_enabled()) {
		panic("recursive_lock_switch_from_mutex(): called with interrupts "
			"disabled for locks %p, %p", from, to);
	}
#endif

	thread_id thread = thread_get_current_thread_id();

	if (thread == RECURSIVE_LOCK_HOLDER(to)) {
		to->recursion++;
		mutex_unlock(from);
		return B_OK;
	}

	status_t status = mutex_switch_lock(from, &to->lock);
	if (status != B_OK)
		return status;

#if !KDEBUG
	to->holder = thread;
#endif
	to->recursion++;
	return B_OK;
}


status_t
recursive_lock_switch_from_read_lock(rw_lock* from, recursive_lock* to)
{
#if KDEBUG
	if (!gKernelStartup && !are_interrupts_enabled()) {
		panic("recursive_lock_switch_from_read_lock(): called with interrupts "
			"disabled for locks %p, %p", from, to);
	}
#endif

	thread_id thread = thread_get_current_thread_id();

	if (thread != RECURSIVE_LOCK_HOLDER(to)) {
		status_t status = mutex_switch_from_read_lock(from, &to->lock);
		if (status != B_OK)
			return status;

#if !KDEBUG
		to->holder = thread;
#endif
	} else {
		rw_lock_read_unlock(from);
	}

	to->recursion++;
	return B_OK;
}


void
recursive_lock_transfer_lock(recursive_lock* lock, thread_id thread)
{
	if (lock->recursion != 1)
		panic("invalid recursion level for lock transfer!");

#if !KDEBUG
	lock->holder = thread;
#endif

	mutex_transfer_lock(&lock->lock, thread);
}


static int
dump_recursive_lock_info(int argc, char** argv)
{
	if (argc < 2) {
		print_debugger_command_usage(argv[0]);
		return 0;
	}

	recursive_lock* lock = (recursive_lock*)parse_expression(argv[1]);

	if (!IS_KERNEL_ADDRESS(lock)) {
		kprintf("invalid address: %p\n", lock);
		return 0;
	}

	kprintf("recrusive_lock %p:\n", lock);
	kprintf("  mutex:           %p\n", &lock->lock);
	kprintf("  name:            %s\n", lock->lock.name);
	kprintf("  flags:           0x%x\n", lock->lock.flags);
#if KDEBUG
	kprintf("  holder:          %" B_PRId32 "\n", lock->lock.holder);
#else
	kprintf("  holder:          %" B_PRId32 "\n", lock->holder);
#endif
	kprintf("  recursion:       %d\n", lock->recursion);

	kprintf("  waiting threads:");
	mutex_waiter* waiter = lock->lock.waiters;
	while (waiter != NULL) {
		kprintf(" %" B_PRId32, waiter->thread->id);
		waiter = waiter->next;
	}
	kputs("\n");

	return 0;
}


//	#pragma mark -


static status_t
rw_lock_wait(rw_lock* lock, bool writer, InterruptsSpinLocker& locker)
{
	// enqueue in waiter list
	rw_lock_waiter waiter;
	waiter.thread = thread_get_current_thread();
	waiter.next = NULL;
	waiter.writer = writer;

	if (lock->waiters != NULL)
		lock->waiters->last->next = &waiter;
	else
		lock->waiters = &waiter;

	lock->waiters->last = &waiter;

	// block
	thread_prepare_to_block(waiter.thread, 0, THREAD_BLOCK_TYPE_RW_LOCK, lock);
	locker.Unlock();

	status_t result = thread_block();

	locker.Lock();
	ASSERT(result != B_OK || waiter.thread == NULL);
	return result;
}


static int32
rw_lock_unblock(rw_lock* lock)
{
	// Check whether there are any waiting threads at all and whether anyone
	// has the write lock.
	rw_lock_waiter* waiter = lock->waiters;
	if (waiter == NULL || lock->holder >= 0)
		return 0;

	// writer at head of queue?
	if (waiter->writer) {
		if (lock->active_readers > 0 || lock->pending_readers > 0)
			return 0;

		// dequeue writer
		lock->waiters = waiter->next;
		if (lock->waiters != NULL)
			lock->waiters->last = waiter->last;

		lock->holder = waiter->thread->id;

		// unblock thread
		thread_unblock(waiter->thread, B_OK);
		waiter->thread = NULL;

		return RW_LOCK_WRITER_COUNT_BASE;
	}

	// wake up one or more readers
	uint32 readerCount = 0;
	do {
		// dequeue reader
		lock->waiters = waiter->next;
		if (lock->waiters != NULL)
			lock->waiters->last = waiter->last;

		readerCount++;

		// unblock thread
		thread_unblock(waiter->thread, B_OK);
		waiter->thread = NULL;
	} while ((waiter = lock->waiters) != NULL && !waiter->writer);

	if (lock->count >= RW_LOCK_WRITER_COUNT_BASE)
		lock->active_readers += readerCount;

	return readerCount;
}


void
rw_lock_init(rw_lock* lock, const char* name)
{
	lock->name = name;
	lock->waiters = NULL;
	B_INITIALIZE_SPINLOCK(&lock->lock);
	lock->holder = -1;
	lock->count = 0;
	lock->owner_count = 0;
	lock->active_readers = 0;
	lock->pending_readers = 0;
	lock->flags = 0;

	T_SCHEDULING_ANALYSIS(InitRWLock(lock, name));
	NotifyWaitObjectListeners(&WaitObjectListener::RWLockInitialized, lock);
}


void
rw_lock_init_etc(rw_lock* lock, const char* name, uint32 flags)
{
	lock->name = (flags & RW_LOCK_FLAG_CLONE_NAME) != 0 ? strdup(name) : name;
	lock->waiters = NULL;
	B_INITIALIZE_SPINLOCK(&lock->lock);
	lock->holder = -1;
	lock->count = 0;
	lock->owner_count = 0;
	lock->active_readers = 0;
	lock->pending_readers = 0;
	lock->flags = flags & RW_LOCK_FLAG_CLONE_NAME;

	T_SCHEDULING_ANALYSIS(InitRWLock(lock, name));
	NotifyWaitObjectListeners(&WaitObjectListener::RWLockInitialized, lock);
}


void
rw_lock_destroy(rw_lock* lock)
{
	char* name = (lock->flags & RW_LOCK_FLAG_CLONE_NAME) != 0
		? (char*)lock->name : NULL;

	// unblock all waiters
	InterruptsSpinLocker locker(lock->lock);

#if KDEBUG
	if ((atomic_get(&lock->count) != 0 || lock->waiters != NULL)
			&& thread_get_current_thread_id() != lock->holder) {
		panic("rw_lock_destroy(): lock is in use and the caller "
			"doesn't hold the write lock (%p)", lock);

		locker.Unlock();
		if (rw_lock_write_lock(lock) != B_OK)
			return;
		locker.Lock();
	}
#endif

	while (rw_lock_waiter* waiter = lock->waiters) {
		// dequeue
		lock->waiters = waiter->next;

		// unblock thread
		thread_unblock(waiter->thread, B_ERROR);
	}

	lock->name = NULL;

	locker.Unlock();

	free(name);
}


#if KDEBUG_RW_LOCK_DEBUG

bool
_rw_lock_is_read_locked(const rw_lock* lock)
{
	if (lock->holder == thread_get_current_thread_id())
		return true;

	Thread* thread = thread_get_current_thread();
	for (size_t i = 0; i < B_COUNT_OF(Thread::held_read_locks); i++) {
		if (thread->held_read_locks[i] == lock)
			return true;
	}
	return false;
}


static void
_rw_lock_set_read_locked(rw_lock* lock)
{
	Thread* thread = thread_get_current_thread();
	for (size_t i = 0; i < B_COUNT_OF(Thread::held_read_locks); i++) {
		if (thread->held_read_locks[i] != NULL)
			continue;

		thread->held_read_locks[i] = lock;
		return;
	}

	panic("too many read locks!");
}


static void
_rw_lock_unset_read_locked(rw_lock* lock)
{
	Thread* thread = thread_get_current_thread();
	for (size_t i = 0; i < B_COUNT_OF(Thread::held_read_locks); i++) {
		if (thread->held_read_locks[i] != lock)
			continue;

		thread->held_read_locks[i] = NULL;
		return;
	}

	panic("_rw_lock_unset_read_locked(): lock %p not read-locked by current thread", lock);
}

#endif


status_t
_rw_lock_read_lock(rw_lock* lock)
{
#if KDEBUG
	if (!gKernelStartup && !are_interrupts_enabled()) {
		panic("_rw_lock_read_lock(): called with interrupts disabled for lock %p",
			lock);
	}
#endif
#if KDEBUG_RW_LOCK_DEBUG
	int32 oldCount = atomic_add(&lock->count, 1);
	if (oldCount < RW_LOCK_WRITER_COUNT_BASE) {
		ASSERT_UNLOCKED_RW_LOCK(lock);
		_rw_lock_set_read_locked(lock);
		return B_OK;
	}
#endif

	InterruptsSpinLocker locker(lock->lock);

	// We might be the writer ourselves.
	if (lock->holder == thread_get_current_thread_id()) {
		lock->owner_count++;
		return B_OK;
	}

	// If we hold a read lock already, but some other thread is waiting
	// for a write lock, then trying to read-lock again will deadlock.
	ASSERT_UNLOCKED_RW_LOCK(lock);

	// The writer that originally had the lock when we called atomic_add() might
	// already have gone and another writer could have overtaken us. In this
	// case the original writer set pending_readers, so we know that we don't
	// have to wait.
	if (lock->pending_readers > 0) {
		lock->pending_readers--;

		if (lock->count >= RW_LOCK_WRITER_COUNT_BASE)
			lock->active_readers++;

#if KDEBUG_RW_LOCK_DEBUG
		_rw_lock_set_read_locked(lock);
#endif
		return B_OK;
	}

	ASSERT(lock->count >= RW_LOCK_WRITER_COUNT_BASE);

	// we need to wait
	status_t status = rw_lock_wait(lock, false, locker);

#if KDEBUG_RW_LOCK_DEBUG
	if (status == B_OK)
		_rw_lock_set_read_locked(lock);
#endif

	return status;
}


status_t
_rw_lock_read_lock_with_timeout(rw_lock* lock, uint32 timeoutFlags,
	bigtime_t timeout)
{
#if KDEBUG
	if (!gKernelStartup && !are_interrupts_enabled()) {
		panic("_rw_lock_read_lock_with_timeout(): called with interrupts "
			"disabled for lock %p", lock);
	}
#endif
#if KDEBUG_RW_LOCK_DEBUG
	int32 oldCount = atomic_add(&lock->count, 1);
	if (oldCount < RW_LOCK_WRITER_COUNT_BASE) {
		ASSERT_UNLOCKED_RW_LOCK(lock);
		_rw_lock_set_read_locked(lock);
		return B_OK;
	}
#endif

	InterruptsSpinLocker locker(lock->lock);

	// We might be the writer ourselves.
	if (lock->holder == thread_get_current_thread_id()) {
		lock->owner_count++;
		return B_OK;
	}

	ASSERT_UNLOCKED_RW_LOCK(lock);

	// The writer that originally had the lock when we called atomic_add() might
	// already have gone and another writer could have overtaken us. In this
	// case the original writer set pending_readers, so we know that we don't
	// have to wait.
	if (lock->pending_readers > 0) {
		lock->pending_readers--;

		if (lock->count >= RW_LOCK_WRITER_COUNT_BASE)
			lock->active_readers++;

#if KDEBUG_RW_LOCK_DEBUG
		_rw_lock_set_read_locked(lock);
#endif
		return B_OK;
	}

	ASSERT(lock->count >= RW_LOCK_WRITER_COUNT_BASE);

	// we need to wait

	// enqueue in waiter list
	rw_lock_waiter waiter;
	waiter.thread = thread_get_current_thread();
	waiter.next = NULL;
	waiter.writer = false;

	if (lock->waiters != NULL)
		lock->waiters->last->next = &waiter;
	else
		lock->waiters = &waiter;

	lock->waiters->last = &waiter;

	// block
	thread_prepare_to_block(waiter.thread, 0, THREAD_BLOCK_TYPE_RW_LOCK, lock);
	locker.Unlock();

	status_t error = thread_block_with_timeout(timeoutFlags, timeout);
	if (error == B_OK || waiter.thread == NULL) {
		// We were unblocked successfully -- potentially our unblocker overtook
		// us after we already failed. In either case, we've got the lock, now.
#if KDEBUG_RW_LOCK_DEBUG
		_rw_lock_set_read_locked(lock);
#endif
		return B_OK;
	}

	locker.Lock();
	// We failed to get the lock -- dequeue from waiter list.
	rw_lock_waiter* previous = NULL;
	rw_lock_waiter* other = lock->waiters;
	while (other != &waiter) {
		previous = other;
		other = other->next;
	}

	if (previous == NULL) {
		// we are the first in line
		lock->waiters = waiter.next;
		if (lock->waiters != NULL)
			lock->waiters->last = waiter.last;
	} else {
		// one or more other waiters are before us in the queue
		previous->next = waiter.next;
		if (lock->waiters->last == &waiter)
			lock->waiters->last = previous;
	}

	// Decrement the count. ATM this is all we have to do. There's at least
	// one writer ahead of us -- otherwise the last writer would have unblocked
	// us (writers only manipulate the lock data with thread spinlock being
	// held) -- so our leaving doesn't make a difference to the ones behind us
	// in the queue.
	atomic_add(&lock->count, -1);

	return error;
}


void
_rw_lock_read_unlock(rw_lock* lock)
{
#if KDEBUG_RW_LOCK_DEBUG
	int32 oldCount = atomic_add(&lock->count, -1);
	if (oldCount < RW_LOCK_WRITER_COUNT_BASE) {
		_rw_lock_unset_read_locked(lock);
		return;
	}
#endif

	InterruptsSpinLocker locker(lock->lock);

	// If we're still holding the write lock or if there are other readers,
	// no-one can be woken up.
	if (lock->holder == thread_get_current_thread_id()) {
		ASSERT((lock->owner_count % RW_LOCK_WRITER_COUNT_BASE) > 0);
		lock->owner_count--;
		return;
	}

#if KDEBUG_RW_LOCK_DEBUG
	_rw_lock_unset_read_locked(lock);
#endif

	if (--lock->active_readers > 0)
		return;

	if (lock->active_readers < 0) {
		panic("rw_lock_read_unlock(): lock %p not read-locked", lock);
		lock->active_readers = 0;
		return;
	}

	rw_lock_unblock(lock);
}


/*!	Before a writer queues up (#96): spins while the lock is held only
	briefly, i.e. by a writer that is running on a CPU, or by readers for a
	bounded time, and nobody is queued yet. Returns as soon as the lock looks
	free, or when waiting is better done asleep; the caller then takes the
	usual path either way.
*/
static void
rw_lock_spin_for_write(rw_lock* lock)
{
	if (gKernelStartup || smp_get_num_cpus() < 2)
		return;

	static const bigtime_t kSpinTime = 25;
	bigtime_t deadline = 0;
	for (uint32 i = 1; ; i++) {
		if (atomic_get(&lock->count) == 0)
			return;
		if (*(rw_lock_waiter* volatile*)&lock->waiters != NULL)
			return;

		if ((i % 32) == 0) {
			thread_id holder = atomic_get(&lock->holder);
			if (holder >= 0) {
				// a writer: only while it runs
				bool running = false;
				for (int32 cpu = 0; cpu < smp_get_num_cpus(); cpu++) {
					if (atomic_get(&gCPU[cpu].running_thread_id) == holder) {
						running = true;
						break;
					}
				}
				if (!running)
					return;
			}

			bigtime_t now = system_time();
			if (deadline == 0)
				deadline = now + kSpinTime;
			else if (now >= deadline)
				return;
		}

		cpu_pause();
	}
}


status_t
rw_lock_write_lock(rw_lock* lock)
{
#if KDEBUG
	if (!gKernelStartup && !are_interrupts_enabled()) {
		panic("_rw_lock_write_lock(): called with interrupts disabled for lock %p",
			lock);
	}
#endif

	if (lock->holder != thread_get_current_thread_id())
		rw_lock_spin_for_write(lock);

	InterruptsSpinLocker locker(lock->lock);

	// If we're already the lock holder, we just need to increment the owner
	// count.
	thread_id thread = thread_get_current_thread_id();
	if (lock->holder == thread) {
		lock->owner_count += RW_LOCK_WRITER_COUNT_BASE;
		return B_OK;
	}

	ASSERT_UNLOCKED_RW_LOCK(lock);

	// announce our claim
	int32 oldCount = atomic_add(&lock->count, RW_LOCK_WRITER_COUNT_BASE);

	if (oldCount == 0) {
		// No-one else held a read or write lock, so it's ours now.
		lock->holder = thread;
		lock->owner_count = RW_LOCK_WRITER_COUNT_BASE;
		return B_OK;
	}

	// We have to wait. If we're the first writer, note the current reader
	// count.
	if (oldCount < RW_LOCK_WRITER_COUNT_BASE)
		lock->active_readers = oldCount - lock->pending_readers;

	status_t status = rw_lock_wait(lock, true, locker);
	if (status == B_OK) {
		lock->holder = thread;
		lock->owner_count = RW_LOCK_WRITER_COUNT_BASE;
	}

	return status;
}


void
_rw_lock_write_unlock(rw_lock* lock)
{
	InterruptsSpinLocker locker(lock->lock);

	if (thread_get_current_thread_id() != lock->holder) {
		panic("rw_lock_write_unlock(): lock %p not write-locked by this thread",
			lock);
		return;
	}

	ASSERT(lock->owner_count >= RW_LOCK_WRITER_COUNT_BASE);

	lock->owner_count -= RW_LOCK_WRITER_COUNT_BASE;
	if (lock->owner_count >= RW_LOCK_WRITER_COUNT_BASE)
		return;

	// We gave up our last write lock -- clean up and unblock waiters.
	int32 readerCount = lock->owner_count;
	lock->holder = -1;
	lock->owner_count = 0;

#if KDEBUG_RW_LOCK_DEBUG
	if (readerCount != 0)
		_rw_lock_set_read_locked(lock);
#endif

	int32 oldCount = atomic_add(&lock->count, -RW_LOCK_WRITER_COUNT_BASE);
	oldCount -= RW_LOCK_WRITER_COUNT_BASE;

	if (oldCount != 0) {
		// If writers are waiting, take over our reader count.
		if (oldCount >= RW_LOCK_WRITER_COUNT_BASE) {
			lock->active_readers = readerCount;
			rw_lock_unblock(lock);
		} else {
			// No waiting writer, but there are one or more readers. We will
			// unblock all waiting readers -- that's the easy part -- and must
			// also make sure that all readers that haven't entered the critical
			// section yet, won't start to wait. Otherwise a writer overtaking
			// such a reader will correctly start to wait, but the reader,
			// seeing the writer count > 0, would also start to wait. We set
			// pending_readers to the number of readers that are still expected
			// to enter the critical section.
			lock->pending_readers = oldCount - readerCount
				- rw_lock_unblock(lock);
		}
	}
}


static int
dump_rw_lock_info(int argc, char** argv)
{
	if (argc < 2) {
		print_debugger_command_usage(argv[0]);
		return 0;
	}

	rw_lock* lock = (rw_lock*)parse_expression(argv[1]);

	if (!IS_KERNEL_ADDRESS(lock)) {
		kprintf("invalid address: %p\n", lock);
		return 0;
	}

	kprintf("rw lock %p:\n", lock);
	kprintf("  name:            %s\n", lock->name);
	kprintf("  holder:          %" B_PRId32 "\n", lock->holder);
	kprintf("  count:           %#" B_PRIx32 "\n", lock->count);
	kprintf("  active readers   %d\n", lock->active_readers);
	kprintf("  pending readers  %d\n", lock->pending_readers);
	kprintf("  owner count:     %#" B_PRIx32 "\n", lock->owner_count);
	kprintf("  flags:           %#" B_PRIx32 "\n", lock->flags);

#if KDEBUG_RW_LOCK_DEBUG
	kprintf("  reader threads:");
	if (lock->active_readers > 0) {
		ThreadListIterator iterator;
		while (Thread* thread = iterator.Next()) {
			for (size_t i = 0; i < B_COUNT_OF(Thread::held_read_locks); i++) {
				if (thread->held_read_locks[i] == lock) {
					kprintf(" %" B_PRId32, thread->id);
					break;
				}
			}
		}
	}
	kprintf("\n");
#endif

	kprintf("  waiting threads:");
	rw_lock_waiter* waiter = lock->waiters;
	while (waiter != NULL) {
		kprintf(" %" B_PRId32 "/%c", waiter->thread->id, waiter->writer ? 'w' : 'r');
		waiter = waiter->next;
	}
	kputs("\n");

	return 0;
}


// #pragma mark -


void
mutex_init(mutex* lock, const char *name)
{
	mutex_init_etc(lock, name, 0);
}


void
mutex_init_etc(mutex* lock, const char *name, uint32 flags)
{
	lock->name = (flags & MUTEX_FLAG_CLONE_NAME) != 0 ? strdup(name) : name;
	lock->waiters = NULL;
	B_INITIALIZE_SPINLOCK(&lock->lock);
#if KDEBUG
	lock->holder = -1;
#else
	lock->count = 0;
	lock->woken = 0;
	lock->holder_cpu = 0;
	lock->holder = 0;
#endif
	lock->flags = flags & MUTEX_FLAG_CLONE_NAME;

	T_SCHEDULING_ANALYSIS(InitMutex(lock, name));
	NotifyWaitObjectListeners(&WaitObjectListener::MutexInitialized, lock);
}


void
mutex_destroy(mutex* lock)
{
	char* name = (lock->flags & MUTEX_FLAG_CLONE_NAME) != 0
		? (char*)lock->name : NULL;

	// unblock all waiters
	InterruptsSpinLocker locker(lock->lock);

#if KDEBUG
	// Never-used statically initialized mutexes will have holder set to 0, because
	// MUTEX_INITIALIZER needs to be the same for both KDEBUG and non-KDEBUG kernels.
	if (lock->holder != 0 && lock->holder != -1
			&& thread_get_current_thread_id() != lock->holder) {
		panic("mutex_destroy(): the lock (%p) is held by %" B_PRId32 ", not "
			"by the caller @! bt %" B_PRId32, lock, lock->holder, lock->holder);
		if (_mutex_lock(lock, &locker) != B_OK)
			return;
		locker.Lock();
	}
#endif

#if !KDEBUG
	// Waiters woken to compete still look at the mutex; once they have
	// taken it or queued up again, they are dealt with below.
	while (lock->woken != 0) {
		locker.Unlock();
		cpu_pause();
		locker.Lock();
	}
#endif

	while (mutex_waiter* waiter = lock->waiters) {
		// dequeue
		lock->waiters = waiter->next;

		// unblock thread
		Thread* thread = waiter->thread;
		waiter->thread = NULL;
		thread_unblock(thread, B_ERROR);
	}

	lock->name = NULL;
	lock->flags = 0;
#if KDEBUG
	lock->holder = INT16_MIN;
#else
	lock->count = INT16_MIN;
#endif

	locker.Unlock();

	free(name);
}


static bool
mutex_dequeue_waiter(mutex* lock, mutex_waiter* waiter)
{
	mutex_waiter* previous = NULL;
	mutex_waiter* other = lock->waiters;
	while (other != NULL && other != waiter) {
		previous = other;
		other = other->next;
	}
	if (other == NULL)
		return false;

	if (previous == NULL) {
		if (waiter->next != NULL)
			waiter->next->last = waiter->last;
		lock->waiters = waiter->next;
	} else {
		if (waiter->next == NULL)
			lock->waiters->last = previous;
		previous->next = waiter->next;
	}
	return true;
}



#if KDEBUG


static inline status_t
mutex_lock_threads_locked(mutex* lock, InterruptsSpinLocker* locker)
{
	return _mutex_lock(lock, locker);
}


void
mutex_transfer_lock(mutex* lock, thread_id thread)
{
	if (thread_get_current_thread_id() != lock->holder)
		panic("mutex_transfer_lock(): current thread is not the lock holder!");
	lock->holder = thread;
}


KDEBUG_STATIC status_t
_mutex_lock(mutex* lock, void* _locker)
{
	if (!gKernelStartup && _locker == NULL && !are_interrupts_enabled()) {
		panic("_mutex_lock(): called with interrupts disabled for lock %p",
			lock);
	}

	// lock only, if !lockLocked
	InterruptsSpinLocker* locker
		= reinterpret_cast<InterruptsSpinLocker*>(_locker);

	InterruptsSpinLocker lockLocker;
	if (locker == NULL) {
		lockLocker.SetTo(lock->lock, false);
		locker = &lockLocker;
	}

	// Might have been released after we decremented the count, but before
	// we acquired the spinlock.
	if (lock->holder == 0 || lock->holder == -1) {
		lock->holder = thread_get_current_thread_id();
		return B_OK;
	} else if (lock->holder == thread_get_current_thread_id()) {
		panic("_mutex_lock(): double lock of %p by thread %" B_PRId32, lock,
			lock->holder);
	} else if (lock->holder < -1) {
		panic("_mutex_lock(): using uninitialized lock %p", lock);
	}

	// enqueue in waiter list
	mutex_waiter waiter;
	waiter.thread = thread_get_current_thread();
	waiter.next = NULL;

	if (lock->waiters != NULL) {
		lock->waiters->last->next = &waiter;
	} else
		lock->waiters = &waiter;

	lock->waiters->last = &waiter;

	// block
	thread_prepare_to_block(waiter.thread, 0, THREAD_BLOCK_TYPE_MUTEX, lock);
	locker->Unlock();

	status_t error = thread_block();
	if (error == B_OK) {
		ASSERT(lock->holder == waiter.thread->id);
	} else {
		// This should only happen when the mutex was destroyed.
		ASSERT(waiter.thread == NULL);
	}
	return error;
}


KDEBUG_STATIC void
_mutex_unlock(mutex* lock)
{
	InterruptsSpinLocker locker(lock->lock);

	if (thread_get_current_thread_id() != lock->holder) {
		panic("_mutex_unlock() failure: thread %" B_PRId32 " is trying to "
			"release mutex %p (current holder %" B_PRId32 ")\n",
			thread_get_current_thread_id(), lock, lock->holder);
		return;
	}

	mutex_waiter* waiter = lock->waiters;
	if (waiter != NULL) {
		// dequeue the first waiter
		lock->waiters = waiter->next;
		if (lock->waiters != NULL)
			lock->waiters->last = waiter->last;

		// Already set the holder to the unblocked thread. Besides that this
		// actually reflects the current situation, setting it to -1 would
		// cause a race condition, since another locker could think the lock
		// is not held by anyone.
		lock->holder = waiter->thread->id;

		// unblock thread
		thread_unblock(waiter->thread, B_OK);
	} else {
		// There are no waiters, so mark the lock as released.
		lock->holder = -1;
	}
}


KDEBUG_STATIC status_t
_mutex_lock_with_timeout(mutex* lock, uint32 timeoutFlags, bigtime_t timeout)
{
	if (!gKernelStartup && !are_interrupts_enabled()) {
		panic("_mutex_lock(): called with interrupts disabled for lock %p",
			lock);
	}

	InterruptsSpinLocker locker(lock->lock);

	// Might have been released after we decremented the count, but before
	// we acquired the spinlock.
	if (lock->holder < 0) {
		lock->holder = thread_get_current_thread_id();
		return B_OK;
	} else if (lock->holder == thread_get_current_thread_id()) {
		panic("_mutex_lock(): double lock of %p by thread %" B_PRId32, lock,
			lock->holder);
	} else if (lock->holder == 0) {
		panic("_mutex_lock(): using uninitialized lock %p", lock);
	}

	// enqueue in waiter list
	mutex_waiter waiter;
	waiter.thread = thread_get_current_thread();
	waiter.next = NULL;

	if (lock->waiters != NULL) {
		lock->waiters->last->next = &waiter;
	} else
		lock->waiters = &waiter;

	lock->waiters->last = &waiter;

	// block
	thread_prepare_to_block(waiter.thread, 0, THREAD_BLOCK_TYPE_MUTEX, lock);
	locker.Unlock();

	status_t error = thread_block_with_timeout(timeoutFlags, timeout);

	if (error == B_OK) {
		ASSERT(lock->holder == waiter.thread->id);
	} else {
		// If the lock was destroyed, our "thread" entry will be NULL.
		if (waiter.thread == NULL)
			return B_ERROR;

		// TODO: There is still a race condition during mutex destruction,
		// if we resume due to a timeout before our thread is set to NULL.

		locker.Lock();

		// If the timeout occurred, we must remove our waiter structure from
		// the queue.
		if (mutex_dequeue_waiter(lock, &waiter)) {
			// the structure was still in the list
		} else {
			// the structure is not in the list -- even though the timeout
			// occurred, this means we own the lock now
			ASSERT(lock->holder == waiter.thread->id);
			return B_OK;
		}
	}

	return error;
}


status_t
mutex_trylock(mutex* lock)
{
	InterruptsSpinLocker _(lock->lock);

	if (lock->holder < 0) {
		lock->holder = thread_get_current_thread_id();
		return B_OK;
	} else if (lock->holder == 0) {
		panic("_mutex_trylock(): using uninitialized lock %p", lock);
	}
	return B_WOULD_BLOCK;
}


status_t
mutex_lock(mutex* lock)
{
	return _mutex_lock(lock, NULL);
}


void
mutex_unlock(mutex* lock)
{
	_mutex_unlock(lock);
}


status_t
mutex_lock_with_timeout(mutex* lock, uint32 timeoutFlags, bigtime_t timeout)
{
	return _mutex_lock_with_timeout(lock, timeoutFlags, timeout);
}


#else	// !KDEBUG


/*!	Release builds (#96): "count" is 0 when the mutex is free and minus the
	number of threads that hold it or want it otherwise. A thread that finds
	it held first spins while the holder is running on another CPU, and only
	then sleeps. Unlocking does not hand the mutex to the first sleeper: it
	marks it released (MUTEX_FLAG_RELEASED) and wakes that sleeper, which
	competes for it with threads that are running anyway. A sleeper that
	loses kMutexHandOffAfter times is handed the mutex directly the next time,
	which bounds starvation.

	The protocol of "count" is unchanged, so modules built with the former
	inline fast paths still work with this; they only do not record the
	holder, which makes the spinning bounded by time instead.
*/


static const bigtime_t kMutexSpinTime = 25;
	// microseconds; a sleep and wake-up costs about that much
static const uint8 kMutexHandOffAfter = 2;
static const uint16 kMutexHolderNotRunning = 0xffff;


static inline void
mutex_set_holder(mutex* lock)
{
	Thread* thread = thread_get_current_thread();
	if (thread == NULL) {
		lock->holder = 0;
		return;
	}
	cpu_ent* cpu = thread->cpu;
	lock->holder_cpu = cpu != NULL ? cpu->cpu_num : kMutexHolderNotRunning;
	lock->holder = thread->id;
}


static inline void
mutex_settle_waiter(mutex* lock, mutex_waiter* waiter)
{
	if (waiter != NULL && waiter->woken) {
		waiter->woken = false;
		lock->woken--;
	}
}


/*!	Called with interrupts enabled and no spinlock held by a thread that has
	already decremented the count. Returns whether it got the mutex. A
	\a waiter woken to compete is settled when it does.
*/
static bool
mutex_spin(mutex* lock, mutex_waiter* waiter = NULL)
{
	if (gKernelStartup || smp_get_num_cpus() < 2)
		return false;

	bigtime_t deadline = 0;
	for (uint32 i = 1; ; i++) {
		if ((*(volatile uint8*)&lock->flags & MUTEX_FLAG_RELEASED) != 0) {
			InterruptsSpinLocker locker(lock->lock);
			if ((lock->flags & MUTEX_FLAG_RELEASED) != 0) {
				lock->flags &= ~MUTEX_FLAG_RELEASED;
				mutex_settle_waiter(lock, waiter);
				locker.Unlock();
				mutex_set_holder(lock);
				return true;
			}
		}

		// Only spin while the holder is running. An unknown holder (taken
		// by a module's inline code, or just being released) gets the
		// benefit of the doubt for the spin time.
		thread_id holder = atomic_get(&lock->holder);
		if (holder > 0) {
			uint32 cpu = *(volatile uint16*)&lock->holder_cpu;
			if (cpu >= (uint32)smp_get_num_cpus()
				|| atomic_get(&gCPU[cpu].running_thread_id) != holder) {
				return false;
			}
		}

		if ((i % 32) == 0) {
			bigtime_t now = system_time();
			if (deadline == 0)
				deadline = now + kMutexSpinTime;
			else if (now >= deadline)
				return false;
		}

		cpu_pause();
	}
}


/*!	The slow path of locking: the caller has decremented the count, holds
	the mutex's spinlock via \a locker, and \a waiter is not queued.
*/
static status_t
mutex_wait(mutex* lock, InterruptsSpinLocker* locker, uint32 timeoutFlags,
	bigtime_t timeout)
{
	if (timeout != B_INFINITE_TIMEOUT
		&& (timeoutFlags & B_RELATIVE_TIMEOUT) != 0) {
		// we might block more than once
		timeoutFlags = (timeoutFlags & ~B_RELATIVE_TIMEOUT)
			| B_ABSOLUTE_TIMEOUT;
		timeout += system_time();
	}

	mutex_waiter waiter;
	waiter.thread = thread_get_current_thread();
	waiter.lost = 0;
	waiter.woken = false;

	while (true) {
		// a waiter woken to compete has the spinlock again here
		mutex_settle_waiter(lock, &waiter);

		// Might have been released after we decremented the count, but
		// before we acquired the spinlock; or we were woken to compete.
		if ((lock->flags & MUTEX_FLAG_RELEASED) != 0) {
			lock->flags &= ~MUTEX_FLAG_RELEASED;
			mutex_set_holder(lock);
			return B_OK;
		}

		// enqueue: at the end the first time, at the front after losing
		waiter.next = NULL;
		waiter.acquired = false;
		waiter.hand_off = waiter.lost >= kMutexHandOffAfter;
		if (lock->waiters == NULL) {
			lock->waiters = &waiter;
			waiter.last = &waiter;
		} else if (waiter.lost == 0) {
			lock->waiters->last->next = &waiter;
			lock->waiters->last = &waiter;
		} else {
			waiter.next = lock->waiters;
			waiter.last = lock->waiters->last;
			lock->waiters = &waiter;
		}

		// block
		thread_prepare_to_block(waiter.thread, 0, THREAD_BLOCK_TYPE_MUTEX,
			lock);
		locker->Unlock();

		status_t error = timeout == B_INFINITE_TIMEOUT
			? thread_block()
			: thread_block_with_timeout(timeoutFlags, timeout);

		if (error == B_OK) {
			if (waiter.acquired) {
				// handed to us
				mutex_set_holder(lock);
				return B_OK;
			}

			// Woken to compete: the mutex is released, unless a running
			// thread has taken it meanwhile. Then wait for that one as if
			// we were new, but keep our place in the queue.
			if (mutex_spin(lock, &waiter))
				return B_OK;

			locker->Lock();
			if ((lock->flags & MUTEX_FLAG_RELEASED) == 0
				&& waiter.lost < 255) {
				waiter.lost++;
			}
			continue;
		}

		// If the lock was destroyed, our "thread" entry will be NULL.
		if (waiter.thread == NULL)
			return B_ERROR;

		// TODO: There is still a race condition during mutex destruction,
		// if we resume due to a timeout before our thread is set to NULL.

		locker->Lock();
		mutex_settle_waiter(lock, &waiter);

		if (!mutex_dequeue_waiter(lock, &waiter)) {
			// We were dequeued and woken before the timeout took effect:
			// either the mutex was handed to us, or it is released and we
			// have been chosen to take it.
			if (waiter.acquired) {
				mutex_set_holder(lock);
				return B_OK;
			}
			if ((lock->flags & MUTEX_FLAG_RELEASED) != 0) {
				lock->flags &= ~MUTEX_FLAG_RELEASED;
				mutex_set_holder(lock);
				return B_OK;
			}
		}

		// we need to fix the lock count
		atomic_add(&lock->count, 1);
		return error;
	}
}


static inline status_t
mutex_lock_threads_locked(mutex* lock, InterruptsSpinLocker* locker)
{
	if (atomic_add(&lock->count, -1) < 0)
		return _mutex_lock(lock, locker);
	mutex_set_holder(lock);
	return B_OK;
}


void
mutex_transfer_lock(mutex* lock, thread_id thread)
{
	lock->holder_cpu = kMutexHolderNotRunning;
	lock->holder = thread;
}


status_t
_mutex_lock(mutex* lock, void* _locker)
{
	InterruptsSpinLocker* locker
		= reinterpret_cast<InterruptsSpinLocker*>(_locker);

	InterruptsSpinLocker lockLocker;
	if (locker == NULL) {
		if (mutex_spin(lock))
			return B_OK;

		lockLocker.SetTo(lock->lock, false);
		locker = &lockLocker;
	}

	return mutex_wait(lock, locker, 0, B_INFINITE_TIMEOUT);
}


void
_mutex_unlock(mutex* lock)
{
	InterruptsSpinLocker locker(lock->lock);

	mutex_waiter* waiter = lock->waiters;
	if (waiter == NULL) {
		// Nobody sleeps, but someone has decremented the count: whoever it
		// is takes it from here.
		lock->flags |= MUTEX_FLAG_RELEASED;
		return;
	}

	// dequeue the first waiter
	lock->waiters = waiter->next;
	if (lock->waiters != NULL)
		lock->waiters->last = waiter->last;

	if (waiter->hand_off || lock->woken == 255) {
		// it has lost often enough (or too many compete already)
		waiter->acquired = true;
		lock->holder_cpu = kMutexHolderNotRunning;
		lock->holder = waiter->thread->id;
	} else {
		lock->flags |= MUTEX_FLAG_RELEASED;
		waiter->woken = true;
		lock->woken++;
	}

	thread_unblock(waiter->thread, B_OK);
}


status_t
_mutex_lock_with_timeout(mutex* lock, uint32 timeoutFlags, bigtime_t timeout)
{
	if (mutex_spin(lock))
		return B_OK;

	InterruptsSpinLocker locker(lock->lock);
	return mutex_wait(lock, &locker, timeoutFlags, timeout);
}


status_t
mutex_trylock(mutex* lock)
{
	if (atomic_test_and_set(&lock->count, -1, 0) != 0)
		return B_WOULD_BLOCK;
	mutex_set_holder(lock);
	return B_OK;
}


status_t
mutex_lock(mutex* lock)
{
	if (atomic_add(&lock->count, -1) < 0)
		return _mutex_lock(lock, NULL);
	mutex_set_holder(lock);
	return B_OK;
}


void
mutex_unlock(mutex* lock)
{
	lock->holder = 0;
	if (atomic_add(&lock->count, 1) < -1)
		_mutex_unlock(lock);
}


status_t
mutex_lock_with_timeout(mutex* lock, uint32 timeoutFlags, bigtime_t timeout)
{
	if (atomic_add(&lock->count, -1) < 0)
		return _mutex_lock_with_timeout(lock, timeoutFlags, timeout);
	mutex_set_holder(lock);
	return B_OK;
}


#endif	// !KDEBUG


status_t
mutex_switch_lock(mutex* from, mutex* to)
{
#if KDEBUG
	if (!gKernelStartup && !are_interrupts_enabled()) {
		panic("mutex_switch_lock(): called with interrupts disabled "
			"for locks %p, %p", from, to);
	}
#endif

	InterruptsSpinLocker locker(to->lock);

	mutex_unlock(from);

	return mutex_lock_threads_locked(to, &locker);
}


status_t
mutex_switch_from_read_lock(rw_lock* from, mutex* to)
{
#if KDEBUG
	if (!gKernelStartup && !are_interrupts_enabled()) {
		panic("mutex_switch_from_read_lock(): called with interrupts disabled "
			"for locks %p, %p", from, to);
	}
#endif
	ASSERT(from->holder != thread_get_current_thread_id());

	InterruptsSpinLocker locker(to->lock);

	rw_lock_read_unlock(from);

	return mutex_lock_threads_locked(to, &locker);
}


static int
dump_mutex_info(int argc, char** argv)
{
	if (argc < 2) {
		print_debugger_command_usage(argv[0]);
		return 0;
	}

	mutex* lock = (mutex*)parse_expression(argv[1]);

	if (!IS_KERNEL_ADDRESS(lock)) {
		kprintf("invalid address: %p\n", lock);
		return 0;
	}

	kprintf("mutex %p:\n", lock);
	kprintf("  name:            %s\n", lock->name);
	kprintf("  flags:           0x%x\n", lock->flags);
#if KDEBUG
	kprintf("  holder:          %" B_PRId32 "\n", lock->holder);
#else
	kprintf("  count:           %" B_PRId32 "\n", lock->count);
	kprintf("  holder:          %" B_PRId32 " (hint, on CPU %u)\n",
		lock->holder, lock->holder_cpu);
#endif

	kprintf("  waiting threads:");
	mutex_waiter* waiter = lock->waiters;
	while (waiter != NULL) {
		kprintf(" %" B_PRId32, waiter->thread->id);
		waiter = waiter->next;
	}
	kputs("\n");

	return 0;
}


// #pragma mark -


void
lock_debug_init()
{
	add_debugger_command_etc("mutex", &dump_mutex_info,
		"Dump info about a mutex",
		"<mutex>\n"
		"Prints info about the specified mutex.\n"
		"  <mutex>  - pointer to the mutex to print the info for.\n", 0);
	add_debugger_command_etc("rwlock", &dump_rw_lock_info,
		"Dump info about an rw lock",
		"<lock>\n"
		"Prints info about the specified rw lock.\n"
		"  <lock>  - pointer to the rw lock to print the info for.\n", 0);
	add_debugger_command_etc("recursivelock", &dump_recursive_lock_info,
		"Dump info about a recursive lock",
		"<lock>\n"
		"Prints info about the specified recursive lock.\n"
		"  <lock>  - pointer to the recursive lock to print the info for.\n",
		0);
}
