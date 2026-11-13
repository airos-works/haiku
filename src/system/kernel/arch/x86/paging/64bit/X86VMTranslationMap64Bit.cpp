/*
 * Copyright 2012, Alex Smith, alex@alex-smith.me.uk
 * Copyright 2008-2011, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2002-2010, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


#include "paging/64bit/X86VMTranslationMap64Bit.h"

#include <interrupts.h>
#include <slab/Slab.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <util/ThreadAutoLock.h>
#include <vm/vm_page.h>
#include <vm/VMAddressSpace.h>
#include <vm/VMCache.h>

#include "paging/64bit/X86PagingMethod64Bit.h"
#include "paging/64bit/X86PagingStructures64Bit.h"
#include "paging/x86_physical_page_mapper.h"


//#define TRACE_X86_VM_TRANSLATION_MAP_64BIT
#ifdef TRACE_X86_VM_TRANSLATION_MAP_64BIT
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...) ;
#endif


// #pragma mark - X86VMTranslationMap64Bit


X86VMTranslationMap64Bit::X86VMTranslationMap64Bit(bool la57)
	:
	fPagingStructures(NULL),
	fLA57(la57),
	fLargePageCount(0)
{
	fSplitReservation.count = 0;
}


X86VMTranslationMap64Bit::~X86VMTranslationMap64Bit()
{
	TRACE("X86VMTranslationMap64Bit::~X86VMTranslationMap64Bit()\n");

	vm_page_unreserve_pages(&fSplitReservation);

	if (fPagingStructures == NULL)
		return;

	if (fPageMapper == NULL) {
		fPagingStructures->RemoveReference();
		return;
	}

	vm_page_reservation reservation = {};
	phys_addr_t address;
	vm_page* page;

	// Free all structures in the bottom half of the PMLTop (user memory).
	uint64* virtualPML5 = NULL;
	if (fLA57)
		virtualPML5 = fPagingStructures->VirtualPMLTop();
	for (uint32 p = 0; p < 256; p++) {
		uint64* virtualPML4 = NULL;
		uint32 limitPML4 = 256;
		if (fLA57) {
			if ((virtualPML5[p] & X86_64_PML5E_PRESENT) == 0)
				continue;

			virtualPML4 = (uint64*)fPageMapper->GetPageTableAt(
				virtualPML5[p] & X86_64_PML5E_ADDRESS_MASK);
			limitPML4 = 512;
		} else {
			virtualPML4 = fPagingStructures->VirtualPMLTop();
		}

		for (uint32 i = 0; i < limitPML4; i++) {
			if ((virtualPML4[i] & X86_64_PML4E_PRESENT) == 0)
				continue;

			uint64* virtualPDPT = (uint64*)fPageMapper->GetPageTableAt(
				virtualPML4[i] & X86_64_PML4E_ADDRESS_MASK);
			for (uint32 j = 0; j < 512; j++) {
				if ((virtualPDPT[j] & X86_64_PDPTE_PRESENT) == 0)
					continue;

				uint64* virtualPageDir = (uint64*)fPageMapper->GetPageTableAt(
					virtualPDPT[j] & X86_64_PDPTE_ADDRESS_MASK);
				for (uint32 k = 0; k < 512; k++) {
					if ((virtualPageDir[k] & X86_64_PDE_PRESENT) == 0
						|| (virtualPageDir[k] & X86_64_PDE_LARGE_PAGE) != 0) {
						// a large page belongs to its cache
						continue;
					}

					address = virtualPageDir[k] & X86_64_PDE_ADDRESS_MASK;
					page = vm_lookup_page(address / B_PAGE_SIZE);
					if (page == NULL) {
						panic("page table %u %u %u on invalid page %#"
							B_PRIxPHYSADDR "\n", i, j, k, address);
					}

					DEBUG_PAGE_ACCESS_START(page);
					vm_page_free_etc(NULL, page, &reservation);
				}

				address = virtualPDPT[j] & X86_64_PDPTE_ADDRESS_MASK;
				page = vm_lookup_page(address / B_PAGE_SIZE);
				if (page == NULL) {
					panic("page directory %u %u on invalid page %#"
						B_PRIxPHYSADDR "\n", i, j, address);
				}

				DEBUG_PAGE_ACCESS_START(page);
				vm_page_free_etc(NULL, page, &reservation);
			}

			address = virtualPML4[i] & X86_64_PML4E_ADDRESS_MASK;
			page = vm_lookup_page(address / B_PAGE_SIZE);
			if (page == NULL) {
				panic("PDPT %u on invalid page %#" B_PRIxPHYSADDR "\n", i,
					address);
			}

			DEBUG_PAGE_ACCESS_START(page);
			vm_page_free_etc(NULL, page, &reservation);
		}

		if (fLA57) {
			address = virtualPML5[p] & X86_64_PML5E_ADDRESS_MASK;
			page = vm_lookup_page(address / B_PAGE_SIZE);
			if (page == NULL) {
				panic("PML4 %u on invalid page %#" B_PRIxPHYSADDR "\n", p,
					address);
			}

			DEBUG_PAGE_ACCESS_START(page);
			vm_page_free_etc(NULL, page, &reservation);
		} else {
			break;
		}
	}

	vm_page_unreserve_pages(&reservation);

	fPageMapper->Delete();

	fPagingStructures->RemoveReference();
}


status_t
X86VMTranslationMap64Bit::Init(bool kernel)
{
	TRACE("X86VMTranslationMap64Bit::Init()\n");

	X86VMTranslationMap::Init(kernel);

	fPagingStructures = new(std::nothrow) X86PagingStructures64Bit;
	if (fPagingStructures == NULL)
		return B_NO_MEMORY;

	X86PagingMethod64Bit* method = X86PagingMethod64Bit::Method();

	if (kernel) {
		// Get the page mapper.
		fPageMapper = method->KernelPhysicalPageMapper();

		// Kernel PMLTop is already mapped.
		fPagingStructures->Init(method->KernelVirtualPMLTop(),
			method->KernelPhysicalPMLTop());
	} else {
		// Allocate a physical page mapper.
		status_t error = method->PhysicalPageMapper()
			->CreateTranslationMapPhysicalPageMapper(&fPageMapper);
		if (error != B_OK)
			return error;

		// Assuming that only the top 2 PMLTop entries are occupied for the
		// kernel.
		STATIC_ASSERT(KERNEL_PMAP_BASE == 0xffffff0000000000);
		STATIC_ASSERT(KERNEL_BASE == 0xffffff0000000000);

		// Allocate and clear the PMLTop.
		uint64* virtualPMLTop = (uint64*)memalign(B_PAGE_SIZE, B_PAGE_SIZE);
		if (virtualPMLTop == NULL)
			return B_NO_MEMORY;
		memset(virtualPMLTop, 0, B_PAGE_SIZE);

		// Copy the top 2 PMLTop entries.
		virtualPMLTop[510] = method->KernelVirtualPMLTop()[510];
		virtualPMLTop[511] = method->KernelVirtualPMLTop()[511];

		// Look up the PMLTop physical address.
		phys_addr_t physicalPMLTop;
		vm_get_page_mapping(VMAddressSpace::KernelID(), (addr_t)virtualPMLTop,
			&physicalPMLTop);

		// Initialize the paging structures.
		fPagingStructures->Init(virtualPMLTop, physicalPMLTop);
	}

	return B_OK;
}


size_t
X86VMTranslationMap64Bit::MaxPagesNeededToMap(addr_t start, addr_t end) const
{
	// If start == 0, the actual base address is not yet known to the caller and
	// we shall assume the worst case, which is where the start address is the
	// last page covered by a PDPT or PML4.
	if (start == 0) {
		start = (fLA57 ? k64BitPML4TRange : k64BitPDPTRange) - B_PAGE_SIZE;
		end += start;
	}

	size_t requiredPML4s = 0;
	if (fLA57) {
		requiredPML4s = end / k64BitPML4TRange + 1
			- start / k64BitPML4TRange;
	}
	size_t requiredPDPTs = end / k64BitPDPTRange + 1
		- start / k64BitPDPTRange;
	size_t requiredPageDirs = end / k64BitPageDirectoryRange + 1
		- start / k64BitPageDirectoryRange;
	size_t requiredPageTables = end / k64BitPageTableRange + 1
		- start / k64BitPageTableRange;

	return requiredPML4s + requiredPDPTs + requiredPageDirs
		+ requiredPageTables;
}


status_t
X86VMTranslationMap64Bit::Map(addr_t virtualAddress, phys_addr_t physicalAddress,
	uint32 attributes, uint32 memoryType, vm_page_reservation* reservation)
{
	TRACE("X86VMTranslationMap64Bit::Map(%#" B_PRIxADDR ", %#" B_PRIxPHYSADDR
		")\n", virtualAddress, physicalAddress);
	ASSERT_LOCKED_RECURSIVE(&fLock);

	_SplitLargePages(virtualAddress, virtualAddress);

	ThreadCPUPinner pinner(thread_get_current_thread());

	// Look up the page table for the virtual address, allocating new tables
	// if required. Shouldn't fail.
	uint64* entry = X86PagingMethod64Bit::PageTableEntryForAddress(
		fPagingStructures->VirtualPMLTop(), virtualAddress, fIsKernelMap,
		true, reservation, fPageMapper, fMapCount);
	ASSERT(entry != NULL);

	// The entry should not already exist.
	ASSERT_PRINT((*entry & X86_64_PTE_PRESENT) == 0,
		"virtual address: %#" B_PRIxADDR ", existing pte: %#" B_PRIx64,
		virtualAddress, *entry);

	// Fill in the table entry.
	X86PagingMethod64Bit::PutPageTableEntryInTable(entry, physicalAddress,
		attributes, memoryType, fIsKernelMap);

	// Note: We don't need to invalidate the TLB for this address, as previously
	// the entry was not present and the TLB doesn't cache those entries.

	fMapCount++;

	return 0;
}


status_t
X86VMTranslationMap64Bit::Unmap(addr_t start, addr_t end)
{
	start = ROUNDDOWN(start, B_PAGE_SIZE);
	if (start >= end)
		return B_OK;

	TRACE("X86VMTranslationMap64Bit::Unmap(%#" B_PRIxADDR ", %#" B_PRIxADDR
		")\n", start, end);

	_SplitLargePages(start, end);

	ThreadCPUPinner pinner(thread_get_current_thread());

	do {
		uint64* pageTable = X86PagingMethod64Bit::PageTableForAddress(
			fPagingStructures->VirtualPMLTop(), start, fIsKernelMap, false,
			NULL, fPageMapper, fMapCount);
		if (pageTable == NULL) {
			// Move on to the next page table.
			start = ROUNDUP(start + 1, k64BitPageTableRange);
			continue;
		}

		for (uint32 index = start / B_PAGE_SIZE % k64BitTableEntryCount;
				index < k64BitTableEntryCount && start < end;
				index++, start += B_PAGE_SIZE) {
			if ((pageTable[index] & X86_64_PTE_PRESENT) == 0)
				continue;

			TRACE("X86VMTranslationMap64Bit::Unmap(): removing page %#"
				B_PRIxADDR " (%#" B_PRIxPHYSADDR ")\n", start,
				pageTable[index] & X86_64_PTE_ADDRESS_MASK);

			uint64 oldEntry = X86PagingMethod64Bit::ClearTableEntry(
				&pageTable[index]);
			fMapCount--;

			if ((oldEntry & X86_64_PTE_IN_TLB_MASK) != 0) {
				// Note, that we only need to invalidate the address, if the
				// accessed flags was set, since only then the entry could have
				// been in any TLB.
				InvalidatePage(start);
			}
		}
	} while (start != 0 && start < end);

	return B_OK;
}


status_t
X86VMTranslationMap64Bit::UnmapPage(VMArea* area, addr_t address,
	bool updatePageQueue, bool deletingAddressSpace, uint32* _flags)
{
	ASSERT(address % B_PAGE_SIZE == 0);
	ASSERT(_flags == NULL || !updatePageQueue);

	TRACE("X86VMTranslationMap64Bit::UnmapPage(%#" B_PRIxADDR ")\n", address);

	_SplitLargePages(address, address);

	ThreadCPUPinner pinner(thread_get_current_thread());

	// Look up the page table for the virtual address.
	uint64* entry = X86PagingMethod64Bit::PageTableEntryForAddress(
		fPagingStructures->VirtualPMLTop(), address, fIsKernelMap,
		false, NULL, fPageMapper, fMapCount);
	if (entry == NULL)
		return B_ENTRY_NOT_FOUND;

	RecursiveLocker locker(fLock);

	uint64 oldEntry = X86PagingMethod64Bit::ClearTableEntry(entry);

	pinner.Unlock();

	if ((oldEntry & X86_64_PTE_PRESENT) == 0)
		return B_ENTRY_NOT_FOUND;

	fMapCount--;

	if ((oldEntry & X86_64_PTE_IN_TLB_MASK) != 0) {
		// Note, that we only need to invalidate the address, if the
		// accessed flags was set, since only then the entry could have been
		// in any TLB.
		if (!deletingAddressSpace)
			InvalidatePage(address);

		if (_flags == NULL) {
			Flush();
				// flush explicitly, since we directly use the lock
		}

		// NOTE: Between clearing the page table entry and Flush() other
		// processors (actually even this processor with another thread of the
		// same team) could still access the page in question via their cached
		// entry. We can obviously lose a modified flag in this case, with the
		// effect that the page looks unmodified (and might thus be recycled),
		// but is actually modified.
		// In most cases this is harmless, but for vm_remove_all_page_mappings()
		// this is actually a problem.
		// Interestingly FreeBSD seems to ignore this problem as well
		// (cf. pmap_remove_all()), unless I've missed something.
	}

	if (_flags == NULL) {
		locker.Detach();
			// PageUnmapped() will unlock for us

		PageUnmapped(area, (oldEntry & X86_64_PTE_ADDRESS_MASK) / B_PAGE_SIZE,
			(oldEntry & X86_64_PTE_ACCESSED) != 0,
			(oldEntry & X86_64_PTE_DIRTY) != 0, updatePageQueue);
	} else {
		uint32 flags = PAGE_PRESENT;
		if ((oldEntry & X86_64_PTE_ACCESSED) != 0)
			flags |= PAGE_ACCESSED;
		if ((oldEntry & X86_64_PTE_DIRTY) != 0)
			flags |= PAGE_MODIFIED;
		*_flags = flags;
	}

	return B_OK;
}


void
X86VMTranslationMap64Bit::UnmapPages(VMArea* area, addr_t base, size_t size,
	bool updatePageQueue, bool deletingAddressSpace)
{
	if (size == 0)
		return;

	addr_t start = base;
	addr_t end = base + size - 1;

	TRACE("X86VMTranslationMap64Bit::UnmapPages(%p, %#" B_PRIxADDR ", %#"
		B_PRIxADDR ")\n", area, start, end);

	VMAreaMappings queue;

	RecursiveLocker locker(fLock);
	_SplitLargePages(start, end);
	ThreadCPUPinner pinner(thread_get_current_thread());

	do {
		uint64* pageTable = X86PagingMethod64Bit::PageTableForAddress(
			fPagingStructures->VirtualPMLTop(), start, fIsKernelMap, false,
			NULL, fPageMapper, fMapCount);
		if (pageTable == NULL) {
			// Move on to the next page table.
			start = ROUNDUP(start + 1, k64BitPageTableRange);
			continue;
		}

		for (uint32 index = start / B_PAGE_SIZE % k64BitTableEntryCount;
				index < k64BitTableEntryCount && start < end;
				index++, start += B_PAGE_SIZE) {
			uint64 oldEntry = X86PagingMethod64Bit::ClearTableEntry(
				&pageTable[index]);
			if ((oldEntry & X86_64_PTE_PRESENT) == 0)
				continue;

			fMapCount--;

			if ((oldEntry & X86_64_PTE_IN_TLB_MASK) != 0) {
				// Note, that we only need to invalidate the address, if the
				// accessed flags was set, since only then the entry could have
				// been in any TLB.
				if (!deletingAddressSpace)
					InvalidatePage(start);
			}

			if (area->cache_type != CACHE_TYPE_DEVICE) {
				page_num_t page = (oldEntry & X86_64_PTE_ADDRESS_MASK) / B_PAGE_SIZE;
				PageUnmapped(area, page,
					(oldEntry & X86_64_PTE_ACCESSED) != 0,
					(oldEntry & X86_64_PTE_DIRTY) != 0,
					updatePageQueue, &queue);
			}
		}

		Flush();
			// flush explicitly, since we directly use the lock
	} while (start != 0 && start < end);

	// TODO: As in UnmapPage() we can lose page dirty flags here. ATM it's not
	// really critical here, as in all cases this method is used, the unmapped
	// area range is unmapped for good (resized/cut) and the pages will likely
	// be freed.

	locker.Unlock();

	// free removed mappings
	bool isKernelSpace = area->address_space == VMAddressSpace::Kernel();
	uint32 freeFlags = CACHE_DONT_WAIT_FOR_MEMORY
		| (isKernelSpace ? CACHE_DONT_LOCK_KERNEL_SPACE : 0);
	while (vm_page_mapping* mapping = queue.RemoveHead())
		vm_free_page_mapping(mapping->page->physical_page_number, mapping, freeFlags);
}


status_t
X86VMTranslationMap64Bit::Query(addr_t virtualAddress,
	phys_addr_t* _physicalAddress, uint32* _flags)
{
	ASSERT((virtualAddress % B_PAGE_SIZE) == 0);

	*_flags = 0;
	*_physicalAddress = 0;

	ThreadCPUPinner pinner(thread_get_current_thread());

	// This function may be called on the physical map area, so we must handle
	// large pages here. Look up the page directory entry for the virtual
	// address.
	uint64* pde = X86PagingMethod64Bit::PageDirectoryEntryForAddress(
		fPagingStructures->VirtualPMLTop(), virtualAddress, fIsKernelMap,
		false, NULL, fPageMapper, fMapCount);
	if (pde == NULL || (*pde & X86_64_PDE_PRESENT) == 0)
		return B_OK;

	uint64 entry;
	if ((*pde & X86_64_PDE_LARGE_PAGE) != 0) {
		entry = *pde;
		*_physicalAddress = (entry & X86_64_PDE_ADDRESS_MASK)
			+ (virtualAddress % 0x200000);
	} else {
		uint64* virtualPageTable = (uint64*)fPageMapper->GetPageTableAt(
			*pde & X86_64_PDE_ADDRESS_MASK);
		entry = virtualPageTable[VADDR_TO_PTE(virtualAddress)];
		*_physicalAddress = entry & X86_64_PTE_ADDRESS_MASK;
	}

	// Translate the page state flags.
	if ((entry & X86_64_PTE_USER) != 0) {
		*_flags |= ((entry & X86_64_PTE_WRITABLE) != 0 ? B_WRITE_AREA : 0)
			| B_READ_AREA
			| ((entry & X86_64_PTE_NOT_EXECUTABLE) == 0 ? B_EXECUTE_AREA : 0);
	}

	*_flags |= ((entry & X86_64_PTE_WRITABLE) != 0 ? B_KERNEL_WRITE_AREA : 0)
		| B_KERNEL_READ_AREA
		| ((entry & X86_64_PTE_NOT_EXECUTABLE) == 0 ? B_KERNEL_EXECUTE_AREA : 0)
		| ((entry & X86_64_PTE_DIRTY) != 0 ? PAGE_MODIFIED : 0)
		| ((entry & X86_64_PTE_ACCESSED) != 0 ? PAGE_ACCESSED : 0)
		| ((entry & X86_64_PTE_PRESENT) != 0 ? PAGE_PRESENT : 0);

	TRACE("X86VMTranslationMap64Bit::Query(%#" B_PRIxADDR ") -> %#"
		B_PRIxPHYSADDR " %#" B_PRIx32 " (entry: %#" B_PRIx64 ")\n",
		virtualAddress, *_physicalAddress, *_flags, entry);

	return B_OK;
}


status_t
X86VMTranslationMap64Bit::QueryInterrupt(addr_t virtualAddress,
	phys_addr_t* _physicalAddress, uint32* _flags)
{
	// With our page mapper, there is no difference in getting a page table
	// when interrupts are enabled or disabled, so just call Query().
	return Query(virtualAddress, _physicalAddress, _flags);
}


status_t
X86VMTranslationMap64Bit::Protect(addr_t start, addr_t end, uint32 attributes,
	uint32 memoryType)
{
	start = ROUNDDOWN(start, B_PAGE_SIZE);
	if (start >= end)
		return B_OK;

	TRACE("X86VMTranslationMap64Bit::Protect(%#" B_PRIxADDR ", %#" B_PRIxADDR
		", %#" B_PRIx32 ")\n", start, end, attributes);

	// compute protection flags
	uint64 newProtectionFlags = 0;
	if ((attributes & B_USER_PROTECTION) != 0) {
		newProtectionFlags = X86_64_PTE_USER;
		if ((attributes & B_WRITE_AREA) != 0)
			newProtectionFlags |= X86_64_PTE_WRITABLE;
		if ((attributes & B_EXECUTE_AREA) == 0
			&& x86_check_feature(IA32_FEATURE_AMD_EXT_NX, FEATURE_EXT_AMD)) {
			newProtectionFlags |= X86_64_PTE_NOT_EXECUTABLE;
		}
	} else if ((attributes & B_KERNEL_WRITE_AREA) != 0)
		newProtectionFlags = X86_64_PTE_WRITABLE;

	_SplitLargePages(start, end);

	ThreadCPUPinner pinner(thread_get_current_thread());

	do {
		uint64* pageTable = X86PagingMethod64Bit::PageTableForAddress(
			fPagingStructures->VirtualPMLTop(), start, fIsKernelMap, false,
			NULL, fPageMapper, fMapCount);
		if (pageTable == NULL) {
			// Move on to the next page table.
			start = ROUNDUP(start + 1, k64BitPageTableRange);
			continue;
		}

		for (uint32 index = start / B_PAGE_SIZE % k64BitTableEntryCount;
				index < k64BitTableEntryCount && start < end;
				index++, start += B_PAGE_SIZE) {
			uint64 entry = pageTable[index];
			if ((entry & X86_64_PTE_PRESENT) == 0)
				continue;

			TRACE("X86VMTranslationMap64Bit::Protect(): protect page %#"
				B_PRIxADDR "\n", start);

			// set the new protection flags -- we want to do that atomically,
			// without changing the accessed or dirty flag
			uint64 oldEntry;
			while (true) {
				oldEntry = X86PagingMethod64Bit::TestAndSetTableEntry(
					&pageTable[index],
					(entry & ~(X86_64_PTE_PROTECTION_MASK
							| X86_64_PTE_MEMORY_TYPE_MASK))
						| newProtectionFlags
						| X86PagingMethod64Bit::MemoryTypeToPageTableEntryFlags(
							memoryType),
					entry);
				if (oldEntry == entry)
					break;
				entry = oldEntry;
			}

			if ((oldEntry & X86_64_PTE_IN_TLB_MASK) != 0) {
				// Note, that we only need to invalidate the address, if the
				// accessed flag was set, since only then the entry could have
				// been in any TLB.
				InvalidatePage(start);
			}
		}
	} while (start != 0 && start < end);

	return B_OK;
}


status_t
X86VMTranslationMap64Bit::ClearFlags(addr_t address, uint32 flags)
{
	TRACE("X86VMTranslationMap64Bit::ClearFlags(%#" B_PRIxADDR ", %#" B_PRIx32
		")\n", address, flags);

	_SplitLargePages(address, address);

	ThreadCPUPinner pinner(thread_get_current_thread());

	uint64* entry = X86PagingMethod64Bit::PageTableEntryForAddress(
		fPagingStructures->VirtualPMLTop(), address, fIsKernelMap,
		false, NULL, fPageMapper, fMapCount);
	if (entry == NULL)
		return B_OK;

	uint64 flagsToClear = ((flags & PAGE_MODIFIED) ? X86_64_PTE_DIRTY : 0)
		| ((flags & PAGE_ACCESSED) ? X86_64_PTE_ACCESSED : 0);

	uint64 oldEntry = X86PagingMethod64Bit::ClearTableEntryFlags(entry,
		flagsToClear);

	if ((oldEntry & flagsToClear) != 0
		|| ((oldEntry & X86_64_PTE_MAY_BE_CACHED) != 0
			&& (flagsToClear & X86_64_PTE_DIRTY) != 0)) {
		InvalidatePage(address);
	}

	return B_OK;
}


bool
X86VMTranslationMap64Bit::ClearAccessedAndModified(VMArea* area, addr_t address,
	bool unmapIfUnaccessed, bool& _modified)
{
	ASSERT(address % B_PAGE_SIZE == 0);

	TRACE("X86VMTranslationMap64Bit::ClearAccessedAndModified(%#" B_PRIxADDR
		")\n", address);

	RecursiveLocker locker(fLock);
	_SplitLargePages(address, address);
	ThreadCPUPinner pinner(thread_get_current_thread());

	uint64* entry = X86PagingMethod64Bit::PageTableEntryForAddress(
		fPagingStructures->VirtualPMLTop(), address, fIsKernelMap,
		false, NULL, fPageMapper, fMapCount);
	if (entry == NULL)
		return false;

	uint64 oldEntry;

	if (unmapIfUnaccessed) {
		while (true) {
			oldEntry = *entry;
			if ((oldEntry & X86_64_PTE_PRESENT) == 0) {
				// page mapping not valid
				return false;
			}

			if (oldEntry & X86_64_PTE_ACCESSED) {
				// page was accessed -- just clear the flags
				oldEntry = X86PagingMethod64Bit::ClearTableEntryFlags(entry,
					X86_64_PTE_ACCESSED | X86_64_PTE_DIRTY
						| X86_64_PTE_MAY_BE_CACHED);
				break;
			}

			// page hasn't been accessed -- unmap it
			if (X86PagingMethod64Bit::TestAndSetTableEntry(entry, 0, oldEntry)
					== oldEntry) {
				break;
			}

			// something changed -- check again
		}
	} else {
		oldEntry = X86PagingMethod64Bit::ClearTableEntryFlags(entry,
			X86_64_PTE_ACCESSED | X86_64_PTE_DIRTY | X86_64_PTE_MAY_BE_CACHED);
	}

	pinner.Unlock();

	_modified = (oldEntry & X86_64_PTE_DIRTY) != 0;

	if ((oldEntry & X86_64_PTE_IN_TLB_MASK) != 0) {
		// Note, that we only need to invalidate the address, if the
		// accessed flags was set, since only then the entry could have been
		// in any TLB.
		InvalidatePage(address);
		Flush();
	}

	if ((oldEntry & X86_64_PTE_ACCESSED) != 0)
		return true;

	if (!unmapIfUnaccessed)
		return false;

	// We have unmapped the address. Do the "high level" stuff.

	fMapCount--;

	locker.Detach();
		// UnaccessedPageUnmapped() will unlock for us

	UnaccessedPageUnmapped(area,
		(oldEntry & X86_64_PTE_ADDRESS_MASK) / B_PAGE_SIZE);

	return false;
}



bool
X86VMTranslationMap64Bit::TestAndClearAccessed(VMArea* area, addr_t address,
	bool& _modified)
{
	ASSERT(address % B_PAGE_SIZE == 0);

	// A stale TLB entry only delays the accessed flag being set again, so
	// no invalidation; the next change of the entry does it, since the
	// entry is marked as possibly cached. The dirty flag stays: clearing it
	// without an invalidation could lose a write.
	// No lock: the caller holds the page's cache lock, so the mapping, and
	// with it the page table, stays; the entry is changed atomically.
	ThreadCPUPinner pinner(thread_get_current_thread());
	_modified = false;

	if (atomic_get(&fLargePageCount) > 0) {
		// A large page has one accessed flag for all its pages: its first
		// page clears it, the others only report it. (A mapping that exists
		// can't become large meanwhile, only be split.)
		uint64* pde = X86PagingMethod64Bit::PageDirectoryEntryForAddress(
			fPagingStructures->VirtualPMLTop(), address, fIsKernelMap, false,
			NULL, fPageMapper, fMapCount);
		uint64 oldEntry = pde != NULL ? *(volatile uint64*)pde : 0;
		if ((oldEntry & (X86_64_PDE_PRESENT | X86_64_PDE_LARGE_PAGE))
				== (X86_64_PDE_PRESENT | X86_64_PDE_LARGE_PAGE)) {
			while (address % k64BitPageTableRange == 0
				&& (oldEntry & X86_64_PDE_ACCESSED) != 0) {
				uint64 newEntry = (oldEntry & ~X86_64_PDE_ACCESSED)
					| X86_64_PTE_MAY_BE_CACHED;
				uint64 previous = X86PagingMethod64Bit::TestAndSetTableEntry(
					pde, newEntry, oldEntry);
				if (previous == oldEntry)
					break;
				oldEntry = previous;
				if ((oldEntry & X86_64_PDE_LARGE_PAGE) == 0)
					break;
			}
			if ((oldEntry & X86_64_PDE_LARGE_PAGE) != 0) {
				_modified = (oldEntry & X86_64_PDE_DIRTY) != 0;
				return (oldEntry & X86_64_PDE_ACCESSED) != 0;
			}
		}
	}

	uint64* entry = X86PagingMethod64Bit::PageTableEntryForAddress(
		fPagingStructures->VirtualPMLTop(), address, fIsKernelMap,
		false, NULL, fPageMapper, fMapCount);
	if (entry == NULL)
		return false;

	uint64 oldEntry;
	while (true) {
		oldEntry = *(volatile uint64*)entry;
		if ((oldEntry & X86_64_PTE_PRESENT) == 0)
			return false;
		if ((oldEntry & X86_64_PTE_ACCESSED) == 0)
			break;

		uint64 newEntry = (oldEntry & ~X86_64_PTE_ACCESSED)
			| X86_64_PTE_MAY_BE_CACHED;
		if (X86PagingMethod64Bit::TestAndSetTableEntry(entry, newEntry,
				oldEntry) == oldEntry) {
			break;
		}
	}

	_modified = (oldEntry & X86_64_PTE_DIRTY) != 0;
	return (oldEntry & X86_64_PTE_ACCESSED) != 0;
}


size_t
X86VMTranslationMap64Bit::LargePageSize() const
{
	return fIsKernelMap ? 0 : k64BitPageTableRange;
}


bool
X86VMTranslationMap64Bit::CanMapLarge(addr_t virtualAddress)
{
	ASSERT_LOCKED_RECURSIVE(&fLock);

	if (fIsKernelMap || virtualAddress % k64BitPageTableRange != 0)
		return false;

	ThreadCPUPinner pinner(thread_get_current_thread());
	uint64* pde = X86PagingMethod64Bit::PageDirectoryEntryForAddress(
		fPagingStructures->VirtualPMLTop(), virtualAddress, fIsKernelMap,
		false, NULL, fPageMapper, fMapCount);
	return pde == NULL || (*pde & X86_64_PDE_PRESENT) == 0;
}


status_t
X86VMTranslationMap64Bit::MapLarge(addr_t virtualAddress,
	phys_addr_t physicalAddress, uint32 attributes, uint32 memoryType,
	vm_page_reservation* reservation)
{
	ASSERT_LOCKED_RECURSIVE(&fLock);

	if (fIsKernelMap || virtualAddress % k64BitPageTableRange != 0
		|| physicalAddress % k64BitPageTableRange != 0) {
		return B_BAD_VALUE;
	}

	// the flags of a page table entry, which are those of a large page
	// directory entry, except for bit 7 (PAT there, large page here)
	uint64 entry;
	X86PagingMethod64Bit::PutPageTableEntryInTable(&entry, physicalAddress,
		attributes, memoryType, false);
	if ((entry & X86_64_PTE_PAT) != 0)
		return B_NOT_SUPPORTED;

	ThreadCPUPinner pinner(thread_get_current_thread());

	// the page directory, allocating it (and above) if needed
	uint64* pde = X86PagingMethod64Bit::PageDirectoryEntryForAddress(
		fPagingStructures->VirtualPMLTop(), virtualAddress, fIsKernelMap,
		true, reservation, fPageMapper, fMapCount);
	if ((*pde & X86_64_PDE_PRESENT) != 0)
		return B_BUSY;
	if (reservation->count == 0)
		return B_NO_MEMORY;

	// keep a page to split it later
	reservation->count--;
	fSplitReservation.count++;

	X86PagingMethod64Bit::SetTableEntry(pde, entry | X86_64_PDE_LARGE_PAGE);
	fMapCount += k64BitTableEntryCount;
	atomic_add(&fLargePageCount, 1);

	// no invalidation: the entry wasn't present
	return B_OK;
}


/*!	Splits the large pages in the range into page tables, so that the
	callers can work on page table entries.
*/
void
X86VMTranslationMap64Bit::_SplitLargePages(addr_t start, addr_t end)
{
	if (atomic_get(&fLargePageCount) == 0)
		return;

	RecursiveLocker locker(fLock);
	ThreadCPUPinner pinner(thread_get_current_thread());

	for (addr_t address = ROUNDDOWN(start, k64BitPageTableRange);
			address <= end; address += k64BitPageTableRange) {
		uint64* pde = X86PagingMethod64Bit::PageDirectoryEntryForAddress(
			fPagingStructures->VirtualPMLTop(), address, fIsKernelMap, false,
			NULL, fPageMapper, fMapCount);
		if (pde != NULL && (*pde & (X86_64_PDE_PRESENT | X86_64_PDE_LARGE_PAGE))
				== (X86_64_PDE_PRESENT | X86_64_PDE_LARGE_PAGE)) {
			_SplitLargePage(address, pde);
		}

		if (address + k64BitPageTableRange < address)
			break;
	}
}


