/*
 * Copyright 2007-2010, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2002-2008, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */

#include <fs/select_sync_pool.h>
#include <wait_for_objects.h>

#include <new>

#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include <OS.h>
#include <Select.h>

#include <AutoDeleter.h>
#include <StackOrHeapArray.h>

#include <event_queue.h>
#include <fs/fd.h>
#include <port.h>
#include <sem.h>
#include <syscalls.h>
#include <syscall_restart.h>
#include <thread.h>
#include <tracing.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>
#include <vfs.h>

#include "select_ops.h"
#include "select_sync.h"


//#define TRACE_WAIT_FOR_OBJECTS
#ifdef TRACE_WAIT_FOR_OBJECTS
#	define PRINT(x) dprintf x
#	define FUNCTION(x) dprintf x
#else
#	define PRINT(x) ;
#	define FUNCTION(x) ;
#endif


using std::nothrow;


struct select_sync_pool_entry
	: DoublyLinkedListLinkImpl<select_sync_pool_entry> {
	selectsync			*sync;
	uint16				events;
};

typedef DoublyLinkedList<select_sync_pool_entry> SelectSyncPoolEntryList;

struct select_sync_pool {
	SelectSyncPoolEntryList	entries;
};


// So many select_infos live in the sync object itself; more come from the
// heap. A poll() or select() of a few descriptors then allocates nothing.
static const uint32 kInlineSelectInfos = 16;


/*!	The sync object of one select()/poll()/wait_for_objects() call. It lives
	on the caller's stack. It used to be a heap object with a semaphore of its
	own, and creating and deleting that semaphore cost about a third of a
	poll() of one descriptor (#227). The waiting thread now blocks directly,
	and Notify() unblocks it.

	Others can still hold references when the call is done: close() of a
	descriptor that is being selected takes its select_infos away and notifies
	and deselects them itself (deselect_select_infos()), and so does a dying
	thread. The destructor waits until they have put their references, so that
	nothing touches the object after it is gone.
*/
struct wait_for_objects_sync : public select_sync {
								wait_for_objects_sync();
	virtual						~wait_for_objects_sync();

			status_t			Init(uint32 count);
			status_t			Wait(uint32 flags, bigtime_t timeout);

	virtual	status_t			Notify(select_info* info, uint16 events);

protected:
	virtual	void				LastReferenceReleased();

public:
			struct select_info*	set;

private:
			spinlock			fLock;
			Thread*				fWaiter;
			bool				fNotified;
			bool				fReleased;
			select_info			fInlineSet[kInlineSelectInfos];
};


select_sync::~select_sync()
{
}


#if WAIT_FOR_OBJECTS_TRACING


namespace WaitForObjectsTracing {


class SelectTraceEntry : public AbstractTraceEntry {
	protected:
		SelectTraceEntry(int count, fd_set* readSet, fd_set* writeSet,
			fd_set* errorSet)
			:
			fReadSet(NULL),
			fWriteSet(NULL),
			fErrorSet(NULL),
			fCount(count)
		{
			int sets = (readSet != NULL ? 1 : 0) + (writeSet != NULL ? 1 : 0)
				+ (errorSet != NULL ? 1 : 0);
			if (sets > 0 && count > 0) {
				uint32 bytes = HOWMANY(count, NFDBITS) * sizeof(fd_mask);
				uint8* allocated = (uint8*)alloc_tracing_buffer(bytes * sets);
				if (allocated != NULL) {
					if (readSet != NULL) {
						fReadSet = (fd_set*)allocated;
						memcpy(fReadSet, readSet, bytes);
						allocated += bytes;
					}
					if (writeSet != NULL) {
						fWriteSet = (fd_set*)allocated;
						memcpy(fWriteSet, writeSet, bytes);
						allocated += bytes;
					}
					if (errorSet != NULL) {
						fErrorSet = (fd_set*)allocated;
						memcpy(fErrorSet, errorSet, bytes);
					}
				}
			}
		}

		void AddDump(TraceOutput& out, const char* name)
		{
			out.Print(name);

			_PrintSet(out, "read", fReadSet);
			_PrintSet(out, ", write", fWriteSet);
			_PrintSet(out, ", error", fErrorSet);
		}

	private:
		void _PrintSet(TraceOutput& out, const char* name, fd_set* set)
		{

			out.Print("%s: <", name);

			if (set != NULL) {
				bool first = true;
				for (int i = 0; i < fCount; i++) {
					if (!FD_ISSET(i, set))
						continue;

					if (first) {
						out.Print("%d", i);
						first = false;
					} else
						out.Print(", %d", i);
				}
			}

			out.Print(">");
		}

