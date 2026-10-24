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

	Nothing is published yet: the bring-up runs when devfs loads the driver
	and logs what it gets. */


#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Drivers.h>
#include <KernelExport.h>
#include <arch/atomic.h>
#include <vm/vm.h>

#include <libfdt.h>


extern void* gFDT;

int32 api_version = B_CUR_DRIVER_API_VERSION;


#define INFO(x...)	dprintf("adreno: " x)
#define ERROR(x...)	dprintf("adreno: " x)


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
#define GMU_GENERAL_7			0x1f9cc
#define GMU_AO_HOST_INTERRUPT_CLR 0x23b04
#define GMU_AO_HOST_INTERRUPT_MASK 0x23b06
#define GMU_RSCC_CONTROL_REQ	0x23b07
#define GMU_RSCC_CONTROL_ACK	0x23b08
#define GMU_AHB_FENCE_RANGE_0	0x23b11

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
#define GMU_CONTEXT_BANK		0


struct register_window {
	area_id			area;
	volatile uint8*	base;

	status_t Map(const char* name, phys_addr_t physical, size_t size)
	{
		area = map_physical_memory(name, physical, size, B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&base);
		return area < 0 ? area : B_OK;
	}

	void Unmap()
	{
		if (area >= 0)
			delete_area(area);
		area = -1;
	}

	uint32 Read(uint32 offset) const
		{ return *(volatile uint32*)(base + offset); }
	void Write(uint32 offset, uint32 value)
		{ *(volatile uint32*)(base + offset) = value; }

	bool Wait(uint32 offset, uint32 mask, uint32 value, bigtime_t timeout)
	{
		bigtime_t end = system_time() + timeout;
		while ((Read(offset) & mask) != value) {
			if (system_time() > end)
				return false;
			spin(10);
		}
		return true;
	}
};


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


/*!	A stage 1 address space of 4 GiB (one level 1 table of 4 entries, level
	2 and 3 tables as needed), walked coherently by the SMMU.
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
	status_t Map(uint32 address, phys_addr_t physical, uint32 attribute,
		bool privileged)
	{
		uint64* level2 = NextTable(fLevel1, (address >> 30) & 3);
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
	status_t MapArea(void* virtualAddress, size_t size, uint32 address,
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

private:
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
	uint64*		fTables[64];
	phys_addr_t	fTablePhysical[64];
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
	const AddressSpace& space)
{
	uint32 cb = SMMU_CB(bank);
	sSmmu.Write(cb + CB_SCTLR, 0);
	sSmmu.Write(SMMU_CBA2R(bank), CBA2R_VA64);
	sSmmu.Write(SMMU_CBAR(bank), CBAR_TYPE_S1_S2BYPASS | CBAR_S1_MEMATTR_WB
		| CBAR_S1_BPSHCFG_NSH | (bank + 1));
	sSmmu.Write(cb + CB_TCR2, CBTCR2_SEP_UPSTREAM | CBTCR2_PASIZE_40);
	sSmmu.Write(cb + CB_TCR, CBTCR_T0SZ_32 | CBTCR_IRGN0_WB | CBTCR_ORGN0_WB
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


static void*
load_firmware(const char* name, size_t* _size)
{
	static const char* kDirectories[] = {
		"/boot/system/non-packaged/data/firmware",
		"/boot/system/data/firmware",
	};
	for (size_t i = 0; i < B_COUNT_OF(kDirectories); i++) {
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


//	#pragma mark - bring-up


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
		INFO("the GMU is running already\n");
		return B_BUSY;
	}

	INFO("SMMU sCR0 %#" B_PRIx32 ", %" B_PRIu32 " stream groups, %" B_PRIu32
		" context banks; GMU in reset %" B_PRIu32 "\n", sSmmu.Read(SMMU_sCR0),
		sSmmu.Read(SMMU_IDR0) & 0xff, sSmmu.Read(SMMU_IDR1) & 0xff,
		gmu_read(GMU_CM3_SYSRESET));

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
	smmu_attach(0, GMU_STREAM_ID, GMU_STREAM_MASK, GMU_CONTEXT_BANK,
		sGmuSpace);
	INFO("GMU stream %#x routed to context bank %d, SCTLR %#" B_PRIx32
		", page tables at %#" B_PRIxPHYSADDR "\n", GMU_STREAM_ID,
		GMU_CONTEXT_BANK, sSmmu.Read(SMMU_CB(GMU_CONTEXT_BANK) + CB_SCTLR),
		sGmuSpace.Root());

	size_t firmwareSize;
	uint8* firmware = (uint8*)load_firmware("qcom/a660_gmu.bin",
		&firmwareSize);
	if (firmware == NULL)
		return B_ENTRY_NOT_FOUND;

	gmu_rpmh_init();
	status = gmu_start(firmware, firmwareSize);
	free(firmware);
	smmu_report(GMU_CONTEXT_BANK, "after the GMU start");
	return status;
}


// devfs calls init_hardware() when it loads the driver: the bring-up runs
// then, once, and the driver reports no device yet.
status_t
init_hardware()
{
	static bool sDone = false;
	if (!sDone) {
		sDone = true;
		status_t status = bring_up();
		INFO("bring-up: %s\n", strerror(status));
	}
	return B_ERROR;
}


status_t
init_driver()
{
	return B_ERROR;
}


void
uninit_driver()
{
}


const char**
publish_devices()
{
	return NULL;
}


device_hooks*
find_device(const char* name)
{
	return NULL;
}
