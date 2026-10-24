/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "sdhci.h"

#include <arch/atomic.h>
#include <vm/vm.h>
#if defined(__aarch64__)
#include <arch/arm64/arch_kernel.h>
#include <arch/arm64/cache_line_size.h>

static void
evict_allocation(void* memory, size_t size)
{
	// Allocation zeroing used cacheable RAM. Evict those lines before
	// converting this private mapping to Normal Non-cacheable memory.
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	size_t lineSize = arm64_data_cache_line_size(ctr);
	for (addr_t p = (addr_t)memory; p < (addr_t)memory + size; p += lineSize)
		asm volatile("dc civac, %0" :: "r"(p) : "memory");
	memory_full_barrier();
}
#endif


void
sdhci_clean_dma_alias(phys_addr_t address, size_t size)
{
#if defined(__aarch64__)
	// as the NVMe driver's nvme_dma_clean_alias()
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	size_t lineSize = arm64_data_cache_line_size(ctr);
	addr_t start = (KERNEL_PMAP_BASE + address) & ~(addr_t)(lineSize - 1);
	addr_t end = KERNEL_PMAP_BASE + address + size;
	for (addr_t p = start; p < end; p += lineSize)
		asm volatile("dc civac, %0" :: "r"(p) : "memory");
	memory_full_barrier();
#endif
}


status_t
sdhci_allocate_dma(area_id* area, void** buffer, phys_addr_t* address)
{
#if defined(__aarch64__)
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = UINT64_C(0x100000000);
	physicalRestrictions.alignment = kSdhciDmaSize;
	physicalRestrictions.boundary = kSdhciDmaSize;
	void* memory;
	area_id id = create_area_etc(B_SYSTEM_TEAM, "SDHCI DMA payload", kSdhciDmaSize,
		B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &memory);
	if (id < B_OK)
		return id;
	physical_entry entry;
	status_t status = get_memory_map(memory, kSdhciDmaSize, &entry, 1);
	if (status != B_OK || entry.size < kSdhciDmaSize
		|| (entry.address & (kSdhciDmaSize - 1)) != 0
		|| entry.address > UINT64_C(0x100000000) - kSdhciDmaSize) {
		delete_area(id);
		return status == B_OK ? B_BAD_DATA : status;
	}
	evict_allocation(memory, kSdhciDmaSize);
	status = vm_set_area_memory_type(id, entry.address, B_WRITE_COMBINING_MEMORY);
	if (status != B_OK) {
		delete_area(id);
		return status;
	}
	memory_full_barrier();
	*area = id;
	*buffer = memory;
	*address = entry.address;
	dprintf("sdhci: noncacheable DMA payload %#" B_PRIxPHYSADDR ", %" B_PRIuSIZE " bytes\n",
		entry.address, kSdhciDmaSize);
	return B_OK;
#else
	return B_NOT_SUPPORTED;
#endif
}