	protected:
		fd_set*	fReadSet;
		fd_set*	fWriteSet;
		fd_set*	fErrorSet;
		int		fCount;
};


class SelectBegin : public SelectTraceEntry {
	public:
		SelectBegin(int count, fd_set* readSet, fd_set* writeSet,
			fd_set* errorSet, bigtime_t timeout)
			:
			SelectTraceEntry(count, readSet, writeSet, errorSet),
			fTimeout(timeout)
		{
			Initialized();
		}

		virtual void AddDump(TraceOutput& out)
		{
			SelectTraceEntry::AddDump(out, "select begin: ");
			out.Print(", timeout: %" B_PRIdBIGTIME, fTimeout);
		}

	private:
		bigtime_t	fTimeout;
};


class SelectDone : public SelectTraceEntry {
	public:
		SelectDone(int count, fd_set* readSet, fd_set* writeSet,
			fd_set* errorSet, status_t status)
			:
			SelectTraceEntry(status == B_OK ? count : 0, readSet, writeSet,
				errorSet),
			fStatus(status)
		{
			Initialized();
		}

		virtual void AddDump(TraceOutput& out)
		{
			if (fStatus == B_OK)
				SelectTraceEntry::AddDump(out, "select done:  ");
			else
				out.Print("select done:  error: %#" B_PRIx32, fStatus);
		}

	private:
		status_t	fStatus;
};


class PollTraceEntry : public AbstractTraceEntry {
	protected:
		PollTraceEntry(pollfd* fds, int count, bool resultEvents)
			:
			fEntries(NULL),
			fCount(0)
		{
			if (fds != NULL && count > 0) {
				for (int i = 0; i < count; i++) {
					if (resultEvents ? fds[i].revents : fds[i].events)
						fCount++;
				}
			}

			if (fCount == 0)
				return;

			fEntries = (FDEntry*)alloc_tracing_buffer(fCount * sizeof(FDEntry));
			if (fEntries != NULL) {
				for (int i = 0; i < fCount; fds++) {
					uint16 events = resultEvents ? fds->revents : fds->events;
					if (events != 0) {
						fEntries[i].fd = fds->fd;
						fEntries[i].events = events;
						i++;
					}
				}
			}
		}

		void AddDump(TraceOutput& out)
		{
			if (fEntries == NULL)
				return;

			static const struct {
				const char*	name;
				uint16		event;
			} kEventNames[] = {
				{ "r", POLLIN },
				{ "w", POLLOUT },
				{ "rb", POLLRDBAND },
				{ "wb", POLLWRBAND },
				{ "rp", POLLPRI },
				{ "err", POLLERR },
				{ "hup", POLLHUP },
				{ "inv", POLLNVAL },
				{ NULL, 0 }
			};

			bool firstFD = true;
			for (int i = 0; i < fCount; i++) {
				if (firstFD) {
					out.Print("<%u: ", fEntries[i].fd);
					firstFD = false;
				} else
					out.Print(", <%u: ", fEntries[i].fd);

				bool firstEvent = true;
				for (int k = 0; kEventNames[k].name != NULL; k++) {
					if ((fEntries[i].events & kEventNames[k].event) != 0) {
						if (firstEvent) {
							out.Print("%s", kEventNames[k].name);
							firstEvent = false;
						} else
							out.Print(", %s", kEventNames[k].name);
					}
				}

				out.Print(">");
			}
		}

	protected:
		struct FDEntry {
			uint16	fd;
			uint16	events;
		};

		FDEntry*	fEntries;
		int			fCount;
};


class PollBegin : public PollTraceEntry {
	public:
		PollBegin(pollfd* fds, int count, bigtime_t timeout)
			:
			PollTraceEntry(fds, count, false),
			fTimeout(timeout)
		{
			Initialized();
		}

		virtual void AddDump(TraceOutput& out)
		{
			out.Print("poll begin: ");
			PollTraceEntry::AddDump(out);
			out.Print(", timeout: %" B_PRIdBIGTIME, fTimeout);
		}

	private:
		bigtime_t	fTimeout;
};


class PollDone : public PollTraceEntry {
	public:
		PollDone(pollfd* fds, int count, int result)
			:
			PollTraceEntry(fds, result >= 0 ? count : 0, true),
			fResult(result)
		{
			Initialized();
		}

		virtual void AddDump(TraceOutput& out)
		{
			if (fResult >= 0) {
				out.Print("poll done:  count: %d: ", fResult);
				PollTraceEntry::AddDump(out);
			} else
				out.Print("poll done:  error: %#x", fResult);
		}

