/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <sys/auxv.h>

#include <errno.h>

#include <OS.h>

#include <commpage_defs.h>
#include <errno_private.h>


extern "C" const void* __gCommPageAddress;


extern "C" unsigned long
getauxval(unsigned long type)
{
	switch (type) {
		case AT_PAGESZ:
			return B_PAGE_SIZE;

#ifdef __aarch64__
		case AT_HWCAP:
		case AT_HWCAP2:
		{
			// The kernel puts the features all CPUs share into the commpage;
			// an older one leaves the entry empty: no features known.
			const addr_t* commPage = (const addr_t*)__gCommPageAddress;
			if (commPage == NULL || commPage[COMMPAGE_ENTRY_ARM64_HWCAPS] == 0)
				return 0;

			const uint64* caps = (const uint64*)((addr_t)commPage
				+ commPage[COMMPAGE_ENTRY_ARM64_HWCAPS]);
			return caps[type == AT_HWCAP ? 0 : 1];
		}
#endif
	}

	__set_errno(ENOENT);
	return 0;
}
