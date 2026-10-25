/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "qcom_smmu.h"

#include <string.h>

#include <KernelExport.h>
#include <vm/vm.h>


static const phys_addr_t kAppsSmmuBase = 0x15000000;
static const uint32 kSmmuIdr0 = 0x20, kSmmuIdr1 = 0x24;
static const uint32 kSmmuSmr = 0x800, kSmmuS2cr = 0xc00;
static const uint32 kSmrValid = 1u << 31;


QcomSmmuDomain::QcomSmmuDomain()
	:
	fName("smmu"),
	fRegisterArea(-1),
	fRegisters(NULL),
	fPageSize(0),
	fContexts(0),
	fBank(-1),
	fEntryCount(0),
	fLevel1(NULL),
	fLevel1Physical(0),
	fTableAreaCount(0)
{
	memset(fLevel2, 0, sizeof(fLevel2));
	memset(fLevel3, 0, sizeof(fLevel3));
}


QcomSmmuDomain::~QcomSmmuDomain()
{
	Detach();
}


/*!	As qcom_iris' smmu_attach(): a context bank no valid stream match entry
	translates to and that is off, a table, the bank set up (CBAR: stage 1,
	stage 2 bypass, write-back, VMID bank + 1; TCR: T0SZ 32; MAIR 0
	write-back, 1 non-cacheable) and switched on, then each stream's match
	entry (its own, if it has one, else a free one) pointed at it.
*/
status_t
QcomSmmuDomain::Attach(const char* name, const uint32 (*streams)[2],
	uint32 streamCount)
{
	if (streamCount > B_COUNT_OF(fEntries))
		return B_BAD_VALUE;
	fName = name;
	fRegisterArea = map_physical_memory("qcom smmu", kAppsSmmuBase, 0x100000,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fRegisters);
	if (fRegisterArea < 0)
		return fRegisterArea;
	uint32 idr1 = _Read(kSmmuIdr1);
	fPageSize = (idr1 & (1u << 31)) != 0 ? 0x10000 : 0x1000;
	fContexts = (1u << (((idr1 >> 28) & 7) + 1)) * fPageSize;
	uint32 contexts = idr1 & 0xff;
	uint32 groups = _Read(kSmmuIdr0) & 0xff;
	if (fContexts + contexts * fPageSize > 0x100000) {
		Detach();
		return B_NOT_SUPPORTED;
	}

	uint64 used[4] = {};
	for (uint32 i = 0; i < groups; i++) {
		if ((_Read(kSmmuSmr + 4 * i) & kSmrValid) == 0)
			continue;
		uint32 s2cr = _Read(kSmmuS2cr + 4 * i);
		if (((s2cr >> 16) & 3) == 0)
			used[(s2cr & 0xff) / 64] |= 1ull << ((s2cr & 0xff) % 64);
	}
	for (int32 bank = contexts - 1; bank >= 0; bank--) {
		if ((used[bank / 64] & (1ull << (bank % 64))) == 0
			&& (_Read(fContexts + bank * fPageSize) & 1) == 0) {
			fBank = bank;
			break;
		}
	}
	if (fBank < 0) {
		dprintf("%s: no free context bank on the apps SMMU\n", fName);
		Detach();
		return B_BUSY;
	}
	fLevel1 = _Table(fLevel1Physical);
	if (fLevel1 == NULL) {
		Detach();
		return B_NO_MEMORY;
	}

	uint32 cb = fContexts + fBank * fPageSize;
	_Write(cb + 0x0, 0);								// SCTLR
	_Write(fPageSize + 0x800 + 4 * fBank, 1);			// CBA2R: VA64
	_Write(fPageSize + 4 * fBank, 1u << 16 | 0xfu << 12 | 3u << 8
		| (fBank + 1));									// CBAR
	_Write(cb + 0x10, 7u << 15 | 2);					// TCR2: 40-bit PA
	_Write(cb + 0x30, 32 | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23);
	_Write(cb + 0x20, (uint32)fLevel1Physical);		// TTBR0
	_Write(cb + 0x24, (uint32)(fLevel1Physical >> 32));
	_Write(cb + 0x38, 0xffu | 0x44u << 8 | 0x04u << 16);	// MAIR0
	_Write(cb + 0x3c, 0);
	_Write(cb + 0x58, 0xffffffff);						// FSR
	_FlushTlb();
	// on, TEX remap, access flags, faults reported
	_Write(cb + 0x0, 1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 6);

	for (uint32 s = 0; s < streamCount; s++) {
		uint32 smr = kSmrValid | streams[s][1] << 16 | streams[s][0];
		// another valid entry that matches the stream too: the SMMU's
		// behaviour is then undefined (a stream match conflict)
		for (uint32 i = 0; i < groups; i++) {
			uint32 value = _Read(kSmmuSmr + 4 * i);
			if ((value & kSmrValid) == 0 || value == smr)
				continue;
			uint32 mask = (value >> 16 | streams[s][1]) & 0x7fff;
			if (((value ^ streams[s][0]) & ~mask & 0x7fff) == 0) {
				dprintf("%s: stream %#" B_PRIx32 " also matches entry %" B_PRIu32
					" (%#" B_PRIx32 ", S2CR %#" B_PRIx32 ")\n", fName,
					streams[s][0], i, value, _Read(kSmmuS2cr + 4 * i));
			}
		}
		int32 entry = -1;
		for (uint32 i = 0; i < groups; i++) {
			uint32 value = _Read(kSmmuSmr + 4 * i);
			if (value == smr) {
				entry = i;
				break;
			}
			if ((value & kSmrValid) == 0 && entry < 0)
				entry = i;
		}
		if (entry < 0) {
			dprintf("%s: no free stream match entry\n", fName);
			Detach();
			return B_BUSY;
		}
		_Write(kSmmuS2cr + 4 * entry, fBank);			// translate
		memory_full_barrier();
		_Write(kSmmuSmr + 4 * entry, smr);
		fEntries[fEntryCount++] = entry;
		dprintf("%s: stream %#" B_PRIx32 " -> apps SMMU context bank %" B_PRId32
			" (entry %" B_PRId32 ")\n", fName, streams[s][0], fBank, entry);
	}
	memory_full_barrier();
	_Write(0x68, 0);		// TLBIALLNSNH
	_Write(0x70, 0);		// sTLBGSYNC
	for (int i = 0; i < 1000 && (_Read(0x74) & 1) != 0; i++)
		spin(10);
	return B_OK;
}


