/*
 * Copyright 2022 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include "PMAPPhysicalPageMapper.h"


status_t
PMAPPhysicalPageMapper::GetPage(
	phys_addr_t physicalAddress, addr_t* _virtualAddress, void** _handle)
{
	ASSERT(physicalAddress < KERNEL_PMAP_SIZE);

	*_virtualAddress = KERNEL_PMAP_BASE + physicalAddress;
	*_handle = NULL;

	return B_OK;
}


status_t
PMAPPhysicalPageMapper::PutPage(addr_t virtualAddress, void* handle)
{
	return B_OK;
}


/*!	Zeroes [start, start + length) of the physical map with DC ZVA, a block
	of DCZID_EL0's size per instruction, without reading the memory first
	(#237). The physical map is Normal memory throughout (RAM, and the frame
	buffer as Normal non-cacheable), where DC ZVA is allowed; it faults on
	device memory, which is why the general memset() does not use it.
	Returns false when the CPU does not allow it.
*/
static bool
zero_with_dc_zva(uint8* start, size_t length)
{
	uint64 dczid = READ_SPECIALREG(DCZID_EL0);
	if ((dczid & (1 << 4)) != 0)
		return false;
		// DZP: prohibited

	size_t blockSize = 4UL << (dczid & 0xf);
	if (blockSize > 2048 || length < 2 * blockSize)
		return false;

	uint8* end = start + length;
	uint8* blocks = (uint8*)ROUNDUP((addr_t)start, blockSize);
	uint8* blocksEnd = (uint8*)ROUNDDOWN((addr_t)end, blockSize);

	if (blocks != start)
		memset(start, 0, blocks - start);
	for (uint8* block = blocks; block < blocksEnd; block += blockSize)
		asm volatile("dc zva, %0" :: "r"(block) : "memory");
	if (blocksEnd != end)
		memset(blocksEnd, 0, end - blocksEnd);
	return true;
}


status_t
PMAPPhysicalPageMapper::MemsetPhysical(phys_addr_t address, int value, phys_size_t length)
{
	ASSERT(address < KERNEL_PMAP_SIZE);
	uint8* start = reinterpret_cast<uint8*>(KERNEL_PMAP_BASE + address);
	if (value != 0 || !zero_with_dc_zva(start, length))
		memset(start, value, length);

	return B_OK;
}

status_t
PMAPPhysicalPageMapper::MemcpyFromPhysical(void* to, phys_addr_t from, size_t length, bool user)
{
	ASSERT(from < KERNEL_PMAP_SIZE);

	if (!user) {
		memcpy(to, reinterpret_cast<void*>(KERNEL_PMAP_BASE + from), length);
		return B_OK;
	} else {
		return user_memcpy(to, reinterpret_cast<void*>(KERNEL_PMAP_BASE + from), length);
	}
}


status_t
PMAPPhysicalPageMapper::MemcpyToPhysical(phys_addr_t to, const void* from, size_t length, bool user)
{
	ASSERT(to < KERNEL_PMAP_SIZE);

	if (!user) {
		memcpy(reinterpret_cast<void*>(KERNEL_PMAP_BASE + to), from, length);
		return B_OK;
	} else {
		return user_memcpy(reinterpret_cast<void*>(KERNEL_PMAP_BASE + to), from, length);
	}

	return B_OK;
}


void
PMAPPhysicalPageMapper::MemcpyPhysicalPage(phys_addr_t to, phys_addr_t from)
{
	ASSERT(to < KERNEL_PMAP_SIZE);
	ASSERT(from < KERNEL_PMAP_SIZE);
	memcpy(reinterpret_cast<void*>(KERNEL_PMAP_BASE + to),
		reinterpret_cast<void*>(KERNEL_PMAP_BASE + from), B_PAGE_SIZE);
}
