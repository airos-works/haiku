/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Qualcomm Adreno 690 of the SC8280XP (Radxa Dragon Q8B), first stage:
	the GPU's CX side powered up, its SMMU set up for the GMU, the GMU (the
	GPU's Cortex-M3 power manager) booted with its firmware and the HFI
	queues started, following Linux' drm/msm (a6xx_gmu.c, a6xx_hfi.c) and
	arm-smmu. The firmware leaves the GPU off and its SMMU at its reset
	state, so everything is set up here.

	The bring-up runs on the first open of the device (adreno_drm.cpp), which
	then uses the ring, fences and address space provided here. */


#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Drivers.h>
#include <KernelExport.h>
#include <arch/atomic.h>
#include <condition_variable.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <smp.h>
#include <vm/vm.h>

#include <libfdt.h>
#include <sys/stat.h>

#include "a690_tables.h"
#include "qcom_scm.h"
#include "adreno_private.h"


extern void* gFDT;


// SC8280XP blocks
#define GCC_BASE				0x100000
#define GCC_SIZE				0x1f4200
#define GPUCC_BASE				0x3d90000
#define GPUCC_SIZE				0x9000
#define GPU_BASE				0x3d00000
#define GMU_BASE				0x3d6a000
#define GMU_SIZE				0x34000
#define RSCC_BASE				0x3de0000
#define RSCC_SIZE				0x10000
#define GMU_PDC_BASE			0xb290000
#define GMU_PDC_SIZE			0x10000
#define SMMU_BASE				0x3da0000
#define SMMU_SIZE				0x20000

// GMU registers: 32 bit word offsets from the GPU's base
#define GMU_CM3_ITCM_START		0x1b400
#define GMU_CM3_DTCM_START		0x1c400
#define GMU_CORE_FW_VERSION		0x1d3f8
#define GMU_ICACHE_CONFIG		0x1f400
#define GMU_DCACHE_CONFIG		0x1f401
#define GMU_SYS_BUS_CONFIG		0x1f40f
#define GMU_CM3_SYSRESET		0x1f800
#define GMU_CM3_BOOT_CONFIG		0x1f801
#define GMU_CM3_FW_INIT_RESULT	0x1f81c
#define GMU_CM3_CFG				0x1f82d
#define GMU_PWR_COL_INTER_FRAME_CTRL 0x1f8c0
#define GMU_RPMH_CTRL			0x1f8e8
#define  RPMH_INTERFACE_ENABLE	(1u << 0)
#define  LLC_VOTE_ENABLE		(1u << 4)
#define  DDR_VOTE_ENABLE		(1u << 8)
#define  MX_VOTE_ENABLE			(1u << 9)
#define  CX_VOTE_ENABLE			(1u << 10)
#define  GFX_VOTE_ENABLE		(1u << 11)
#define GPU_GMU_CX_GMU_CX_FAL_INTF 0x1f8f0
#define GPU_GMU_CX_GMU_CX_FALNEXT_INTF 0x1f8f1
#define GPU_GMU_CX_GMU_PWR_COL_CP_MSG 0x1f900
#define GPU_GMU_CX_GMU_PWR_COL_CP_RESP 0x1f901
#define GMU_HFI_CTRL_STATUS		0x1f980
#define GMU_HFI_SFR_ADDR		0x1f982
#define GMU_HFI_QTBL_INFO		0x1f984
#define GMU_HFI_QTBL_ADDR		0x1f985
#define GMU_HFI_CTRL_INIT		0x1f986
#define GMU_GMU2HOST_INTR_CLR	0x1f991
#define GMU_GMU2HOST_INTR_INFO	0x1f992
#define  GMU2HOST_MSGQ			(1u << 0)
#define GMU_GMU2HOST_INTR_MASK	0x1f993
#define GMU_HOST2GMU_INTR_SET	0x1f994
#define GMU_GENERAL_7			0x1f9cc
#define GMU_AO_HOST_INTERRUPT_CLR 0x23b04
#define GMU_AO_HOST_INTERRUPT_MASK 0x23b06
#define GMU_RSCC_CONTROL_REQ	0x23b07
#define GMU_RSCC_CONTROL_ACK	0x23b08
#define GMU_AHB_FENCE_RANGE_0	0x23b11
#define GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL 0x1a880
#define GPU_GMU_AO_GMU_CGC_MODE_CNTL 0x23b09
#define GPU_GMU_AO_GMU_CGC_DELAY_CNTL 0x23b0a
#define GPU_GMU_AO_GMU_CGC_HYST_CNTL 0x23b0b

// RSCC registers, 32 bit word offsets
#define RSCC_RSC_STATUS0_DRV0	0x0004
#define RSCC_PDC_SEQ_START_ADDR	0x0008
#define RSCC_PDC_MATCH_VALUE_LO	0x0009
#define RSCC_PDC_MATCH_VALUE_HI	0x000a
#define RSCC_PDC_SLAVE_ID_DRV0	0x000b
#define RSCC_HIDDEN_TCS_CMD0_ADDR 0x000d
#define RSCC_HIDDEN_TCS_CMD0_DATA 0x000e
#define RSCC_OVERRIDE_START_ADDR 0x0100
#define RSCC_SEQ_BUSY_DRV0		0x0101
#define RSCC_SEQ_MEM_0_DRV0		0x0180

// PDC registers, 32 bit word offsets
#define PDC_GPU_ENABLE_PDC		0x1140
#define PDC_GPU_SEQ_START_ADDR	0x1148

// GMU memory, at the device addresses the firmware expects (Linux' A660
// family layout)
#define GMU_ICACHE_ADDRESS		0x00004000
#define GMU_ICACHE_SIZE			(16 * 1024 * 1024 - 16 * 1024)
#define GMU_DUMMY_ADDRESS		0x60000000
#define GMU_DUMMY_SIZE			(8 * 1024)
#define GMU_UNCACHED_ADDRESS	0x60002000	// log, HFI
#define GMU_DEBUG_ADDRESS		0x60400000
#define GMU_DEBUG_SIZE			(28 * 1024)
#define GMU_LOG_SIZE			(16 * 1024)
#define GMU_HFI_SIZE			(16 * 1024)
#define DTCM_BASE				0x10004000	// A650 family
#define TCM_SIZE				(16 * 1024)

// GPU registers (32 bit words), GX side
#define RBBM_STATUS				0x0210

#define GPU_SIZE				0x40000

#define CMD_DB_BASE				0x80860000
#define CMD_DB_SIZE				0x20000

// the GPU's chip ID, in the GMU's layout
#define A690_CHIP_ID			0x06090000
#define GMU_CHIP_ID				(A690_CHIP_ID & 0xffff0000)

// SMMU-500
#define SMMU_sCR0				0x0
#define  sCR0_CLIENTPD			(1u << 0)
#define  sCR0_USFCFG			(1u << 10)
#define SMMU_IDR0				0x20
#define SMMU_IDR1				0x24
#define SMMU_TLBIALLNSNH		0x68
#define SMMU_sTLBGSYNC			0x70
#define SMMU_sTLBGSTATUS		0x74
#define SMMU_SMR(n)				(0x800 + 4 * (n))
#define  SMR_VALID				(1u << 31)
#define SMMU_S2CR(n)			(0xc00 + 4 * (n))
#define  S2CR_TYPE_TRANSLATE	(0u << 16)
#define SMMU_CBAR(n)			(0x1000 + 4 * (n))
#define  CBAR_TYPE_S1_S2BYPASS	(1u << 16)
#define  CBAR_S1_MEMATTR_WB		(0xfu << 12)
#define  CBAR_S1_BPSHCFG_NSH	(3u << 8)
#define SMMU_CBA2R(n)			(0x1800 + 4 * (n))
#define  CBA2R_VA64				(1u << 0)
#define SMMU_CB(n)				(0x10000 + 0x1000 * (n))
#define CB_SCTLR				0x0
#define  CBSCTLR_M				(1u << 0)
#define  CBSCTLR_TRE				(1u << 1)
#define  CBSCTLR_AFE				(1u << 2)
#define  CBSCTLR_CFRE				(1u << 5)
#define  CBSCTLR_CFIE				(1u << 6)
#define CB_TCR2					0x10
#define  CBTCR2_SEP_UPSTREAM		(7u << 15)
#define  CBTCR2_PASIZE_40			2
#define CB_TTBR0				0x20
#define CB_TCR					0x30
#define  CBTCR_T0SZ_32			32
#define  CBTCR_T0SZ_39			25
#define  CBTCR_IRGN0_WB			(1u << 8)
#define  CBTCR_ORGN0_WB			(1u << 10)
#define  CBTCR_SH0_INNER			(3u << 12)
#define  CBTCR_EPD1				(1u << 23)
#define CB_MAIR0				0x38
#define CB_MAIR1				0x3c
#define CB_FSR					0x58
#define CB_FAR					0x60
#define CB_FSYNR0				0x68
#define CB_TLBIALL				0x618
#define CB_TLBSYNC				0x7f0
#define CB_TLBSTATUS			0x7f4

// stage 1 page table entries (AArch64, 4 KiB granule)
#define PTE_VALID				(1ull << 0)
#define PTE_TABLE				(1ull << 1)
#define PTE_PAGE				(1ull << 1)
#define PTE_ATTR_INDEX(i)		((uint64)(i) << 2)
#define PTE_AP_RW_ANY			(1ull << 6)
#define PTE_SH_INNER			(3ull << 8)
#define PTE_AF					(1ull << 10)
#define PTE_ADDRESS_MASK		0x0000fffffffff000ull
// MAIR: 0 = write-back, 1 = non-cacheable, 2 = device
#define MAIR0_VALUE				(0xffu | 0x44u << 8 | 0x04u << 16)
#define ATTR_CACHED				0
#define ATTR_UNCACHED			1

#define GMU_STREAM_ID			0x5
#define GMU_STREAM_MASK			0xc00
// Linux' Adreno SMMU keeps context bank 0 for the GPU (its CP switches
// that bank's page tables), the GMU gets the next one
#define GPU_CONTEXT_BANK		0
#define GMU_CONTEXT_BANK		1
#define GPU_STREAM_MASK			0xc00


static register_window sGcc, sGpucc, sGmu, sRscc, sPdc, sSmmu;


static inline uint32
gmu_read(uint32 reg)
{
	return sGmu.Read(reg * 4 - (GMU_BASE - GPU_BASE));
}


static inline void
gmu_write(uint32 reg, uint32 value)
{
	sGmu.Write(reg * 4 - (GMU_BASE - GPU_BASE), value);
}


static bool
gmu_wait(uint32 reg, uint32 mask, uint32 value, bigtime_t timeout)
{
	return sGmu.Wait(reg * 4 - (GMU_BASE - GPU_BASE), mask, value, timeout);
}


