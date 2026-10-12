/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2026, air/OS.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

// GMC 8.1 programming follows Linux 6.18.52 gmc_v8_0.c (MIT).
#include "Gart.h"
#include <KernelExport.h>
#include <vm/vm.h>
#include <string.h>

status_t
Gart::Flush()
{
	__sync_synchronize();
	(void)table[0];
	regs[0x1520] = 1; // HDP_MEM_COHERENCY_FLUSH_CNTL: CPU PTE writes
	(void)regs[0x1520];
	regs[0x51e] = 1; // VM_INVALIDATE_REQUEST: VMID0 only
	bigtime_t deadline = system_time() + 100000;
	while ((regs[0x51f] & 1) == 0) {
		if (system_time() >= deadline)
			return B_TIMED_OUT;
		snooze(10);
	}
	return B_OK;
}

status_t
Gart::Initialize(volatile uint32* r, const amdgpu_info& info,
	const amdgpu::AtomVramReservation& reservation)
{
	tableArea = dummyArea = commandArea = -1;
	enabled = false;
	boundPages = scatterBoundaries = 0;
	regs = r;
	const uint64 tableOffset = 16ULL << 20;
	const uint64 tableBytes = kSize / 4096 * 8;
	if ((r[0x504] & 1) != 0 || (r[0x505] & 1) != 0
		|| info.vram_gpu_base < kBase + kSize
		|| !amdgpu_vram_range_is_safe(r, info, reservation, tableOffset, tableBytes))
		return B_NOT_ALLOWED;
	tableArea = map_physical_memory("amdgpu GART page table",
		info.bar_address[0] + tableOffset, tableBytes, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&table);
	if (tableArea < 0)
		return tableArea;
	virtual_address_restrictions va = {};
	physical_address_restrictions pa = {};
	void* dummy = NULL;
	dummyArea = create_area_etc(B_SYSTEM_TEAM, "amdgpu GART fault page", 4096,
		B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0, &va, &pa, &dummy);
	status_t status = dummyArea < 0 ? dummyArea : B_OK;
	physical_entry entry = {};
	uint32 count = 1;
	if (status == B_OK)
		status = get_memory_map_etc(B_SYSTEM_TEAM, dummy, 4096, &entry, &count);
	if (status == B_OK && (entry.size != 4096 || (entry.address & 4095) != 0
		|| entry.address >= (1ULL << 40)))
		status = B_BAD_VALUE;
	if (status != B_OK) {
		Uninitialize(false);
		return status;
	}
	memset(dummy, 0, 4096);
	for (uint64 i = 0; i < tableBytes / 8; i++)
		table[i] = 0;
	// This PCIe driver uses GART page tables, not the legacy AGP aperture.
	// A zero top AND bottom inherited from firmware leaves the first 4 MiB
	// in that aperture. Match gmc_v8_0_mc_program's disabled range, also
	// observed on this WX5100 under Linux, before enabling either VM context.
	r[0x80c] = 0; // MC_VM_AGP_BASE
	r[0x80a] = 0; // MC_VM_AGP_TOP
	r[0x80b] = 0x3ffff; // MC_VM_AGP_BOT > TOP
	r[0x819] = (r[0x819] & ~0x7bu) | 0x5b;
	// L2 enabled, PTE/PDE LRU updates, queue 7, context1 identity mode 1,
	// fault-default address refers to system RAM. Preserve unrelated fields.
	r[0x500] = (r[0x500] & ~0x1b8000u) | 0x000b8e03;
	r[0x501] |= 3;
	// Linux VI default fragment size is 9. Individual PTEs use 4 KiB pages.
	r[0x502] = (r[0x502] & ~0x001f803fu) | 0x00148009;
	r[0x578] &= ~0x3ffc0u;
	r[0x557] = kBase >> 12;
	r[0x55f] = (kBase + kSize - 1) >> 12;
	r[0x54f] = (info.vram_gpu_base + tableOffset) >> 12;
	r[0x546] = entry.address >> 12;
	r[0x50c] = 0;
	// Flat VMID0. Redirect faults to our pinned dummy page; record faults,
	// but do not enable interrupts before an interrupt handler is installed.
	r[0x504] = 1 | 0x10 | 0x80 | 0x400 | 0x800 | 0x2000 | 0x4000
		| 0x10000 | 0x20000 | 0x80000 | 0x100000 | 0x400000 | 0x800000;
	enabled = true;
	status = Flush();
	if (status != B_OK) {
		Uninitialize(true);
		return status;
	}
	// Reserve the first 128 KiB of GART for the snooped CP ring and commands.
	commandArea = create_area_etc(B_SYSTEM_TEAM, "amdgpu command RAM", kCommandBytes,
		B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0, &va, &pa,
		(void**)&commandMemory);
	status = commandArea < 0 ? commandArea : B_OK;
	if (status == B_OK) {
		memset((void*)commandMemory, 0, kCommandBytes);
		status = Bind(0, kCommandBytes, (const void*)commandMemory, true);
	}
	if (status != B_OK) {
		Uninitialize(status == B_TIMED_OUT);
		return status;
	}
	dprintf("amdgpu: GART 1024 MiB, private VMID0, table %#" B_PRIx64 "\n",
		info.vram_gpu_base + tableOffset);
	return B_OK;
}

