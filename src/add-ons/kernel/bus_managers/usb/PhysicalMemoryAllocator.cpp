/*
 * Copyright 2006-2026, Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */

#include <malloc.h>
#include <string.h>
#include <KernelExport.h>
#include <heap.h>
#include <SupportDefs.h>
#include <util/AutoLock.h>
#include <util/kernel_cpp.h>
#include <vm/vm.h>

#include "PhysicalMemoryAllocator.h"


//#define TRACE_PHYSICAL_MEMORY_ALLOCATOR
#ifdef TRACE_PHYSICAL_MEMORY_ALLOCATOR
#define TRACE(x)		dprintf x
#define TRACE_ERROR(x)	dprintf x
#else
#define TRACE(x)		/* nothing */
#define TRACE_ERROR(x)	dprintf x
#endif


// The pool grows by chunks of this many blocks of the largest size, each a
// contiguous area of its own, up to the maximum its owner gives. Booting from
// a USB disk with a keyboard and a tablet had at most 145 KiB in use at once
// and a few hundred bytes once the desktop was up; the pools used to take
// their whole maximum (8 MiB of contiguous low memory and 2 MiB of
// bookkeeping each) at once.
static const uint32 kChunkBlocks = 1;

// In the kernel debugger, transfers come from a reserve of 64 small slots at
// the end of the first chunk, as nothing can be allocated or locked there.
static const uint32 kDebugChunkSize = 128;
static const size_t kDebugReserveSize = 64 * kDebugChunkSize;


struct PhysicalMemoryAllocator::Chunk {
	Chunk*			next;
	area_id			area;
	uint8*			logicalBase;
	phys_addr_t		physicalBase;
	size_t			usedBytes;
	size_t*			arrayOffset;
	uint8**			array;
};


PhysicalMemoryAllocator::PhysicalMemoryAllocator(const char *name,
	size_t minSize, size_t maxSize, uint32 maxBlockCount, bool uncached)
	:	fStatus(B_NO_INIT),
		fUncached(uncached),
		fChunks(NULL),
		fChunkCount(0),
		fMaxChunkCount(0),
		fChunkSize(0),
		fGrowing(false),
		fDebugLogicalBase(NULL),
		fDebugPhysicalBase(0),
		fDebugChunkSize(kDebugChunkSize),
		fDebugUseMap(0)
{
	fName = strdup(name);
	mutex_init_etc(&fLock, fName, MUTEX_FLAG_CLONE_NAME);

	fArrayCount = 1;
	size_t biggestSize = minSize;
	while (biggestSize < maxSize) {
		fArrayCount++;
		biggestSize *= 2;
	}

	fBlockSize = (size_t *)malloc(fArrayCount * sizeof(size_t));
	fArrayLength = (size_t *)malloc(fArrayCount * sizeof(size_t));
	if (fName == NULL || fBlockSize == NULL || fArrayLength == NULL)
		return;

	size_t arraySlots = biggestSize / minSize * kChunkBlocks;
	for (int32 i = 0; i < fArrayCount; i++) {
		fBlockSize[i] = minSize << i;
		fArrayLength[i] = arraySlots;
		arraySlots /= 2;
	}

	fChunkSize = biggestSize * kChunkBlocks;
	fMaxChunkCount = (max_c(maxBlockCount, 1) + kChunkBlocks - 1)
		/ kChunkBlocks;

	fNoMemoryCondition.Init(this, "USB PMA");
	fStatus = B_OK;
}


PhysicalMemoryAllocator::~PhysicalMemoryAllocator()
{
	mutex_lock(&fLock);

	while (fChunks != NULL) {
		Chunk* chunk = fChunks;
		fChunks = chunk->next;
		delete_area(chunk->area);
		free(chunk);
	}

	free(fArrayLength);
	free(fBlockSize);
	free(fName);

	mutex_destroy(&fLock);
}