	private:
		int		fResult;
};

}	// namespace WaitForObjectsTracing

#	define T(x)	new(std::nothrow) WaitForObjectsTracing::x

#else
#	define T(x)
#endif	// WAIT_FOR_OBJECTS_TRACING


// #pragma mark -


/*!
	Clears all bits in the fd_set - since we are using variable sized
	arrays in the kernel, we can't use the FD_ZERO() macro provided by
	sys/select.h for this task.
	All other FD_xxx() macros are safe to use, though.
*/
static inline void
fd_zero(fd_set *set, int numFDs)
{
	if (set != NULL)
		memset(set, 0, HOWMANY(numFDs, NFDBITS) * sizeof(fd_mask));
}


void
acquire_select_sync(select_sync* sync)
{
	FUNCTION(("acquire_select_sync(%p)\n", sync));
	sync->AcquireReference();
}


void
put_select_sync(select_sync* sync)
{
	FUNCTION(("put_select_sync(%p): -> %ld\n", sync, sync->CountReferences() - 1));
	sync->ReleaseReference();
}


wait_for_objects_sync::wait_for_objects_sync()
	:
	set(NULL),
	fWaiter(NULL),
	fNotified(false),
	fReleased(false)
{
	B_INITIALIZE_SPINLOCK(&fLock);
}


wait_for_objects_sync::~wait_for_objects_sync()
{
	// Put our own reference. If someone else still has one (see above), wait
	// for it: the last one wakes us in LastReferenceReleased(). The wait is
	// not interruptible, it is short and the object must not go away early.
	if (ReleaseReference() != 1) {
		Thread* thread = thread_get_current_thread();

		InterruptsSpinLocker locker(fLock);
		while (!fReleased) {
			thread_prepare_to_block(thread, 0, THREAD_BLOCK_TYPE_OTHER,
				"select sync");
			fWaiter = thread;
			locker.Unlock();

			thread_block();
				// A late Notify() can wake us as well, so check again.

			locker.Lock();
			fWaiter = NULL;
		}
	}

	if (set != fInlineSet)
		delete[] set;
}


status_t
wait_for_objects_sync::Init(uint32 count)
{
	if (count <= kInlineSelectInfos)
		set = fInlineSet;
	else {
		set = new(nothrow) select_info[count];
		if (set == NULL)
			return B_NO_MEMORY;
	}

	for (uint32 i = 0; i < count; i++) {
		set[i].next = NULL;
		set[i].sync = this;
	}

	return B_OK;
}


/*!	Waits until Notify() is called with one of an info's selected events, as
	acquire_sem_etc(B_CAN_INTERRUPT | flags, timeout) of the old semaphore did,
	with the same results: B_OK when notified (even when the timeout has
	passed or a signal is pending), B_WOULD_BLOCK for a relative timeout of 0,
	B_TIMED_OUT, B_INTERRUPTED, and B_BAD_VALUE for both timeout flags.
*/
status_t
wait_for_objects_sync::Wait(uint32 flags, bigtime_t timeout)
{
	if ((flags & (B_RELATIVE_TIMEOUT | B_ABSOLUTE_TIMEOUT))
			== (B_RELATIVE_TIMEOUT | B_ABSOLUTE_TIMEOUT)) {
		return B_BAD_VALUE;
	}

	flags |= B_CAN_INTERRUPT;

	Thread* thread = thread_get_current_thread();

	InterruptsSpinLocker locker(fLock);
	if (fNotified)
		return B_OK;

	if ((flags & B_RELATIVE_TIMEOUT) != 0 && timeout <= 0)
		return B_WOULD_BLOCK;
	if ((flags & B_ABSOLUTE_TIMEOUT) != 0 && timeout < 0)
		return B_TIMED_OUT;

	SpinLocker schedulerLocker(thread->scheduler_lock);
	if (thread_is_interrupted(thread, flags))
		return B_INTERRUPTED;
	schedulerLocker.Unlock();

	// As in switch_sem_etc(): poll() and select() turn a zero timeout into
	// "now", don't block on a timer that fires at once.
	if ((flags & B_ABSOLUTE_TIMEOUT) != 0
		&& timeout <= ((flags & B_TIMEOUT_REAL_TIME_BASE) != 0
			? real_time_clock_usecs() : system_time())) {
		return B_TIMED_OUT;
	}

	thread_prepare_to_block(thread, flags, THREAD_BLOCK_TYPE_OTHER, "select");
	fWaiter = thread;
	locker.Unlock();

	status_t status;
	if ((flags & (B_RELATIVE_TIMEOUT | B_ABSOLUTE_TIMEOUT)) == 0
		|| timeout == B_INFINITE_TIMEOUT) {
		status = thread_block();
	} else
		status = thread_block_with_timeout(flags, timeout);

	// Once fWaiter is cleared, no Notify() can unblock this thread any more
	// (a timed out or interrupted thread is not waiting, so an unblock in
	// between does nothing).
	locker.Lock();
	fWaiter = NULL;

	return status;
}