status_t
Gart::Bind(uint64 offset, uint64 bytes, const void* cpu, bool executable)
{
	if (!enabled || bytes == 0 || ((offset | bytes | (addr_t)cpu) & 4095) != 0
		|| offset >= kSize || bytes > kSize - offset)
		return B_BAD_VALUE;
	// All pages are already wired by create_area_etc(B_FULL_LOCK). Do not
	// assume physical contiguity, and reject pages beyond VI's 40-bit PTE.
	uint64 previous = 0, scatter = 0;
	for (uint64 i = 0; i < bytes; i += 4096) {
		physical_entry entry = {};
		uint32 count = 1;
		status_t status = get_memory_map_etc(B_SYSTEM_TEAM, (const uint8*)cpu + i,
			4096, &entry, &count);
		if (status != B_OK || entry.size != 4096 || (entry.address & 4095) != 0
			|| entry.address >= (1ULL << 40)) {
			// No command can reference this allocation yet. Clear any partial
			// map before the caller releases the backing memory.
			status_t unbind = Unbind(offset, bytes);
			return unbind != B_OK ? unbind : B_BAD_ADDRESS;
		}
		if (i != 0 && entry.address != previous + 4096)
			scatter++;
		previous = entry.address;
		table[(offset + i) / 4096] = entry.address | 0x67 | (executable ? 0x10 : 0);
	}
	status_t status = Flush();
	if (status == B_OK) {
		boundPages += bytes / 4096;
		scatterBoundaries += scatter;
	}
	return status;
}

status_t
Gart::Unbind(uint64 offset, uint64 bytes)
{
	if (!enabled || bytes == 0 || ((offset | bytes) & 4095) != 0
		|| offset >= kSize || bytes > kSize - offset)
		return B_BAD_VALUE;
	for (uint64 i = 0; i < bytes; i += 4096)
		table[(offset + i) / 4096] = 0;
	return Flush();
}

void
Gart::Uninitialize(bool faulted)
{
	if (enabled) {
		regs[0x504] &= ~1u;
		(void)regs[0x504];
		if (Flush() != B_OK)
			faulted = true;
		enabled = false;
	}
	if (tableArea >= 0)
		delete_area(tableArea);
	// If completion is uncertain, even the fault page must remain wired.
	if (dummyArea >= 0 && !faulted)
		delete_area(dummyArea);
	if (commandArea >= 0 && !faulted)
		delete_area(commandArea);
	tableArea = dummyArea = commandArea = -1;
}