//!	The streams' entries freed (faulted, as unmatched), the bank off.
void
QcomSmmuDomain::Detach()
{
	if (fRegisters != NULL) {
		for (uint32 i = 0; i < fEntryCount; i++)
			_Write(kSmmuSmr + 4 * fEntries[i], 0);
		if (fBank >= 0)
			_Write(fContexts + fBank * fPageSize, 0);
		memory_full_barrier();
		delete_area(fRegisterArea);
	}
	fEntryCount = 0;
	fBank = -1;
	fRegisters = NULL;
	fRegisterArea = -1;
	for (uint32 i = 0; i < fTableAreaCount; i++)
		delete_area(fTableAreas[i]);
	fTableAreaCount = 0;
	fLevel1 = NULL;
	memset(fLevel2, 0, sizeof(fLevel2));
	memset(fLevel3, 0, sizeof(fLevel3));
}


status_t
QcomSmmuDomain::Map(uint64 address, phys_addr_t physical, size_t size,
	bool cached)
{
	if (fLevel1 == NULL)
		return B_NO_INIT;
	for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE) {
		uint64* level3 = _Level3(address + offset, true);
		if (level3 == NULL)
			return B_NO_MEMORY;
		// a page, AttrIndx (0 or 1), read/write at EL0, inner shareable,
		// accessed
		level3[((address + offset) >> 12) & 511]
			= ((physical + offset) & 0x0000fffffffff000ull) | 3
			| (cached ? 0 : 1ull << 2) | 1ull << 6 | 3ull << 8 | 1ull << 10;
	}
	memory_full_barrier();
	_FlushTlb();
	return B_OK;
}


void
QcomSmmuDomain::Unmap(uint64 address, size_t size)
{
	for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE) {
		uint64* level3 = _Level3(address + offset, false);
		if (level3 != NULL)
			level3[((address + offset) >> 12) & 511] = 0;
	}
	memory_full_barrier();
	_FlushTlb();
}