/*!	Can be called in interrupt context and with spinlocks held, like
	notify_select_event().
*/
status_t
wait_for_objects_sync::Notify(select_info* info, uint16 events)
{
	atomic_or(&info->events, events);

	// only wake up the waiting select()/poll() call if the events
	// match one of the selected ones
	if ((info->selected_events & events) == 0)
		return B_OK;

	InterruptsSpinLocker locker(fLock);
	fNotified = true;
	if (fWaiter != NULL)
		thread_unblock(fWaiter, B_OK);

	return B_OK;
}


void
wait_for_objects_sync::LastReferenceReleased()
{
	// The object is on the stack of the thread that waits in the destructor,
	// don't delete it. The locker is the last access to it: the destructor
	// cannot return before it has the lock again.
	InterruptsSpinLocker locker(fLock);
	fReleased = true;
	if (fWaiter != NULL)
		thread_unblock(fWaiter, B_OK);
}


static int
common_select(int numFDs, fd_set *readSet, fd_set *writeSet, fd_set *errorSet,
	bigtime_t timeout, const sigset_t *sigMask, bool kernel)
{
	status_t status = B_OK;
	int fd;

	FUNCTION(("[%ld] common_select(%d, %p, %p, %p, %lld, %p, %d)\n",
		find_thread(NULL), numFDs, readSet, writeSet, errorSet, timeout,
		sigMask, kernel));

	// check if fds are valid before doing anything

	for (fd = 0; fd < numFDs; fd++) {
		if (((readSet && FD_ISSET(fd, readSet))
			|| (writeSet && FD_ISSET(fd, writeSet))
			|| (errorSet && FD_ISSET(fd, errorSet)))
			&& !fd_is_valid(fd, kernel))
			return B_FILE_ERROR;
	}

	wait_for_objects_sync sync;
	status = sync.Init(numFDs);
	if (status != B_OK)
		return status;

	T(SelectBegin(numFDs, readSet, writeSet, errorSet, timeout));

	// start selecting file descriptors

	for (fd = 0; fd < numFDs; fd++) {
		sync.set[fd].selected_events = 0;
		sync.set[fd].events = 0;

		if (readSet && FD_ISSET(fd, readSet)) {
			sync.set[fd].selected_events = SELECT_FLAG(B_SELECT_READ)
				| SELECT_FLAG(B_SELECT_DISCONNECTED) | SELECT_FLAG(B_SELECT_ERROR);
		}
		if (writeSet && FD_ISSET(fd, writeSet)) {
			sync.set[fd].selected_events |= SELECT_FLAG(B_SELECT_WRITE)
				| SELECT_FLAG(B_SELECT_ERROR);
		}
		if (errorSet && FD_ISSET(fd, errorSet))
			sync.set[fd].selected_events |= SELECT_FLAG(B_SELECT_ERROR);

		if (sync.set[fd].selected_events != 0) {
			select_fd(fd, sync.set + fd, kernel);
				// array position is the same as the fd for select()
		}
	}

	// set new signal mask
	sigset_t oldSigMask;
	if (sigMask != NULL) {
		sigprocmask(SIG_SETMASK, sigMask, &oldSigMask);
		if (!kernel) {
			Thread *thread = thread_get_current_thread();
			thread->old_sig_block_mask = oldSigMask;
			thread->flags |= THREAD_FLAGS_OLD_SIGMASK;
		}
	}

	// wait for something to happen
	status = sync.Wait(timeout >= 0 ? B_ABSOLUTE_TIMEOUT : 0, timeout);

	// restore the old signal mask
	if (sigMask != NULL && kernel)
		sigprocmask(SIG_SETMASK, &oldSigMask, NULL);

	PRINT(("common_select(): Wait() returned: %lx\n", status));

	// deselect file descriptors

	// Only what was given to select_fd() can be on a descriptor's list. The
	// sets are still as the caller passed them; selected_events is not a
	// guide, select_fd() zeroes it when no hook took an event but keeps the
	// info on the list all the same. Each deselect_fd() write-locks the
	// descriptor table, so a high nfds used to cost a lock per index (#227).
	for (fd = 0; fd < numFDs; fd++) {
		if ((readSet != NULL && FD_ISSET(fd, readSet))
			|| (writeSet != NULL && FD_ISSET(fd, writeSet))
			|| (errorSet != NULL && FD_ISSET(fd, errorSet))) {
			deselect_fd(fd, sync.set + fd, kernel);
		}
	}

	PRINT(("common_select(): events deselected\n"));

	// collect the events that have happened in the meantime

	int count = 0;

	if (status == B_INTERRUPTED) {
		// We must not clear the sets in this case, as applications may
		// rely on the contents of them.
		T(SelectDone(numFDs, readSet, writeSet, errorSet, status));
		return B_INTERRUPTED;
	}

	// Clear sets to store the received events
	// (we can't use the macros, because we have variable sized arrays;
	// the other FD_xxx() macros are safe, though).
	fd_zero(readSet, numFDs);
	fd_zero(writeSet, numFDs);
	fd_zero(errorSet, numFDs);

	if (status == B_OK) {
		for (count = 0, fd = 0; fd < numFDs; fd++) {
			if (readSet && sync.set[fd].events & (SELECT_FLAG(B_SELECT_READ)
					| SELECT_FLAG(B_SELECT_DISCONNECTED) | SELECT_FLAG(B_SELECT_ERROR))) {
				FD_SET(fd, readSet);
				count++;
			}
			if (writeSet
				&& sync.set[fd].events & (SELECT_FLAG(B_SELECT_WRITE)
					| SELECT_FLAG(B_SELECT_ERROR))) {
				FD_SET(fd, writeSet);
				count++;
			}
			if (errorSet
				&& sync.set[fd].events & SELECT_FLAG(B_SELECT_ERROR)) {
				FD_SET(fd, errorSet);
				count++;
			}
		}
	}

	// B_TIMED_OUT and B_WOULD_BLOCK are supposed to return 0

	T(SelectDone(numFDs, readSet, writeSet, errorSet, status));

	return count;
}


