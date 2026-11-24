/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "sdhci.h"

#include <arch/atomic.h>
#include <driver_settings.h>
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
sdhci_allocate_dma(area_id* area, void** buffer, phys_addr_t* address,
	size_t size)
{
#if defined(__aarch64__)
	// the payload, or (with \a size) a page for its ADMA2 descriptors
	const size_t kSdhciDmaSize = size != 0 ? size : ::kSdhciDmaSize;
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = UINT64_C(0x100000000);
	physicalRestrictions.alignment = kSdhciDmaSize;
	physicalRestrictions.boundary = kSdhciDmaSize;
	void* memory;
	area_id id = create_area_etc(B_SYSTEM_TEAM, size != 0
			? "SDHCI ADMA descriptors" : "SDHCI DMA payload", kSdhciDmaSize,
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
	if (size == 0)
		return B_NOT_SUPPORTED;
	// x86: only an ADMA2 descriptor table, in ordinary (coherent) memory
	// below 4 GiB, which 32-bit ADMA2 can address
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = UINT64_C(0x100000000);
	void* memory;
	area_id id = create_area_etc(B_SYSTEM_TEAM, "SDHCI ADMA descriptors",
		B_PAGE_SIZE, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		0, 0, &virtualRestrictions, &physicalRestrictions, &memory);
	if (id < B_OK)
		return id;
	physical_entry entry;
	status_t status = get_memory_map(memory, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK || entry.address > UINT64_C(0x100000000) - B_PAGE_SIZE) {
		delete_area(id);
		return status == B_OK ? B_BAD_DATA : status;
	}
	*area = id;
	*buffer = memory;
	*address = entry.address;
	return B_OK;
#endif
}


sdhci_dma_mode
sdhci_read_dma_mode()
{
	// Both are off until they have run on the boards' own hosts: the ROCK 5
	// boots from its eMMC through this driver (airos-works/haiku#181).
	sdhci_dma_mode mode = { false, false };
	void* handle = load_driver_settings("sdhci");
	if (handle != NULL) {
		mode.gather = get_driver_boolean_parameter(handle, "gather_io", false,
			true);
		mode.adma2 = get_driver_boolean_parameter(handle, "adma2", false,
			true);
		unload_driver_settings(handle);
	}
#if defined(__aarch64__)
	// every transfer goes through the private buffer
	mode.adma2 = false;
#else
	mode.gather = false;
#endif
	return mode;
}


sdhci_dma_limits
sdhci_get_dma_limits(const sdhci_dma_mode& mode)
{
	if (mode.gather) {
		// pieces of whole blocks, copied together into the buffer: a
		// command as long as the buffer
		return { (uint32)(kSdhciDmaSize / B_PAGE_SIZE),
			(uint32)(kSdhciDmaSize / 512), (uint32)(kSdhciDmaSize / 512) };
	}
	if (mode.adma2) {
		// one descriptor per piece; a controller without ADMA2 gets one
		// SDMA command per piece instead (SdhciBus::DoIO())
		return { kSdhciAdmaMaxSegments, kSdhciAdmaMaxLength / 512,
			kSdhciAdmaMaxTransfer / 512 };
	}
	// one piece of memory per command, as long as SDMA can take it within
	// the 512 KiB boundary
	return { 1, (1 << 10) - 1, (1 << 10) - 1 };
}