void
X86VMTranslationMap64Bit::_SplitLargePage(addr_t address, uint64* pde)
{
	// Make the entry non-present and invalidate it first, so that no CPU
	// sets the accessed or dirty flag in it while it is copied. Accesses
	// meanwhile fault and wait for our lock.
	uint64 oldEntry = *(volatile uint64*)pde;
	while (true) {
		uint64 previous = X86PagingMethod64Bit::TestAndSetTableEntry(pde,
			oldEntry & ~X86_64_PDE_PRESENT, oldEntry);
		if (previous == oldEntry)
			break;
		oldEntry = previous;
	}
	InvalidatePage(address);
	Flush();
	oldEntry = *(volatile uint64*)pde;

	vm_page* page = vm_page_allocate_page(&fSplitReservation,
		PAGE_STATE_WIRED | VM_PAGE_ALLOC_CLEAR);
	DEBUG_PAGE_ACCESS_END(page);
	phys_addr_t physicalTable
		= (phys_addr_t)page->physical_page_number * B_PAGE_SIZE;

	uint64* table = (uint64*)fPageMapper->GetPageTableAt(physicalTable);
	phys_addr_t base = oldEntry & X86_64_PDE_ADDRESS_MASK
		& ~(phys_addr_t)(k64BitPageTableRange - 1);
	uint64 flags = X86_64_PTE_PRESENT | (oldEntry
		& (X86_64_PTE_WRITABLE | X86_64_PTE_USER | X86_64_PTE_WRITE_THROUGH
			| X86_64_PTE_CACHING_DISABLED | X86_64_PTE_ACCESSED
			| X86_64_PTE_DIRTY | X86_64_PTE_GLOBAL | X86_64_PTE_MAY_BE_CACHED
			| X86_64_PTE_NOT_EXECUTABLE));
	for (uint32 i = 0; i < k64BitTableEntryCount; i++)
		table[i] = (base + (phys_addr_t)i * B_PAGE_SIZE) | flags;

	X86PagingMethod64Bit::SetTableEntry(pde,
		(physicalTable & X86_64_PDE_ADDRESS_MASK) | X86_64_PDE_PRESENT
			| X86_64_PDE_WRITABLE | X86_64_PDE_USER);
	fMapCount++;
	atomic_add(&fLargePageCount, -1);
}