//	#pragma mark - power


static bool
enable_branch(register_window& cc, uint32 offset, bool check, const char* name)
{
	cc.Write(offset, cc.Read(offset) | 1);
	memory_full_barrier();
	if (check && !cc.Wait(offset, 1u << 31, 0, 10000)) {
		ERROR("%s does not start (%#" B_PRIx32 ")\n", name, cc.Read(offset));
		return false;
	}
	return true;
}


static bool
update_rcg(uint32 command, uint32 config)
{
	sGpucc.Write(command + 4, config);
	sGpucc.Write(command, sGpucc.Read(command) | 1);
	return sGpucc.Wait(command, 1, 0, 10000);
}


/*!	What Linux' GMU resume asks of the clock framework and genpd: GCC's GPU
	clocks, the GMU and hub clocks at 200 MHz, the CX power domain.
*/
static status_t
power_up_cx()
{
	// GPLL0 and GPLL0/2 for the GPU clock controller
	sGcc.Write(0x52000, sGcc.Read(0x52000) | 1u << 15 | 1u << 16);
	enable_branch(sGcc, 0x71010, false, "gcc_gpu_memnoc_gfx");
	enable_branch(sGcc, 0x71020, false, "gcc_gpu_snoc_dvm_gfx");
	enable_branch(sGcc, 0x7115c, false, "gcc_ddrss_gpu_axi");

	// CX GDSC on, flip-flops retained; it is votable, its state is in its
	// hardware control register
	sGpucc.Write(0x106c, (sGpucc.Read(0x106c) & ~1u) | 1u << 11);
	if (!sGpucc.Wait(0x1540, 1u << 31, 1u << 31, 100000)) {
		ERROR("CX GDSC does not power on (%#" B_PRIx32 ")\n",
			sGpucc.Read(0x1540));
		return B_TIMED_OUT;
	}

	// GMU: GPLL0/2 / 1.5, hub: GPLL0 / 3
	if (!update_rcg(0x1120, 6u << 8 | 2u) || !update_rcg(0x117c, 5u << 8 | 5u)) {
		ERROR("clock sources do not update\n");
		return B_TIMED_OUT;
	}

	enable_branch(sGpucc, 0x109c, true, "gpu_cc_cxo");
	enable_branch(sGpucc, 0x1170, false, "gpu_cc_cb");
	enable_branch(sGpucc, 0x1078, false, "gpu_cc_ahb");
	if (!enable_branch(sGpucc, 0x1098, true, "gpu_cc_cx_gmu"))
		return B_TIMED_OUT;
	enable_branch(sGpucc, 0x1204, true, "gpu_cc_hub_cx_int");
	enable_branch(sGpucc, 0x1178, true, "gpu_cc_hub_aon");
	enable_branch(sGpucc, 0x108c, false, "gpu_cc_cx_snoc_dvm");
	enable_branch(sGpucc, 0x5000, false, "gpu_cc_hlos1_vote_gpu_smmu");
	spin(100);
	return B_OK;
}


//	#pragma mark - SMMU


/*!	A stage 1 address space of up to 512 GiB (a level 1 table, level 2 and
	3 tables as needed), walked coherently by the SMMU.
*/
class AddressSpace {
public:
	status_t Init()
	{
		fLevel1 = AllocateTable(fLevel1Physical);
		return fLevel1 != NULL ? B_OK : B_NO_MEMORY;
	}

	phys_addr_t Root() const { return fLevel1Physical; }

	/*!	\a privileged pages are for privileged accesses only, which may then
		execute them (a page writable without privileges never is).
	*/
	status_t Map(uint64 address, phys_addr_t physical, uint32 attribute,
		bool privileged)
	{
		uint64* level2 = NextTable(fLevel1, (address >> 30) & 511);
		if (level2 == NULL)
			return B_NO_MEMORY;
		uint64* level3 = NextTable(level2, (address >> 21) & 511);
		if (level3 == NULL)
			return B_NO_MEMORY;
		level3[(address >> 12) & 511] = (physical & PTE_ADDRESS_MASK)
			| PTE_VALID | PTE_PAGE | PTE_AF | PTE_SH_INNER
			| (privileged ? 0 : PTE_AP_RW_ANY) | PTE_ATTR_INDEX(attribute);
		return B_OK;
	}

	/*!	Maps every page of a locked kernel area at \a address. */
	status_t MapArea(void* virtualAddress, size_t size, uint64 address,
		uint32 attribute, bool privileged)
	{
		for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE) {
			physical_entry entry;
			status_t status = get_memory_map((uint8*)virtualAddress + offset,
				B_PAGE_SIZE, &entry, 1);
			if (status != B_OK)
				return status;
			status = Map(address + offset, entry.address, attribute,
				privileged);
			if (status != B_OK)
				return status;
		}
		memory_full_barrier();
		return B_OK;
	}

	void Unmap(uint64 address, size_t size)
	{
		for (uint64 page = address; page < address + size;
				page += B_PAGE_SIZE) {
			uint64* level2 = FindTable(fLevel1, (page >> 30) & 511);
			uint64* level3 = level2 != NULL
				? FindTable(level2, (page >> 21) & 511) : NULL;
			if (level3 != NULL)
				level3[(page >> 12) & 511] = 0;
		}
		memory_full_barrier();
	}

	/*!	The physical address \a address maps to, for the fault dump. */
	bool Translate(uint64 address, phys_addr_t& physical)
	{
		uint64* level2 = FindTable(fLevel1, (address >> 30) & 511);
		uint64* level3 = level2 != NULL
			? FindTable(level2, (address >> 21) & 511) : NULL;
		if (level3 == NULL)
			return false;
		uint64 entry = level3[(address >> 12) & 511];
		if ((entry & PTE_VALID) == 0)
			return false;
		physical = (entry & PTE_ADDRESS_MASK) | (address & (B_PAGE_SIZE - 1));
		return true;
	}

private:
	uint64* FindTable(uint64* table, uint32 index)
	{
		if ((table[index] & PTE_VALID) == 0)
			return NULL;
		phys_addr_t physical = table[index] & PTE_ADDRESS_MASK;
		for (size_t i = 0; i < fTableCount; i++) {
			if (fTablePhysical[i] == physical)
				return fTables[i];
		}
		return NULL;
	}

	uint64* AllocateTable(phys_addr_t& physical)
	{
		void* table;
		area_id area = create_area("adreno page table", &table,
			B_ANY_KERNEL_ADDRESS, B_PAGE_SIZE, B_CONTIGUOUS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
		if (area < 0)
			return NULL;
		memset(table, 0, B_PAGE_SIZE);
		physical_entry entry;
		get_memory_map(table, B_PAGE_SIZE, &entry, 1);
		physical = entry.address;
		return (uint64*)table;
	}

	uint64* NextTable(uint64* table, uint32 index)
	{
		if ((table[index] & PTE_VALID) == 0) {
			if (fTableCount == B_COUNT_OF(fTables))
				return NULL;
			phys_addr_t physical;
			uint64* next = AllocateTable(physical);
			if (next == NULL)
				return NULL;
			memory_full_barrier();
			table[index] = physical | PTE_VALID | PTE_TABLE;
			fTables[fTableCount] = next;
			fTablePhysical[fTableCount++] = physical;
			return next;
		}
		phys_addr_t physical = table[index] & PTE_ADDRESS_MASK;
		for (size_t i = 0; i < fTableCount; i++) {
			if (fTablePhysical[i] == physical)
				return fTables[i];
		}
		return NULL;
	}

	uint64*		fLevel1 = NULL;
	phys_addr_t	fLevel1Physical = 0;
	uint64*		fTables[1024];
	phys_addr_t	fTablePhysical[1024];
	size_t		fTableCount = 0;
};


static AddressSpace sGmuSpace;


static void
smmu_sync_context(uint32 bank)
{
	sSmmu.Write(SMMU_CB(bank) + CB_TLBSYNC, 0);
	if (!sSmmu.Wait(SMMU_CB(bank) + CB_TLBSTATUS, 1, 0, 100000))
		ERROR("SMMU context %" B_PRIu32 " TLB sync timed out\n", bank);
}


/*!	Routes \a stream (with \a mask) to context bank \a bank, translating
	through \a space. Stream match entry \a entry is used.
*/
static void
smmu_attach(uint32 entry, uint32 stream, uint32 mask, uint32 bank,
	const AddressSpace& space, uint32 vaSizeField)
{
	uint32 cb = SMMU_CB(bank);
	sSmmu.Write(cb + CB_SCTLR, 0);
	sSmmu.Write(SMMU_CBA2R(bank), CBA2R_VA64);
	sSmmu.Write(SMMU_CBAR(bank), CBAR_TYPE_S1_S2BYPASS | CBAR_S1_MEMATTR_WB
		| CBAR_S1_BPSHCFG_NSH | (bank + 1));
	sSmmu.Write(cb + CB_TCR2, CBTCR2_SEP_UPSTREAM | CBTCR2_PASIZE_40);
	sSmmu.Write(cb + CB_TCR, vaSizeField | CBTCR_IRGN0_WB | CBTCR_ORGN0_WB
		| CBTCR_SH0_INNER | CBTCR_EPD1);
	uint64 root = space.Root();
	sSmmu.Write(cb + CB_TTBR0, (uint32)root);
	sSmmu.Write(cb + CB_TTBR0 + 4, (uint32)(root >> 32));
	sSmmu.Write(cb + CB_MAIR0, MAIR0_VALUE);
	sSmmu.Write(cb + CB_MAIR1, 0);
	sSmmu.Write(cb + CB_FSR, 0xffffffff);
	sSmmu.Write(cb + CB_TLBIALL, 0);
	smmu_sync_context(bank);
	sSmmu.Write(cb + CB_SCTLR, CBSCTLR_M | CBSCTLR_TRE | CBSCTLR_AFE | CBSCTLR_CFRE
		| CBSCTLR_CFIE);

	sSmmu.Write(SMMU_S2CR(entry), S2CR_TYPE_TRANSLATE | bank);
	sSmmu.Write(SMMU_SMR(entry), SMR_VALID | mask << 16 | stream);
	memory_full_barrier();

	sSmmu.Write(SMMU_TLBIALLNSNH, 0);
	sSmmu.Write(SMMU_sTLBGSYNC, 0);
	if (!sSmmu.Wait(SMMU_sTLBGSTATUS, 1, 0, 100000))
		ERROR("SMMU global TLB sync timed out\n");
}