static int
common_poll(struct pollfd *fds, nfds_t numFDs, bigtime_t timeout,
	const sigset_t *sigMask, bool kernel)
{
	wait_for_objects_sync sync;
	status_t status = sync.Init(numFDs);
	if (status != B_OK)
		return status;

	T(PollBegin(fds, numFDs, timeout));

	// start polling file descriptors (by selecting them)

	bool invalid = false;
	for (uint32 i = 0; i < numFDs; i++) {
		int fd = fds[i].fd;

		// initialize events masks
		fds[i].events |= POLLNVAL | POLLERR | POLLHUP;
		sync.set[i].selected_events = fds[i].events;
		sync.set[i].events = 0;
		fds[i].revents = 0;

		if (fd >= 0 && select_fd(fd, sync.set + i, kernel) != B_OK) {
			// If the FD returned events as well as an error, ignore the error.
			if (sync.set[i].events != 0)
				continue;

			sync.set[i].events = POLLNVAL;
			fds[i].revents = POLLNVAL;
				// indicates that the FD doesn't need to be deselected
			invalid = true;
		}
	}

	// set new signal mask
	sigset_t oldSigMask;
	if (sigMask != NULL) {
		sigprocmask(SIG_SETMASK, sigMask, &oldSigMask);
		if (!kernel) {
			Thread *thread = thread_get_current_thread();
			thread->old_sig_block_mask = oldSigMask;
			thread->flags |= THREAD_FLAGS_OLD_SIGMASK;
		}
	}

	if (!invalid)
		status = sync.Wait(timeout >= 0 ? B_ABSOLUTE_TIMEOUT : 0, timeout);

	// restore the old signal mask
	if (sigMask != NULL && kernel)
		sigprocmask(SIG_SETMASK, &oldSigMask, NULL);

	// deselect file descriptors

	for (uint32 i = 0; i < numFDs; i++) {
		if (fds[i].fd >= 0 && (sync.set[i].events & POLLNVAL) == 0)
			deselect_fd(fds[i].fd, sync.set + i, kernel);
	}

	// collect the events that have happened in the meantime

	int count = 0;
	switch (status) {
		case B_OK:
			for (uint32 i = 0; i < numFDs; i++) {
				if (fds[i].fd < 0)
					continue;

				// POLLxxx flags and B_SELECT_xxx flags are compatible
				fds[i].revents = sync.set[i].events & fds[i].events;
				if (fds[i].revents != 0)
					count++;
			}
			break;
		case B_INTERRUPTED:
			count = B_INTERRUPTED;
			break;
		default:
			// B_TIMED_OUT, and B_WOULD_BLOCK
			break;
	}

	T(PollDone(fds, numFDs, count));

	return count;
}


