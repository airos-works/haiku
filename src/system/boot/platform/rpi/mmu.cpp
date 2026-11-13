/*
 * Copyright 2026, air/OS. All rights reserved.
 * Copyright 2019-2023 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Memory management for the Raspberry Pi boot platform.

	There is no firmware to ask for memory, so the loader keeps its own
	books: gKernelArgs.physical_memory_range holds the RAM the device tree
	describes, gKernelArgs.physical_allocated_range everything that is taken.
	The loader runs in EL1 on an identity map with the caches on; the kernel's
	tables hang off TTBR1 and are filled in just before the kernel starts. */


#include "mmu.h"

#include <string.h>

#include <algorithm>
#include <new>

#include <arch/cpu.h>
#include <arch_kernel.h>
#include <boot/addr_range.h>
#include <boot/kernel_args.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <kernel.h>

#include "aarch64.h"
#include "arch_mmu.h"
#include "dtb.h"


//#define TRACE_MMU
#ifdef TRACE_MMU
#	define TRACE(x...) dprintf(x)
#else
#	define TRACE(x...) ;
#endif


extern uint8 _start;
extern uint8 _image_end;


struct memory_region {
	memory_region*	next;
	addr_t			vaddr;
	phys_addr_t		paddr;
	size_t			size;
};


// The BCM2711's peripherals, including the PCIe window, the GIC and the
// main peripherals. Everything else that is not RAM belongs to the VideoCore
// (the frame buffer is in there).
static const phys_addr_t kPeripheralBase = 0xfc000000;

// The BCM2712's (Pi 5) peripherals are all above 64 GB: the SoC's own at
// 0x10_0000_0000, the PCIe windows, and with them the RP1 (UARTs, USB,
// Ethernet) at 0x1f_0000_0000. Below 4 GB is only RAM there.
static const phys_addr_t kBCM2712PeripheralBase = 0x1000000000ull;
static const phys_addr_t kBCM2712PeripheralEnd = 0x2000000000ull;

static const size_t kMaxKernelSize = 0x2000000;		// 32 MB
static addr_t sNextVirtualAddress = KERNEL_LOAD_BASE + kMaxKernelSize;
static memory_region* sAllocatedRegions = NULL;

static ARMv8TranslationRegime::TranslationDescriptor sTranslation4Kb48bits = {
	{L0_SHIFT, L0_ADDR_MASK, false, true, false },
	{L1_SHIFT, Ln_ADDR_MASK, true, true,  false },
	{L2_SHIFT, Ln_ADDR_MASK, true, true,  false },
	{L3_SHIFT, Ln_ADDR_MASK, false, false, true }
};

static ARMv8TranslationRegime sRegime(sTranslation4Kb48bits);

// The identity map is needed before anything else can run (with the MMU off
// all memory behaves like device memory, where unaligned accesses fault), so
// its tables are part of the image: level 0, level 1, and four level 2
// tables for the low 4 GB.
static uint64 sIdentityTables[6][512] __attribute__((aligned(4096)));
static uint64 sKernelRootTable[512] __attribute__((aligned(4096)));

static uint64* const sTTBR0 = sIdentityTables[0];
static uint64* const sTTBR1 = sKernelRootTable;


//	#pragma mark - physical memory


extern "C" phys_addr_t
mmu_allocate_physical(size_t size, phys_addr_t maxAddress)
{
	size = ROUNDUP(size, B_PAGE_SIZE);

	for (uint32 i = 0; i < gKernelArgs.num_physical_memory_ranges; i++) {
		const addr_range& range = gKernelArgs.physical_memory_range[i];
		uint64 end = range.start + range.size;
		if (maxAddress != 0)
			end = std::min(end, (uint64)maxAddress);

		uint64 address;
		if (!get_free_address_range(gKernelArgs.physical_allocated_range,
				gKernelArgs.num_physical_allocated_ranges, range.start, size,
				&address)) {
			continue;
		}
		if (address + size > end)
			continue;

		if (insert_physical_allocated_range(address, size) != B_OK)
			return 0;
		return address;
	}

	return 0;
}


