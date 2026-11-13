/*
 * Copyright 2006-2013, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler, axeld@pinc-software.de
 */


#include "utility.h"

#include <ByteOrder.h>
#include <KernelExport.h>

#include <condition_variable.h>
#include <kernel.h>
#include <unistd.h>
#include <net_buffer.h>
#include <syscall_restart.h>
#include <util/AutoLock.h>

#include "stack_private.h"


//#define TRACE_UTILITY
#ifdef TRACE_UTILITY
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...) ;
#endif


// The pending timers, in a pairing heap ordered by due time: adding a timer
// is O(1), taking the next one or removing any is O(log n) amortized. The
// list scanned in full on every wake-up before made thousands of timers
// (connections in TIME_WAIT) slow down every other timer of the stack
// (airos-works/haiku#121).
static net_timer* sTimerHeap;
static mutex sTimerLock;
static sem_id sTimerWaitSem;
static ConditionVariable sWaitForTimerCondition;
static net_timer* sCurrentTimer;
static thread_id sTimerThread;
static bigtime_t sTimerTimeout;


// #pragma mark - UserBuffer


void*
UserBuffer::Push(void* source, size_t length)
{
	if (fStatus != B_OK)
		return NULL;

	if (fAvailable < length) {
		fStatus = ENOBUFS;
		return NULL;
	}

#ifdef _KERNEL_MODE
	fStatus = user_memcpy(fBuffer, source, length);
	if (fStatus != B_OK)
		return NULL;
#else
	memcpy(fBuffer, source, length);
#endif

	void* current = fBuffer;

	fAvailable -= length;
	fBuffer += length;

	return current;
}


status_t
UserBuffer::Pad(size_t length)
{
	if (fStatus != B_OK)
		return fStatus;

	if (fAvailable < length)
		return fStatus = ENOBUFS;

	fStatus = user_memset(fBuffer, 0, length);
	if (fStatus != B_OK)
		return fStatus;

	fAvailable -= length;
	fBuffer += length;

	return B_OK;
}


status_t
UserBuffer::PadToNext(size_t length)
{
	return Pad((BytesConsumed() + length - 1) / length - BytesConsumed());
}


// #pragma mark -


uint16
compute_checksum(uint8* _buffer, size_t length)
{
	uint16* buffer = (uint16*)_buffer;
	uint32 sum = 0;

	// TODO: unfold loop for speed
	// TODO: write processor dependent version for speed
	while (length >= 2) {
		sum += *buffer++;
		length -= 2;
	}

	if (length) {
		// give the last byte it's proper endian-aware treatment
#if B_HOST_IS_LENDIAN
		sum += *(uint8*)buffer;
#else
		uint8 ordered[2];
		ordered[0] = *(uint8*)buffer;
		ordered[1] = 0;
		sum += *(uint16*)ordered;
#endif
	}

	while (sum >> 16) {
		sum = (sum & 0xffff) + (sum >> 16);
	}

	return sum;
}


uint16
checksum(uint8* buffer, size_t length)
{
	return ~compute_checksum(buffer, length);
}


//	#pragma mark - Notifications


status_t
notify_socket(net_socket* socket, uint8 event, int32 value)
{
	return gNetSocketModule.notify(socket, event, value);
}


static inline void
fifo_notify_one_reader(int32& waiting, sem_id sem)
{
	if (waiting > 0) {
		waiting--;
		release_sem_etc(sem, 1, B_DO_NOT_RESCHEDULE);
	}
}


//	#pragma mark - fifo


status_t
init_fifo(net_fifo* fifo, const char* name, size_t maxBytes)
{
	mutex_init_etc(&fifo->lock, name, MUTEX_FLAG_CLONE_NAME);
	fifo->notify = create_sem(0, name);
	if (fifo->notify < B_OK) {
		mutex_destroy(&fifo->lock);
		return fifo->notify;
	}

	fifo->max_bytes = maxBytes;
	fifo->current_bytes = 0;
	fifo->waiting = 0;
	list_init(&fifo->buffers);

	return B_OK;
}


void
uninit_fifo(net_fifo* fifo)
{
	clear_fifo(fifo);

	mutex_destroy(&fifo->lock);
	delete_sem(fifo->notify);
}


static inline status_t
base_fifo_enqueue_buffer(net_fifo* fifo, net_buffer* buffer)
{
	if (fifo->max_bytes > 0
		&& fifo->current_bytes + buffer->size > fifo->max_bytes)
		return ENOBUFS;

	list_add_item(&fifo->buffers, buffer);
	fifo->current_bytes += buffer->size;
	fifo_notify_one_reader(fifo->waiting, fifo->notify);

	return B_OK;
}