void
QcomSmmuDomain::ClearAccessFlags(uint64 address, size_t size)
{
	for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE) {
		uint64* level3 = _Level3(address + offset, false);
		if (level3 != NULL)
			level3[((address + offset) >> 12) & 511] &= ~(1ull << 10);
	}
	memory_full_barrier();
	_FlushTlb();
}


uint32
QcomSmmuDomain::Faults(const char* when)
{
	if (fRegisters == NULL || fBank < 0)
		return 0;
	// a global fault: a stream no entry matches, say (sGFSR, sGFSYNR0-1)
	uint32 global = _Read(0x48);
	if (global != 0 && when != NULL) {
		dprintf("%s: %s: SMMU global fault, GFSR %#" B_PRIx32 " GFSYNR0 %#"
			B_PRIx32 " stream %#" B_PRIx32 " GFAR %#" B_PRIx32 "%08" B_PRIx32
			"\n", fName, when, global, _Read(0x50), _Read(0x54) & 0xffff,
			_Read(0x44), _Read(0x40));
		_Write(0x48, global);
	}
	uint32 cb = fContexts + fBank * fPageSize;
	uint32 status = _Read(cb + 0x58) & ~0x600u;		// not FORMAT
	if (status != 0 && when != NULL) {
		dprintf("%s: %s: SMMU fault, FSR %#" B_PRIx32 " FAR %#" B_PRIx32 "%08"
			B_PRIx32 " FSYNR0 %#" B_PRIx32 " stream %#" B_PRIx32 "\n", fName,
			when, status, _Read(cb + 0x64), _Read(cb + 0x60), _Read(cb + 0x68),
			_Read(fPageSize + 0x400 + 4 * fBank) & 0xffff);
		_Write(cb + 0x58, 0xffffffff);
	}
	return status;
}


uint64*
QcomSmmuDomain::_Table(phys_addr_t& physical)
{
	if (fTableAreaCount == B_COUNT_OF(fTableAreas))
		return NULL;
	void* table;
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	area_id area = create_area_etc(B_SYSTEM_TEAM, "qcom smmu table",
		B_PAGE_SIZE, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		0, 0, &virtualRestrictions, &physicalRestrictions, &table);
	if (area < 0)
		return NULL;
	memset(table, 0, B_PAGE_SIZE);
	physical_entry entry;
	get_memory_map(table, B_PAGE_SIZE, &entry, 1);
	physical = entry.address;
	fTableAreas[fTableAreaCount++] = area;
	return (uint64*)table;
}


uint64*
QcomSmmuDomain::_Level3(uint64 address, bool create)
{
	if (address >= (1ull << 32))
		return NULL;
	uint32 gigabyte = address >> 30;
	uint32 block = address >> 21;
	if (fLevel2[gigabyte] == NULL) {
		if (!create)
			return NULL;
		phys_addr_t physical;
		fLevel2[gigabyte] = _Table(physical);
		if (fLevel2[gigabyte] == NULL)
			return NULL;
		memory_full_barrier();
		fLevel1[gigabyte] = physical | 3;
	}
	if (fLevel3[block] == NULL) {
		if (!create)
			return NULL;
		phys_addr_t physical;
		fLevel3[block] = _Table(physical);
		if (fLevel3[block] == NULL)
			return NULL;
		memory_full_barrier();
		fLevel2[gigabyte][block & 511] = physical | 3;
	}
	return fLevel3[block];
}


void
QcomSmmuDomain::_FlushTlb()
{
	if (fRegisters == NULL || fBank < 0)
		return;
	uint32 cb = fContexts + fBank * fPageSize;
	_Write(cb + 0x618, 0);			// TLBIALL
	_Write(cb + 0x7f0, 0);			// TLBSYNC
	for (int i = 0; i < 1000 && (_Read(cb + 0x7f4) & 1) != 0; i++)
		spin(10);
}


uint32
QcomSmmuDomain::_Read(uint32 offset) const
{
	return *(volatile uint32*)(fRegisters + offset);
}


void
QcomSmmuDomain::_Write(uint32 offset, uint32 value)
{
	*(volatile uint32*)(fRegisters + offset) = value;
}
