/*
 * Copyright 2010, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2019, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

extern "C" {
#include <compat/sys/malloc.h>
}

#include <stdio.h>
#include <string.h>

#include <util/BitUtils.h>

#include <kernel/heap.h>
#include <kernel/vm/vm.h>
#include <kernel/vm/vm_priv.h>
#include <kernel/vm/VMAddressSpace.h>
#include <kernel/vm/VMTranslationMap.h>

#if defined(FBSD_NONCOHERENT_DMA)
#include <arch/arm64/cache_poc.h>
#include <util/AutoLock.h>

struct dma_allocation {
	dma_allocation* next;
	void* address;
	size_t size;
	phys_addr_t physical;
	area_id area;
	bool cacheable;
};

static spinlock sDMAAllocationLock = B_SPINLOCK_INITIALIZER;
static dma_allocation* sDMAAllocations = NULL;
#endif


void*
_kernel_malloc(size_t size, int flags)
{
	// According to the FreeBSD kernel malloc man page the allocator is expected
	// to return power of two aligned addresses for allocations up to one page
	// size. While it also states that this shouldn't be relied upon, some drivers
	// may depend on it.
	void *ptr
		= memalign_etc(size >= PAGESIZE ? PAGESIZE : next_power_of_2(size), size,
			(flags & M_NOWAIT) ? HEAP_DONT_WAIT_FOR_MEMORY : 0);
	if (ptr == NULL)
		return NULL;

	if (flags & M_ZERO)
		memset(ptr, 0, size);

	return ptr;
}


void
_kernel_free(void *ptr)
{
	free(ptr);
}


void *
_kernel_contigmalloc(const char *file, int line, size_t size, int flags,
	vm_paddr_t low, vm_paddr_t high, unsigned long alignment,
	unsigned long boundary)
{
	return _kernel_contigmalloc_etc(file, line, size, flags, low, high,
		alignment, boundary, false);
}


void*
_kernel_contigmalloc_etc(const char* file, int line, size_t size, int flags,
	vm_paddr_t low, vm_paddr_t high, unsigned long alignment,
	unsigned long boundary, bool cacheable)
{
	const bool zero = (flags & M_ZERO) != 0, dontWait = (flags & M_NOWAIT) != 0;

	if (size == 0 || size > SIZE_MAX - (B_PAGE_SIZE - 1)
		|| low > high || alignment == 0
		|| (alignment & (alignment - 1)) != 0
		|| (boundary != 0 && ((boundary & (boundary - 1)) != 0
			|| size > boundary))) {
		return NULL;
	}
#if defined(FBSD_NONCOHERENT_DMA)
	const size_t requestedSize = size;
#endif
	size = ROUNDUP(size, B_PAGE_SIZE);
	// VM rounds the lower restriction down to a page. Round up here so a
	// non-page-aligned exclusion limit cannot admit a forbidden address.
	if (low > UINT64_MAX - (B_PAGE_SIZE - 1))
		return NULL;
	low = ROUNDUP(low, B_PAGE_SIZE);
	if (low > high || size - 1 > high - low)
		return NULL;

	uint32 creationFlags = (zero ? 0 : CREATE_AREA_DONT_CLEAR)
		| (dontWait ? CREATE_AREA_DONT_WAIT : 0);

	char name[B_OS_NAME_LENGTH];
	const char* baseName = strrchr(file, '/');
	baseName = baseName != NULL ? baseName + 1 : file;
	snprintf(name, sizeof(name), "contig:%s:%d", baseName, line);

	virtual_address_restrictions virtualRestrictions = {};

	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.low_address = low;
	// The VM upper restriction is exclusive; contigmalloc's is inclusive.
	physicalRestrictions.high_address = high == UINT64_MAX ? 0 : high + 1;
	physicalRestrictions.alignment = alignment;
	// A page-aligned start already satisfies boundaries smaller than a page.
	// Likewise an alignment at least as large as the boundary needs no extra
	// VM constraint. Only the requested bytes, not page padding, are DMA'd.
	if (boundary >= B_PAGE_SIZE && boundary > alignment)
		physicalRestrictions.boundary = boundary;

#if defined(FBSD_NONCOHERENT_DMA)
	dma_allocation* allocation = (dma_allocation*)_kernel_malloc(
		sizeof(dma_allocation), flags & (M_NOWAIT | M_WAITOK));
	if (allocation == NULL)
		return NULL;
#endif

	void* address;
	area_id area = create_area_etc(B_SYSTEM_TEAM, name, size, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, creationFlags, 0,
		&virtualRestrictions, &physicalRestrictions, &address);
	if (area < 0) {
#if defined(FBSD_NONCOHERENT_DMA)
		_kernel_free(allocation);
#endif
		return NULL;
	}

#if defined(FBSD_NONCOHERENT_DMA)
	physical_entry entry;
	if (get_memory_map(address, size, &entry, 1) != B_OK
		|| entry.size < size) {
		delete_area(area);
		_kernel_free(allocation);
		return NULL;
	}

	// Every line must belong to this private, page-aligned allocation. Prevent
	// migration while using this CPU's CTR on systems with different line sizes.
	cpu_status irqState = disable_interrupts();
	const size_t lineSize = arm64_current_data_cache_line_size();
	if (lineSize > B_PAGE_SIZE) {
		restore_interrupts(irqState);
		delete_area(area);
		_kernel_free(allocation);
		return NULL;
	}
	if (!cacheable) {
		// Clear dirty lines before changing Normal Write-back RAM to Normal
		// Non-cacheable. Descriptor rings retain this coherent direct mapping.
		for (size_t offset = 0; offset < size; offset += lineSize) {
			arm64_clean_invalidate_data_cache_line_poc((addr_t)address + offset);
		}
		memory_full_barrier();
	}
	restore_interrupts(irqState);
	if (!cacheable) {
		if (vm_set_area_memory_type(area, entry.address,
				B_WRITE_COMBINING_MEMORY) != B_OK) {
			delete_area(area);
			_kernel_free(allocation);
			return NULL;
		}
		memory_full_barrier();
	}

	allocation->address = address;
	allocation->size = requestedSize;
	allocation->physical = entry.address;
	allocation->area = area;
	allocation->cacheable = cacheable;
	{
		BPrivate::InterruptsSpinLocker locker(&sDMAAllocationLock);
		allocation->next = sDMAAllocations;
		sDMAAllocations = allocation;
	}
#endif

	return address;
}


void
_kernel_contigfree(void *addr, size_t size)
{
	if (addr == NULL)
		return;

#if defined(FBSD_NONCOHERENT_DMA)
	dma_allocation* allocation = NULL;
	{
		BPrivate::InterruptsSpinLocker locker(&sDMAAllocationLock);
		for (dma_allocation** link = &sDMAAllocations; *link != NULL;
				link = &(*link)->next) {
			if ((*link)->address == addr) {
				allocation = *link;
				*link = allocation->next;
				break;
			}
		}
	}
	if (allocation == NULL) {
		panic("fbsd compat: freeing unknown DMA allocation %p", addr);
		return;
	}
	delete_area(allocation->area);
	_kernel_free(allocation);
#else
	delete_area(area_for(addr));
#endif
}


#if defined(FBSD_NONCOHERENT_DMA)
bool
_kernel_contig_dma_address(const void* address, size_t size,
	vm_paddr_t* physicalAddress)
{
	if (address == NULL || size == 0)
		return false;

	// This leaf lock is safe under a driver's interrupt lock. In particular,
	// do not inspect VM areas or acquire the address-space lock from map_load.
	BPrivate::InterruptsSpinLocker locker(&sDMAAllocationLock);
	for (dma_allocation* allocation = sDMAAllocations; allocation != NULL;
			allocation = allocation->next) {
		addr_t offset = (addr_t)address - (addr_t)allocation->address;
		if (!allocation->cacheable
			&& (addr_t)address >= (addr_t)allocation->address
			&& offset < allocation->size && size <= allocation->size - offset) {
			*physicalAddress = allocation->physical + offset;
			return true;
		}
	}
	return false;
}
#endif


vm_paddr_t
pmap_kextract(vm_offset_t virtualAddress)
{
	if (IS_KERNEL_ADDRESS(virtualAddress)) {
		// Walk the kernel's page tables directly, as FreeBSD's pmap_kextract()
		// does: get_memory_map() looks up and references the address space
		// and locks it for every call, and drivers call this for every buffer
		// they hand to the device (airos-works/haiku#122). The caller owns
		// the buffer, so its mapping cannot go away meanwhile.
		const addr_t page = ROUNDDOWN((addr_t)virtualAddress, B_PAGE_SIZE);
		phys_addr_t physical;
		uint32 flags;
		if (VMAddressSpace::Kernel()->TranslationMap()->QueryInterrupt(page,
				&physical, &flags) == B_OK
			&& (flags & PAGE_PRESENT) != 0) {
			return (vm_paddr_t)(physical + (virtualAddress - page));
		}
	}

	physical_entry entry;
	status_t status = get_memory_map((void *)virtualAddress, 1, &entry, 1);
	if (status < B_OK) {
		panic("fbsd compat: get_memory_map failed for %p, error %08" B_PRIx32
			"\n", (void *)virtualAddress, status);
	}

	return (vm_paddr_t)entry.address;
}
