/*
 * Copyright 2006-2026, Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */
#ifndef _PHYSICAL_MEMORY_ALLOCATOR_H_
#define _PHYSICAL_MEMORY_ALLOCATOR_H_


#include <condition_variable.h>
#include <SupportDefs.h>
#include <lock.h>


class PhysicalMemoryAllocator {
public:
									PhysicalMemoryAllocator(const char *name,
										size_t minSize,
										size_t maxSize,
										uint32 maxBlockCount,
										bool uncached = false);
									~PhysicalMemoryAllocator();

		status_t					InitCheck() { return fStatus; }

static	area_id					AllocateArea(const char* name, size_t size,
										void** logicalAddress,
										phys_addr_t* physicalAddress,
										bool uncached, uint32 flags = 0);

		status_t					Allocate(size_t size,
										void **logicalAddress,
										phys_addr_t *physicalAddress);

		// one of both addresses needs to be provided, the other may be NULL
		status_t					Deallocate(size_t size,
										void *logicalAddress,
										phys_addr_t physicalAddress);

private:
		struct Chunk;

		Chunk*						_CreateChunk(bool debugReserve);
		bool						_AllocateFromChunk(Chunk* chunk,
										int32 arrayToUse, size_t& offset);

		char						*fName;
		status_t					fStatus;
		bool						fUncached;

		mutex						fLock;
		Chunk*						fChunks;
		uint32						fChunkCount;
		uint32						fMaxChunkCount;
		size_t						fChunkSize;
		bool						fGrowing;

		int32						fArrayCount;
		size_t						*fBlockSize;
		size_t						*fArrayLength;

		ConditionVariable			fNoMemoryCondition;

		uint8						*fDebugLogicalBase;
		phys_addr_t					fDebugPhysicalBase;
		uint32						fDebugChunkSize;
		uint64						fDebugUseMap;
};

#endif // !_PHYSICAL_MEMORY_ALLOCATOR_H_
