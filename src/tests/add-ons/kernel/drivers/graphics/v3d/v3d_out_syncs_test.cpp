/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Tests how the v3d driver reads the sync objects a job signals from a
	submit request (read_out_syncs(), #193): the single out_sync, one
	multi-sync extension, and chains no Mesa sends - two multi-sync
	extensions, one whose "next" points back at itself, too many entries,
	unknown extensions, addresses outside the program's memory. None may
	write more than MAX_OUT_SYNCS entries.
	Builds for Haiku (jam v3d_out_syncs_test) and on any host:
	  g++ -std=c++17 -g -fsanitize=address,undefined \
	      -I<haiku>/headers/os/support -I<haiku>/headers/private \
	      -I<haiku>/src/add-ons/kernel/drivers/graphics/v3d \
	      v3d_out_syncs_test.cpp -o v3d_out_syncs_test && ./v3d_out_syncs_test
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef __HAIKU__
// what the headers under test use of SupportDefs.h
#	define _SUPPORT_DEFS_H
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
typedef unsigned long long uint64;
typedef int int32;
typedef long long int64;
typedef int status_t;
enum {
	B_OK = 0,
	B_BAD_VALUE = -1000,
	B_BAD_ADDRESS,
	B_NOT_SUPPORTED
};
#else
#	include <SupportDefs.h>
#endif

#include <graphics/v3d/v3d_drm.h>


// The submitting program's memory: sUserMemory, at kUserBase.
static const uint64 kUserBase = 0x10000;
static const size_t kUserSize = 0x4000;
static uint8* sUserMemory;


static status_t
v3d_copy_from_user(void* to, uint64 address, size_t size)
{
	if (address < kUserBase || address - kUserBase > kUserSize
		|| size > kUserSize - (address - kUserBase)) {
		return B_BAD_ADDRESS;
	}
	memcpy(to, sUserMemory + (address - kUserBase), size);
	return B_OK;
}

#include "v3d_out_syncs.h"


static int sFailures = 0;
static int sChecks = 0;

#define CHECK(condition) \
	do { \
		sChecks++; \
		if (!(condition)) { \
			sFailures++; \
			fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, \
				#condition); \
		} \
	} while (0)


static const uint32 kGuard = 0xdeadbeef;

// As in v3d_control(): an array of MAX_OUT_SYNCS entries. On the heap, so
// that the address sanitizer of a host build sees a write past it; the
// guards are for where there is none.
struct OutSyncs {
	uint32*	guarded;
	uint32*	entries;
	uint32	count;

	OutSyncs()
	{
		guarded = (uint32*)malloc((MAX_OUT_SYNCS + 2) * sizeof(uint32));
		guarded[0] = kGuard;
		guarded[MAX_OUT_SYNCS + 1] = kGuard;
		entries = guarded + 1;
		memset(entries, 0, MAX_OUT_SYNCS * sizeof(uint32));
		count = 0;
	}

	~OutSyncs()
	{
		free(guarded);
	}

	bool Intact() const
	{
		return guarded[0] == kGuard && guarded[MAX_OUT_SYNCS + 1] == kGuard
			&& count <= MAX_OUT_SYNCS;
	}
};


/*!	Puts a multi-sync extension with \a count out_syncs (handles \a first
	and up) at \a offset of the program's memory, its entries at
	\a entriesOffset, and returns its address.
*/
static uint64
put_multi_sync(size_t offset, size_t entriesOffset, uint32 count,
	uint32 first, uint64 next)
{
	drm_v3d_multi_sync sync;
	memset(&sync, 0, sizeof(sync));
	sync.base.next = next;
	sync.base.id = DRM_V3D_EXT_ID_MULTI_SYNC;
	sync.out_syncs = kUserBase + entriesOffset;
	sync.out_sync_count = count;
	memcpy(sUserMemory + offset, &sync, sizeof(sync));

	for (uint32 i = 0; i < count
			&& entriesOffset + (i + 1) * sizeof(drm_v3d_sem) <= kUserSize;
			i++) {
		drm_v3d_sem semaphore;
		memset(&semaphore, 0, sizeof(semaphore));
		semaphore.handle = first + i;
		memcpy(sUserMemory + entriesOffset + i * sizeof(drm_v3d_sem),
			&semaphore, sizeof(semaphore));
	}
	return kUserBase + offset;
}


int
main()
{
	sUserMemory = (uint8*)calloc(1, kUserSize);

	// no extension: the request's own out_sync, if any
	{
		OutSyncs syncs;
		CHECK(read_out_syncs(0, 0, 5, syncs.entries, syncs.count) == B_OK);
		CHECK(syncs.count == 1 && syncs.entries[0] == 5);
		CHECK(syncs.Intact());
	}
	{
		OutSyncs syncs;
		CHECK(read_out_syncs(0, 0, 0, syncs.entries, syncs.count) == B_OK);
		CHECK(syncs.count == 0 && syncs.Intact());
	}
	{
		// the flag without a chain
		OutSyncs syncs;
		CHECK(read_out_syncs(DRM_V3D_SUBMIT_EXTENSION, 0, 5, syncs.entries,
			syncs.count) == B_OK);
		CHECK(syncs.count == 0 && syncs.Intact());
	}

	// one multi-sync extension
	{
		OutSyncs syncs;
		uint64 chain = put_multi_sync(0, 0x1000, 3, 100, 0);
		CHECK(read_out_syncs(DRM_V3D_SUBMIT_EXTENSION, chain, 5,
			syncs.entries, syncs.count) == B_OK);
		CHECK(syncs.count == 3 && syncs.entries[0] == 100
			&& syncs.entries[1] == 101 && syncs.entries[2] == 102);
		CHECK(syncs.Intact());
	}
	{
		// as many entries as there is room for, and one more
		OutSyncs syncs;
		uint64 chain = put_multi_sync(0, 0x1000, MAX_OUT_SYNCS, 100, 0);
		CHECK(read_out_syncs(DRM_V3D_SUBMIT_EXTENSION, chain, 0,
			syncs.entries, syncs.count) == B_OK);
		CHECK(syncs.count == MAX_OUT_SYNCS
			&& syncs.entries[MAX_OUT_SYNCS - 1] == 100 + MAX_OUT_SYNCS - 1);
		CHECK(syncs.Intact());
	}
	{
		OutSyncs syncs;
		uint64 chain = put_multi_sync(0, 0x1000, MAX_OUT_SYNCS + 1, 100, 0);
		CHECK(read_out_syncs(DRM_V3D_SUBMIT_EXTENSION, chain, 0,
			syncs.entries, syncs.count) == B_BAD_VALUE);
		CHECK(syncs.Intact());
	}
	{
		OutSyncs syncs;
		uint64 chain = put_multi_sync(0, 0x1000, 0xffffffffu, 100, 0);
		CHECK(read_out_syncs(DRM_V3D_SUBMIT_EXTENSION, chain, 0,
			syncs.entries, syncs.count) == B_BAD_VALUE);
		CHECK(syncs.Intact());
	}

	// two multi-sync extensions in one chain: 32 entries for an array of 16
	{
		OutSyncs syncs;
		uint64 second = put_multi_sync(0x100, 0x2000, MAX_OUT_SYNCS, 200, 0);
		uint64 chain = put_multi_sync(0, 0x1000, MAX_OUT_SYNCS, 100, second);
		CHECK(read_out_syncs(DRM_V3D_SUBMIT_EXTENSION, chain, 0,
			syncs.entries, syncs.count) == B_BAD_VALUE);
		CHECK(syncs.Intact());
	}
	{
		// also when both would fit
		OutSyncs syncs;
		uint64 second = put_multi_sync(0x100, 0x2000, 2, 200, 0);
		uint64 chain = put_multi_sync(0, 0x1000, 2, 100, second);
		CHECK(read_out_syncs(DRM_V3D_SUBMIT_EXTENSION, chain, 0,
			syncs.entries, syncs.count) == B_BAD_VALUE);
		CHECK(syncs.Intact());
	}

	// an extension whose "next" is itself: 128 entries before
	{
		OutSyncs syncs;
		uint64 chain = put_multi_sync(0, 0x1000, MAX_OUT_SYNCS, 100,
			kUserBase);
		CHECK(read_out_syncs(DRM_V3D_SUBMIT_EXTENSION, chain, 0,
			syncs.entries, syncs.count) == B_BAD_VALUE);
		CHECK(syncs.Intact());
	}
	{
		// ... and with no entries at all, a chain that would never end
		OutSyncs syncs;
		uint64 chain = put_multi_sync(0, 0x1000, 0, 100, kUserBase);
		CHECK(read_out_syncs(DRM_V3D_SUBMIT_EXTENSION, chain, 0,
			syncs.entries, syncs.count) == B_BAD_VALUE);
		CHECK(syncs.Intact());
	}

	// extensions the driver does not know
	{
		OutSyncs syncs;
		drm_v3d_extension extension;
		memset(&extension, 0, sizeof(extension));
		extension.id = DRM_V3D_EXT_ID_CPU_INDIRECT_CSD;
		memcpy(sUserMemory, &extension, sizeof(extension));
		CHECK(read_out_syncs(DRM_V3D_SUBMIT_EXTENSION, kUserBase, 0,
			syncs.entries, syncs.count) == B_NOT_SUPPORTED);
		CHECK(syncs.Intact());
	}

	// addresses that are not the program's
	{
		OutSyncs syncs;
		CHECK(read_out_syncs(DRM_V3D_SUBMIT_EXTENSION, 0x1000, 0,
			syncs.entries, syncs.count) == B_BAD_ADDRESS);
		CHECK(read_out_syncs(DRM_V3D_SUBMIT_EXTENSION, kUserBase + kUserSize
			- 4, 0, syncs.entries, syncs.count) == B_BAD_ADDRESS);
		CHECK(syncs.Intact());
	}
	{
		// the entries end outside
		OutSyncs syncs;
		uint64 chain = put_multi_sync(0, kUserSize - 2 * sizeof(drm_v3d_sem),
			4, 100, 0);
		CHECK(read_out_syncs(DRM_V3D_SUBMIT_EXTENSION, chain, 0,
			syncs.entries, syncs.count) == B_BAD_ADDRESS);
		CHECK(syncs.Intact());
	}
	{
		// the second extension is nowhere
		OutSyncs syncs;
		uint64 chain = put_multi_sync(0, 0x1000, 2, 100, 0x800);
		CHECK(read_out_syncs(DRM_V3D_SUBMIT_EXTENSION, chain, 0,
			syncs.entries, syncs.count) == B_BAD_ADDRESS);
		CHECK(syncs.Intact());
	}

	free(sUserMemory);

	printf("v3d_out_syncs_test: %d checks, %d failed\n", sChecks, sFailures);
	return sFailures == 0 ? 0 : 1;
}