static void
smmu_report(uint32 bank, const char* when)
{
	uint32 cb = SMMU_CB(bank);
	// bits 10:9 only give the format
	uint32 fsr = sSmmu.Read(cb + CB_FSR);
	if ((fsr & ~(3u << 9)) == 0)
		return;
	ERROR("SMMU context %" B_PRIu32 " fault %s: FSR %#" B_PRIx32 " FAR %#"
		B_PRIx32 "%08" B_PRIx32 " FSYNR0 %#" B_PRIx32 "\n", bank, when, fsr,
		sSmmu.Read(cb + CB_FAR + 4), sSmmu.Read(cb + CB_FAR),
		sSmmu.Read(cb + CB_FSYNR0));
}


//	#pragma mark - GMU memory and firmware


struct gmu_buffer {
	area_id		area = -1;
	uint8*		virt = NULL;
	uint32		iova = 0;
	size_t		size = 0;

	/*!	A buffer the GMU reads at \a address. Small ones are contiguous and
		write-combined for the CPU too; large ones (the icache, written once)
		stay cacheable and are cleaned by Flush().
	*/
	status_t Allocate(const char* name, size_t bytes, uint32 address)
	{
		size = (bytes + B_PAGE_SIZE - 1) & ~(size_t)(B_PAGE_SIZE - 1);
		iova = address;
		bool contiguous = size <= 64 * 1024;
		area = create_area(name, (void**)&virt, B_ANY_KERNEL_ADDRESS, size,
			contiguous ? B_CONTIGUOUS : B_FULL_LOCK,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
		if (area < 0)
			return area;
		memset(virt, 0, size);
		Flush(0, size);
		if (contiguous) {
			physical_entry entry;
			get_memory_map(virt, size, &entry, 1);
			vm_set_area_memory_type(area, entry.address,
				B_WRITE_COMBINING_MEMORY);
		}
		// Like Linux: what lives at a fixed address is the GMU's own,
		// privileged memory (code among it).
		return sGmuSpace.MapArea(virt, size, iova, ATTR_UNCACHED,
			address < GMU_UNCACHED_ADDRESS || address >= GMU_DEBUG_ADDRESS
				|| address == GMU_DUMMY_ADDRESS);
	}

	void Flush(size_t offset, size_t length)
	{
		addr_t start = ((addr_t)virt + offset) & ~(addr_t)63;
		for (addr_t line = start; line < (addr_t)virt + offset + length;
				line += 64)
			asm volatile("dc civac, %0" : : "r" (line) : "memory");
		memory_full_barrier();
	}

	bool Contains(uint32 address, uint32 length) const
		{ return address >= iova && address + length <= iova + size; }
};


static gmu_buffer sIcache, sDummy, sDebug, sLog, sHfi;


static const char* kFirmwareDirectories[] = {
	"/boot/system/non-packaged/data/firmware",
	"/boot/system/data/firmware",
};


static bool
firmware_present(const char* name)
{
	for (size_t i = 0; i < B_COUNT_OF(kFirmwareDirectories); i++) {
		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "%s/%s", kFirmwareDirectories[i], name);
		struct stat st;
		if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
			return true;
	}
	return false;
}


static void*
load_firmware(const char* name, size_t* _size)
{
	const char** kDirectories = kFirmwareDirectories;
	for (size_t i = 0; i < B_COUNT_OF(kFirmwareDirectories); i++) {
		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "%s/%s", kDirectories[i], name);
		int fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		off_t size = lseek(fd, 0, SEEK_END);
		lseek(fd, 0, SEEK_SET);
		void* data = size > 0 ? malloc(size) : NULL;
		if (data != NULL && read(fd, data, size) != size) {
			free(data);
			data = NULL;
		}
		close(fd);
		if (data != NULL) {
			*_size = size;
			INFO("firmware %s, %" B_PRIdOFF " bytes\n", path, size);
			return data;
		}
	}
	ERROR("firmware %s not found\n", name);
	return NULL;
}


struct block_header {
	uint32	addr;
	uint32	size;
	uint32	type;
	uint32	value;
	uint32	data[];
};


/*!	The A650 family's GMU firmware is a list of blocks for its TCMs and for
	memory the host allocates at fixed addresses.
*/
static status_t
gmu_load_firmware(const uint8* image, size_t size)
{
	const uint8* end = image + size;
	const block_header* block = (const block_header*)image;
	while ((const uint8*)block + sizeof(block_header) <= end) {
		if ((const uint8*)block->data + block->size > end)
			return B_BAD_DATA;
		if (block->size != 0) {
			if (block->addr < TCM_SIZE) {
				for (uint32 i = 0; i < block->size / 4; i++)
					gmu_write(GMU_CM3_ITCM_START + block->addr / 4 + i,
						block->data[i]);
			} else if (block->addr >= DTCM_BASE
				&& block->addr < DTCM_BASE + TCM_SIZE) {
				for (uint32 i = 0; i < block->size / 4; i++) {
					gmu_write(GMU_CM3_DTCM_START
						+ (block->addr - DTCM_BASE) / 4 + i, block->data[i]);
				}
			} else {
				gmu_buffer* buffers[] = {&sIcache, &sDummy, &sDebug};
				bool placed = false;
				for (gmu_buffer* buffer : buffers) {
					if (buffer->Contains(block->addr, block->size)) {
						memcpy(buffer->virt + block->addr - buffer->iova,
							block->data, block->size);
						buffer->Flush(block->addr - buffer->iova, block->size);
						placed = true;
						break;
					}
				}
				if (!placed) {
					ERROR("firmware block at %#" B_PRIx32 " (%" B_PRIu32
						" bytes) fits no region\n", block->addr, block->size);
				}
			}
		}
		block = (const block_header*)((const uint8*)block->data + block->size);
	}
	memory_full_barrier();

	uint32 version = gmu_read(GMU_CORE_FW_VERSION);
	INFO("GMU firmware %" B_PRIu32 ".%" B_PRIu32 ".%" B_PRIu32 "\n",
		version >> 28, (version >> 16) & 0xfff, version & 0xffff);
	return B_OK;
}


//	#pragma mark - GMU


static inline void
rscc_write(uint32 reg, uint32 value)
{
	sRscc.Write(reg * 4, value);
}


static inline void
pdc_write(uint32 reg, uint32 value)
{
	sPdc.Write(reg * 4, value);
}


/*!	The GPU RSC's sleep/wake sequencer and its handshake with the PDC, as
	Linux' a6xx_gmu_rpmh_init() sets them for the A650 family (whose PDC
	sequence the AOP has loaded).
*/
static void
gmu_rpmh_init()
{
	rscc_write(RSCC_RSC_STATUS0_DRV0, 1u << 24);
	rscc_write(RSCC_PDC_SLAVE_ID_DRV0, 1);
	rscc_write(RSCC_HIDDEN_TCS_CMD0_DATA, 0);
	rscc_write(RSCC_HIDDEN_TCS_CMD0_ADDR, 0);
	rscc_write(RSCC_HIDDEN_TCS_CMD0_DATA + 2, 0);
	rscc_write(RSCC_HIDDEN_TCS_CMD0_ADDR + 2, 0);
	rscc_write(RSCC_HIDDEN_TCS_CMD0_DATA + 4, 0x80000000);
	rscc_write(RSCC_HIDDEN_TCS_CMD0_ADDR + 4, 0);
	rscc_write(RSCC_OVERRIDE_START_ADDR, 0);
	rscc_write(RSCC_PDC_SEQ_START_ADDR, 0x4520);
	rscc_write(RSCC_PDC_MATCH_VALUE_LO, 0x4510);
	rscc_write(RSCC_PDC_MATCH_VALUE_HI, 0x4514);
	rscc_write(RSCC_SEQ_MEM_0_DRV0, 0xeaaae5a0);
	rscc_write(RSCC_SEQ_MEM_0_DRV0 + 1, 0xe1a1ebab);
	rscc_write(RSCC_SEQ_MEM_0_DRV0 + 2, 0xa2e0a581);
	rscc_write(RSCC_SEQ_MEM_0_DRV0 + 3, 0xecac82e2);
	rscc_write(RSCC_SEQ_MEM_0_DRV0 + 4, 0x0020edad);

	pdc_write(PDC_GPU_SEQ_START_ADDR, 0);
	pdc_write(PDC_GPU_ENABLE_PDC, 0x80000001);
	memory_full_barrier();
}


static status_t __attribute__((unused))
gmu_rpmh_start()
{
	gmu_write(GMU_RSCC_CONTROL_REQ, 1u << 1);
	if (!gmu_wait(GMU_RSCC_CONTROL_ACK, 1u << 1, 1u << 1, 10000)) {
		ERROR("the GPU RSC does not power on\n");
		return B_TIMED_OUT;
	}
	if (!sRscc.Wait(RSCC_SEQ_BUSY_DRV0 * 4, 0xffffffff, 0, 10000)) {
		ERROR("the GPU RSC sequence is stuck\n");
		return B_TIMED_OUT;
	}
	gmu_write(GMU_RSCC_CONTROL_REQ, 0);
	return B_OK;
}


static status_t
gmu_start(const uint8* firmware, size_t firmwareSize)
{
	gmu_write(GMU_AO_HOST_INTERRUPT_CLR, ~0u);
	gmu_write(GMU_AO_HOST_INTERRUPT_MASK, ~0u);

	// vote veto for FAL10, TCM retention
	gmu_write(GPU_GMU_CX_GMU_CX_FALNEXT_INTF, 1);
	gmu_write(GPU_GMU_CX_GMU_CX_FAL_INTF, 1);
	gmu_write(GMU_GENERAL_7, 1);

	// (Linux wakes the RSC with gmu_rpmh_start() only after it put it to
	// sleep: not on the first, cold start.)
	status_t status = gmu_load_firmware(firmware, firmwareSize);
	if (status != B_OK)
		return status;

	gmu_write(GMU_CM3_FW_INIT_RESULT, 0);
	gmu_write(GMU_CM3_BOOT_CONFIG, 0x02);
	gmu_write(GMU_HFI_QTBL_ADDR, sHfi.iova);
	gmu_write(GMU_HFI_QTBL_INFO, 1);
	gmu_write(GMU_AHB_FENCE_RANGE_0, 1u << 31 | 0xau << 18 | 0xa0u);
	gmu_write(GMU_CM3_CFG, 0x4052);
	gmu_write(GMU_HFI_SFR_ADDR, GMU_CHIP_ID);
	gmu_write(GPU_GMU_CX_GMU_PWR_COL_CP_MSG,
		sLog.iova | (sLog.size / B_PAGE_SIZE - 1));

	// power config: no write buffers, no power collapse yet, RPMh client on
	gmu_write(GMU_SYS_BUS_CONFIG, 1);
	gmu_write(GMU_ICACHE_CONFIG, 1);
	gmu_write(GMU_DCACHE_CONFIG, 1);
	gmu_write(GMU_PWR_COL_INTER_FRAME_CTRL, 0x9c40400);
	gmu_write(GMU_RPMH_CTRL, gmu_read(GMU_RPMH_CTRL) | RPMH_INTERFACE_ENABLE
		| LLC_VOTE_ENABLE | DDR_VOTE_ENABLE | MX_VOTE_ENABLE | CX_VOTE_ENABLE
		| GFX_VOTE_ENABLE);

	uint32 mask = 0xffffffff, expected = 0xbabeface;
	if (gmu_read(GMU_CM3_DTCM_START + 0xff8) > 0x20010004) {
		mask = 0x1ff;
		expected = 0x100;
	}
	gmu_write(GMU_CM3_SYSRESET, 1);
	gmu_write(GPU_GMU_CX_GMU_PWR_COL_CP_RESP, 0);
	memory_full_barrier();
	gmu_write(GMU_CM3_SYSRESET, 0);
	if (!gmu_wait(GMU_CM3_FW_INIT_RESULT, mask, expected, 100000)) {
		ERROR("GMU firmware did not start: init result %#" B_PRIx32 "\n",
			gmu_read(GMU_CM3_FW_INIT_RESULT));
		return B_TIMED_OUT;
	}
	INFO("GMU firmware running (init result %#" B_PRIx32 ")\n",
		gmu_read(GMU_CM3_FW_INIT_RESULT));

	gmu_write(GMU_HFI_CTRL_INIT, 1);
	if (!gmu_wait(GMU_HFI_CTRL_STATUS, 1, 1, 100000)) {
		ERROR("HFI queues do not start\n");
		return B_TIMED_OUT;
	}
	INFO("HFI queues started\n");
	return B_OK;
}