static ssize_t
common_wait_for_objects(object_wait_info* infos, int numInfos, uint32 flags,
	bigtime_t timeout, bool kernel)
{
	status_t status = B_OK;

	wait_for_objects_sync sync;
	status = sync.Init(numInfos);
	if (status != B_OK)
		return status;

	// start selecting objects

	bool invalid = false;
	for (int i = 0; i < numInfos; i++) {
		uint16 type = infos[i].type;
		int32 object = infos[i].object;

		// initialize events masks
		infos[i].events |= B_EVENT_INVALID | B_EVENT_ERROR | B_EVENT_DISCONNECTED;
		sync.set[i].selected_events = infos[i].events;
		sync.set[i].events = 0;

		if (select_object(type, object, sync.set + i, kernel) != B_OK) {
			// If the object returned events as well as an error, ignore the error.
			if (sync.set[i].events != 0)
				continue;

			sync.set[i].events = B_EVENT_INVALID;
			infos[i].events = B_EVENT_INVALID;
				// indicates that the object doesn't need to be deselected
			invalid = true;
		}
	}

	if (!invalid)
		status = sync.Wait(flags, timeout);

	// deselect objects

	for (int i = 0; i < numInfos; i++) {
		uint16 type = infos[i].type;
		if ((sync.set[i].events & B_EVENT_INVALID) == 0)
			deselect_object(type, infos[i].object, sync.set + i, kernel);
	}

	// collect the events that have happened in the meantime

	ssize_t count = 0;
	if (status == B_OK) {
		for (int i = 0; i < numInfos; i++) {
			infos[i].events &= sync.set[i].events;
			if (infos[i].events != 0)
				count++;
		}
	} else {
		// B_INTERRUPTED, B_TIMED_OUT, and B_WOULD_BLOCK
		count = status;
		for (int i = 0; i < numInfos; i++)
			infos[i].events = 0;
	}

	return count;
}


// #pragma mark - kernel private


status_t
notify_select_events(select_info* info, uint16 events)
{
	FUNCTION(("notify_select_events(%p (%p), 0x%x)\n", info, info->sync,
		events));

	if (info == NULL || info->sync == NULL)
		return B_BAD_VALUE;

	return info->sync->Notify(info, events);
}


void
notify_select_events_list(select_info* list, uint16 events)
{
	struct select_info* info = list;
	while (info != NULL) {
		select_info* next = info->next;
		notify_select_events(info, events);
		info = next;
	}
}


//	#pragma mark - public kernel API


status_t
notify_select_event(struct selectsync *sync, uint8 event)
{
	return notify_select_events((select_info*)sync, SELECT_FLAG(event));
}


//	#pragma mark - private kernel exported API


static select_sync_pool_entry *
find_select_sync_pool_entry(select_sync_pool *pool, selectsync *sync)
{
	for (SelectSyncPoolEntryList::Iterator it = pool->entries.GetIterator();
		 it.HasNext();) {
		select_sync_pool_entry *entry = it.Next();
		if (entry->sync == sync)
			return entry;
	}

	return NULL;
}


static status_t
add_select_sync_pool_entry(select_sync_pool *pool, selectsync *sync,
	uint16 events)
{
	// check, whether the entry does already exist
	select_sync_pool_entry *entry = find_select_sync_pool_entry(pool, sync);
	if (!entry) {
		entry = new (std::nothrow) select_sync_pool_entry;
		if (!entry)
			return B_NO_MEMORY;

		entry->sync = sync;
		entry->events = 0;

		pool->entries.Add(entry);
	}

	entry->events |= events;

	return B_OK;
}


/*!	Adds all of \a events (SELECT_FLAG()s) for \a sync at once. A select()
	hook that takes several events in one call (fd_ops::fd_select_events)
	looks the entry up once, where the per-event hooks did so (and took
	their lock) once per event: at least three times per descriptor and
	poll() (#227).
*/
status_t
add_select_sync_pool_events(select_sync_pool **_pool, selectsync *sync,
	uint16 events)
{
	// create the pool, if necessary
	select_sync_pool *pool = *_pool;
	if (!pool) {
		pool = new (std::nothrow) select_sync_pool;
		if (!pool)
			return B_NO_MEMORY;

		*_pool = pool;
	}

	// add the entry
	status_t error = add_select_sync_pool_entry(pool, sync, events);

	// cleanup
	if (pool->entries.IsEmpty()) {
		delete pool;
		*_pool = NULL;
	}

	return error;
}


