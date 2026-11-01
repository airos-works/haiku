/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_PAGE_TABLE_H
#define MALI_CSF_PAGE_TABLE_H

#include "CsfVm.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

namespace MaliCSF {

struct PageTableRegion {
	uint64_t address;
	uint64_t bytes;
	uint64_t offset;
	uint64_t bufferBytes;
	const uint64_t* pages;
	uint32_t flags;
};

// Build an immutable four-level, 4 KiB GPU page-table generation. Both backing
// buffers and the page-table pages may be physically scattered. The owner must
// retain the entire generation until hardware has finished using its root.
// CPU mappings/cache maintenance and hardware activation belong to the caller.
class GpuPageTable {
public:
	static bool ValidRange(uint64_t address, uint64_t bytes, uint64_t limit)
	{
		return bytes != 0 && ((address | bytes) & 4095) == 0
			&& address < limit && bytes <= limit - address;
	}
	static bool ValidPermissions(uint32_t flags)
	{
		return (flags & ~7u) == 0 && (flags & (kVmReadOnly | kVmNoExecute)) != 0;
	}
	static bool ValidPhysical(uint64_t address)
	{
		return (address & 4095) == 0 && address < (UINT64_C(1) << 40);
	}
	static unsigned CountPages(const PageTableRegion* regions, unsigned count,
		uint64_t limit)
	{
		if (count > kMaxVmMappings || (count != 0 && regions == NULL)
			|| limit == 0 || limit > (UINT64_C(1) << 48) || (limit & 4095) != 0)
			return 0;
		unsigned tables = 1;
		uint64_t previousEnd = 0, mappedBytes = 0;
		uint64_t prefixes[3] = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
		for (unsigned i = 0; i < count; i++) {
			const PageTableRegion& region = regions[i];
			if (!ValidRange(region.address, region.bytes, limit)
				|| region.address < previousEnd || region.pages == NULL
				|| ((region.offset | region.bufferBytes) & 4095) != 0
				|| region.offset > region.bufferBytes
				|| region.bytes > region.bufferBytes - region.offset
				|| !ValidPermissions(region.flags)
				|| region.bytes > kMaxVmMappedBytes - mappedBytes)
				return 0;
			mappedBytes += region.bytes;
			previousEnd = region.address + region.bytes;
			for (uint64_t p = region.offset / 4096;
				p < (region.offset + region.bytes) / 4096; p++) {
				if (!ValidPhysical(region.pages[p]))
					return 0;
			}
			// Sorted ranges make each newly occupied prefix a new table. Holes
			// allocate nothing; ranges within the same 2 MiB window share a leaf.
			for (uint64_t chunk = region.address >> 21;
				chunk <= (previousEnd - 1) >> 21; chunk++) {
				const uint64_t next[3] = {chunk >> 18, chunk >> 9, chunk};
				for (unsigned level = 0; level < 3; level++) {
					if (next[level] != prefixes[level]) {
						prefixes[level] = next[level];
						if (++tables > kMaxVmTablePages)
							return 0;
					}
				}
			}
		}
		return tables;
	}
	static int ComparePhysical(const void* a, const void* b)
	{
		uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
		return x < y ? -1 : x > y ? 1 : 0;
	}
	static bool Build(void* data, size_t bytes, const uint64_t* physicalPages,
		unsigned tablePages, const PageTableRegion* regions, unsigned count, uint64_t limit)
	{
		unsigned required = CountPages(regions, count, limit);
		if (required == 0 || data == NULL || (uintptr_t(data) & 4095) != 0
			|| physicalPages == NULL || tablePages != required || bytes != required * 4096)
			return false;
		// No page may hold two tables. A sorted copy checks that in n log n:
		// pairwise comparison took milliseconds once a VM maps a few GiB.
		uint64_t* sorted = (uint64_t*)malloc(required * sizeof(uint64_t));
		if (sorted == NULL)
			return false;
		for (unsigned i = 0; i < required; i++) {
			if (!ValidPhysical(physicalPages[i])) {
				free(sorted);
				return false;
			}
			sorted[i] = physicalPages[i];
		}
		qsort(sorted, required, sizeof(uint64_t), ComparePhysical);
		for (unsigned i = 1; i < required; i++) {
			if (sorted[i] == sorted[i - 1]) {
				free(sorted);
				return false;
			}
		}
		free(sorted);
		memset(data, 0, bytes);
		uint64_t* tables[4] = {(uint64_t*)data, NULL, NULL, NULL};
		uint64_t prefixes[3] = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
		unsigned next = 1;
		for (unsigned i = 0; i < count; i++) {
			const PageTableRegion& region = regions[i];
			uint64_t attributes = 3 | (1u << 10) | (1u << 6);
			if ((region.flags & kVmReadOnly) != 0) attributes |= 1u << 7;
			if ((region.flags & kVmNoExecute) != 0) attributes |= UINT64_C(3) << 53;
			attributes |= (region.flags & kVmUncached) != 0 ? (2u << 8) : (1u << 2) | (3u << 8);
			for (uint64_t offset = 0; offset < region.bytes; offset += 4096) {
				uint64_t address = region.address + offset;
				for (unsigned level = 1; level < 4; level++) {
					unsigned shift = 48 - level * 9;
					uint64_t prefix = address >> shift;
					if (prefix == prefixes[level - 1]) continue;
					prefixes[level - 1] = prefix;
					tables[level] = (uint64_t*)((uint8_t*)data + next * 4096);
					tables[level - 1][prefix & 511] = physicalPages[next++] | 3;
				}
				tables[3][(address >> 12) & 511]
					= region.pages[(region.offset + offset) / 4096] | attributes;
			}
		}
		return next == required;
	}
};

} // namespace MaliCSF
#endif