//	#pragma mark - cmd-db, performance levels


/*	The AOP's command DB: the RPMh resources and, for the voltage arcs, the
	levels they accept (Linux' drivers/soc/qcom/cmd-db.c). */

struct cmd_db_rsc_header {
	uint16	slave_id;
	uint16	header_offset;
	uint16	data_offset;
	uint16	count;
	uint16	version;
	uint16	reserved[3];
};

struct cmd_db_entry {
	char	id[8];
	uint32	priority[2];
	uint32	address;
	uint16	length;
	uint16	offset;
};

struct cmd_db_header {
	uint32	version;
	uint8	magic[4];
	cmd_db_rsc_header resources[8];
	uint32	checksum;
	uint32	reserved;
	uint8	data[];
};

static register_window sCmdDb;


/*!	Returns the auxiliary data (the level list of an arc) of \a id. */
static const uint16*
cmd_db_levels(const char* id, uint32* _count)
{
	const cmd_db_header* header = (const cmd_db_header*)sCmdDb.base;
	static const uint8 kMagic[] = {0xdb, 0x30, 0x03, 0x0c};
	if (memcmp(header->magic, kMagic, 4) != 0)
		return NULL;

	char query[8] = {};
	memcpy(query, id, strnlen(id, 8));
	for (int i = 0; i < 8 && header->resources[i].slave_id != 0; i++) {
		const cmd_db_rsc_header& rsc = header->resources[i];
		const cmd_db_entry* entry
			= (const cmd_db_entry*)(header->data + rsc.header_offset);
		for (int j = 0; j < rsc.count; j++, entry++) {
			if (memcmp(entry->id, query, 8) == 0) {
				*_count = entry->length / 2;
				return (const uint16*)(header->data + rsc.data_offset
					+ entry->offset);
			}
		}
	}
	return NULL;
}


struct performance_level {
	uint32	frequency;	// Hz, 0 = off
	uint32	level;		// RPMh arc level
};


/*!	The "off" level, then the OPP table of the node compatible with
	\a compatible, as Linux' a6xx_gmu_build_freq_table() lists them.
*/
static uint32
opp_levels(const char* compatible, performance_level* levels, uint32 max)
{
	uint32 count = 0;
	levels[count++] = {0, 0};
	int node = fdt_node_offset_by_compatible(gFDT, -1, compatible);
	int length;
	const fdt32_t* phandle = node >= 0 ? (const fdt32_t*)fdt_getprop(gFDT,
		node, "operating-points-v2", &length) : NULL;
	int table = phandle != NULL
		? fdt_node_offset_by_phandle(gFDT, fdt32_to_cpu(*phandle)) : -1;
	if (table < 0)
		return count;
	int opp;
	fdt_for_each_subnode(opp, gFDT, table) {
		const fdt32_t* hz = (const fdt32_t*)fdt_getprop(gFDT, opp, "opp-hz",
			&length);
		const fdt32_t* level = (const fdt32_t*)fdt_getprop(gFDT, opp,
			"opp-level", NULL);
		if (hz == NULL || length != 8 || level == NULL || count == max)
			continue;
		levels[count++] = {fdt32_to_cpu(hz[1]), fdt32_to_cpu(*level)};
	}
	// ascending, like dev_pm_opp_find_freq_ceil() walks them
	for (uint32 i = 1; i < count; i++) {
		for (uint32 j = i + 1; j < count; j++) {
			if (levels[j].frequency < levels[i].frequency) {
				performance_level swap = levels[i];
				levels[i] = levels[j];
				levels[j] = swap;
			}
		}
	}
	return count;
}


/*!	Linux' a6xx_gmu_rpmh_arc_votes_init(): for each level, the index of the
	first primary arc level that covers it, and a secondary one.
*/
static status_t
arc_votes(const performance_level* levels, uint32 count, const char* primary,
	const char* secondary, uint32* votes)
{
	uint32 primaryCount, secondaryCount;
	const uint16* pri = cmd_db_levels(primary, &primaryCount);
	const uint16* sec = cmd_db_levels(secondary, &secondaryCount);
	if (pri == NULL || sec == NULL || primaryCount == 0
		|| secondaryCount == 0) {
		ERROR("cmd-db has no %s or %s\n", primary, secondary);
		return B_ENTRY_NOT_FOUND;
	}

	for (uint32 i = 0; i < count; i++) {
		uint32 level = levels[i].level;
		uint32 p = 0, s = 0, j;
		for (j = 0; j < primaryCount; j++) {
			if (pri[j] >= level) {
				p = j;
				break;
			}
		}
		if (j == primaryCount) {
			ERROR("level %" B_PRIu32 " not in %s\n", level, primary);
			return B_BAD_VALUE;
		}
		for (j = 0; j < secondaryCount; j++) {
			if (sec[j] >= level) {
				s = j;
				break;
			} else if (sec[j] != 0)
				s = j;
		}
		votes[i] = (uint32)(pri[p] & 0xffff) << 16 | s << 8 | p;
	}
	return B_OK;
}


//	#pragma mark - HFI


struct hfi_queue_table_header {
	uint32	version;
	uint32	size;
	uint32	queue_header_offset;
	uint32	queue_header_size;
	uint32	queue_count;
	uint32	active_queues;
};

struct hfi_queue_header {
	uint32	status;
	uint32	iova;
	uint32	type;
	uint32	size;
	uint32	message_size;
	uint32	dropped;
	uint32	rx_watermark;
	uint32	tx_watermark;
	uint32	rx_request;
	uint32	tx_request;
	uint32	read_index;
	uint32	write_index;
};

#define HFI_COMMAND_QUEUE		0
#define HFI_RESPONSE_QUEUE		1
#define HFI_QUEUE_DWORDS		1024

#define HFI_H2F_MSG_INIT		0
#define HFI_H2F_MSG_FW_VERSION	1
#define HFI_H2F_MSG_BW_TABLE	3
#define HFI_H2F_MSG_PERF_TABLE	4
#define HFI_H2F_MSG_TEST		5
#define HFI_H2F_MSG_START		10
#define HFI_H2F_MSG_CORE_FW_START 14
#define HFI_H2F_MSG_GX_BW_PERF_VOTE 30
#define HFI_F2H_MSG_ERROR		100

static hfi_queue_header* sQueueHeaders;
static uint32* sQueueData[2];
static uint32 sSequence;


/*!	The queue table and the command and response queues, 4 KiB each, in the
	HFI buffer (Linux' a6xx_hfi_init()). Before the GMU starts.
*/
static void
hfi_init()
{
	hfi_queue_table_header* table = (hfi_queue_table_header*)sHfi.virt;
	sQueueHeaders = (hfi_queue_header*)(table + 1);
	table->version = 0;
	table->size = sizeof(*table) + 2 * sizeof(hfi_queue_header);
	table->queue_header_offset = sizeof(*table) / 4;
	table->queue_header_size = sizeof(hfi_queue_header) / 4;
	table->queue_count = 2;
	table->active_queues = 2;
	for (uint32 i = 0; i < 2; i++) {
		hfi_queue_header& header = sQueueHeaders[i];
		uint32 offset = (i + 1) * B_PAGE_SIZE;
		sQueueData[i] = (uint32*)(sHfi.virt + offset);
		header.iova = sHfi.iova + offset;
		header.type = 10 << 8 | i;
		header.status = 1;
		header.size = HFI_QUEUE_DWORDS;
		header.message_size = 0;
		header.dropped = 0;
		header.rx_watermark = 1;
		header.tx_watermark = 1;
		header.rx_request = 1;
		header.tx_request = 0;
		header.read_index = 0;
		header.write_index = 0;
	}
	memory_full_barrier();
}


static status_t
hfi_queue_write(const uint32* data, uint32 dwords)
{
	volatile hfi_queue_header& header = sQueueHeaders[HFI_COMMAND_QUEUE];
	uint32 index = header.write_index;
	uint32 space = (header.read_index - index - 1) & (HFI_QUEUE_DWORDS - 1);
	if (space < dwords + 3)
		return B_BUSY;
	uint32* queue = sQueueData[HFI_COMMAND_QUEUE];
	for (uint32 i = 0; i < dwords; i++) {
		queue[index] = data[i];
		index = (index + 1) % HFI_QUEUE_DWORDS;
	}
	for (; index % 4 != 0; index = (index + 1) % HFI_QUEUE_DWORDS)
		queue[index] = 0xfafafafa;
	memory_full_barrier();
	header.write_index = index;
	memory_full_barrier();
	gmu_write(GMU_HOST2GMU_INTR_SET, 1);
	return B_OK;
}


