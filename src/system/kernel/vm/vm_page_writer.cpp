/*
 * Copyright 2010-2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2002-2010, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


#include <string.h>
#include <stdlib.h>

#include <algorithm>

#include <KernelExport.h>
#include <OS.h>

#include <AutoDeleter.h>
#include <util/AutoLock.h>

#include <heap.h>
#include <low_resource_manager.h>
#include <thread.h>
#include <vfs.h>
#include <vm/vm.h>
#include <vm/vm_priv.h>
#include <vm/vm_page.h>
#include <vm/VMCache.h>

#include "IORequest.h"
#include "PageCacheLocker.h"
#include "ModifiedPageQueue.h"


//#define TRACE_VM_PAGE_WRITER
#ifdef TRACE_VM_PAGE_WRITER
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif

#define MAX_PAGE_WRITER_IO_PRIORITY				B_URGENT_DISPLAY_PRIORITY
	// maximum I/O priority of the page writer
#define MAX_PAGE_WRITER_IO_PRIORITY_THRESHOLD	10000
	// the maximum I/O priority shall be reached when this many pages need to
	// be written

#define PAGES_FLUSH_DURATION_LOCAL_QUOTA		(3 * 1000 * 1000)
#define PAGES_FLUSH_DURATION_GLOBAL_QUOTA		(5 * 1000 * 1000)
	// target maximum time needed to write out all modified pages, in one & all queues

#define PAGE_WRITER_IDLE_PASS_BREAK				250000
	// how long the page writer rests after it went through its whole queue
	// and found nothing it can write

ConditionVariable ModifiedPageQueue::sUnderQuotaCondition;
int64 ModifiedPageQueue::sGlobalModifiedCount = 0;
bigtime_t ModifiedPageQueue::sGlobalEstimatedWriteDuration = -1;


#if PAGE_WRITER_TRACING

namespace PageWriterTracing {

class WritePage : public AbstractTraceEntry {
	public:
		WritePage(vm_page* page)
			:
			fCache(page->Cache()),
			fPage(page)
		{
			Initialized();
		}

		virtual void AddDump(TraceOutput& out)
		{
			out.Print("page write: %p, cache: %p", fPage, fCache);
		}

	private:
		VMCache*	fCache;
		vm_page*	fPage;
};

}	// namespace PageWriterTracing

#	define TPW(x)	new(std::nothrow) PageWriterTracing::x

#else
#	define TPW(x)
#endif	// PAGE_WRITER_TRACING



class PageWriteTransfer;
class PageWriteWrapper;


class PageWriterRun {
public:
	PageWriterRun()
		:
		fWrappers(NULL),
		fTransfers(NULL)
	{
	}

	status_t Init(uint32 maxPages, uint32 maxTransfers,
		uint32 allocationFlags, bool holdsReferences);
	~PageWriterRun();

	void PrepareNextRun();
	bool AddPage(vm_page* page);
	uint32 PageCount() const { return fWrapperCount; }

	void Start(uint32 flags);
	uint32 Finish();
	void ReleaseReferences();

	void PageWritten(PageWriteTransfer* transfer, status_t status,
		bool partialTransfer, size_t bytesTransferred);

private:
	uint32				fMaxPages;
	uint32				fMaxTransfers;
	bool				fHoldsReferences;
		// a cache and a store reference for each page, released when done
	uint32				fWrapperCount;
	uint32				fTransferCount;
	int32				fPendingTransfers;
	int32				fNotified;
		// set once the last transfer's notification is complete: until then
		// the notifier still uses fAllFinishedCondition
	PageWriteWrapper*	fWrappers;
	PageWriteTransfer*	fTransfers;
	ConditionVariable	fAllFinishedCondition;
	ConditionVariableEntry fWaitEntry;
};


class PageWriteTransfer : public AsyncIOCallback {
public:
	void SetTo(PageWriterRun* run, vm_page* page, int32 maxPages);
	bool AddPage(vm_page* page);

	status_t Schedule(uint32 flags);

	void SetStatus(status_t status, size_t transferred);

	status_t Status() const	{ return fStatus; }
	struct VMCache* Cache() const { return fCache; }
	uint32 PageCount() const { return fPageCount; }

	virtual void IOFinished(status_t status, bool partialTransfer,
		generic_size_t bytesTransferred);

private:
	PageWriterRun*		fRun;
	struct VMCache*		fCache;
	off_t				fOffset;
	uint32				fPageCount;
	int32				fMaxPages;
	status_t			fStatus;
	uint32				fVecCount;
	generic_io_vec		fVecs[32]; // TODO: make dynamic/configurable
};


class PageWriteWrapper {
public:
	PageWriteWrapper();
	~PageWriteWrapper();
	void SetTo(vm_page* page);
	bool Done(status_t result);

private:
	vm_page*			fPage;
	struct VMCache*		fCache;
	bool				fIsActive;
};


PageWriteWrapper::PageWriteWrapper()
	:
	fIsActive(false)
{
}


PageWriteWrapper::~PageWriteWrapper()
{
	if (fIsActive)
		panic("page write wrapper going out of scope but isn't completed");
}


/*!	The page's cache must be locked.
*/
void
PageWriteWrapper::SetTo(vm_page* page)
{
	DEBUG_PAGE_ACCESS_CHECK(page);

	if (page->busy)
		panic("setting page write wrapper to busy page");

	if (fIsActive)
		panic("re-setting page write wrapper that isn't completed");

	fPage = page;
	fCache = page->Cache();
	fIsActive = true;

	fPage->busy = true;
	fPage->busy_io = true;

	// We have a modified page -- however, while we're writing it back,
	// the page might still be mapped. In order not to lose any changes to the
	// page, we mark it clean before actually writing it back; if
	// writing the page fails for some reason, we'll just keep it in the
	// modified page list, but that should happen only rarely.

	// If the page is changed after we cleared the dirty flag, but before we
	// had the chance to write it back, then we'll write it again later -- that
	// will probably not happen that often, though.

	vm_clear_map_flags(fPage, PAGE_MODIFIED);
}


