/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "VramAllocator.h"
#include <stdlib.h>

bool
VramAllocator::Init(uint64_t size)
{
	if (bits != NULL || size == 0 || (size & 4095) != 0 || size > (1ULL << 40))
		return false;
	pages = size >> 12;
	bits = (uint32_t*)calloc((pages + 31) / 32, sizeof(uint32_t));
	allocated = 0;
	return bits != NULL;
}

void
VramAllocator::Uninit()
{
	free(bits);
	bits = NULL;
	pages = allocated = 0;
}

bool
VramAllocator::Reserve(uint64_t offset, uint64_t size)
{
	if (bits == NULL || offset > pages * 4096 || size > pages * 4096 - offset)
		return false;
	if (size == 0)
		return true;
	uint64_t end = (offset + size + 4095) >> 12;
	for (uint64_t page = offset >> 12; page < end; page++)
		bits[page / 32] |= 1u << (page % 32);
	return true;
}

bool
VramAllocator::Allocate(uint64_t size, uint64_t alignment, uint64_t limit,
	uint64_t& offset, uint64_t minimum)
{
	if (bits == NULL || size == 0 || (size & 4095) != 0 || alignment < 4096
		|| (alignment & (alignment - 1)) != 0 || alignment > (1ULL << 40)
		|| (minimum & 4095) != 0 || minimum > limit || minimum > pages * 4096)
		return false;
	uint64_t count = size >> 12;
	uint64_t maxPage = limit >> 12;
	if (maxPage > pages)
		maxPage = pages;
	if (count > maxPage)
		return false;
	uint64_t alignPages = alignment >> 12;
	uint64_t begin = ((minimum >> 12) + alignPages - 1) & ~(alignPages - 1);
	while (begin <= maxPage - count) {
		uint64_t i = 0;
		while (i < count && (bits[(begin + i) / 32] & (1u << ((begin + i) % 32))) == 0)
			i++;
		if (i == count) {
			offset = begin << 12;
			Reserve(offset, size);
			allocated += size;
			return true;
		}
		begin = (begin + i + alignPages) & ~(alignPages - 1);
	}
	return false;
}

bool
VramAllocator::Free(uint64_t offset, uint64_t size)
{
	if (bits == NULL || (offset & 4095) != 0 || size == 0 || (size & 4095) != 0
		|| offset > pages * 4096 || size > pages * 4096 - offset || size > allocated)
		return false;
	uint64_t begin = offset >> 12;
	uint64_t count = size >> 12;
	for (uint64_t i = 0; i < count; i++) {
		if ((bits[(begin + i) / 32] & (1u << ((begin + i) % 32))) == 0)
			return false;
	}
	for (uint64_t i = 0; i < count; i++)
		bits[(begin + i) / 32] &= ~(1u << ((begin + i) % 32));
	allocated -= size;
	return true;
}