static uint32
hfi_queue_read(uint32* data, uint32 max)
{
	volatile hfi_queue_header& header = sQueueHeaders[HFI_RESPONSE_QUEUE];
	uint32 index = header.read_index;
	if (index == header.write_index) {
		header.rx_request = 1;
		return 0;
	}
	uint32* queue = sQueueData[HFI_RESPONSE_QUEUE];
	uint32 size = (queue[index] >> 8) & 0xff;
	if (size > max)
		size = max;
	for (uint32 i = 0; i < size; i++) {
		data[i] = queue[index];
		index = (index + 1) % HFI_QUEUE_DWORDS;
	}
	index = ((index + 3) & ~3u) % HFI_QUEUE_DWORDS;
	memory_full_barrier();
	header.read_index = index;
	return size;
}


/*!	Sends a message (whose first word is left for its header) and waits for
	the GMU's acknowledgement.
*/
static status_t
hfi_send(uint32 id, uint32* message, uint32 size, uint32* reply = NULL,
	uint32 replySize = 0)
{
	uint32 sequence = ++sSequence % 0xfff;
	uint32 dwords = size / 4;
	message[0] = sequence << 20 | 0 << 16 | dwords << 8 | id;
	status_t status = hfi_queue_write(message, dwords);
	if (status != B_OK) {
		ERROR("HFI message %" B_PRIu32 " does not fit the queue\n", id);
		return status;
	}

	bigtime_t timeout = system_time() + 1000000;
	while (system_time() < timeout) {
		if ((gmu_read(GMU_GMU2HOST_INTR_INFO) & GMU2HOST_MSGQ) == 0) {
			spin(100);
			continue;
		}
		gmu_write(GMU_GMU2HOST_INTR_CLR, GMU2HOST_MSGQ);

		uint32 response[3 + 16];
		uint32 length;
		while ((length = hfi_queue_read(response, B_COUNT_OF(response))) != 0) {
			if ((response[0] & 0xff) == HFI_F2H_MSG_ERROR) {
				ERROR("GMU firmware error %" B_PRIu32 "\n", response[1]);
				continue;
			}
			if (length < 3 || ((response[1] >> 20) & 0xfff) != sequence)
				continue;
			if (response[2] != 0) {
				ERROR("HFI message %" B_PRIu32 " failed: %" B_PRIu32 "\n", id,
					response[2]);
				return B_ERROR;
			}
			if (reply != NULL)
				memcpy(reply, response + 3, min_c(replySize, (length - 3) * 4));
			return B_OK;
		}
	}
	ERROR("HFI message %" B_PRIu32 " was not answered\n", id);
	return B_TIMED_OUT;
}


/*!	Linux' a6xx_hfi_start() for the A650 family: performance and bandwidth
	tables, the core firmware start, the start.
*/
static status_t
hfi_start()
{
	performance_level gpuLevels[16], gmuLevels[4];
	uint32 gpuCount = opp_levels("qcom,adreno-690.0", gpuLevels, 16);
	uint32 gmuCount = opp_levels("qcom,adreno-gmu-690.0", gmuLevels, 4);

	struct {
		uint32	header;
		uint32	gpu_levels;
		uint32	gmu_levels;
		struct { uint32 vote, acd, frequency; } gx[16];
		struct { uint32 vote, frequency; } cx[4];
	} perf = {};
	uint32 gxVotes[16], cxVotes[4];
	const char* secondary = "mx.lvl";
	uint32 unused;
	if (cmd_db_levels("gmxc.lvl", &unused) != NULL)
		secondary = "gmxc.lvl";
	if (arc_votes(gpuLevels, gpuCount, "gfx.lvl", secondary, gxVotes) != B_OK
		|| arc_votes(gmuLevels, gmuCount, "cx.lvl", "mx.lvl", cxVotes)
			!= B_OK) {
		return B_ERROR;
	}
	perf.gpu_levels = gpuCount;
	perf.gmu_levels = gmuCount;
	for (uint32 i = 0; i < gpuCount; i++) {
		perf.gx[i] = {gxVotes[i], 0xffffffff, gpuLevels[i].frequency / 1000};
		INFO("GPU level %" B_PRIu32 ": %" B_PRIu32 " kHz, arc %" B_PRIu32
			", vote %#" B_PRIx32 "\n", i, gpuLevels[i].frequency / 1000,
			gpuLevels[i].level, gxVotes[i]);
	}
	for (uint32 i = 0; i < gmuCount; i++)
		perf.cx[i] = {cxVotes[i], gmuLevels[i].frequency / 1000};
	status_t status = hfi_send(HFI_H2F_MSG_PERF_TABLE, (uint32*)&perf,
		sizeof(perf));
	if (status != B_OK)
		return status;

	// the A690's fixed table: one "off" level (Linux' a690_build_bw_table())
	struct {
		uint32	header;
		uint32	bw_level_num;
		uint32	cnoc_cmds_num;
		uint32	ddr_cmds_num;
		uint32	cnoc_wait_bitmask;
		uint32	ddr_wait_bitmask;
		uint32	cnoc_cmds_addrs[6];
		uint32	cnoc_cmds_data[2][6];
		uint32	ddr_cmds_addrs[8];
		uint32	ddr_cmds_data[16][8];
	} bw = {};
	bw.bw_level_num = 1;
	bw.ddr_cmds_num = 3;
	bw.ddr_wait_bitmask = 0x01;
	bw.ddr_cmds_addrs[0] = 0x50004;
	bw.ddr_cmds_addrs[1] = 0x50000;
	bw.ddr_cmds_addrs[2] = 0x500ac;
	bw.ddr_cmds_data[0][0] = 0x40000000;
	bw.ddr_cmds_data[0][1] = 0x40000000;
	bw.ddr_cmds_data[0][2] = 0x40000000;
	bw.cnoc_cmds_num = 1;
	bw.cnoc_wait_bitmask = 0x01;
	bw.cnoc_cmds_addrs[0] = 0x5003c;
	bw.cnoc_cmds_data[0][0] = 0x40000000;
	bw.cnoc_cmds_data[1][0] = 0x60000001;
	status = hfi_send(HFI_H2F_MSG_BW_TABLE, (uint32*)&bw, sizeof(bw));
	if (status != B_OK)
		return status;

	uint32 coreStart[2] = {};
	status = hfi_send(HFI_H2F_MSG_CORE_FW_START, coreStart, sizeof(coreStart));
	if (status != B_OK)
		return status;

	uint32 start[1] = {};
	return hfi_send(HFI_H2F_MSG_START, start, sizeof(start));
}


/*!	Asks the GMU to keep the GPU (GX) powered: the "GPU set" out-of-band
	request of the A650 family's firmware.
*/
static status_t
gmu_gpu_set()
{
	gmu_write(GMU_HOST2GMU_INTR_SET, 1u << 30);
	if (!gmu_wait(GMU_GMU2HOST_INTR_INFO, 1u << 31, 1u << 31, 100000)) {
		ERROR("the GMU does not acknowledge the GPU power request (%#" B_PRIx32
			")\n", gmu_read(GMU_GMU2HOST_INTR_INFO));
		return B_TIMED_OUT;
	}
	gmu_write(GMU_GMU2HOST_INTR_CLR, 1u << 31);
	return B_OK;
}


//	#pragma mark - zap shader


/*	The GPU starts in secure mode; the CP only leaves it after the secure
	world has loaded the vendor's signed "zap shader". That goes through
	TrustZone's peripheral authentication service (PAS), as Linux' qcom_scm
	and mdt_loader do it: the ELF header and hash segment as metadata, the
	loadable segments into the reserved region, then authentication. */

#define GPU_PAS_ID				13


static status_t
zap_shader_load()
{
	int node = fdt_node_offset_by_compatible(gFDT, -1, "qcom,adreno-690.0");
	int zap = node >= 0 ? fdt_subnode_offset(gFDT, node, "zap-shader") : -1;
	int length;
	const fdt32_t* region = zap >= 0 ? (const fdt32_t*)fdt_getprop(gFDT, zap,
		"memory-region", &length) : NULL;
	int memory = region != NULL
		? fdt_node_offset_by_phandle(gFDT, fdt32_to_cpu(*region)) : -1;
	const fdt32_t* reg = memory >= 0
		? (const fdt32_t*)fdt_getprop(gFDT, memory, "reg", &length) : NULL;
	if (reg == NULL || length < 16) {
		ERROR("no zap shader memory in the device tree\n");
		return B_ENTRY_NOT_FOUND;
	}
	phys_addr_t regionBase = (phys_addr_t)fdt32_to_cpu(reg[0]) << 32
		| fdt32_to_cpu(reg[1]);
	size_t regionSize = (size_t)fdt32_to_cpu(reg[3]);

	size_t size;
	uint8* image = (uint8*)load_firmware("qcom/sc8280xp/qcdxkmsuc8280.mbn",
		&size);
	if (image == NULL)
		return B_ENTRY_NOT_FOUND;
	status_t status = qcom_pas_load("zap shader", GPU_PAS_ID, image, size,
		regionBase, regionSize);
	free(image);
	if (status == B_OK)
		status = qcom_pas_auth_and_reset("zap shader", GPU_PAS_ID);
	if (status != B_OK)
		return status;
	INFO("zap shader loaded\n");
	return B_OK;
}


//	#pragma mark - GPU


/*	The GPU's own address space (context bank 0, streams 0 and 1), and what
	Linux' a6xx hw_init() and a6xx_cp_init() do with it. */

#define GPU_VA_SQE				0x01000000
#define GPU_VA_RING				0x01100000
#define GPU_VA_MEMPTRS			0x01200000
#define GPU_VA_SCRATCH			0x01300000
#define RING_SIZE				(32 * 1024)

#define CP_TYPE4_PKT			(4u << 28)
#define CP_TYPE7_PKT			(7u << 28)
#define CP_NOP					0x10
#define CP_MEM_WRITE			0x3d
#define CP_ME_INIT				0x48
#define CP_SET_SECURE_MODE		0x66
#define CP_EVENT_WRITE			0x46
#define  EVENT_CACHE_FLUSH_TS	4
#define  EVENT_PC_CCU_INVALIDATE_DEPTH 24
#define  EVENT_PC_CCU_INVALIDATE_COLOR 25
#define  EVENT_IRQ				(1u << 31)
#define MEMPTR_RPTR				0
#define MEMPTR_FENCE			4

#define RBBM_INT_0_MASK			0x00038
#define RBBM_INT_CLEAR_CMD		0x00037
#define  INT_CP_AHB_ERROR		(1u << 1)
#define  INT_CP_HW_ERROR		(1u << 9)
#define  INT_CP_CACHE_FLUSH_TS	(1u << 20)
#define  INT_RBBM_HANG_DETECT	(1u << 23)
#define  INT_UCHE_OOB_ACCESS	(1u << 24)
#define GPU_INT_MASK			(INT_CP_AHB_ERROR | INT_CP_HW_ERROR \
	| INT_CP_CACHE_FLUSH_TS | INT_RBBM_HANG_DETECT | INT_UCHE_OOB_ACCESS)