static void
free_physical(phys_addr_t address, size_t size)
{
	remove_physical_allocated_range(address, ROUNDUP(size, B_PAGE_SIZE));
}


extern "C" phys_addr_t
mmu_allocate_page()
{
	phys_addr_t page = mmu_allocate_physical(B_PAGE_SIZE, 0);
	if (page == 0)
		panic("Out of memory for page tables");
	return page;
}


extern "C" addr_t
get_next_virtual_address(size_t size)
{
	addr_t address = sNextVirtualAddress;
	sNextVirtualAddress += ROUNDUP(size, B_PAGE_SIZE);
	return address;
}


//	#pragma mark - platform allocator


extern "C" ssize_t
platform_allocate_heap_region(size_t size, void** _base)
{
	size = ROUNDUP(size, B_PAGE_SIZE);
	phys_addr_t base = mmu_allocate_physical(size, 0);
	if (base == 0)
		return B_NO_MEMORY;

	*_base = (void*)base;
	return size;
}


extern "C" void
platform_free_heap_region(void* base, size_t size)
{
	free_physical((phys_addr_t)base, size);
}


static status_t
allocate_region(void** _address, size_t size, phys_addr_t maxAddress)
{
	phys_addr_t address = mmu_allocate_physical(size, maxAddress);
	if (address == 0)
		return B_NO_MEMORY;

	memory_region* region = new(std::nothrow) memory_region {
		next: sAllocatedRegions,
		vaddr: 0,
		paddr: address,
		size: size
	};
	if (region == NULL) {
		free_physical(address, size);
		return B_NO_MEMORY;
	}

	if (*_address != NULL) {
		// This is only useful for mapping the kernel itself.
		addr_t virtualAddress = (addr_t)*_address;
		if (virtualAddress < KERNEL_LOAD_BASE
			|| (virtualAddress + size) > (KERNEL_LOAD_BASE + kMaxKernelSize)) {
			free_physical(address, size);
			delete region;
			return B_BAD_VALUE;
		}
		region->vaddr = virtualAddress;
	}

	sAllocatedRegions = region;
	*_address = (void*)region->paddr;
	return B_OK;
}


/*!	The loader's address space is not the kernel's: regions are handed out
	at their physical address, and get their kernel address when the
	kernel_args are converted, just before the kernel starts.
*/
extern "C" status_t
platform_allocate_region(void** _address, size_t size, uint8 protection)
{
	return allocate_region(_address, size, 0);
}


extern "C" status_t
platform_allocate_region_below(void** _address, size_t size,
	phys_addr_t maxAddress)
{
	*_address = NULL;
	return allocate_region(_address, size, maxAddress);
}


extern "C" status_t
platform_assign_kernel_address_for_region(void* address, addr_t assign)
{
	phys_addr_t addr = (phys_addr_t)address;
	for (memory_region* region = sAllocatedRegions; region != NULL;
			region = region->next) {
		if (region->paddr <= addr && addr < region->paddr + region->size) {
			if (region->paddr != addr)
				return EINVAL;
			if (region->vaddr != 0)
				return EALREADY;
			region->vaddr = assign;
			return B_OK;
		}
	}
	return B_ERROR;
}


extern "C" status_t
platform_bootloader_address_to_kernel_address(void* address, addr_t* _result)
{
	phys_addr_t addr = (phys_addr_t)address;
	for (memory_region* region = sAllocatedRegions; region != NULL;
			region = region->next) {
		if (region->paddr <= addr && addr < region->paddr + region->size) {
			// Lazily allocate virtual memory.
			if (region->vaddr == 0)
				region->vaddr = get_next_virtual_address(region->size);
			*_result = region->vaddr + (addr - region->paddr);
			return B_OK;
		}
	}
	return B_ERROR;
}


extern "C" status_t
platform_kernel_address_to_bootloader_address(addr_t address, void** _result)
{
	for (memory_region* region = sAllocatedRegions; region != NULL;
			region = region->next) {
		if (region->vaddr != 0 && region->vaddr <= address
			&& address < region->vaddr + region->size) {
			*_result = (void*)(region->paddr + (address - region->vaddr));
			return B_OK;
		}
	}
	return B_ERROR;
}