status_t
fifo_enqueue_buffer(net_fifo* fifo, net_buffer* buffer)
{
	MutexLocker locker(fifo->lock);
	return base_fifo_enqueue_buffer(fifo, buffer);
}


/*!	Gets the first buffer from the FIFO. If there is no buffer, it
	will wait depending on the \a flags and \a timeout.
	The following flags are supported:
		MSG_DONTWAIT - ignores the timeout and never wait for a buffer; if your
			socket is O_NONBLOCK, you should specify this flag. A \a timeout of
			zero is equivalent to this flag, though.
		MSG_PEEK - returns a clone of the buffer and keep the original
			in the FIFO.
*/
ssize_t
fifo_dequeue_buffer(net_fifo* fifo, uint32 flags, bigtime_t timeout,
	net_buffer** _buffer)
{
	if ((flags & ~(MSG_DONTWAIT | MSG_PEEK)) != 0)
		return EOPNOTSUPP;

	MutexLocker locker(fifo->lock);
	const bool dontWait = (flags & MSG_DONTWAIT) != 0 || timeout == 0;
	status_t status;

	while (true) {
		net_buffer* buffer = (net_buffer*)list_get_first_item(&fifo->buffers);
		if (buffer != NULL) {
			if ((flags & MSG_PEEK) != 0) {
				// we need to clone the buffer for inspection; we can't give a
				// handle to a buffer that we're still using
				buffer = gNetBufferModule.clone(buffer, false);
				if (buffer == NULL) {
					status = B_NO_MEMORY;
					break;
				}
			} else {
				list_remove_item(&fifo->buffers, buffer);
				fifo->current_bytes -= buffer->size;
			}

			*_buffer = buffer;
			status = B_OK;
			break;
		}

		if (!dontWait)
			fifo->waiting++;

		locker.Unlock();

		if (dontWait)
			return B_WOULD_BLOCK;

		// we need to wait until a new buffer becomes available
		status = acquire_sem_etc(fifo->notify, 1,
			B_CAN_INTERRUPT | B_ABSOLUTE_TIMEOUT, timeout);
		if (status < B_OK)
			return status;

		locker.Lock();
	}

	// if another thread is waiting for data, since we didn't
	// eat the buffer, it will get it
	if (flags & MSG_PEEK)
		fifo_notify_one_reader(fifo->waiting, fifo->notify);

	return status;
}


status_t
clear_fifo(net_fifo* fifo)
{
	MutexLocker locker(fifo->lock);

	while (true) {
		net_buffer* buffer = (net_buffer*)list_remove_head_item(&fifo->buffers);
		if (buffer == NULL)
			break;

		gNetBufferModule.free(buffer);
	}

	fifo->current_bytes = 0;
	return B_OK;
}


status_t
fifo_socket_enqueue_buffer(net_fifo* fifo, net_socket* socket, uint8 event,
	net_buffer* _buffer)
{
	net_buffer *buffer = gNetBufferModule.clone(_buffer, false);
	if (buffer == NULL)
		return B_NO_MEMORY;

	MutexLocker locker(fifo->lock);

	status_t status = base_fifo_enqueue_buffer(fifo, buffer);
	if (status < B_OK)
		gNetBufferModule.free(buffer);
	else
		notify_socket(socket, event, fifo->current_bytes);

	return status;
}


//	#pragma mark - Timer


static inline bool
timer_is_earlier(const net_timer* a, const net_timer* b)
{
	return a->due < b->due;
}


/*!	Melds two heaps whose roots have no siblings and no parent. */
static net_timer*
timer_heap_meld(net_timer* a, net_timer* b)
{
	if (a == NULL)
		return b;
	if (b == NULL)
		return a;

	if (timer_is_earlier(b, a)) {
		net_timer* swap = a;
		a = b;
		b = swap;
	}

	// b becomes the first child of a
	b->heap_next = a->heap_child;
	if (a->heap_child != NULL)
		a->heap_child->heap_previous = b;
	b->heap_previous = a;
	a->heap_child = b;
	return a;
}


/*!	Melds the sibling list starting at \a first into one heap (the classic
	two passes, without recursion).
*/
static net_timer*
timer_heap_merge_pairs(net_timer* first)
{
	net_timer* pairs = NULL;
	while (first != NULL) {
		net_timer* a = first;
		net_timer* b = a->heap_next;
		first = b != NULL ? b->heap_next : NULL;

		a->heap_next = a->heap_previous = NULL;
		if (b != NULL)
			b->heap_next = b->heap_previous = NULL;

		net_timer* melded = timer_heap_meld(a, b);
		melded->heap_next = pairs;
		pairs = melded;
	}

	net_timer* result = NULL;
	while (pairs != NULL) {
		net_timer* next = pairs->heap_next;
		pairs->heap_next = NULL;
		result = timer_heap_meld(result, pairs);
		pairs = next;
	}

	return result;
}