/*! Moves a previously modified page into a now appropriate queue.
	The page queues must not be locked.
*/
static void
move_page_to_appropriate_queue(vm_page *page)
{
	DEBUG_PAGE_ACCESS_CHECK(page);

	// Note, this logic must be in sync with what the page daemon does.
	int32 state;
	if (page->IsMapped())
		state = PAGE_STATE_ACTIVE;
	else if (page->modified)
		state = PAGE_STATE_MODIFIED;
	else
		state = PAGE_STATE_CACHED;

// TODO: If free + cached pages are low, we might directly want to free the
// page.
	vm_page_set_state(page, state);
}


/*!	The page's cache must be locked.
	The page queues must not be locked.
	\return \c true if the page was written successfully respectively could be
		handled somehow, \c false otherwise.
*/
bool
PageWriteWrapper::Done(status_t result)
{
	if (!fIsActive)
		panic("completing page write wrapper that is not active");

	DEBUG_PAGE_ACCESS_START(fPage);

	fPage->busy = false;
		// Set unbusy and notify later by hand.

	bool success = true;

	if (!fPage->busy_io) {
		// The busy_io flag was cleared. That means the cache tried to remove
		// the page while we were trying to write it. Let the cache handle the rest.
		fCache->FreeRemovedPage(fPage);
	} else if (result == B_OK) {
		// put it into the active/inactive queue
		move_page_to_appropriate_queue(fPage);
		fPage->busy_io = false;
		DEBUG_PAGE_ACCESS_END(fPage);

		fCache->NotifyPageEvents(fPage, PAGE_EVENT_NOT_BUSY);
	} else {
		// Writing the page failed -- mark the page modified and move it to
		// an appropriate queue other than the modified queue, so we don't
		// keep trying to write it over and over again. We keep
		// non-temporary pages in the modified queue, though, so they don't
		// get lost in the inactive queue.
		dprintf("PageWriteWrapper: Failed to write page %p: %s\n", fPage,
			strerror(result));

		fPage->modified = true;
		if (!fCache->temporary)
			vm_page_set_state(fPage, PAGE_STATE_MODIFIED);
		else if (fPage->IsMapped())
			vm_page_set_state(fPage, PAGE_STATE_ACTIVE);
		else
			vm_page_set_state(fPage, PAGE_STATE_INACTIVE);

		fPage->busy_io = false;
		DEBUG_PAGE_ACCESS_END(fPage);

		fCache->NotifyPageEvents(fPage, PAGE_EVENT_NOT_BUSY);
		success = false;
	}

	fIsActive = false;

	return success;
}


/*!	The page's cache must be locked.
*/
void
PageWriteTransfer::SetTo(PageWriterRun* run, vm_page* page, int32 maxPages)
{
	fRun = run;
	fCache = page->Cache();
	fOffset = page->cache_offset;
	fPageCount = 1;
	fMaxPages = maxPages;
	fStatus = B_OK;

	fVecs[0].base = (phys_addr_t)page->physical_page_number << PAGE_SHIFT;
	fVecs[0].length = B_PAGE_SIZE;
	fVecCount = 1;
}