extern "C" status_t
platform_free_region(void* address, size_t size)
{
	for (memory_region** ref = &sAllocatedRegions; *ref != NULL;
			ref = &(*ref)->next) {
		if ((*ref)->paddr == (phys_addr_t)address && (*ref)->size == size) {
			memory_region* old = *ref;
			*ref = old->next;
			free_physical(old->paddr, old->size);
			delete old;
			return B_OK;
		}
	}
	panic("platform_free_region: unknown region %p", address);
	return B_ERROR;
}


bool
mmu_next_region(void** cookie, addr_t* vaddr, phys_addr_t* paddr, size_t* size)
{
	if (*cookie == NULL)
		*cookie = sAllocatedRegions;
	else
		*cookie = ((memory_region*)*cookie)->next;

	memory_region* region = (memory_region*)*cookie;
	if (region == NULL)
		return false;

	if (region->vaddr == 0)
		region->vaddr = get_next_virtual_address(region->size);

	*vaddr = region->vaddr;
	*paddr = region->paddr;
	*size = region->size;
	return true;
}


//	#pragma mark - page tables


static uint64
map_region(addr_t virt_addr, addr_t phys_addr, size_t size, uint32_t level,
	uint64_t flags, uint64* descriptor)
{
	ARMv8TranslationTableDescriptor ttd(descriptor);

	if (level >= sRegime.MaxLevels())
		panic("Too many levels at mapping\n");

	uint64 currentLevelSize = sRegime.EntrySize(level);

	ttd.JumpTo(sRegime.DescriptorIndex(virt_addr, level));

	uint64 remainingSizeInTable = sRegime.TableSize(level)
		- currentLevelSize * sRegime.DescriptorIndex(virt_addr, level);

	while (remainingSizeInTable > 0 && size > 0) {
		uint64 sizeMapped = 0;
		if (size >= currentLevelSize
			&& sRegime.Aligned(phys_addr, level)
			&& sRegime.Aligned(virt_addr, level)) {
			if (sRegime.BlocksAllowed(level))
				ttd.SetAsBlock(reinterpret_cast<uint64*>(phys_addr), flags);
			else
				ttd.SetAsPage(reinterpret_cast<uint64*>(phys_addr), flags);
			sizeMapped = currentLevelSize;
		} else {
			if (ttd.IsInvalid()) {
				uint64* page = sRegime.AllocatePage();
				ttd.SetToTable(page, flags);
			}
			sizeMapped = size - map_region(virt_addr, phys_addr, size,
				level + 1, flags, ttd.Dereference());
		}

		virt_addr += sizeMapped;
		phys_addr += sizeMapped;
		size -= sizeMapped;
		remainingSizeInTable -= currentLevelSize;

		ttd.Next();
	}

	return size;
}


static void
map_range(addr_t virt_addr, phys_addr_t phys_addr, size_t size, uint64_t flags)
{
	TRACE("map 0x%0lx --> 0x%0lx, len=0x%0lx, flags=0x%0lx\n",
		(uint64_t)virt_addr, (uint64_t)phys_addr, (uint64_t)size, flags);

	if (size == 0)
		return;

	uint64* table = arch_mmu_is_kernel_address(virt_addr) ? sTTBR1 : sTTBR0;
	map_region(virt_addr, phys_addr, PAGE_ALIGN(size), 0, flags, table);

	if (arch_mmu_is_kernel_address(virt_addr)) {
		ASSERT_ALWAYS(insert_virtual_allocated_range(virt_addr, size) >= B_OK);
	}
}


static bool
is_ram(phys_addr_t address, size_t size)
{
	for (uint32 i = 0; i < gKernelArgs.num_physical_memory_ranges; i++) {
		const addr_range& range = gKernelArgs.physical_memory_range[i];
		if (address < range.start + range.size
			&& range.start < address + size) {
			return true;
		}
	}
	return false;
}