static void
timer_heap_add(net_timer* timer)
{
	timer->heap_next = timer->heap_previous = timer->heap_child = NULL;
	sTimerHeap = timer_heap_meld(sTimerHeap, timer);
}


static void
timer_heap_remove(net_timer* timer)
{
	if (timer == sTimerHeap) {
		sTimerHeap = timer_heap_merge_pairs(timer->heap_child);
		timer->heap_child = NULL;
		return;
	}

	// unlink it from its parent's list of children
	net_timer* previous = timer->heap_previous;
	if (previous->heap_child == timer)
		previous->heap_child = timer->heap_next;
	else
		previous->heap_next = timer->heap_next;
	if (timer->heap_next != NULL)
		timer->heap_next->heap_previous = previous;

	net_timer* children = timer_heap_merge_pairs(timer->heap_child);
	timer->heap_next = timer->heap_previous = timer->heap_child = NULL;
	sTimerHeap = timer_heap_meld(sTimerHeap, children);
}


static status_t
timer_thread(void* /*data*/)
{
	status_t status = B_OK;

	do {
		bigtime_t timeout = B_INFINITE_TIMEOUT;

		if (status == B_TIMED_OUT || status == B_OK) {
			// execute the timers that are due, and find the next timeout
			mutex_lock(&sTimerLock);

			while (sTimerHeap != NULL) {
				net_timer* timer = sTimerHeap;
				if (timer->due > system_time()) {
					timeout = timer->due;
					break;
				}

				timer_heap_remove(timer);
				timer->due = -1;
				sCurrentTimer = timer;

				mutex_unlock(&sTimerLock);
				timer->hook(timer, timer->data);
				mutex_lock(&sTimerLock);

				sCurrentTimer = NULL;
				sWaitForTimerCondition.NotifyAll();
			}

			sTimerTimeout = timeout;
			mutex_unlock(&sTimerLock);
		}

		status = acquire_sem_etc(sTimerWaitSem, 1, B_ABSOLUTE_TIMEOUT, timeout);
			// the wait sem normally can't be acquired, so we
			// have to look at the status value the call returns:
			//
			// B_OK - a new timer has been added or canceled
			// B_TIMED_OUT - look for timers to be executed
			// B_BAD_SEM_ID - we are asked to quit
	} while (status != B_BAD_SEM_ID);

	return B_OK;
}


/*!
	Initializes a timer before use. You can also use this function to change
	a timer later on, but make sure you have canceled it before using set_timer().
*/
void
init_timer(net_timer* timer, net_timer_func hook, void* data)
{
	timer->heap_next = timer->heap_previous = timer->heap_child = NULL;
	timer->hook = hook;
	timer->data = data;
	timer->due = 0;
	timer->flags = 0;
}


/*!	Sets or cancels a timer. When the \a delay is below zero, an eventually
	running timer is canceled, if not, it is scheduled to be executed after the
	specified \a delay.
	You need to have initialized the timer before calling this function.

	In case you need to change a running timer, you have to cancel it first,
	before making any changes.
*/
void
set_timer(net_timer* timer, bigtime_t delay)
{
	MutexLocker locker(sTimerLock);

	TRACE("set_timer %p, hook %p, data %p\n", timer, timer->hook, timer->data);

	if (timer->due > 0) {
		// this timer is scheduled: take it out, it is cancelled or moves
		timer_heap_remove(timer);
		timer->due = 0;
	}

	if (delay >= 0) {
		// (re)schedule this timer; a due time of 0 or less means "not
		// scheduled", so it is at least 1
		timer->due = max_c(system_time() + delay, 1);
		timer_heap_add(timer);

		// notify timer about the change if necessary
		if (sTimerTimeout > timer->due)
			release_sem(sTimerWaitSem);
	}
}


bool
cancel_timer(struct net_timer* timer)
{
	MutexLocker locker(sTimerLock);

	TRACE("cancel_timer %p, hook %p, data %p\n", timer, timer->hook,
		timer->data);

	if (timer->due <= 0)
		return false;

	// this timer is scheduled, cancel it
	timer_heap_remove(timer);
	timer->due = 0;
	return true;
}


