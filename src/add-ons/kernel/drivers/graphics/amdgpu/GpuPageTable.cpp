/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "GpuPageTable.h"
#include <stdlib.h>

bool
GpuPageTable::Range(uint64_t address, uint32_t pages)
{
	return pages != 0 && (address & 4095) == 0 && address < kSize
		&& (uint64_t)pages <= (kSize - address) / 4096;
}

bool
GpuPageTable::ValidEntry(uint64_t entry)
{
	return (entry & 1) != 0 && (entry & ~(kPhysicalMask | 0x77)) == 0
		&& (entry & 0x60) != 0 && ((entry & 0x10) == 0 || (entry & 0x20) != 0);
}

bool
GpuPageTable::Initialize(void* newOwner, Allocate newAllocate, Release newRelease)
{
	if (leaves != NULL || directory.cpu != NULL || newAllocate == NULL || newRelease == NULL)
		return false;
	owner = newOwner;
	allocate = newAllocate;
	release = newRelease;
	leaves = (Allocation*)calloc(kDirectoryEntries, sizeof(Allocation));
	if (leaves == NULL)
		return false;
	if (!allocate(owner, kDirectoryBytes, directory)) {
		free(leaves);
		leaves = NULL;
		return false;
	}
	for (uint32_t i = 0; i < kDirectoryEntries; i++)
		directory.cpu[i] = 0;
	mappedPages = leafCount = 0;
	return true;
}

uint64_t
GpuPageTable::Entry(uint64_t address) const
{
	if (leaves == NULL || address >= kSize)
		return 0;
	const Allocation& leaf = leaves[address >> 21];
	return leaf.cpu == NULL ? 0 : leaf.cpu[(address >> 12) & 511];
}

GpuPageTable::Result
GpuPageTable::Map(uint64_t address, const uint64_t* entries, uint32_t pages)
{
	if (leaves == NULL || entries == NULL || !Range(address, pages))
		return INVALID;
	for (uint32_t i = 0; i < pages; i++) {
		if (!ValidEntry(entries[i]))
			return INVALID;
		if (Entry(address + (uint64_t)i * 4096) != 0)
			return OVERLAP;
	}
	const uint32_t first = address >> 21;
	const uint32_t last = (address + (uint64_t)pages * 4096 - 1) >> 21;
	for (uint32_t index = first; index <= last; index++) {
		Allocation& leaf = leaves[index];
		if (leaf.cpu != NULL)
			continue;
		if (!allocate(owner, 4096, leaf)) {
			// New leaves have not been published. Failure is atomic, even
			// when this range also touches existing leaves.
			for (uint32_t undo = first; undo < index; undo++) {
				if (leaves[undo].cpu != NULL && directory.cpu[undo] == 0) {
					release(owner, 4096, leaves[undo], true);
					leaves[undo] = {};
				}
			}
			return NO_MEMORY;
		}
		for (uint32_t i = 0; i < 512; i++)
			leaf.cpu[i] = 0;
	}
	for (uint32_t i = 0; i < pages; i++) {
		uint64_t page = (address >> 12) + i;
		leaves[page >> 9].cpu[page & 511] = entries[i];
	}
	__sync_synchronize();
	for (uint32_t index = first; index <= last; index++) {
		if (directory.cpu[index] == 0) {
			directory.cpu[index] = leaves[index].gpu | 1;
			leafCount++;
		}
	}
	mappedPages += pages;
	return OK;
}

bool
GpuPageTable::Unmap(uint64_t address, uint32_t pages)
{
	if (leaves == NULL || !Range(address, pages))
		return false;
	for (uint32_t i = 0; i < pages; i++) {
		if (Entry(address + (uint64_t)i * 4096) == 0)
			return false;
	}
	for (uint32_t i = 0; i < pages; i++) {
		uint64_t page = (address >> 12) + i;
		leaves[page >> 9].cpu[page & 511] = 0;
	}
	__sync_synchronize();
	const uint32_t first = address >> 21;
	const uint32_t last = (address + (uint64_t)pages * 4096 - 1) >> 21;
	for (uint32_t index = first; index <= last; index++) {
		bool empty = true;
		for (uint32_t i = 0; i < 512; i++)
			empty &= leaves[index].cpu[i] == 0;
		if (empty) {
			directory.cpu[index] = 0;
			__sync_synchronize();
			release(owner, 4096, leaves[index], true);
			leaves[index] = {};
			leafCount--;
		}
	}
	mappedPages -= pages;
	return true;
}

void
GpuPageTable::Uninitialize(bool reclaim)
{
	if (leaves == NULL)
		return;
	for (uint32_t i = 0; i < kDirectoryEntries; i++) {
		if (leaves[i].cpu != NULL)
			release(owner, 4096, leaves[i], reclaim);
	}
	release(owner, kDirectoryBytes, directory, reclaim);
	free(leaves);
	leaves = NULL;
	directory = {};
	mappedPages = leafCount = 0;
}