/*!	The page's cache must be locked.
*/
bool
PageWriteTransfer::AddPage(vm_page* page)
{
	if (page->Cache() != fCache
		|| (fMaxPages >= 0 && fPageCount >= (uint32)fMaxPages))
		return false;

	phys_addr_t nextBase = fVecs[fVecCount - 1].base
		+ fVecs[fVecCount - 1].length;

	if ((phys_addr_t)page->physical_page_number << PAGE_SHIFT == nextBase
		&& (off_t)page->cache_offset == fOffset + fPageCount) {
		// append to last iovec
		fVecs[fVecCount - 1].length += B_PAGE_SIZE;
		fPageCount++;
		return true;
	}

	nextBase = fVecs[0].base - B_PAGE_SIZE;
	if ((phys_addr_t)page->physical_page_number << PAGE_SHIFT == nextBase
		&& (off_t)page->cache_offset == fOffset - 1) {
		// prepend to first iovec and adjust offset
		fVecs[0].base = nextBase;
		fVecs[0].length += B_PAGE_SIZE;
		fOffset = page->cache_offset;
		fPageCount++;
		return true;
	}

	if (((off_t)page->cache_offset == fOffset + fPageCount
			|| (off_t)page->cache_offset == fOffset - 1)
		&& fVecCount < sizeof(fVecs) / sizeof(fVecs[0])) {
		// not physically contiguous or not in the right order
		uint32 vectorIndex;
		if ((off_t)page->cache_offset < fOffset) {
			// we are pre-pending another vector, move the other vecs
			for (uint32 i = fVecCount; i > 0; i--)
				fVecs[i] = fVecs[i - 1];

			fOffset = page->cache_offset;
			vectorIndex = 0;
		} else
			vectorIndex = fVecCount;

		fVecs[vectorIndex].base
			= (phys_addr_t)page->physical_page_number << PAGE_SHIFT;
		fVecs[vectorIndex].length = B_PAGE_SIZE;

		fVecCount++;
		fPageCount++;
		return true;
	}

	return false;
}


status_t
PageWriteTransfer::Schedule(uint32 flags)
{
	off_t writeOffset = (off_t)fOffset << PAGE_SHIFT;
	generic_size_t writeLength = (phys_size_t)fPageCount << PAGE_SHIFT;

	if (fRun != NULL) {
		return fCache->WriteAsync(writeOffset, fVecs, fVecCount, writeLength,
			flags | B_PHYSICAL_IO_REQUEST, this);
	}

	status_t status = fCache->Write(writeOffset, fVecs, fVecCount,
		flags | B_PHYSICAL_IO_REQUEST, &writeLength);

	SetStatus(status, writeLength);
	return fStatus;
}


void
PageWriteTransfer::SetStatus(status_t status, size_t transferred)
{
	// only succeed if all pages up to the last one have been written fully
	// and the last page has at least been written partially
	if (status == B_OK && transferred <= (fPageCount - 1) * B_PAGE_SIZE)
		status = B_ERROR;

	fStatus = status;
}


void
PageWriteTransfer::IOFinished(status_t status, bool partialTransfer,
	generic_size_t bytesTransferred)
{
	SetStatus(status, bytesTransferred);
	fRun->PageWritten(this, fStatus, partialTransfer, bytesTransferred);
}


status_t
PageWriterRun::Init(uint32 maxPages, uint32 maxTransfers,
	uint32 allocationFlags, bool holdsReferences)
{
	fMaxPages = maxPages;
	fMaxTransfers = maxTransfers;
	fHoldsReferences = holdsReferences;
	fWrapperCount = 0;
	fTransferCount = 0;
	fPendingTransfers = 0;
	fNotified = 1;

	fWrappers = new(malloc_flags(allocationFlags)) PageWriteWrapper[maxPages];
	fTransfers
		= new(malloc_flags(allocationFlags)) PageWriteTransfer[maxTransfers];
	if (fWrappers == NULL || fTransfers == NULL)
		return B_NO_MEMORY;

	return B_OK;
}


PageWriterRun::~PageWriterRun()
{
	delete[] fWrappers;
	delete[] fTransfers;
}


void
PageWriterRun::PrepareNextRun()
{
	fWrapperCount = 0;
	fTransferCount = 0;
	fPendingTransfers = 0;
}


/*!	The page's cache must be locked.
	\return \c false when the run has no room for the page; then it is left
		alone.
*/
bool
PageWriterRun::AddPage(vm_page* page)
{
	if (fWrapperCount == fMaxPages)
		return false;

	if (fTransferCount == 0 || !fTransfers[fTransferCount - 1].AddPage(page)) {
		if (fTransferCount == fMaxTransfers)
			return false;
		fTransfers[fTransferCount++].SetTo(this, page,
			page->Cache()->MaxPagesPerAsyncWrite());
	}

	fWrappers[fWrapperCount++].SetTo(page);
	return true;
}


/*!	Schedules the writes of all pages previously added; Finish() waits for
	them. The same thread has to call both.
*/
void
PageWriterRun::Start(uint32 flags)
{
	atomic_set(&fPendingTransfers, fTransferCount);
	atomic_set(&fNotified, 0);

	fAllFinishedCondition.Init(this, "page writer wait for I/O");
	fAllFinishedCondition.Add(&fWaitEntry);

	if (fTransferCount == 0) {
		atomic_set(&fNotified, 1);
		fAllFinishedCondition.NotifyAll();
		return;
	}

	// schedule writes
	for (uint32 i = 0; i < fTransferCount; i++)
		fTransfers[i].Schedule(flags);
}


