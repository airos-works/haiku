/*
 * Copyright 2004-2009, Marcus Overhagen. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "util.h"

#include <KernelExport.h>
#include <arch/atomic.h>
#include <OS.h>
#include <vm/vm.h>
#include <string.h>
#include <driver_settings.h>
#if defined(__aarch64__)
#include <arch/arm64/arch_kernel.h>
#include <arch/arm64/cache_line_size.h>
#endif


#define TRACE(a...) dprintf("ahci: " a)
#define ERROR(a...) dprintf("ahci: " a)


static inline uint32
round_to_pagesize(uint32 size)
{
	return (size + B_PAGE_SIZE - 1) & ~(B_PAGE_SIZE - 1);
}


area_id
alloc_mem(void **virt, phys_addr_t *phy, size_t size, uint32 protection,
	const char *name, bool supports64Bit)
{
	physical_entry pe;
	void * virtadr;
	area_id areaid;
	status_t rv;

	TRACE("allocating %ld bytes for %s\n", size, name);

	if (size == 0 || size > SIZE_MAX - (B_PAGE_SIZE - 1))
		return B_BAD_VALUE;
	size = (size + B_PAGE_SIZE - 1) & ~(size_t)(B_PAGE_SIZE - 1);
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	if (!supports64Bit)
		physicalRestrictions.high_address = UINT64_C(0x100000000);
	areaid = create_area_etc(B_SYSTEM_TEAM, name, size, B_CONTIGUOUS,
		protection, 0, 0, &virtualRestrictions, &physicalRestrictions, &virtadr);
	if (areaid < B_OK) {
		ERROR("couldn't allocate area %s\n", name);
		return B_ERROR;
	}
	rv = get_memory_map(virtadr, size, &pe, 1);
	if (rv < B_OK || pe.size < size
		|| pe.address > ~(phys_addr_t)0 - (size - 1)
		|| (!supports64Bit && pe.address + size - 1 > UINT32_MAX)) {
		delete_area(areaid);
		ERROR("couldn't get mapping for %s\n", name);
		return B_ERROR;
	}
#if defined(__aarch64__)
	// Allocation zeroing used a cached mapping. Evict it before converting the
	// private DMA area to Normal Non-cacheable RAM. Device memory cannot serve
	// payloads because ordinary copies may make unaligned accesses.
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	size_t lineSize = arm64_data_cache_line_size(ctr);
	for (addr_t p = (addr_t)virtadr; p < (addr_t)virtadr + size; p += lineSize)
		asm volatile("dc civac, %0" :: "r"(p) : "memory");
	memory_full_barrier();
	rv = vm_set_area_memory_type(areaid, pe.address, B_WRITE_COMBINING_MEMORY);
	if (rv != B_OK) {
		delete_area(areaid);
		return rv;
	}
	memory_full_barrier();
#endif
	if (virt)
		*virt = virtadr;
	if (phy)
		*phy = pe.address;
	TRACE("area = %" B_PRId32 ", size = %ld, virt = %p, phy = %#" B_PRIxPHYSADDR "\n",
		areaid, size, virtadr, pe.address);
	return areaid;
}


area_id
map_mem(void **virt, phys_addr_t phy, size_t size, uint32 protection,
	const char *name)
{
	uint32 offset;
	phys_addr_t phyadr;
	void *mapadr;
	area_id area;

	TRACE("mapping physical address %#" B_PRIxPHYSADDR " with %" B_PRIuSIZE
		" bytes for %s\n", phy, size, name);

	offset = phy & (B_PAGE_SIZE - 1);
	phyadr = phy - offset;
	size = round_to_pagesize(size + offset);
	area = map_physical_memory(name, phyadr, size,
		B_ANY_KERNEL_BLOCK_ADDRESS | B_UNCACHED_MEMORY, protection, &mapadr);
	if (area < B_OK) {
		ERROR("mapping '%s' failed, error 0x%" B_PRIx32 " (%s)\n", name,
			area, strerror(area));
		return area;
	}

	*virt = (char *)mapadr + offset;

	TRACE("physical = %#" B_PRIxPHYSADDR ", virtual = %p, offset = %"
		B_PRId32 ", phyadr = %#" B_PRIxPHYSADDR ", mapadr = %p, size = %"
		B_PRIuSIZE ", area = 0x%08" B_PRIx32 "\n", phy, *virt, offset, phyadr,
		mapadr, size, area);

	return area;
}


status_t
sg_memcpy(const physical_entry *sgTable, int sgCount, const void *data,
	size_t dataSize)
{
	if (sgTable == NULL || data == NULL) {
		if (dataSize == 0)
			return B_OK;
		return B_ERROR;
	}
	int i;
	for (i = 0; i < sgCount && dataSize > 0; i++) {
		size_t size = min_c(dataSize, sgTable[i].size);

		TRACE("sg_memcpy phyAddr %#" B_PRIxPHYSADDR ", size %lu\n",
			sgTable[i].address, size);

		vm_memcpy_to_physical(sgTable[i].address, data, size, false);

		data = (char *)data + size;
		dataSize -= size;
	}
	if (dataSize != 0)
		return B_ERROR;
	return B_OK;
}


void
swap_words(void *data, size_t size)
{
	uint16 *word = (uint16*)data;
	size_t count = size / 2;
	while (count--) {
		*word = (*word << 8) | (*word >> 8);
		word++;
	}
}


#if defined(__aarch64__)
static int32 sDirectDMA = -1;
	// not yet read
#endif


bool
ahci_direct_dma(void)
{
#if defined(__aarch64__)
	if (atomic_get(&sDirectDMA) < 0) {
		// Off by default until it has run on a board that is not coherent
		// (the ROCK 5): a wrong cache maintenance corrupts data.
		void *settings = load_driver_settings("ahci");
		bool direct = settings != NULL && get_driver_boolean_parameter(
			settings, "direct_dma", false, true);
		if (settings != NULL)
			unload_driver_settings(settings);
		// The SCSI manager's DMA buffers align to at most a block: a CPU
		// that does not give its granule (or a larger one) keeps bouncing.
		uint64 ctr;
		asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
		uint32 granule = (ctr >> 24) & 0xf;
		if (direct && (granule == 0 || (4u << granule) > 512)) {
			TRACE("direct DMA not used: cache writeback granule %" B_PRIu32
				"\n", granule);
			direct = false;
		}
		atomic_set(&sDirectDMA, direct ? 1 : 0);
		if (direct) {
			TRACE("ARM64 direct DMA: the request's pages, aligned to %"
				B_PRIu32 " bytes, with cache maintenance\n",
				ahci_dma_alignment());
		}
	}
	return atomic_get(&sDirectDMA) != 0;
#else
	return false;
#endif
}


uint32
ahci_dma_alignment(void)
{
#if defined(__aarch64__)
	if (ahci_direct_dma()) {
		// the cache writeback granule (CTR_EL0.CWG, given and at most a
		// block: ahci_direct_dma())
		uint64 ctr;
		asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
		return 4 << ((ctr >> 24) & 0xf);
	}
#endif
	return 2;
}


/*!	Cleans and invalidates the data cache lines of the first \a dataSize
	bytes of the S/G table's memory, through the linear map: before a
	transfer, so that the controller reads what the CPU wrote and no dirty
	line is written back over what it writes; after it wrote them, so that
	no line the CPU loaded during the transfer hides it.
*/
void
ahci_dma_sync(const physical_entry *sgTable, int sgCount, size_t dataSize)
{
#if defined(__aarch64__)
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	const size_t lineSize = arm64_data_cache_line_size(ctr);
	for (int i = 0; i < sgCount && dataSize > 0; i++) {
		size_t size = min_c(sgTable[i].size, dataSize);
		addr_t start = (KERNEL_PMAP_BASE + sgTable[i].address)
			& ~(addr_t)(lineSize - 1);
		addr_t end = KERNEL_PMAP_BASE + sgTable[i].address + size;
		for (addr_t p = start; p < end; p += lineSize)
			asm volatile("dc civac, %0" :: "r"(p) : "memory");
		dataSize -= size;
	}
	memory_full_barrier();
#endif
}