status_t
wait_for_timer(struct net_timer* timer)
{
	if (find_thread(NULL) == sTimerThread) {
		// let's not wait for ourselves...
		return B_BAD_VALUE;
	}

	while (true) {
		MutexLocker locker(sTimerLock);

		if (timer->due <= 0 && sCurrentTimer != timer)
			return B_OK;

		// we actually need to wait for this timer
		ConditionVariableEntry entry;
		sWaitForTimerCondition.Add(&entry);

		locker.Unlock();

		entry.Wait();
	}

	return B_OK;
}


bool
is_timer_active(net_timer* timer)
{
	return timer->due > 0;
}


bool
is_timer_running(net_timer* timer)
{
	return timer == sCurrentTimer;
}


static int
dump_timer(int argc, char** argv)
{
	kprintf("timer       hook        data        due in\n");

	// walk the heap depth first: children, then siblings
	struct net_timer* timer = sTimerHeap;
	int32 count = 0;
	while (timer != NULL) {
		kprintf("%p  %p  %p  %" B_PRId64 "\n", timer, timer->hook, timer->data,
			timer->due > 0 ? timer->due - system_time() : -1);
		count++;

		if (timer->heap_child != NULL) {
			timer = timer->heap_child;
			continue;
		}
		while (timer != NULL && timer->heap_next == NULL) {
			// back up to the first ancestor that has a next sibling
			net_timer* previous = timer->heap_previous;
			while (previous != NULL && previous->heap_child != timer) {
				timer = previous;
				previous = timer->heap_previous;
			}
			timer = previous;
		}
		if (timer != NULL)
			timer = timer->heap_next;
	}
	kprintf("%" B_PRId32 " timers\n", count);

	return 0;
}


status_t
init_timers(void)
{
	sTimerHeap = NULL;
	sTimerTimeout = B_INFINITE_TIMEOUT;

	status_t status = B_OK;
	mutex_init(&sTimerLock, "net timer");

	sTimerWaitSem = create_sem(0, "net timer wait");
	if (sTimerWaitSem < B_OK) {
		status = sTimerWaitSem;
		goto err1;
	}

	// above the receive threads: timers send ACKs and retransmissions,
	// and their hooks are short
	sTimerThread = spawn_kernel_thread(timer_thread, "net timer",
		B_URGENT_DISPLAY_PRIORITY, NULL);
	if (sTimerThread < B_OK) {
		status = sTimerThread;
		goto err2;
	}

	sWaitForTimerCondition.Init(NULL, "wait for net timer");

	add_debugger_command("net_timer", dump_timer,
		"Lists all active network timer");

	return resume_thread(sTimerThread);

err1:
	mutex_destroy(&sTimerLock);
err2:
	delete_sem(sTimerWaitSem);
	return status;
}


void
uninit_timers(void)
{
	delete_sem(sTimerWaitSem);

	status_t status;
	wait_for_thread(sTimerThread, &status);

	mutex_lock(&sTimerLock);

	mutex_destroy(&sTimerLock);

	remove_debugger_command("net_timer", dump_timer);
}


//	#pragma mark - Syscall restart


bool
is_syscall(void)
{
	return is_called_via_syscall();
}


/*!	Checks a buffer that a request structure points to. A request that came
	from user space may only point into user space: user_memcpy() accepts any
	range that does not cross into the kernel, so without this check a program
	could make the kernel write to or read from a kernel address of its
	choosing (airos-works/haiku#130).
*/
status_t
check_request_buffer(const void* buffer, size_t size)
{
	if (is_syscall() && !is_user_address_range(buffer, size))
		return B_BAD_ADDRESS;

	return B_OK;
}


/*!	Whether the caller may change the network configuration: the kernel and
	root may, other users may not (airos-works/haiku#130).
*/
bool
may_change_configuration(void)
{
	return !is_syscall() || geteuid() == 0;
}


bool
is_restarted_syscall(void)
{
	return syscall_restart_is_restarted();
}


bigtime_t
set_syscall_restart_timeout(bigtime_t relativeTimeout)
{
	bigtime_t timeout;
	if (relativeTimeout == 0 || relativeTimeout == B_INFINITE_TIMEOUT)
		timeout = relativeTimeout;
	else
		timeout = system_time() + relativeTimeout;

	Thread* thread = thread_get_current_thread();
	if ((thread->flags & THREAD_FLAGS_SYSCALL) != 0)
		*(bigtime_t*)thread->syscall_restart.parameters = timeout;

	return timeout;
}


bigtime_t
restore_syscall_restart_timeout(void)
{
	Thread* thread = thread_get_current_thread();
	return *(bigtime_t*)thread->syscall_restart.parameters;
}