/*!	Waits for the writes Start() scheduled, and marks the pages depending on
	the outcome. A run that holds references has to release them afterwards
	(ReleaseReferences()).
	\return The number of pages that could not be written or otherwise handled.
*/
uint32
PageWriterRun::Finish()
{
	// wait until all pages have been written
	fWaitEntry.Wait();

	// The waiter is woken from inside NotifyAll(), which goes on using the
	// condition variable (its entry list, its spinlock) for a moment, on the
	// thread that completed the last transfer. This run is started again or
	// deleted next (vm_page_write_modified_page_range() does that at once):
	// re-initializing the variable under the notifier corrupted its lock and
	// let the old notification wake the next wait early, completing pages
	// still being written ("completing page write wrapper that is not
	// active", a fault in ~PageWriterRun()).
	while (atomic_get(&fNotified) == 0)
		cpu_pause();

	// mark pages depending on whether they could be written or not

	uint32 failedPages = 0;
	uint32 wrapperIndex = 0;
	for (uint32 i = 0; i < fTransferCount; i++) {
		PageWriteTransfer& transfer = fTransfers[i];
		transfer.Cache()->Lock();

		for (uint32 j = 0; j < transfer.PageCount(); j++) {
			if (!fWrappers[wrapperIndex++].Done(transfer.Status()))
				failedPages++;
		}

		transfer.Cache()->Unlock();
	}

	ASSERT(wrapperIndex == fWrapperCount);

	return failedPages;
}


/*!	Releases the cache and store references of a finished run's pages.
	This can be the last reference to a vnode, and then to its cache: file
	system locks may be waited for here.
*/
void
PageWriterRun::ReleaseReferences()
{
	for (uint32 i = 0; fHoldsReferences && i < fTransferCount; i++) {
		PageWriteTransfer& transfer = fTransfers[i];
		struct VMCache* cache = transfer.Cache();

		// We've acquired a references for each page
		for (uint32 j = 0; j < transfer.PageCount(); j++) {
			// We release the cache references after all pages were made
			// unbusy again - otherwise releasing a vnode could deadlock.
			cache->ReleaseStoreRef();
			cache->ReleaseRef();
		}
	}
}


void
PageWriterRun::PageWritten(PageWriteTransfer* transfer, status_t status,
	bool partialTransfer, size_t bytesTransferred)
{
	if (atomic_add(&fPendingTransfers, -1) == 1) {
		fAllFinishedCondition.NotifyAll();
		// the last access to this run
		atomic_set(&fNotified, 1);
	}
}


static vm_page*
next_modified_page(VMPageQueue& queue, page_num_t& maxPagesToSee)
{
	InterruptsSpinLocker locker(queue.GetLock());

	while (maxPagesToSee > 0) {
		vm_page* page = queue.Head();
		if (page == NULL)
			return NULL;

		queue.Requeue(page, true);

		maxPagesToSee--;

		if (!page->busy)
			return page;
	}

	return NULL;
}