static uint64
identity_flags(phys_addr_t address, size_t size, bool memoryKnown)
{
	MemoryAttributeIndirection mair;
	const uint64 kBlock = 0x1;

	// Before the device tree has been read the SoC is not known: the
	// BCM2711's window is device memory then. The loader allocates from the
	// bottom of RAM, so a Pi 5 has no business up there before that.
	if ((address >= kPeripheralBase && address < 0x100000000ull
			&& (!memoryKnown || !gBCM2712))
		|| (address >= kBCM2712PeripheralBase
			&& address < kBCM2712PeripheralEnd)) {
		return ARMv8TranslationTableDescriptor::DefaultPeripheralAttribute
			| mair.MaskOf(MAIR_DEVICE_nGnRnE) | kBlock;
	}
	if (memoryKnown && !is_ram(address, size)) {
		// the VideoCore's memory: the frame buffer is in there
		return ARMv8TranslationTableDescriptor::DefaultPeripheralAttribute
			| mair.MaskOf(MAIR_NORMAL_NC) | kBlock;
	}
	return ARMv8TranslationTableDescriptor::DefaultCodeAttribute
		| mair.MaskOf(MAIR_NORMAL_WB) | kBlock;
}


/*!	The identity map: the low 4 GB in 2 MB blocks, the RAM above that (the
	8 and 16 GB boards have some) and the Pi 5's peripherals in 1 GB blocks.
	Before the device tree has been read, everything that is not a
	peripheral is taken to be RAM; afterwards what is not RAM gets mapped
	uncached.
*/
static void
build_identity_map(bool memoryKnown)
{
	const uint64 kTable = 0x3;
	const uint64 kGigabyte = 1ull << 30;
	const uint64 kBlockSize = 1ull << 21;

	uint64* level1 = sIdentityTables[1];
	sIdentityTables[0][0] = (uint64)level1 | kTable;

	for (uint64 gigabyte = 0; gigabyte < kBCM2712PeripheralEnd / kGigabyte;
			gigabyte++) {
		uint64 base = gigabyte * kGigabyte;
		if (base >= kBCM2712PeripheralBase) {
			level1[gigabyte] = base | identity_flags(base, kGigabyte, false);
			continue;
		}
		if (gigabyte >= 16) {
			level1[gigabyte] = 0;
			continue;
		}
		if (gigabyte >= 4) {
			if (!memoryKnown || is_ram(base, kGigabyte))
				level1[gigabyte] = base | identity_flags(base, kGigabyte, false);
			else
				level1[gigabyte] = 0;
			continue;
		}

		uint64* level2 = sIdentityTables[2 + gigabyte];
		for (uint64 i = 0; i < 512; i++) {
			uint64 address = base + i * kBlockSize;
			uint64 entry = address
				| identity_flags(address, kBlockSize, memoryKnown);
			if (level2[i] == entry)
				continue;

			if (level2[i] != 0) {
				// break before make
				level2[i] = 0;
				asm("dsb sy\n"
					"tlbi vmalle1\n"
					"dsb sy\n"
					"isb");
			}
			level2[i] = entry;
		}
		level1[gigabyte] = (uint64)level2 | kTable;
	}

	asm("dsb sy\n"
		"isb");
}


void
mmu_enable_on_this_cpu()
{
	// 48-bit address space
	// Inner-shareable, write-back/write-allocate page table walks
	uint64 tcr = TCR_TxSZ(16)
		| TCR_SH0_IS | TCR_IRGN0_WBWA | TCR_ORGN0_WBWA
		| TCR_SH1_IS | TCR_IRGN1_WBWA | TCR_ORGN1_WBWA
		| TCR_TG0_4K | TCR_TG1_4K;

	uint64 paSize = READ_SPECIALREG(ID_AA64MMFR0_EL1)
		& ID_AA64MMFR0_PA_RANGE_MASK;
	if (paSize == ID_AA64MMFR0_PA_RANGE_4P)
		paSize = ID_AA64MMFR0_PA_RANGE_256T;
	tcr |= paSize << TCR_IPS_SHIFT;

	WRITE_SPECIALREG(TCR_EL1, tcr);
	WRITE_SPECIALREG(MAIR_EL1, MAIR_VALUE);
	WRITE_SPECIALREG(TTBR0_EL1, (uint64)sTTBR0);
	WRITE_SPECIALREG(TTBR1_EL1, (uint64)sTTBR1);
	WRITE_SPECIALREG(CNTKCTL_EL1, 0b11);
	asm("isb\n"
		"tlbi vmalle1\n"
		"dsb sy\n"
		"isb");

	WRITE_SPECIALREG(SCTLR_EL1, SCTLR_LSMAOE | SCTLR_nTLSMD
		| SCTLR_UCI | SCTLR_SPAN | SCTLR_IESB | SCTLR_nTWE | SCTLR_nTWI
		| SCTLR_UCT | SCTLR_DZE | SCTLR_SED | SCTLR_SA0 | SCTLR_SA
		| SCTLR_M | SCTLR_C | SCTLR_I);
	asm("isb");
}