area_id
PhysicalMemoryAllocator::AllocateArea(const char* name, size_t size,
	void** logicalAddress, phys_addr_t* physicalAddress, bool uncached,
	uint32 flags)
{
	if (size == 0 || size > SIZE_MAX - (B_PAGE_SIZE - 1))
		return B_BAD_VALUE;
#ifndef __aarch64__
	if (uncached)
		return B_NOT_SUPPORTED;
#endif
	void* address;
	size = (size + B_PAGE_SIZE - 1) & ~(B_PAGE_SIZE - 1);
	virtual_address_restrictions virtualRestrictions = {};
	virtualRestrictions.address_specification = B_ANY_KERNEL_ADDRESS;
	physical_address_restrictions physicalRestrictions = {};
	area_id area = create_area_etc(B_SYSTEM_TEAM, name, size,
		B_32_BIT_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, flags, 0,
		&virtualRestrictions, &physicalRestrictions, &address);
	if (area < B_OK)
		return area;
	physical_entry entry;
	status_t status = get_memory_map(address, size, &entry, 1);
	if (status == B_OK && (entry.size < size || entry.address > UINT32_MAX
			|| size - 1 > UINT32_MAX - entry.address)) {
		status = B_BAD_ADDRESS;
	}
#ifdef __aarch64__
	if (status == B_OK && uncached) {
		// create_area() zeroes RAM through a cached mapping. Remove those
		// lines before changing the private DMA mapping to Normal Non-cacheable.
		// The pool must only be accessed through this mapping until it is freed.
		uint64 ctr;
		asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
		const size_t lineSize = 4UL << ((ctr >> 16) & 15);
		for (addr_t p = (addr_t)address; p < (addr_t)address + size; p += lineSize)
			asm volatile("dc civac, %0" :: "r"(p) : "memory");
		asm volatile("dsb sy" ::: "memory");
		// On ARM64 B_UNCACHED_MEMORY is Device memory (no unaligned access).
		// B_WRITE_COMBINING_MEMORY selects Normal Non-cacheable RAM instead.
		status = vm_set_area_memory_type(area, entry.address,
			B_WRITE_COMBINING_MEMORY);
		memory_full_barrier();
	}
#endif
	if (status != B_OK) {
		delete_area(area);
		return status;
	}
	memset(address, 0, size);
	*logicalAddress = address;
	*physicalAddress = entry.address;
	return area;
}


/*!	Creates a chunk of the pool with its bookkeeping. It is called where
	transfers are submitted, also for the I/O that is to free memory: it must
	not wait for memory, the caller waits for a free block instead.
*/
PhysicalMemoryAllocator::Chunk*
PhysicalMemoryAllocator::_CreateChunk(bool debugReserve)
{
	size_t bookkeeping = sizeof(Chunk)
		+ fArrayCount * (sizeof(size_t) + sizeof(uint8*));
	for (int32 i = 0; i < fArrayCount; i++)
		bookkeeping += fArrayLength[i];

	Chunk* chunk = (Chunk*)malloc_etc(bookkeeping,
		HEAP_DONT_WAIT_FOR_MEMORY | HEAP_PRIORITY_VIP);
	if (chunk == NULL)
		return NULL;
	memset(chunk, 0, bookkeeping);

	chunk->arrayOffset = (size_t*)(chunk + 1);
	chunk->array = (uint8**)(chunk->arrayOffset + fArrayCount);
	uint8* array = (uint8*)(chunk->array + fArrayCount);
	for (int32 i = 0; i < fArrayCount; i++) {
		chunk->array[i] = array;
		chunk->arrayOffset[i] = fArrayLength[i] - 1;
		array += fArrayLength[i];
	}

	void* logicalBase;
	chunk->area = AllocateArea(fName,
		fChunkSize + (debugReserve ? kDebugReserveSize : 0), &logicalBase,
		&chunk->physicalBase, fUncached,
		CREATE_AREA_DONT_WAIT | CREATE_AREA_PRIORITY_VIP);
	if (chunk->area < B_OK) {
		TRACE_ERROR(("PMA: \"%s\" could not grow: %s\n", fName,
			strerror(chunk->area)));
		free(chunk);
		return NULL;
	}

	chunk->logicalBase = (uint8*)logicalBase;
	return chunk;
}