/*!	The page writer continuously takes some pages from the modified
	queue, writes them back, and moves them back to the active queue.
	It runs in its own thread, and is only there to keep the number
	of modified pages low, so that more pages can be reused with
	fewer costs.
*/
status_t
ModifiedPageQueue::_PageWriter()
{
	ModifiedPageQueue& queue = *this;

	const uint32 kNumPages = 256;
#ifdef TRACE_VM_PAGE_WRITER
	uint32 writtenPages = 0;
	bigtime_t lastWrittenTime = 0;
	bigtime_t pageCollectionTime = 0;
#endif

	// Two runs: one is written while the next is collected and scheduled,
	// so that the device does not go idle between them.
	PageWriterRun runs[2];
	if (runs[0].Init(kNumPages, kNumPages, 0, true) != B_OK
		|| runs[1].Init(kNumPages, kNumPages, 0, true) != B_OK) {
		panic("page writer: Failed to init PageWriterRun!");
		return B_ERROR;
	}
	uint32 nextRun = 0;

	PageWriterRun* pendingRun = NULL;
	uint32 pendingPages = 0;
	page_num_t pendingPagesSeen = 0;
	bigtime_t pendingStart = 0;
	bigtime_t lastFinished = 0;

	page_num_t pagesSinceLastSuccessfulWrite = 0;

	// waits for the run in flight, and does all the cleanup
	auto finishPendingRun = [&]() {
		if (pendingRun == NULL)
			return;

		PageWriterRun* run = pendingRun;
		pendingRun = NULL;
		uint32 failedPages = run->Finish();

		if (failedPages == pendingPages)
			pagesSinceLastSuccessfulWrite += pendingPagesSeen;
		else
			pagesSinceLastSuccessfulWrite = 0;

		// (the part of its time it had the device to itself, or shared it
		// with the next run only)
		bigtime_t now = system_time();
		if (failedPages == 0 && pendingPages >= 8) {
			fLastAveragePageWriteDuration
				= (now - max_c(pendingStart, lastFinished)) / pendingPages;
		}
		lastFinished = now;

		// Those who wait for the quota hear of the written pages before the
		// references are released: the last reference to a vnode can make
		// us wait for a file system lock, which one of them may hold.
		if (!IsOverQuota())
			sUnderQuotaCondition.NotifyAll();

		run->ReleaseReferences();
	};

	while (fWriterThread >= 0) {
		// Report what this queue holds now, also when there is nothing to
		// write: the pages may have gone without us (freed, or moved to
		// another queue), and what we reported last counts against the
		// global quota until it is replaced. Those who wait for the quota,
		// on whichever queue, have to look again then.
		if (!IsOverQuota())
			sUnderQuotaCondition.NotifyAll();

		if (queue.Count() < kNumPages) {
			// nothing urgent: complete the writes before sleeping
			if (pendingRun != NULL) {
				finishPendingRun();
				continue;
			}

			// wait the full amount when no one triggers us
			if (!fPageWriterCondition.Wait(PAGES_FLUSH_DURATION_LOCAL_QUOTA, true))
				continue;
		}

		page_num_t modifiedPages = queue.Count();
		if (modifiedPages == 0) {
			_SetUnwritablePages(0);
			finishPendingRun();
			continue;
		}

		if (modifiedPages <= pagesSinceLastSuccessfulWrite) {
			// We ran through the whole queue without being able to write a
			// single page. Take a break.
			snooze(500000);
			pagesSinceLastSuccessfulWrite = 0;
		}

#if ENABLE_SWAP_SUPPORT
		const bool activePaging = vm_page_should_do_active_paging();
#endif

		// depending on how urgent it becomes to get pages to disk, we adjust
		// our I/O priority
		uint32 lowPagesState = low_resource_state(B_KERNEL_RESOURCE_PAGES);
		int32 ioPriority = B_IDLE_PRIORITY;
		if (lowPagesState >= B_LOW_RESOURCE_CRITICAL
			|| modifiedPages > MAX_PAGE_WRITER_IO_PRIORITY_THRESHOLD) {
			ioPriority = MAX_PAGE_WRITER_IO_PRIORITY;
		} else {
			ioPriority = (uint64)MAX_PAGE_WRITER_IO_PRIORITY * modifiedPages
				/ MAX_PAGE_WRITER_IO_PRIORITY_THRESHOLD;
		}

		thread_set_io_priority(ioPriority);

		uint32 numPages = 0;
		PageWriterRun& run = runs[nextRun];
		run.PrepareNextRun();

		// collect pages to be written
#ifdef TRACE_VM_PAGE_WRITER
		pageCollectionTime -= system_time();
#endif

		page_num_t maxPagesToSee = modifiedPages;

		// pages we cannot write as long as their vnode is on its way out
		page_num_t unwritablePages = 0;
		VMCache* unwritableCache = NULL;

		while (numPages < kNumPages && maxPagesToSee > 0) {
			vm_page *page = next_modified_page(queue, maxPagesToSee);
			if (page == NULL)
				break;

			// (wired pages included: they are taken out of the queue below)
			PageCacheLocker cacheLocker(page, false, false);
			if (!cacheLocker.IsLocked())
				continue;

			VMCache *cache = page->Cache();

			// If the page is busy or its state has changed while we were
			// locking the cache, just ignore it.
			if (page->busy || page->State() != PAGE_STATE_MODIFIED)
				continue;

			DEBUG_PAGE_ACCESS_START(page);

			// Don't write back wired (locked) pages.
			if (page->WiredCount() > 0) {
				vm_page_set_state(page, PAGE_STATE_ACTIVE);
				DEBUG_PAGE_ACCESS_END(page);
				continue;
			}

			// Write back temporary pages only when we're actively paging.
			if (cache->temporary
#if ENABLE_SWAP_SUPPORT
				&& (!activePaging
					|| !cache->CanWritePage(
							(off_t)page->cache_offset << PAGE_SHIFT))
#endif
				) {
				// We can't/don't want to do anything with this page, so move it
				// to one of the other queues.
				if (page->mappings.IsEmpty())
					vm_page_set_state(page, PAGE_STATE_INACTIVE);
				else
					vm_page_set_state(page, PAGE_STATE_ACTIVE);

				DEBUG_PAGE_ACCESS_END(page);
				continue;
			}

			// We need our own reference to the store, as it might currently be
			// destroyed. Then the page stays where it is: whoever frees the
			// vnode writes the page or drops it. That can take as long as a
			// file system lock is waited for, so such pages are counted (and
			// their vnode is asked for once only in a row of them).
			if (cache == unwritableCache
				|| cache->AcquireUnreferencedStoreRef() != B_OK) {
				DEBUG_PAGE_ACCESS_END(page);
				unwritableCache = cache;
				unwritablePages++;
				continue;
			}

			run.AddPage(page);

			DEBUG_PAGE_ACCESS_END(page);
			//dprintf("write page %p, cache %p (%ld)\n", page, page->cache, page->cache->ref_count);
			TPW(WritePage(page));

			cache->AcquireRefLocked();
			numPages++;

			// Write adjacent pages at the same time, if they're also modified.
			if (cache->temporary)
				continue;
			while (page->cache_next != NULL && numPages < kNumPages) {
				page = page->cache_next;
				if (page->busy || page->State() != PAGE_STATE_MODIFIED)
					break;
				if (page->WiredCount() > 0)
					break;

				DEBUG_PAGE_ACCESS_START(page);
				queue.RequeueUnlocked(page, true);
				run.AddPage(page);
				DEBUG_PAGE_ACCESS_END(page);

				cache->AcquireStoreRef();
				cache->AcquireRefLocked();
				numPages++;
				if (maxPagesToSee > 0)
					maxPagesToSee--;
			}
		}

#ifdef TRACE_VM_PAGE_WRITER
		pageCollectionTime += system_time();
#endif
		// What cannot be written does not count against the quota. (A run
		// that got full did not see the whole queue: then this is the least
		// there is.)
		_SetUnwritablePages(unwritablePages);

		if (numPages == 0) {
			if (pendingRun != NULL) {
				finishPendingRun();
				continue;
			}

			// Through the whole queue, and no page in it can be written now:
			// they are busy (being filled, or written by someone else) or
			// belong to vnodes that are being freed. With kNumPages or more
			// of them we would be back here at once, and go round at full
			// speed for as long as that takes, which is forever if what
			// keeps the pages waits for someone who waits for the quota
			// (airos-works/haiku#146). Those have been told above; rest.
			if (!IsOverQuota())
				sUnderQuotaCondition.NotifyAll();
			snooze(PAGE_WRITER_IDLE_PASS_BREAK);
			continue;
		}

		// write pages to disk, then finish the previous run while these are
		// written
		bigtime_t runStart = system_time();
		run.Start(B_VIP_IO_REQUEST);
		finishPendingRun();

		pendingRun = &run;
		pendingPages = numPages;
		pendingPagesSeen = modifiedPages - maxPagesToSee;
		pendingStart = runStart;
		nextRun ^= 1;

#ifdef TRACE_VM_PAGE_WRITER
		// debug output only...
		writtenPages += numPages;
		if (writtenPages >= 1024) {
			bigtime_t now = system_time();
			TRACE(("page writer: scheduled 1024 pages (total: %" B_PRIu64
				" ms, collect: %" B_PRIu64 " ms)\n",
				(now - lastWrittenTime) / 1000, pageCollectionTime / 1000));
			lastWrittenTime = now;

			writtenPages -= 1024;
			pageCollectionTime = 0;
		}
#endif
	}

	finishPendingRun();
	return B_OK;
}