status_t
add_select_sync_pool_entry(select_sync_pool **_pool, selectsync *sync,
	uint8 event)
{
	return add_select_sync_pool_events(_pool, sync, SELECT_FLAG(event));
}


/*!	Removes all of \a events (SELECT_FLAG()s) of \a sync at once, see
	add_select_sync_pool_events().
*/
status_t
remove_select_sync_pool_events(select_sync_pool **_pool, selectsync *sync,
	uint16 events)
{
	select_sync_pool *pool = *_pool;
	if (!pool)
		return B_ENTRY_NOT_FOUND;

	// clear the event flags of the concerned entries
	bool found = false;
	for (SelectSyncPoolEntryList::Iterator it = pool->entries.GetIterator();
		 it.HasNext();) {
		select_sync_pool_entry *entry = it.Next();
		if (entry->sync == sync) {
			found = true;
			entry->events &= ~events;

			// remove the entry, if no longer needed
			if (entry->events == 0) {
				it.Remove();
				delete entry;
			}
		}
	}

	if (!found)
		return B_ENTRY_NOT_FOUND;

	// delete the pool, if no longer needed
	if (pool->entries.IsEmpty()) {
		delete pool;
		*_pool = NULL;
	}

	return B_OK;
}


status_t
remove_select_sync_pool_entry(select_sync_pool **_pool, selectsync *sync,
	uint8 event)
{
	return remove_select_sync_pool_events(_pool, sync, SELECT_FLAG(event));
}


void
delete_select_sync_pool(select_sync_pool *pool)
{
	if (!pool)
		return;

	while (select_sync_pool_entry *entry = pool->entries.Head()) {
		pool->entries.Remove(entry);
		delete entry;
	}

	delete pool;
}


void
notify_select_event_pool(select_sync_pool *pool, uint8 event)
{
	if (!pool)
		return;

	FUNCTION(("notify_select_event_pool(%p, %u)\n", pool, event));

	for (SelectSyncPoolEntryList::Iterator it = pool->entries.GetIterator();
		 it.HasNext();) {
		select_sync_pool_entry *entry = it.Next();
		if (entry->events & SELECT_FLAG(event))
			notify_select_event(entry->sync, event);
	}
}


//	#pragma mark - Kernel POSIX layer


ssize_t
_kern_select(int numFDs, fd_set *readSet, fd_set *writeSet, fd_set *errorSet,
	bigtime_t timeout, const sigset_t *sigMask)
{
	if (timeout >= 0)
		timeout += system_time();

	return common_select(numFDs, readSet, writeSet, errorSet, timeout,
		sigMask, true);
}


ssize_t
_kern_poll(struct pollfd *fds, int numFDs, bigtime_t timeout,
	const sigset_t *sigMask)
{
	if (timeout >= 0)
		timeout += system_time();

	return common_poll(fds, numFDs, timeout, sigMask, true);
}


ssize_t
_kern_wait_for_objects(object_wait_info* infos, int numInfos, uint32 flags,
	bigtime_t timeout)
{
	return common_wait_for_objects(infos, numInfos, flags, timeout, true);
}


//	#pragma mark - User syscalls


static bool
check_max_fds(int numFDs)
{
	if (numFDs <= 0)
		return true;

	struct io_context *context = get_current_io_context(false);
	ReadLocker locker(&context->lock);
	return (size_t)numFDs <= context->table_size;
}