// what a CP_HW_ERROR was (a6xx.xml: A6XX_CP_INT)
#define CP_HW_FAULT				0x00821
#define CP_INTERRUPT_STATUS		0x00823
#define CP_PROTECT_STATUS		0x00824
#define CP_RB_RPTR				0x00806
#define CP_RB_WPTR				0x00807
#define CP_SQE_STAT_ADDR		0x00908
#define CP_SQE_STAT_DATA		0x00909
#define CP_IB1_BASE				0x00928
#define CP_IB1_REM_SIZE			0x0092a
#define CP_IB2_BASE				0x0092b
#define CP_IB2_REM_SIZE			0x0092d
#define  CP_INT_OPCODE_ERROR	(1u << 0)
#define  CP_INT_UCODE_ERROR		(1u << 1)
#define  CP_INT_HW_FAULT		(1u << 2)
#define  CP_INT_PROTECTION		(1u << 4)
#define  CP_INT_ILLEGAL_INSTR	(1u << 7)
#define GMU_ALWAYS_ON_COUNTER_L	0x1f888
#define GMU_ALWAYS_ON_COUNTER_H	0x1f889

static AddressSpace sGpuSpace;
static register_window sGpu;
static gmu_buffer sSqe, sRing, sMemptrs, sScratch;
static uint32 sRingNext;


static inline uint32
gpu_read(uint32 reg)
{
	return sGpu.Read(reg * 4);
}


static inline void
gpu_write(uint32 reg, uint32 value)
{
	sGpu.Write(reg * 4, value);
}


static inline void
gpu_write64(uint32 reg, uint64 value)
{
	gpu_write(reg, (uint32)value);
	gpu_write(reg + 1, (uint32)(value >> 32));
}


static inline uint32
pm4_parity(uint32 value)
{
	return (0x9669 >> (0xf & (value ^ (value >> 4) ^ (value >> 8)
		^ (value >> 12) ^ (value >> 16) ^ (value >> 20) ^ (value >> 24)
		^ (value >> 28)))) & 1;
}


static inline uint32
pkt7(uint32 opcode, uint32 count)
{
	return CP_TYPE7_PKT | count | pm4_parity(count) << 15
		| (opcode & 0x7f) << 16 | pm4_parity(opcode) << 23;
}


static void
ring_emit(uint32 value)
{
	((uint32*)sRing.virt)[sRingNext] = value;
	sRingNext = (sRingNext + 1) % (RING_SIZE / 4);
}


static void
ring_flush()
{
	memory_full_barrier();
	gpu_write(REG_A6XX_CP_RB_WPTR, sRingNext);
}


static uint32
ring_rptr()
{
	return ((volatile uint32*)sMemptrs.virt)[MEMPTR_RPTR / 4];
}


static bool
gpu_idle(const char* what)
{
	bigtime_t timeout = system_time() + 1000000;
	while (ring_rptr() != sRingNext || gpu_read(REG_A6XX_CP_RB_RPTR)
			!= sRingNext) {
		if (system_time() > timeout) {
			ERROR("%s: the CP does not drain its ring: rptr %" B_PRIu32
				" (register %" B_PRIu32 ") wptr %" B_PRIu32 ", status %#"
				B_PRIx32 ", interrupts %#" B_PRIx32 "\n", what, ring_rptr(),
				gpu_read(REG_A6XX_CP_RB_RPTR), sRingNext,
				gpu_read(REG_A6XX_RBBM_STATUS),
				gpu_read(REG_A6XX_RBBM_INT_0_STATUS));
			return false;
		}
		spin(10);
	}
	// no block busy but the hang detector (bit 0 is "GPU busy ignoring AHB")
	timeout = system_time() + 1000000;
	while ((gpu_read(REG_A6XX_RBBM_STATUS) & ~1u) != 0) {
		if (system_time() > timeout) {
			ERROR("%s: the GPU stays busy: status %#" B_PRIx32 "\n", what,
				gpu_read(REG_A6XX_RBBM_STATUS));
			return false;
		}
		spin(10);
	}
	return true;
}