/*static*/ status_t
ModifiedPageQueue::_WriterThreadEntry(void* _this)
{
	return ((ModifiedPageQueue*)_this)->_PageWriter();
}


//	#pragma mark - private kernel API


/*!	vm_page_write_modified_page_range() one transfer at a time, waited for:
	what it does when it cannot get the memory for more.
*/
static status_t
write_modified_page_range_synchronously(struct VMCache* cache,
	uint32 firstPage, uint32 endPage)
{
	static const int32 kMaxPages = 256;
	int32 maxPages = cache->MaxPagesPerWrite();
	if (maxPages < 0 || maxPages > kMaxPages)
		maxPages = kMaxPages;

	const uint32 allocationFlags = HEAP_DONT_WAIT_FOR_MEMORY
		| HEAP_DONT_LOCK_KERNEL_SPACE;

	PageWriteWrapper stackWrappersPool[2];
	PageWriteWrapper* stackWrappers[1];
	PageWriteWrapper* wrapperPool
		= new(malloc_flags(allocationFlags)) PageWriteWrapper[maxPages + 1];
	PageWriteWrapper** wrappers
		= new(malloc_flags(allocationFlags)) PageWriteWrapper*[maxPages];
	if (wrapperPool == NULL || wrappers == NULL) {
		// don't fail, just limit our capabilities
		delete[] wrapperPool;
		delete[] wrappers;
		wrapperPool = stackWrappersPool;
		wrappers = stackWrappers;
		maxPages = 1;
	}

	int32 nextWrapper = 0;
	int32 usedWrappers = 0;

	PageWriteTransfer transfer;
	bool transferEmpty = true;

	VMCachePagesTree::Iterator it
		= cache->pages.GetIterator(firstPage, true, true);

	while (true) {
		vm_page* page = it.Next();
		if (page == NULL || page->cache_offset >= endPage) {
			if (transferEmpty)
				break;

			page = NULL;
		}

		if (page != NULL) {
			if (page->busy
				|| (page->State() != PAGE_STATE_MODIFIED
					&& !vm_test_map_modification(page))) {
				page = NULL;
			}
		}

		PageWriteWrapper* wrapper = NULL;
		if (page != NULL) {
			wrapper = &wrapperPool[nextWrapper++];
			if (nextWrapper > maxPages)
				nextWrapper = 0;

			DEBUG_PAGE_ACCESS_START(page);

			wrapper->SetTo(page);

			if (transferEmpty || transfer.AddPage(page)) {
				if (transferEmpty) {
					transfer.SetTo(NULL, page, maxPages);
					transferEmpty = false;
				}

				DEBUG_PAGE_ACCESS_END(page);

				wrappers[usedWrappers++] = wrapper;
				continue;
			}

			DEBUG_PAGE_ACCESS_END(page);
		}

		if (transferEmpty)
			continue;

		cache->Unlock();
		status_t status = transfer.Schedule(0);
		cache->Lock();

		for (int32 i = 0; i < usedWrappers; i++)
			wrappers[i]->Done(status);

		usedWrappers = 0;

		if (page != NULL) {
			transfer.SetTo(NULL, page, maxPages);
			wrappers[usedWrappers++] = wrapper;
		} else
			transferEmpty = true;
	}

	if (wrapperPool != stackWrappersPool) {
		delete[] wrapperPool;
		delete[] wrappers;
	}

	return B_OK;
}


