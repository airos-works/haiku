/*
 * Copyright 2007, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "PageCacheLocker.h"

#include <vm/VMCache.h>


/*!	\param ignoreWired Whether pages someone has wired are left alone as
		well. The page writer wants them, to take them out of its queue.
*/
bool
PageCacheLocker::_IgnorePage(vm_page* page, bool ignoreWired)
{
	if (page->busy || page->State() == PAGE_STATE_WIRED
		|| page->State() == PAGE_STATE_FREE || page->State() == PAGE_STATE_CLEAR
		|| page->State() == PAGE_STATE_UNUSED
		|| (ignoreWired && page->WiredCount() > 0))
		return true;

	return false;
}


bool
PageCacheLocker::Lock(vm_page* page, bool dontWait, bool ignoreWired)
{
	if (_IgnorePage(page, ignoreWired))
		return false;

	// Grab a reference to this cache.
	VMCache* cache = vm_cache_acquire_locked_page_cache(page, dontWait);
	if (cache == NULL)
		return false;

	if (_IgnorePage(page, ignoreWired)) {
		cache->ReleaseRefAndUnlock();
		return false;
	}

	fPage = page;
	return true;
}


void
PageCacheLocker::Unlock()
{
	if (fPage == NULL)
		return;

	fPage->Cache()->ReleaseRefAndUnlock();

	fPage = NULL;
}