static status_t
gpu_buffer(gmu_buffer& buffer, const char* name, size_t size, uint32 address)
{
	buffer.size = (size + B_PAGE_SIZE - 1) & ~(size_t)(B_PAGE_SIZE - 1);
	buffer.iova = address;
	buffer.area = create_area(name, (void**)&buffer.virt, B_ANY_KERNEL_ADDRESS,
		buffer.size, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (buffer.area < 0)
		return buffer.area;
	memset(buffer.virt, 0, buffer.size);
	physical_entry entry;
	get_memory_map(buffer.virt, buffer.size, &entry, 1);
	// the GPU's accesses are I/O coherent through the SMMU's cached attribute
	return sGpuSpace.MapArea(buffer.virt, buffer.size, address, ATTR_CACHED,
		false);
}


static status_t
gpu_hw_init()
{
	if (sGpu.Map("adreno gpu", GPU_BASE, GPU_SIZE) != B_OK)
		return B_NO_MEMORY;

	status_t status = sGpuSpace.Init();
	if (status != B_OK)
		return status;
	size_t sqeSize;
	uint8* sqe = (uint8*)load_firmware("qcom/a660_sqe.fw", &sqeSize);
	if (sqe == NULL)
		return B_ENTRY_NOT_FOUND;
	if (gpu_buffer(sSqe, "adreno sqe", sqeSize, GPU_VA_SQE) != B_OK
		|| gpu_buffer(sRing, "adreno ring", RING_SIZE, GPU_VA_RING) != B_OK
		|| gpu_buffer(sMemptrs, "adreno memptrs", B_PAGE_SIZE, GPU_VA_MEMPTRS)
			!= B_OK
		|| gpu_buffer(sScratch, "adreno scratch", B_PAGE_SIZE, GPU_VA_SCRATCH)
			!= B_OK) {
		free(sqe);
		return B_NO_MEMORY;
	}
	// the firmware's first word is not loaded (adreno_fw_create_bo())
	memcpy(sSqe.virt, sqe + 4, sqeSize - 4);
	INFO("SQE firmware version %#" B_PRIx32 "\n", ((uint32*)sqe)[1]);
	free(sqe);
	memory_full_barrier();

	smmu_attach(0, 0x0, GPU_STREAM_MASK, GPU_CONTEXT_BANK, sGpuSpace,
		CBTCR_T0SZ_39);
	smmu_attach(1, 0x1, GPU_STREAM_MASK, GPU_CONTEXT_BANK, sGpuSpace,
		CBTCR_T0SZ_39);

	// GBIF out of halt, no trusted memory range, 64 bit addressing
	gpu_write(REG_A6XX_GBIF_HALT, 0);
	gpu_read(REG_A6XX_GBIF_HALT);
	gpu_write(REG_A6XX_RBBM_GBIF_HALT, 0);
	gpu_read(REG_A6XX_RBBM_GBIF_HALT);
	gpu_write(REG_A6XX_RBBM_SECVID_TSB_CNTL, 0);
	gpu_write64(REG_A6XX_RBBM_SECVID_TSB_TRUSTED_BASE, 0);
	gpu_write(REG_A6XX_RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
	static const uint32 kAddressModes[] = {
		REG_A6XX_CP_ADDR_MODE_CNTL, REG_A6XX_VSC_ADDR_MODE_CNTL,
		REG_A6XX_GRAS_ADDR_MODE_CNTL, REG_A6XX_RB_ADDR_MODE_CNTL,
		REG_A6XX_PC_ADDR_MODE_CNTL, REG_A6XX_HLSQ_ADDR_MODE_CNTL,
		REG_A6XX_VFD_ADDR_MODE_CNTL, REG_A6XX_VPC_ADDR_MODE_CNTL,
		REG_A6XX_UCHE_ADDR_MODE_CNTL, REG_A6XX_SP_ADDR_MODE_CNTL,
		REG_A6XX_TPL1_ADDR_MODE_CNTL,
		REG_A6XX_RBBM_SECVID_TSB_ADDR_MODE_CNTL,
	};
	for (uint32 reg : kAddressModes)
		gpu_write(reg, 1);

	// hardware clock gating (a6xx_set_hwcg(), A690 values)
	gmu_write(GPU_GMU_AO_GMU_CGC_MODE_CNTL, 0x00020200);
	gmu_write(GPU_GMU_AO_GMU_CGC_DELAY_CNTL, 0x10111);
	gmu_write(GPU_GMU_AO_GMU_CGC_HYST_CNTL, 0x5555);
	gmu_write(GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL,
		gmu_read(GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL) & ~1u);
	for (size_t i = 0; i < B_COUNT_OF(kA690ClockGating); i++)
		gpu_write(kA690ClockGating[i].offset, kA690ClockGating[i].value);
	gmu_write(GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL,
		gmu_read(GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL) | 1u);
	gpu_write(REG_A6XX_RBBM_CLOCK_CNTL, 0x8aa8aa82);

	// GBIF
	gpu_write(REG_A6XX_GBIF_QSB_SIDE0, 0x00071620);
	gpu_write(REG_A6XX_GBIF_QSB_SIDE1, 0x00071620);
	gpu_write(REG_A6XX_GBIF_QSB_SIDE2, 0x00071620);
	gpu_write(REG_A6XX_GBIF_QSB_SIDE3, 0x00071620);
	gpu_write(REG_A6XX_RBBM_GBIF_CLIENT_QOS_CNTL, 0x3);
	gpu_write(REG_A6XX_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);

	// UCHE: the trap range at the top of the address space
	const uint64 trapBase = 0x1fffffffff000ull;
	gpu_write64(REG_A6XX_UCHE_WRITE_RANGE_MAX, trapBase + 0xfc0);
	gpu_write64(REG_A6XX_UCHE_TRAP_BASE, trapBase);
	gpu_write64(REG_A6XX_UCHE_WRITE_THRU_BASE, trapBase);
	gpu_write(REG_A6XX_UCHE_FILTER_CNTL, 0x804);
	gpu_write(REG_A6XX_UCHE_CACHE_WAYS, 0x4);
	gpu_write(REG_A6XX_CP_ROQ_THRESHOLDS_2, 0x02000140);
	gpu_write(REG_A6XX_CP_ROQ_THRESHOLDS_1, 0x8040362c);
	gpu_write(REG_A6XX_CP_LPAC_PROG_FIFO_SIZE, 0x00000020);
	gpu_write(REG_A6XX_CP_MEM_POOL_SIZE, 128);
	gpu_write(REG_A6XX_PC_DBG_ECO_CNTL, 0x00800200);
	gpu_write(REG_A6XX_CP_AHB_CNTL, 0x1);
	gpu_write(REG_A6XX_RBBM_PERFCTR_CNTL, 0x1);
	gpu_write(REG_A6XX_CP_PERFCTR_CP_SEL(0), 0);

	// UBWC 4.0, highest bank bit 16, level 2 swizzling on, macrotiles
	const uint32 hbb = 16 - 13, hbbHi = hbb >> 2, hbbLo = hbb & 3;
	gpu_write(REG_A6XX_RB_NC_MODE_CNTL, 1u << 11 | hbbHi << 10 | 1u << 4
		| hbbLo << 1);
	gpu_write(REG_A6XX_TPL1_NC_MODE_CNTL, hbbHi << 4 | hbbLo << 1);
	gpu_write(REG_A6XX_SP_NC_MODE_CNTL, hbbHi << 10 | 2u << 4 | hbbLo << 1);
	gpu_write(REG_A6XX_UCHE_MODE_CNTL, hbbLo << 21);
	gpu_write(REG_A6XX_RBBM_NC_MODE_CNTL, 1);

	gpu_write(REG_A6XX_RBBM_INTERFACE_HANG_INT_CNTL, 1u << 30 | 0x4fffff);
	gpu_write(REG_A6XX_UCHE_CLIENT_PF, 1u << 7 | 0x1);
	gpu_write(REG_A6XX_TPL1_BICUBIC_WEIGHTS_TABLE(0), 0);
	gpu_write(REG_A6XX_TPL1_BICUBIC_WEIGHTS_TABLE(1), 0x3fe05ff4);
	gpu_write(REG_A6XX_TPL1_BICUBIC_WEIGHTS_TABLE(2), 0x3fa0ebee);
	gpu_write(REG_A6XX_TPL1_BICUBIC_WEIGHTS_TABLE(3), 0x3f5193ed);
	gpu_write(REG_A6XX_TPL1_BICUBIC_WEIGHTS_TABLE(4), 0x3f0243f0);

	// CP register protection
	gpu_write(REG_A6XX_CP_PROTECT_CNTL, 1u << 0 | 1u << 1 | 1u << 3);
	for (size_t i = 0; i < B_COUNT_OF(kA690Protect) - 1; i++) {
		if (kA690Protect[i] != 0)
			gpu_write(REG_A6XX_CP_PROTECT(i), kA690Protect[i]);
	}
	gpu_write(REG_A6XX_CP_PROTECT(A690_PROTECT_COUNT - 1),
		kA690Protect[B_COUNT_OF(kA690Protect) - 1]);

	gpu_write(REG_A6XX_CP_CHICKEN_DBG, 0x00028801);
	gpu_write(REG_A6XX_RBBM_GBIF_CLIENT_QOS_CNTL, 0x0);
	gpu_write(REG_A6XX_UCHE_CMDQ_CONFIG, 0x90);
	gpu_write(REG_A6XX_CP_APRIV_CNTL, 1u << 6 | 1u << 5 | 1u << 3 | 1u << 2
		| 1u << 1);
	gpu_write(REG_A6XX_RBBM_INT_0_MASK, GPU_INT_MASK);

	// the ring: 32 KiB, 32 byte blocks, the hardware's rptr shadow
	gpu_write64(REG_A6XX_CP_SQE_INSTR_BASE, sSqe.iova);
	gpu_write64(REG_A6XX_CP_RB_BASE, sRing.iova);
	gpu_write(REG_A6XX_CP_RB_CNTL, 12u | 2u << 8);
	gpu_write64(REG_A6XX_CP_RB_RPTR_ADDR, sMemptrs.iova);
	sRingNext = 0;

	gpu_write(REG_A6XX_CP_SQE_CNTL, 1);

	ring_emit(pkt7(CP_ME_INIT, 8));
	ring_emit(0x0000002f);
	ring_emit(0x00000003);
	ring_emit(0x20000000);
	for (int i = 0; i < 5; i++)
		ring_emit(0);
	ring_flush();
	if (!gpu_idle("CP_ME_INIT"))
		return B_TIMED_OUT;
	INFO("the CP runs its ring (rptr %" B_PRIu32 ")\n", ring_rptr());

	if (zap_shader_load() == B_OK) {
		ring_emit(pkt7(CP_SET_SECURE_MODE, 1));
		ring_emit(0);
		ring_flush();
		if (!gpu_idle("CP_SET_SECURE_MODE"))
			return B_TIMED_OUT;
		INFO("the GPU left secure mode\n");
	}

	// a test: the CP writes a word to memory
	ring_emit(pkt7(CP_MEM_WRITE, 3));
	ring_emit((uint32)sScratch.iova);
	ring_emit(0);
	ring_emit(0xcafe0690);
	ring_flush();
	bool idle = gpu_idle("CP_MEM_WRITE");
	memory_full_barrier();
	INFO("CP_MEM_WRITE: %s, memory reads %#" B_PRIx32 "\n",
		idle ? "done" : "timed out", ((volatile uint32*)sScratch.virt)[0]);
	return idle && ((volatile uint32*)sScratch.virt)[0] == 0xcafe0690
		? B_OK : B_ERROR;
}


//	#pragma mark - bring-up


static mutex sStartLock = MUTEX_INITIALIZER("adreno start");
static status_t sStartStatus = B_NO_INIT;
static mutex sRingLock = MUTEX_INITIALIZER("adreno ring");
static uint32 sNextFence = 1;
static ConditionVariable sFenceCondition;
static uint32 sInterrupt;
static bool sInterruptInstalled;
static int32 sFaults;

// the first fault's state, dumped by a thread (the interrupt handler cannot
// map memory)
static sem_id sFaultSemaphore = -1;
static uint64 sFaultIb1;
static uint32 sFaultIb1Left;
static uint32 sFaultRptr;


static void
dump_gpu_words(const char* what, uint64 iova, int32 count)
{
	for (int32 i = 0; i < count; i += 4) {
		uint32 words[4] = {};
		for (int32 j = 0; j < 4; j++) {
			phys_addr_t physical;
			if (!sGpuSpace.Translate(iova + (i + j) * 4, physical)
				|| vm_memcpy_from_physical(&words[j], physical, 4, false)
					!= B_OK) {
				words[j] = 0xdeaddead;
			}
		}
		dprintf("adreno: %s %#" B_PRIx64 ": %08" B_PRIx32 " %08" B_PRIx32
			" %08" B_PRIx32 " %08" B_PRIx32 "\n", what, iova + i * 4, words[0],
			words[1], words[2], words[3]);
	}
}


/*!	Where the CP was at the first fault: the ring's last indirect buffers
	and the words around the fetch point of the one it ran.
*/
static status_t
fault_dumper(void*)
{
	if (acquire_sem(sFaultSemaphore) != B_OK)
		return B_OK;
	uint32 size = RING_SIZE / 4;
	uint32* ring = (uint32*)sRing.virt;
	uint32 ibSize = 0;
	dprintf("adreno: fault: IB1 %#" B_PRIx64 ", %#" B_PRIx32 " dwords left, "
		"ring read pointer %#" B_PRIx32 "\n", sFaultIb1, sFaultIb1Left,
		sFaultRptr);
	// the CP_INDIRECT_BUFFER packets before the read pointer
	for (uint32 back = 1; back < 256; back++) {
		uint32 index = (sFaultRptr - back) & (size - 1);
		if ((ring[index] >> 28) != 7 || ((ring[index] >> 16) & 0x7f) != 0x3f)
			continue;
		uint64 base = ring[(index + 1) & (size - 1)]
			| (uint64)ring[(index + 2) & (size - 1)] << 32;
		uint32 dwords = ring[(index + 3) & (size - 1)] & 0xfffff;
		dprintf("adreno: fault: ring %#" B_PRIx32 ": IB %#" B_PRIx64 ", %#"
			B_PRIx32 " dwords\n", index, base, dwords);
		if (base == sFaultIb1 && ibSize == 0)
			ibSize = dwords;
	}
	uint64 fetch = sFaultIb1;
	if (ibSize != 0 && sFaultIb1Left <= ibSize)
		fetch += (uint64)(ibSize - sFaultIb1Left) * 4;
	dump_gpu_words("IB", fetch, 16);
	phys_addr_t physical = 0;
	sGpuSpace.Translate(fetch, physical);
	dprintf("adreno: fault: the GPU's page is at %#" B_PRIxPHYSADDR "\n",
		physical);
	adreno_dump_buffer(fetch, 16);
	return B_OK;
}


static int32
gpu_interrupt(void*)
{
	uint32 status = gpu_read(REG_A6XX_RBBM_INT_0_STATUS);
	if (status == 0)
		return B_UNHANDLED_INTERRUPT;
	gpu_write(RBBM_INT_CLEAR_CMD, status);

	if ((status & ~INT_CP_CACHE_FLUSH_TS) != 0) {
		int32 faults = atomic_add(&sFaults, 1);
		// the first few in detail, as a6xx_cp_hw_err_irq() reports them
		if (faults < 4) {
			dprintf("adreno: GPU interrupt %#" B_PRIx32 ", RBBM status %#"
				B_PRIx32 ", ring %#" B_PRIx32 "/%#" B_PRIx32 ", IB1 %#" B_PRIx32
				" (%#" B_PRIx32 " left), IB2 %#" B_PRIx32 " (%#" B_PRIx32
				" left)\n", status, gpu_read(REG_A6XX_RBBM_STATUS),
				gpu_read(CP_RB_RPTR), gpu_read(CP_RB_WPTR),
				gpu_read(CP_IB1_BASE), gpu_read(CP_IB1_REM_SIZE),
				gpu_read(CP_IB2_BASE), gpu_read(CP_IB2_REM_SIZE));
		}
		if (faults == 0 && sFaultSemaphore >= 0) {
			sFaultIb1 = gpu_read(CP_IB1_BASE)
				| (uint64)gpu_read(CP_IB1_BASE + 1) << 32;
			sFaultIb1Left = gpu_read(CP_IB1_REM_SIZE);
			sFaultRptr = gpu_read(CP_RB_RPTR);
			release_sem_etc(sFaultSemaphore, 1, B_DO_NOT_RESCHEDULE);
		}
		if (faults < 4 && (status & INT_CP_HW_ERROR) != 0) {
			uint32 cp = gpu_read(CP_INTERRUPT_STATUS);
			if ((cp & CP_INT_OPCODE_ERROR) != 0) {
				gpu_write(CP_SQE_STAT_ADDR, 1);
				dprintf("adreno: CP opcode error, opcode %#" B_PRIx32 "\n",
					gpu_read(CP_SQE_STAT_DATA));
			}
			if ((cp & CP_INT_UCODE_ERROR) != 0)
				dprintf("adreno: CP microcode error\n");
			if ((cp & CP_INT_HW_FAULT) != 0) {
				dprintf("adreno: CP hardware fault %#" B_PRIx32 "\n",
					gpu_read(CP_HW_FAULT));
			}
			if ((cp & CP_INT_PROTECTION) != 0) {
				uint32 protect = gpu_read(CP_PROTECT_STATUS);
				dprintf("adreno: CP protected register %s at %#" B_PRIx32
					" (status %#" B_PRIx32 ")\n",
					(protect & (1u << 20)) != 0 ? "read" : "write",
					protect & 0x3ffff, protect);
			}
			if ((cp & CP_INT_ILLEGAL_INSTR) != 0)
				dprintf("adreno: CP illegal instruction\n");
			if (cp == 0)
				dprintf("adreno: CP interrupt status 0\n");
		} else if (faults == 4)
			dprintf("adreno: more GPU faults, not logged\n");
	}
	if ((status & INT_CP_CACHE_FLUSH_TS) != 0)
		sFenceCondition.NotifyAll();
	return B_INVOKE_SCHEDULER;
}


static status_t
bring_up()
{
	if (gFDT == NULL
		|| fdt_node_offset_by_compatible(gFDT, -1, "qcom,adreno-690.0") < 0) {
		return B_DEVICE_NOT_FOUND;
	}

	if (sGcc.Map("adreno gcc", GCC_BASE, GCC_SIZE) != B_OK
		|| sGpucc.Map("adreno gpucc", GPUCC_BASE, GPUCC_SIZE) != B_OK)
		return B_NO_MEMORY;
	if ((sGcc.Read(0x71004) & (1u << 31)) != 0) {
		ERROR("the GPU's configuration bus clock is off\n");
		return B_NOT_SUPPORTED;
	}

	status_t status = power_up_cx();
	if (status != B_OK)
		return status;

	if (sGmu.Map("adreno gmu", GMU_BASE, GMU_SIZE) != B_OK
		|| sRscc.Map("adreno rscc", RSCC_BASE, RSCC_SIZE) != B_OK
		|| sPdc.Map("adreno gmu pdc", GMU_PDC_BASE, GMU_PDC_SIZE) != B_OK
		|| sSmmu.Map("adreno smmu", SMMU_BASE, SMMU_SIZE) != B_OK)
		return B_NO_MEMORY;

	if (gmu_read(GMU_CM3_SYSRESET) == 0) {
		ERROR("the GMU runs already (a second driver?)\n");
		return B_BUSY;
	}

	INFO("SMMU sCR0 %#" B_PRIx32 ", %" B_PRIu32 " stream groups, %" B_PRIu32
		" context banks\n", sSmmu.Read(SMMU_sCR0),
		sSmmu.Read(SMMU_IDR0) & 0xff, sSmmu.Read(SMMU_IDR1) & 0xff);

	status = sGmuSpace.Init();
	if (status != B_OK)
		return status;
	if (sIcache.Allocate("adreno gmu icache", GMU_ICACHE_SIZE,
			GMU_ICACHE_ADDRESS) != B_OK
		|| sDummy.Allocate("adreno gmu dummy", GMU_DUMMY_SIZE,
			GMU_DUMMY_ADDRESS) != B_OK
		|| sDebug.Allocate("adreno gmu debug", GMU_DEBUG_SIZE,
			GMU_DEBUG_ADDRESS) != B_OK
		|| sLog.Allocate("adreno gmu log", GMU_LOG_SIZE,
			GMU_UNCACHED_ADDRESS) != B_OK
		|| sHfi.Allocate("adreno gmu hfi", GMU_HFI_SIZE,
			GMU_UNCACHED_ADDRESS + GMU_LOG_SIZE) != B_OK) {
		ERROR("GMU memory\n");
		return B_NO_MEMORY;
	}
	smmu_attach(2, GMU_STREAM_ID, GMU_STREAM_MASK, GMU_CONTEXT_BANK,
		sGmuSpace, CBTCR_T0SZ_32);

	size_t firmwareSize;
	uint8* firmware = (uint8*)load_firmware("qcom/a660_gmu.bin",
		&firmwareSize);
	if (firmware == NULL)
		return B_ENTRY_NOT_FOUND;

	if (sCmdDb.Map("adreno cmd-db", CMD_DB_BASE, CMD_DB_SIZE) != B_OK)
		return B_NO_MEMORY;

	hfi_init();
	gmu_rpmh_init();
	status = gmu_start(firmware, firmwareSize);
	free(firmware);
	smmu_report(GMU_CONTEXT_BANK, "after the GMU start");
	if (status != B_OK)
		return status;

	status = hfi_start();
	smmu_report(GMU_CONTEXT_BANK, "after the HFI start");
	if (status != B_OK)
		return status;

	// the lowest GPU level to start with (Linux' a6xx_gmu_set_initial_freq())
	uint32 vote[4] = {0, 1, 1, 0};
	status = hfi_send(HFI_H2F_MSG_GX_BW_PERF_VOTE, vote, sizeof(vote));
	if (status != B_OK)
		return status;

	status = gmu_gpu_set();
	if (status != B_OK)
		return status;

	sFenceCondition.Init(&sFenceCondition, "adreno fence");

	// the GPU's interrupt, an SPI
	int node = fdt_node_offset_by_compatible(gFDT, -1, "qcom,adreno-690.0");
	int length;
	const fdt32_t* interrupts = (const fdt32_t*)fdt_getprop(gFDT, node,
		"interrupts", &length);
	if (interrupts == NULL || length < 12 || fdt32_to_cpu(interrupts[0]) != 0)
		return B_BAD_DATA;
	sInterrupt = fdt32_to_cpu(interrupts[1]) + 32;

	status = gpu_hw_init();
	smmu_report(GPU_CONTEXT_BANK, "after the GPU start");
	if (status != B_OK)
		return status;

	gpu_write(RBBM_INT_CLEAR_CMD, 0xffffffff);
	sFaultSemaphore = create_sem(0, "adreno fault");
	if (sFaultSemaphore >= 0) {
		thread_id dumper = spawn_kernel_thread(fault_dumper, "adreno fault",
			B_NORMAL_PRIORITY, NULL);
		if (dumper >= 0)
			resume_thread(dumper);
	}
	status = install_io_interrupt_handler(sInterrupt, gpu_interrupt, NULL, 0);
	if (status != B_OK)
		return status;
	sInterruptInstalled = true;
	INFO("GPU running, interrupt %" B_PRIu32 "\n", sInterrupt);
	return B_OK;
}


//	#pragma mark - interface


status_t
adreno_gpu_start()
{
	MutexLocker locker(sStartLock);
	if (sStartStatus == B_NO_INIT) {
		// without its firmware nothing is touched, and a later request
		// tries again (the boot may not have put it in place yet)
		static const char* kFirmware[] = {"qcom/a660_gmu.bin",
			"qcom/a660_sqe.fw", "qcom/sc8280xp/qcdxkmsuc8280.mbn"};
		for (size_t i = 0; i < B_COUNT_OF(kFirmware); i++) {
			if (!firmware_present(kFirmware[i]))
				return B_ENTRY_NOT_FOUND;
		}
		sStartStatus = bring_up();
		if (sStartStatus != B_OK)
			ERROR("bring-up failed: %s\n", strerror(sStartStatus));
		else
			sStartStatus = B_OK;
	}
	return sStartStatus;
}


bool
adreno_gpu_running()
{
	return sStartStatus == B_OK;
}


status_t
adreno_gpu_map(uint64 iova, void* address, size_t size, bool cached)
{
	return sGpuSpace.MapArea(address, size, iova,
		cached ? ATTR_CACHED : ATTR_UNCACHED, false);
}


void
adreno_gpu_unmap(uint64 iova, size_t size)
{
	sGpuSpace.Unmap(iova, size);
	uint32 cb = SMMU_CB(GPU_CONTEXT_BANK);
	sSmmu.Write(cb + CB_TLBIALL, 0);
	smmu_sync_context(GPU_CONTEXT_BANK);
}


void
adreno_ring_lock()
{
	mutex_lock(&sRingLock);
}


void
adreno_ring_unlock()
{
	mutex_unlock(&sRingLock);
}


status_t
adreno_ring_reserve(uint32 dwords)
{
	const uint32 size = RING_SIZE / 4;
	if (dwords + 16 >= size)
		return B_BAD_VALUE;
	bigtime_t timeout = system_time() + 5000000;
	while (((ring_rptr() - sRingNext - 1) & (size - 1)) < dwords + 16) {
		if (system_time() > timeout) {
			ERROR("the ring stays full (rptr %" B_PRIu32 ", wptr %" B_PRIu32
				")\n", ring_rptr(), sRingNext);
			return B_TIMED_OUT;
		}
		snooze(100);
	}
	return B_OK;
}


void
adreno_ring_emit(uint32 value)
{
	ring_emit(value);
}


uint32
adreno_ring_flush_fence()
{
	uint32 fence = sNextFence++;
	ring_emit(pkt7(CP_EVENT_WRITE, 4));
	ring_emit(EVENT_CACHE_FLUSH_TS | EVENT_IRQ);
	uint64 fenceAddress = (uint64)sMemptrs.iova + MEMPTR_FENCE;
	ring_emit((uint32)fenceAddress);
	ring_emit((uint32)(fenceAddress >> 32));
	ring_emit(fence);
	ring_flush();
	return fence;
}


uint32
adreno_completed_fence()
{
	return ((volatile uint32*)sMemptrs.virt)[MEMPTR_FENCE / 4];
}


status_t
adreno_wait_fence(uint32 fence, bigtime_t deadline)
{
	while ((int32)(adreno_completed_fence() - fence) < 0) {
		ConditionVariableEntry entry;
		sFenceCondition.Add(&entry);
		if ((int32)(adreno_completed_fence() - fence) >= 0)
			break;
		status_t status = entry.Wait(B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT,
			deadline == B_INFINITE_TIMEOUT ? B_INFINITE_TIMEOUT : deadline);
		if (status == B_TIMED_OUT || status == B_INTERRUPTED)
			return (int32)(adreno_completed_fence() - fence) >= 0
				? B_OK : status;
	}
	return B_OK;
}


uint64
adreno_timestamp()
{
	uint32 high, low;
	do {
		high = gmu_read(GMU_ALWAYS_ON_COUNTER_H);
		low = gmu_read(GMU_ALWAYS_ON_COUNTER_L);
	} while (gmu_read(GMU_ALWAYS_ON_COUNTER_H) != high);
	return (uint64)high << 32 | low;
}


uint32
adreno_faults()
{
	return (uint32)sFaults;
}


uint32
adreno_pkt7(uint32 opcode, uint32 count)
{
	return pkt7(opcode, count);
}