/*!	Writes a range of modified pages of a cache to disk.
	You need to hold the VMCache lock when calling this function.
	Note that the cache lock is released in this function.
	\param cache The cache.
	\param firstPage Offset (in page size units) of the first page in the range.
	\param endPage End offset (in page size units) of the page range. The page
		at this offset is not included.
*/
status_t
vm_page_write_modified_page_range(struct VMCache* cache, uint32 firstPage,
	uint32 endPage)
{
	// Off for now: on a 16-core Xeon with an NVMe disk the asynchronous runs
	// below still panicked under concurrent fsync() and sync() (a transfer of
	// a run being finished without its cache, after the notification race in
	// PageWriterRun was fixed): some completion still reaches a run that is
	// already finished or reused. Until that is found, fsync() and sync()
	// write one transfer at a time as before efe75687b7.
	static const bool kAsynchronousRuns = false;
	if (!kAsynchronousRuns)
		return write_modified_page_range_synchronously(cache, firstPage, endPage);

	// Asynchronous writes, in runs: one is written while the next is
	// collected and scheduled. (The caller's reference to the cache keeps
	// its store, which may be on its way out: no references of our own.)
	static const uint32 kMaxPages = 256;
	static const uint32 kMaxTransfers = 32;
	const uint32 allocationFlags = HEAP_DONT_WAIT_FOR_MEMORY
		| HEAP_DONT_LOCK_KERNEL_SPACE;

	PageWriterRun* runs = new(malloc_flags(allocationFlags)) PageWriterRun[2];
	if (runs == NULL
		|| runs[0].Init(kMaxPages, kMaxTransfers, allocationFlags, false)
			!= B_OK
		|| runs[1].Init(kMaxPages, kMaxTransfers, allocationFlags, false)
			!= B_OK) {
		delete[] runs;
		return write_modified_page_range_synchronously(cache, firstPage,
			endPage);
	}

	PageWriterRun* pendingRun = NULL;
	uint32 nextRun = 0;
	uint32 position = firstPage;
	bool atEnd = false;
	while (!atEnd) {
		PageWriterRun& run = runs[nextRun];
		run.PrepareNextRun();

		atEnd = true;
		VMCachePagesTree::Iterator it
			= cache->pages.GetIterator(position, true, true);
		while (vm_page* page = it.Next()) {
			if (page->cache_offset >= endPage)
				break;

			if (!page->busy && (page->State() == PAGE_STATE_MODIFIED
					|| vm_test_map_modification(page))) {
				DEBUG_PAGE_ACCESS_START(page);
				const bool added = run.AddPage(page);
				DEBUG_PAGE_ACCESS_END(page);
				if (!added) {
					// the run is full: the next one starts with this page
					atEnd = false;
					break;
				}
			}
			position = page->cache_offset + 1;
		}

		if (run.PageCount() == 0)
			break;

		cache->Unlock();
		run.Start(0);
		if (pendingRun != NULL)
			pendingRun->Finish();
		cache->Lock();

		pendingRun = &run;
		nextRun ^= 1;
	}

	if (pendingRun != NULL) {
		cache->Unlock();
		pendingRun->Finish();
		cache->Lock();
	}

	delete[] runs;
	return B_OK;
}


/*!	You need to hold the VMCache lock when calling this function.
	Note that the cache lock is released in this function.
*/
status_t
vm_page_write_modified_pages(VMCache *cache)
{
	return vm_page_write_modified_page_range(cache, 0,
		(cache->virtual_end + B_PAGE_SIZE - 1) >> PAGE_SHIFT);
}


/*!	Schedules the page writer to write back the specified \a page.
	Note, however, that it might not do this immediately, and it can well
	take several seconds until the page is actually written out.
*/
void
vm_page_schedule_write_page(vm_page *page)
{
	ASSERT(page->State() == PAGE_STATE_MODIFIED);

	VMPageQueue* queue = NULL;
	vm_page_requeue(page, false, &queue);

	((ModifiedPageQueue*)queue)->NotifyWriter();
}