ssize_t
_user_select(int numFDs, fd_set *userReadSet, fd_set *userWriteSet,
	fd_set *userErrorSet, bigtime_t timeout, const sigset_t *userSigMask)
{
	uint32 bytes = HOWMANY(numFDs, NFDBITS) * sizeof(fd_mask);
	int result;

	if (timeout >= 0) {
		timeout += system_time();
		// deal with overflow
		if (timeout < 0)
			timeout = B_INFINITE_TIMEOUT;
	}

	if (numFDs < 0 || (numFDs > FD_SETSIZE && !check_max_fds(numFDs)))
		return B_BAD_VALUE;

	if ((userReadSet != NULL && !IS_USER_ADDRESS(userReadSet))
		|| (userWriteSet != NULL && !IS_USER_ADDRESS(userWriteSet))
		|| (userErrorSet != NULL && !IS_USER_ADDRESS(userErrorSet))
		|| (userSigMask != NULL && !IS_USER_ADDRESS(userSigMask)))
		return B_BAD_ADDRESS;

	// copy parameters

	BStackOrHeapArray<char, 128> sets(bytes * (
		((userReadSet != NULL) ? 1 : 0) +
		((userWriteSet != NULL) ? 1 : 0) +
		((userErrorSet != NULL) ? 1 : 0)));
	if (!sets.IsValid())
		return B_NO_MEMORY;

	char *nextSet = &sets[0];
	fd_set *readSet = NULL, *writeSet = NULL, *errorSet = NULL;

	if (userReadSet != NULL) {
		readSet = (fd_set *)nextSet;
		nextSet += bytes;

		if (user_memcpy(readSet, userReadSet, bytes) != B_OK)
			return B_BAD_ADDRESS;
	}

	if (userWriteSet != NULL) {
		writeSet = (fd_set *)nextSet;
		nextSet += bytes;

		if (user_memcpy(writeSet, userWriteSet, bytes) != B_OK)
			return B_BAD_ADDRESS;
	}

	if (userErrorSet != NULL) {
		errorSet = (fd_set *)nextSet;

		if (user_memcpy(errorSet, userErrorSet, bytes) != B_OK)
			return B_BAD_ADDRESS;
	}

	sigset_t sigMask;
	if (userSigMask != NULL
			&& user_memcpy(&sigMask, userSigMask, sizeof(sigMask)) != B_OK) {
		return B_BAD_ADDRESS;
	}

	result = common_select(numFDs, readSet, writeSet, errorSet, timeout,
		userSigMask ? &sigMask : NULL, false);

	// copy back results

	if (result >= B_OK
		&& ((readSet != NULL
				&& user_memcpy(userReadSet, readSet, bytes) < B_OK)
			|| (writeSet != NULL
				&& user_memcpy(userWriteSet, writeSet, bytes) < B_OK)
			|| (errorSet != NULL
				&& user_memcpy(userErrorSet, errorSet, bytes) < B_OK))) {
		result = B_BAD_ADDRESS;
	}

	return result;
}


ssize_t
_user_poll(struct pollfd *userfds, int numFDs, bigtime_t timeout,
	const sigset_t *userSigMask)
{
	if (timeout >= 0) {
		timeout += system_time();
		// deal with overflow
		if (timeout < 0)
			timeout = B_INFINITE_TIMEOUT;
	}

	if (numFDs < 0 || !check_max_fds(numFDs))
		return B_BAD_VALUE;

	BStackOrHeapArray<struct pollfd, 16> fds(numFDs);
	if (!fds.IsValid())
		return B_NO_MEMORY;

	const size_t bytes = numFDs * sizeof(struct pollfd);
	if (numFDs != 0) {
		if (userfds == NULL || !IS_USER_ADDRESS(userfds))
			return B_BAD_ADDRESS;

		if (user_memcpy(fds, userfds, bytes) < B_OK)
			return B_BAD_ADDRESS;
	}

	sigset_t sigMask;
	if (userSigMask != NULL
		&& (!IS_USER_ADDRESS(userSigMask)
			|| user_memcpy(&sigMask, userSigMask, sizeof(sigMask)) < B_OK)) {
		return B_BAD_ADDRESS;
	}

	status_t result = common_poll(fds, numFDs, timeout,
		userSigMask != NULL ? &sigMask : NULL, false);

	// copy back results
	if (numFDs > 0 && user_memcpy(userfds, fds, bytes) != 0) {
		if (result >= 0)
			result = B_BAD_ADDRESS;
	}

	return result;
}


ssize_t
_user_wait_for_objects(object_wait_info* userInfos, int numInfos, uint32 flags,
	bigtime_t timeout)
{
	syscall_restart_handle_timeout_pre(flags, timeout);

	if (numInfos < 0 || !check_max_fds(numInfos - sem_max_sems()
			- port_max_ports() - thread_max_threads())) {
		return B_BAD_VALUE;
	}

	if (numInfos == 0) {
		// special case: no infos
		ssize_t result = common_wait_for_objects(NULL, 0, flags, timeout,
			false);
		return result < 0
			? syscall_restart_handle_timeout_post(result, timeout) : result;
	}

	if (userInfos == NULL || !IS_USER_ADDRESS(userInfos))
		return B_BAD_ADDRESS;

	BStackOrHeapArray<object_wait_info, 16> infos(numInfos);
	if (!infos.IsValid())
		return B_NO_MEMORY;
	const int bytes = sizeof(object_wait_info) * numInfos;

	if (user_memcpy(infos, userInfos, bytes) != B_OK)
		return B_BAD_ADDRESS;

	ssize_t result = common_wait_for_objects(infos, numInfos, flags, timeout, false);

	if (result >= 0 && user_memcpy(userInfos, infos, bytes) != B_OK) {
		result = B_BAD_ADDRESS;
	} else {
		syscall_restart_handle_timeout_post(result, timeout);
	}

	return result;
}