/*!	Takes a free block of fBlockSize[arrayToUse] bytes from \a chunk, if it
	has one. Called with the lock held.
*/
bool
PhysicalMemoryAllocator::_AllocateFromChunk(Chunk* chunk, int32 arrayToUse,
	size_t& offset)
{
	if (fChunkSize - chunk->usedBytes < fBlockSize[arrayToUse])
		return false;

	// look for a free slot, from the one after the last one taken
	uint8* targetArray = chunk->array[arrayToUse];
	const size_t arrayLength = fArrayLength[arrayToUse];
	size_t start = chunk->arrayOffset[arrayToUse] + 1;
	if (start >= arrayLength)
		start = 0;
	uint8* slot = (uint8*)memchr(targetArray + start, 0, arrayLength - start);
	if (slot == NULL)
		slot = (uint8*)memchr(targetArray, 0, start);
	if (slot == NULL)
		return false;

	const size_t i = slot - targetArray;
	chunk->arrayOffset[arrayToUse] = i;
	chunk->usedBytes += fBlockSize[arrayToUse];

	// fill upwards to the smallest block
	size_t fillSize = 1;
	size_t arrayIndex = i;
	for (int32 j = arrayToUse; j >= 0; j--) {
		memset(&chunk->array[j][arrayIndex], 1, fillSize);
		fillSize <<= 1;
		arrayIndex <<= 1;
	}

	// fill downwards to the biggest block
	arrayIndex = i >> 1;
	for (int32 j = arrayToUse + 1; j < fArrayCount; j++) {
		chunk->array[j][arrayIndex]++;
		if (chunk->array[j][arrayIndex] > 1)
			break;

		arrayIndex >>= 1;
	}

	offset = fBlockSize[arrayToUse] * i;
	return true;
}


status_t
PhysicalMemoryAllocator::Allocate(size_t size, void **logicalAddress,
	phys_addr_t *physicalAddress)
{
	if (debug_debugger_running()) {
		if (size > fDebugChunkSize) {
			kprintf("usb allocation of %" B_PRIuSIZE
				" does not fit debug chunk size %" B_PRIu32 "!\n",
				size, fDebugChunkSize);
			return B_NO_MEMORY;
		}
		if (fDebugLogicalBase == NULL)
			return B_NO_MEMORY;

		for (size_t i = 0; i < sizeof(fDebugUseMap) * 8; i++) {
			uint64 mask = 1LL << i;
			if ((fDebugUseMap & mask) == 0) {
				fDebugUseMap |= mask;
				*logicalAddress = fDebugLogicalBase + i * fDebugChunkSize;
				*physicalAddress = fDebugPhysicalBase + i * fDebugChunkSize;
				return B_OK;
			}
		}

		return B_NO_MEMORY;
	}

	if (size == 0 || size > fBlockSize[fArrayCount - 1]) {
		TRACE_ERROR(("PMA: bad value for allocate (%ld bytes)\n", size));
		return B_BAD_VALUE;
	}

	int32 arrayToUse = 0;
	while (fBlockSize[arrayToUse] < size)
		arrayToUse++;

	MutexLocker locker(&fLock);
	if (!locker.IsLocked())
		return B_ERROR;

	const bigtime_t limit = system_time() + 2 * 1000 * 1000;
	while (true) {
		TRACE(("PMA: will use array %ld (blocksize: %ld) to allocate %ld bytes\n", arrayToUse, fBlockSize[arrayToUse], size));
		for (Chunk* chunk = fChunks; chunk != NULL; chunk = chunk->next) {
			size_t offset;
			if (_AllocateFromChunk(chunk, arrayToUse, offset)) {
				*logicalAddress = chunk->logicalBase + offset;
				*physicalAddress = chunk->physicalBase + offset;
				return B_OK;
			}
		}

		if (!fGrowing && fChunkCount < fMaxChunkCount) {
			// Add a chunk. Frees go on meanwhile; other allocations wait
			// for it or for a free block.
			const bool first = fChunks == NULL;
			fGrowing = true;
			locker.Unlock();

			Chunk* chunk = _CreateChunk(first);

			if (!locker.Lock())
				return B_ERROR;
			fGrowing = false;

			if (chunk != NULL) {
				// The earlier chunks are searched first: append it.
				Chunk** last = &fChunks;
				while (*last != NULL)
					last = &(*last)->next;
				*last = chunk;
				fChunkCount++;

				if (first) {
					fDebugLogicalBase = chunk->logicalBase + fChunkSize;
					fDebugPhysicalBase = chunk->physicalBase + fChunkSize;
				}

				fNoMemoryCondition.NotifyAll();
				continue;
			}
		}

		// no slot found, we need to wait some being Deallocate()'d

		const bigtime_t now = system_time();
		if (now >= limit)
			break;

		ConditionVariableEntry entry;
		fNoMemoryCondition.Add(&entry);

		const bool growing = fGrowing;
		locker.Unlock();

		if (!growing) {
			TRACE_ERROR(("PMA: found no free slot to store %ld bytes, waiting\n",
				size));
		}

		entry.Wait(B_RELATIVE_TIMEOUT, min_c(limit - now, 1000 * 1000));

		if (!locker.Lock())
			return B_ERROR;
	}

	TRACE_ERROR(("PMA: timed out waiting for a free slot, giving up\n"));
	return B_NO_MEMORY;
}