/*!	Cache must be locked.
*/
void
vm_page_schedule_write_page_range(struct VMCache *cache, uint32 firstPage,
	uint32 endPage)
{
	VMPageQueue* queue = NULL;

	uint32 modified = 0;
	for (VMCachePagesTree::Iterator it
				= cache->pages.GetIterator(firstPage, true, true);
			vm_page *page = it.Next();) {
		if (page->cache_offset >= endPage)
			break;

		if (!page->busy && page->State() == PAGE_STATE_MODIFIED) {
			DEBUG_PAGE_ACCESS_START(page);
			vm_page_requeue(page, false, &queue);
			modified++;
			DEBUG_PAGE_ACCESS_END(page);
		}
	}

	if (modified > 0)
		((ModifiedPageQueue*)queue)->NotifyWriter();
}


ModifiedPageQueue::~ModifiedPageQueue()
{
	thread_id writerThread = fWriterThread;
	if (writerThread < 0)
		return;

	fWriterThread = -1;
	fPageWriterCondition.WakeUp();
	wait_for_thread(writerThread, NULL);
}


status_t
ModifiedPageQueue::StartWriter(const char* name)
{
	if (sGlobalEstimatedWriteDuration == -1) {
		sUnderQuotaCondition.Init(this, "pguq");
		sGlobalEstimatedWriteDuration = 0;
	}

	fPageWriterCondition.Init("pgwr");
	fLastAveragePageWriteDuration = 0;
	fUnwritablePages = 0;

	char threadName[B_OS_NAME_LENGTH];
	snprintf(threadName, sizeof(threadName), "page writer: %s", name);
	fWriterThread = spawn_kernel_thread(&_WriterThreadEntry, threadName,
		B_NORMAL_PRIORITY + 1, this);
	if (fWriterThread < 0)
		return fWriterThread;

	return resume_thread(fWriterThread);
}


void
ModifiedPageQueue::_SetUnwritablePages(page_num_t count)
{
	InterruptsSpinLocker _(fLock);
	fUnwritablePages = count;
}


/*!	Whether writing out the queue's pages, and \a additionalPages more, is
	estimated to take longer than the quota allows.

	Only pages the page writer can write count: those it found it cannot
	(their vnode is being freed) leave the queue when someone else is done
	with them, and that someone may be waiting, directly or not, for a thread
	that waits for the quota with a file system lock held.

	For the same reason a queue with nothing to write is never over its own
	quota, whatever \a additionalPages come to: the estimate is as good as
	the last run was typical (a single one that the device stalled in can put
	it at seconds for a handful of pages), and nothing is written that would
	replace it as long as nobody may add a page.
*/
bool
ModifiedPageQueue::IsOverQuota(page_num_t additionalPages)
{
	InterruptsSpinLocker _(fLock);

	// (pages the page writer could not write may have left the queue since
	// it looked: what was added after that counts in full)
	if (fUnwritablePages > fCount)
		fUnwritablePages = fCount;
	const page_num_t writablePages = fCount - fUnwritablePages;

	bigtime_t estimatedWriteDuration
		= (writablePages * fLastAveragePageWriteDuration);
	if ((int64)fCount != fLastReportedModifiedCount
			|| estimatedWriteDuration != fLastReportedEstimatedWriteDuration) {
		atomic_add64(&sGlobalModifiedCount, fCount - fLastReportedModifiedCount);
		fLastReportedModifiedCount = fCount;

		atomic_add64(&sGlobalEstimatedWriteDuration,
			estimatedWriteDuration - fLastReportedEstimatedWriteDuration);
		fLastReportedEstimatedWriteDuration = estimatedWriteDuration;
	}

	if (writablePages == 0) {
		// only what the other queues hold can be too much
		return atomic_get64(&sGlobalEstimatedWriteDuration)
			> PAGES_FLUSH_DURATION_GLOBAL_QUOTA;
	}

	bigtime_t additionalPagesDuration = fLastAveragePageWriteDuration * additionalPages;
	if ((estimatedWriteDuration + additionalPagesDuration)
			> PAGES_FLUSH_DURATION_LOCAL_QUOTA)
		return true;

	return ((atomic_get64(&sGlobalEstimatedWriteDuration) + additionalPagesDuration)
		> PAGES_FLUSH_DURATION_GLOBAL_QUOTA);
}


status_t
ModifiedPageQueue::WaitIfOverQuota(page_num_t additionalPages,
	bigtime_t timeout, uint32 flags)
{
	if ((flags & B_RELATIVE_TIMEOUT) != 0) {
		// Convert to an absolute timeout, so that we can wait multiple times if needed.
		timeout += system_time();
		flags = (flags & ~B_RELATIVE_TIMEOUT) | B_ABSOLUTE_TIMEOUT;
	}

	while (IsOverQuota(additionalPages)) {
		ConditionVariableEntry waitEntry;
		sUnderQuotaCondition.Add(&waitEntry);

		if (!IsOverQuota(additionalPages))
			return B_OK;

		fPageWriterCondition.WakeUp();
		status_t status = waitEntry.Wait(flags, timeout);
		if (status != B_OK)
			return status;

		// The queue itself is now under-quota, but it may still be over
		// when considering additionalPages. So, we loop again.
	}

	return B_OK;
}