bool
X86VMTranslationMap64Bit::DebugGetReverseMappingInfo(phys_addr_t physicalAddress,
	ReverseMappingInfoCallback& callback)
{
	uint64* virtualPML5 = NULL;
	if (fLA57)
		virtualPML5 = fPagingStructures->VirtualPMLTop();
	for (uint32 p = 0; p < (fIsKernelMap ? 512 : 256); p++) {
		uint64* virtualPML4 = NULL;
		uint32 limitPML4 = (fIsKernelMap ? 512 : 256);
		if (fLA57) {
			if ((virtualPML5[p] & X86_64_PML5E_PRESENT) == 0)
				continue;

			virtualPML4 = (uint64*)fPageMapper->GetPageTableAt(
				virtualPML5[p] & X86_64_PML5E_ADDRESS_MASK);
			limitPML4 = 512;
		} else {
			virtualPML4 = fPagingStructures->VirtualPMLTop();
		}

		for (uint32 i = 0; i < limitPML4; i++) {
			if ((virtualPML4[i] & X86_64_PML4E_PRESENT) == 0)
				continue;
			uint64 addressMask = 0;
			if (fLA57) {
				if (p >= 256)
					addressMask = 0xff00000000000000LL;
			} else {
				if (i >= 256)
					addressMask = 0xfffff00000000000LL;
			}

			const uint64* virtualPDPT = (uint64*)fPageMapper->GetPageTableAt(
				virtualPML4[i] & X86_64_PML4E_ADDRESS_MASK);
			for (uint32 j = 0; j < 512; j++) {
				if ((virtualPDPT[j] & X86_64_PDPTE_PRESENT) == 0)
					continue;

				const uint64* virtualPageDir = (uint64*)fPageMapper->GetPageTableAt(
					virtualPDPT[j] & X86_64_PDPTE_ADDRESS_MASK);
				for (uint32 k = 0; k < 512; k++) {
					if ((virtualPageDir[k] & X86_64_PDE_PRESENT) == 0)
						continue;

					if ((virtualPageDir[k] & X86_64_PDE_LARGE_PAGE) != 0) {
						phys_addr_t largeAddress = virtualPageDir[k] & X86_64_PDE_ADDRESS_MASK;
						if (physicalAddress >= largeAddress
								&& physicalAddress < (largeAddress + k64BitPageTableRange)) {
							off_t offset = physicalAddress - largeAddress;
							addr_t virtualAddress = p * k64BitPML4TRange
								+ i * k64BitPDPTRange
								+ j * k64BitPageDirectoryRange
								+ k * k64BitPageTableRange
								+ offset;
							virtualAddress |= addressMask;
							if (callback.HandleVirtualAddress(virtualAddress))
								return true;
						}
						continue;
					}

					const uint64* virtualPageTable = (uint64*)fPageMapper->GetPageTableAt(
						virtualPageDir[k] & X86_64_PDE_ADDRESS_MASK);
					for (uint32 l = 0; l < 512; l++) {
						if ((virtualPageTable[l] & X86_64_PTE_PRESENT) == 0)
							continue;

						if ((virtualPageTable[l] & X86_64_PTE_ADDRESS_MASK) == physicalAddress) {
							addr_t virtualAddress = p * k64BitPML4TRange
								+ i * k64BitPDPTRange
								+ j * k64BitPageDirectoryRange
								+ k * k64BitPageTableRange
								+ l * B_PAGE_SIZE;
							virtualAddress |= addressMask;
							if (callback.HandleVirtualAddress(virtualAddress))
								return true;
						}
					}
				}
			}
		}

		if (!fLA57)
			break;
	}

	return false;
}


X86PagingStructures*
X86VMTranslationMap64Bit::PagingStructures() const
{
	return fPagingStructures;
}