status_t
PhysicalMemoryAllocator::Deallocate(size_t size, void *logicalAddress,
	phys_addr_t physicalAddress)
{
	if (debug_debugger_running()) {
		uint32 index = ((uint8 *)logicalAddress - fDebugLogicalBase)
			/ fDebugChunkSize;
		fDebugUseMap &= ~(1LL << index);
		return B_OK;
	}

	if (size == 0 || size > fBlockSize[fArrayCount - 1]) {
		TRACE_ERROR(("PMA: bad value for deallocate (%ld bytes)\n", size));
		return B_BAD_VALUE;
	}

	if (logicalAddress == NULL && physicalAddress == 0) {
		TRACE_ERROR(("PMA: no value given for either physical or logical address\n"));
		return B_BAD_VALUE;
	}

	int32 arrayToUse = 0;
	while (fBlockSize[arrayToUse] < size)
		arrayToUse++;

	MutexLocker locker(&fLock);
	if (!locker.IsLocked())
		return B_ERROR;

	Chunk* chunk = fChunks;
	size_t offset = 0;
	for (; chunk != NULL; chunk = chunk->next) {
		if (logicalAddress != NULL) {
			offset = (addr_t)logicalAddress - (addr_t)chunk->logicalBase;
			if ((addr_t)logicalAddress >= (addr_t)chunk->logicalBase
				&& offset < fChunkSize) {
				break;
			}
		} else {
			offset = physicalAddress - chunk->physicalBase;
			if (physicalAddress >= chunk->physicalBase
				&& offset < fChunkSize) {
				break;
			}
		}
	}

	if (chunk == NULL) {
		TRACE_ERROR(("PMA: provided address is not in the pool\n"));
		return B_BAD_VALUE;
	}

	uint32 index = offset / fBlockSize[arrayToUse];

	TRACE(("PMA: will use array %ld (index: %ld) to deallocate %ld bytes\n", arrayToUse, index, size));

	if (chunk->array[arrayToUse][index] == 0) {
		TRACE_ERROR(("PMA: address was not allocated!\n"));
		return B_BAD_VALUE;
	}

	// clear upwards to the smallest block
	size_t fillSize = 1;
	size_t arrayIndex = index;
	for (int32 i = arrayToUse; i >= 0; i--) {
		memset(&chunk->array[i][arrayIndex], 0, fillSize);
		fillSize <<= 1;
		arrayIndex <<= 1;
	}

	// clear downwards to the biggest block
	arrayIndex = index >> 1;
	for (int32 i = arrayToUse + 1; i < fArrayCount; i++) {
		chunk->array[i][arrayIndex]--;
		if (chunk->array[i][arrayIndex] > 0)
			break;

		arrayIndex >>= 1;
	}

	chunk->usedBytes -= fBlockSize[arrayToUse];
	fNoMemoryCondition.NotifyAll();

	return B_OK;
}