void
mmu_flush_data_cache(void* address, size_t size)
{
	const addr_t kLineSize = 64;
	addr_t end = (addr_t)address + size;
	for (addr_t line = (addr_t)address & ~(kLineSize - 1); line < end;
			line += kLineSize) {
		asm("dc civac, %0" : : "r" (line));
	}
	asm("dsb sy");
}


void
mmu_init_early()
{
	build_identity_map(false);
	mmu_enable_on_this_cpu();
}


void
mmu_init()
{
	// What the firmware and the loader itself occupy: everything up to the
	// end of the image (the armstub and its spin table live in the first
	// page).
	insert_physical_allocated_range(0,
		ROUNDUP((addr_t)&_image_end, B_PAGE_SIZE));

	build_identity_map(true);
}


uint64
mmu_init_for_kernel()
{
	MemoryAttributeIndirection mair;
	const uint64 ramFlags = ARMv8TranslationTableDescriptor::DefaultCodeAttribute
		| mair.MaskOf(MAIR_NORMAL_WB);
	const uint64 deviceFlags
		= ARMv8TranslationTableDescriptor::DefaultPeripheralAttribute
			| mair.MaskOf(MAIR_DEVICE_nGnRnE);

	// the regions the loader allocated: the kernel, its modules and arguments
	void* cookie = NULL;
	addr_t vaddr;
	phys_addr_t paddr;
	size_t size;
	while (mmu_next_region(&cookie, &vaddr, &paddr, &size))
		map_range(vaddr, paddr, size, ramFlags);

	// the kernel's map of all physical memory
	for (uint32 i = 0; i < gKernelArgs.num_physical_memory_ranges; i++) {
		addr_range range = gKernelArgs.physical_memory_range[i];
		map_range(KERNEL_PMAP_BASE + range.start, range.start, range.size,
			ramFlags);
	}

	if (gKernelArgs.arch_args.uart.kind[0] != 0) {
		// The kernel uses the UART before it can map anything.
		uint64 regsStart = gKernelArgs.arch_args.uart.regs.start;
		uint64 offset = regsStart & (B_PAGE_SIZE - 1);
		uint64 regsSize = ROUNDUP(gKernelArgs.arch_args.uart.regs.size + offset,
			B_PAGE_SIZE);
		uint64 base = get_next_virtual_address(regsSize);

		map_range(base, regsStart - offset, regsSize, deviceFlags);
		gKernelArgs.arch_args.uart.regs.start = base + offset;
	}

	sort_address_ranges(gKernelArgs.virtual_allocated_range,
		gKernelArgs.num_virtual_allocated_ranges);

	addr_t virtualPageDirectory = 0;
	platform_bootloader_address_to_kernel_address((void*)sTTBR1,
		&virtualPageDirectory);

	gKernelArgs.arch_args.phys_pgdir = (uint64)sTTBR1;
	gKernelArgs.arch_args.vir_pgdir = (uint32)virtualPageDirectory;
	gKernelArgs.arch_args.next_pagetable = 0;

	// The tables changed under a live TTBR1; nothing was mapped there before,
	// but make sure the new entries are seen.
	asm("dsb sy\n"
		"tlbi vmalle1\n"
		"dsb sy\n"
		"isb");

	return (uint64)sTTBR1;
}
