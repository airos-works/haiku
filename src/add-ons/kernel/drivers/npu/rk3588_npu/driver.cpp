/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Driver for the neural processor of the Rockchip RK3588 ("RKNN", three
	cores of an NVDLA descendant). See rk3588_npu.h for what it offers.

	What is known about the hardware comes from Linux 6.18's "rocket"
	driver (drivers/accel/rocket), its power domain, clock and IOMMU
	drivers (drivers/pmdomain/rockchip/pm-domains.c,
	drivers/clk/rockchip/clk-rk3588.c, drivers/iommu/rockchip-iommu.c) and
	the RK3588 device tree; nothing here was copied from them.

	The device trees the firmware of today's boards hands over do not
	describe the processor (the nodes were added to Linux in 6.18), so the
	driver attaches to the clock and reset unit of an RK3588 and uses the
	addresses the SoC has; where a tree does have a
	"rockchip,rk3588-rknn-core" node it describes the same thing.

	For now the driver uses the first core only: jobs run one at a time.
	The core is powered when the first client opens the device and
	switched off when the last one closes it.
*/


#include <bus/FDT.h>
#include <KernelExport.h>
#include <device_manager.h>
#include <driver_settings.h>
#include <lock.h>
#include <team.h>
#include <AutoDeleter.h>
#include <util/AutoLock.h>
#include <util/DoublyLinkedList.h>
#include <vm/vm.h>
#include <arch/arm64/cache_poc.h>

#include <algorithm>
#include <fcntl.h>
#include <new>
#include <stdlib.h>
#include <string.h>

#include "rk3588_npu.h"


#define DRIVER_NAME "drivers/npu/rk3588_npu/driver_v1"
#define DEVICE_NAME "drivers/npu/rk3588_npu/device_v1"

#define TRACE(x...) dprintf("rk3588_npu: " x)


static device_manager_info* sDeviceManager;


// #pragma mark - the hardware


// The clock and reset unit (CRU): "hiword mask" registers, where the
// upper half of a written word says which bits of the lower half count.
static const phys_addr_t kCruBase = 0xfd7c0000;
static const size_t kCruSize = 0x1000;
static const uint32 kCruClockSelect73 = 0x300 + 73 * 4;
	// bits 1:0 hclk_npu_root source, 6:2 clk_npu_dsu0 divider - 1,
	// 9:7 clk_npu_dsu0 source (0 GPLL, 1 CPLL, 2 AUPLL, 3 NPLL, 4 SPLL)
static const uint32 kCruClockGate29 = 0x800 + 29 * 4;
	// bit 0 hclk_npu_root, 1 clk_npu_dsu0, 4 pclk_npu_root
static const uint32 kCruClockGate30 = 0x800 + 30 * 4;
	// bit 6 aclk_npu0, 8 hclk_npu0
static const uint32 kCruSoftReset30 = 0xa00 + 30 * 4;
	// bit 6 srst_a_rknn0, 7 its bus interface, 8 srst_h_rknn0, 9 its
	// bus interface
static const uint32 kGate29Bits = (1 << 0) | (1 << 1) | (1 << 4);
static const uint32 kGate30Bits = (1 << 6) | (1 << 8);
// The core is reset, its bus interfaces are not: resetting those while the
// core is in the middle of a transfer (a job that hangs) leaves the bus in
// a state that only a restart of the machine ends -- no job completes any
// more and the power domain cannot be taken down.
static const uint32 kReset30Bits = (1 << 6) | (1 << 8);

// GPLL runs at 1188 MHz on every RK3588 board (the firmware and Linux
// depend on it). A sixth of it is the 200 MHz Linux gives the processor,
// which every supply voltage in its operating table allows.
static const uint32 kGpllRate = 1188000000;
static const uint32 kDefaultClockDivider = 6;

// The power management unit (PMU), hiword mask registers too.
static const phys_addr_t kPmuBase = 0xfd8d8000;
static const size_t kPmuSize = 0x1000;
static const uint32 kPmuIdleRequest = 0x10c;
static const uint32 kPmuIdleAck = 0x118;
static const uint32 kPmuIdleStatus = 0x120;
static const uint32 kPmuPowerGate = 0x14c;
	// 1: off
static const uint32 kPmuPowerStatus = 0x180;
	// 1: off
static const uint32 kPmuMemoryPower = 0x1a0;
static const uint32 kPmuChainStatus = 0x1f0;
	// 1: on
static const uint32 kPmuMemoryStatus = 0x1f8;
	// 1: off
static const uint32 kPmuRepairStatus = 0x290;
	// 1: on

// The domain all of the processor is in ("NPU") and that of the first
// core and what the cores share ("NPUTOP").
static const uint32 kNpuPowerBit = 1 << 1;
static const uint32 kTopPowerBit = 1 << 3;
static const uint32 kTopMemoryBit = 1 << 11;
static const uint32 kTopRepairBit = 1 << 2;
static const uint32 kTopIdleBit = 1 << 1;

// The first core. Its register blocks follow one another in one 64 KiB
// window; the two IOMMU instances that translate its bus accesses are in
// it too.
static const phys_addr_t kCoreBase = 0xfdab0000;
static const size_t kCoreSize = 0x10000;
static const uint32 kCoreInterrupt = 110 + 32;
	// GIC shared peripheral interrupt 110, level, shared with the IOMMU

static const uint32 kPcVersion = 0x0000;
static const uint32 kPcVersionNumber = 0x0004;
static const uint32 kPcOperationEnable = 0x0008;
static const uint32 kPcBaseAddress = 0x0010;
static const uint32 kPcRegisterAmounts = 0x0014;
static const uint32 kPcInterruptMask = 0x0020;
static const uint32 kPcInterruptClear = 0x0024;
static const uint32 kPcInterruptRawStatus = 0x002c;
static const uint32 kPcTaskControl = 0x0030;
static const uint32 kPcTaskDmaBase = 0x0034;
static const uint32 kCnaPointer = 0x1004;
static const uint32 kCorePointer = 0x3004;

static const uint32 kInterruptDone = (1 << 8) | (1 << 9);
	// the post-processing unit finished, in either register group
static const uint32 kInterruptDmaError = (1 << 12) | (1 << 13);
static const uint32 kInterruptAll = 0x1ffff;

// pointer "ping-pong" mode: the two register groups of each unit take
// turns, the executing one switches when a task is done
static const uint32 kPointerSetup = (1 << 1) | (1 << 2) | (1 << 3);
// one task per start, task counter cleared
static const uint32 kTaskControl = (1 << 14) | (1 << 13) | (1 << 12) | 1;

static const uint32 kMmuOffsets[] = { 0x9000, 0xa000 };
static const uint32 kMmuCount = 2;

static const uint32 kMmuDirectory = 0x00;
static const uint32 kMmuStatus = 0x04;
static const uint32 kMmuCommand = 0x08;
static const uint32 kMmuFaultAddress = 0x0c;
static const uint32 kMmuInterruptRawStatus = 0x14;
static const uint32 kMmuInterruptClear = 0x18;
static const uint32 kMmuInterruptMask = 0x1c;
static const uint32 kMmuInterruptStatus = 0x20;

static const uint32 kMmuStatusPaging = 1 << 0;
static const uint32 kMmuStatusPageFault = 1 << 1;
static const uint32 kMmuStatusStall = 1 << 2;

static const uint32 kMmuEnablePaging = 0;
static const uint32 kMmuDisablePaging = 1;
static const uint32 kMmuEnableStall = 2;
static const uint32 kMmuDisableStall = 3;
static const uint32 kMmuZapCache = 4;
static const uint32 kMmuForceReset = 6;

static const uint32 kMmuInterrupts = 0x3;
	// page fault, bus error

// The IOMMU's tables: a directory of 1024 page tables of 1024 pages
// each, 32 bit entries. Bits 31:12 are bits 31:12 of the address, bits
// 11:8 its bits 35:32, bits 7:4 its bits 39:36; in page entries bit 1
// allows reading and bit 2 writing; bit 0 says the entry is valid.
static const uint32 kTableEntries = 1024;
static const uint32 kEntryValid = 1 << 0;
static const uint32 kEntryReadWrite = (1 << 1) | (1 << 2);

// Where clients' buffers go in the device's address space. Nothing is
// ever put at address 0.
static const uint32 kDeviceSpaceBase = 0x01000000;
static const uint32 kDeviceSpaceEnd = 0xf0000000;

static const uint64 kMaxBufferSize = 512 * 1024 * 1024;
static const uint32 kMaxTasks = 4096;
static const bigtime_t kDefaultTimeout = 3 * 1000 * 1000;
static const bigtime_t kFaultPollInterval = 50 * 1000;
static const bigtime_t kMaxTimeout = 60 * 1000 * 1000;


// #pragma mark - structures


struct Buffer : DoublyLinkedListLinkImpl<Buffer> {
	uint32		handle;
	size_t		size;
	area_id		kernelArea;
	uint8*		kernelAddress;
	area_id		userArea;
	uint32		deviceAddress;
};

typedef DoublyLinkedList<Buffer> BufferList;


/*!	An address space of the device: what one client's jobs can reach. */
struct Domain {
	area_id		directoryArea;
	uint32*		directory;
	phys_addr_t	directoryPhysical;
	area_id*	tableAreas;
	uint32**	tables;
};


struct Device;

struct Client {
	Device*		device;
	team_id		team;
	mutex		lock;
		// the buffers
	BufferList	buffers;
	uint32		nextHandle;
	Domain		domain;
};


struct Device {
	device_node*	node;
	mutex			lock;
		// power and the count of clients
	int32			clients;
	uint32			clockDivider;
	uint32			hardwareVersion;

	area_id			cruArea;
	area_id			pmuArea;
	area_id			coreArea;
	volatile uint8*	cru;
	volatile uint8*	pmu;
	volatile uint8*	core;

	mutex			jobLock;
		// one job at a time; also held to change what a domain maps
	Domain*			attached;
		// whose tables the IOMMU uses

	// The job on the core. The interrupt handler starts its tasks one
	// after the other.
	spinlock		jobSpin;
	bool			powered;
	bool			active;
	bool			failed;
	bool			faulted;
		// stopped by the IOMMU
	const rk3588_npu_task* tasks;
	uint32			taskCount;
	uint32			nextTask;
	uint32			tasksDone;
	uint32			interruptStatus;
	uint32			faultAddress;
	sem_id			done;
};


static inline uint32
read32(volatile uint8* base, uint32 offset)
{
	return *(volatile uint32*)(base + offset);
}


static inline void
write32(volatile uint8* base, uint32 offset, uint32 value)
{
	*(volatile uint32*)(base + offset) = value;
}


//! Sets or clears bits of a hiword mask register.
static inline void
write_masked(volatile uint8* base, uint32 offset, uint32 bits, bool set)
{
	write32(base, offset, (bits << 16) | (set ? bits : 0));
}


/*!	Waits until bits of a register are all set or all clear. */
static bool
wait_for_bits(volatile uint8* base, uint32 offset, uint32 bits, bool set,
	bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	while (true) {
		uint32 value = read32(base, offset) & bits;
		if (set ? value == bits : value == 0)
			return true;
		if (system_time() > end)
			return false;
		spin(10);
	}
}


/*!	Writes the CPU's view of a range of memory to memory proper, for the
	device to read, or drops it, to read what the device wrote. */
static void
sync_cache(const void* address, size_t size, bool forDevice)
{
	uint64 line = arm64_current_data_cache_line_size();
	uintptr_t start = (uintptr_t)address & ~(line - 1);
	uintptr_t end = (uintptr_t)address + size;

	for (uintptr_t p = start; p < end; p += line) {
		if (forDevice)
			arm64_clean_data_cache_line_poc(p);
		else
			arm64_clean_invalidate_data_cache_line_poc(p);
	}
	asm volatile("dsb sy" ::: "memory");
}


// #pragma mark - IOMMU


static inline uint32
make_entry(phys_addr_t address)
{
	return (uint32)(address & 0xfffff000)
		| (uint32)((address >> 32) & 0xf) << 8
		| (uint32)((address >> 36) & 0xf) << 4
		| kEntryValid;
}


/*!	A page for a table of the IOMMU, below 4 GiB (the directory register
	is 32 bits wide), zeroed, with its physical address. */
static area_id
allocate_table(const char* name, uint32** _table, phys_addr_t* _physical)
{
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = (phys_addr_t)1 << 32;

	void* address;
	area_id area = create_area_etc(B_SYSTEM_TEAM, name, B_PAGE_SIZE,
		B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &address);
	if (area < 0)
		return area;

	physical_entry entry;
	status_t status = get_memory_map(address, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK) {
		delete_area(area);
		return status;
	}

	memset(address, 0, B_PAGE_SIZE);
	sync_cache(address, B_PAGE_SIZE, true);

	*_table = (uint32*)address;
	*_physical = entry.address;
	return area;
}


static status_t
init_domain(Domain& domain)
{
	domain.tableAreas = (area_id*)malloc(kTableEntries * sizeof(area_id));
	domain.tables = (uint32**)calloc(kTableEntries, sizeof(uint32*));
	if (domain.tableAreas == NULL || domain.tables == NULL) {
		free(domain.tableAreas);
		free(domain.tables);
		domain.tableAreas = NULL;
		domain.tables = NULL;
		return B_NO_MEMORY;
	}
	for (uint32 i = 0; i < kTableEntries; i++)
		domain.tableAreas[i] = -1;

	domain.directoryArea = allocate_table("rk3588_npu directory",
		&domain.directory, &domain.directoryPhysical);
	if (domain.directoryArea < 0) {
		free(domain.tableAreas);
		free(domain.tables);
		domain.tableAreas = NULL;
		domain.tables = NULL;
		return domain.directoryArea;
	}
	return B_OK;
}


static void
destroy_domain(Domain& domain)
{
	if (domain.tables == NULL)
		return;

	for (uint32 i = 0; i < kTableEntries; i++) {
		if (domain.tableAreas[i] >= 0)
			delete_area(domain.tableAreas[i]);
	}
	delete_area(domain.directoryArea);
	free(domain.tableAreas);
	free(domain.tables);
	domain.tableAreas = NULL;
	domain.tables = NULL;
}


/*!	Makes the pages of a kernel buffer appear at a device address. */
static status_t
map_in_domain(Domain& domain, uint32 deviceAddress, const uint8* address,
	size_t size)
{
	for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE) {
		uint32 target = deviceAddress + offset;
		uint32 directoryIndex = target >> 22;
		uint32 tableIndex = (target >> 12) & (kTableEntries - 1);

		uint32* table = domain.tables[directoryIndex];
		if (table == NULL) {
			phys_addr_t physical;
			area_id area = allocate_table("rk3588_npu page table", &table,
				&physical);
			if (area < 0)
				return area;

			domain.tableAreas[directoryIndex] = area;
			domain.tables[directoryIndex] = table;
			domain.directory[directoryIndex] = make_entry(physical);
			sync_cache(&domain.directory[directoryIndex], sizeof(uint32),
				true);
		}

		physical_entry entry;
		status_t status = get_memory_map(address + offset, B_PAGE_SIZE,
			&entry, 1);
		if (status != B_OK)
			return status;

		table[tableIndex] = make_entry(entry.address) | kEntryReadWrite;
		if (tableIndex == kTableEntries - 1 || offset + B_PAGE_SIZE >= size)
			sync_cache(table, B_PAGE_SIZE, true);
	}

	// tables that were filled without reaching their end or the buffer's
	uint32 first = deviceAddress >> 22;
	uint32 last = (deviceAddress + size - 1) >> 22;
	for (uint32 i = first; i <= last; i++) {
		if (domain.tables[i] != NULL)
			sync_cache(domain.tables[i], B_PAGE_SIZE, true);
	}
	return B_OK;
}


static void
unmap_in_domain(Domain& domain, uint32 deviceAddress, size_t size)
{
	for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE) {
		uint32 target = deviceAddress + offset;
		uint32* table = domain.tables[target >> 22];
		if (table != NULL)
			table[(target >> 12) & (kTableEntries - 1)] = 0;
	}

	uint32 first = deviceAddress >> 22;
	uint32 last = (deviceAddress + size - 1) >> 22;
	for (uint32 i = first; i <= last; i++) {
		if (domain.tables[i] != NULL)
			sync_cache(domain.tables[i], B_PAGE_SIZE, true);
	}
}


static void
mmu_command(Device* device, uint32 command)
{
	for (uint32 i = 0; i < kMmuCount; i++)
		write32(device->core, kMmuOffsets[i] + kMmuCommand, command);
}


static bool
mmu_wait(Device* device, uint32 bit, bool set, bigtime_t timeout)
{
	for (uint32 i = 0; i < kMmuCount; i++) {
		if (!wait_for_bits(device->core, kMmuOffsets[i] + kMmuStatus, bit,
				set, timeout)) {
			return false;
		}
	}
	return true;
}


static bool
mmu_all(Device* device, uint32 bit)
{
	for (uint32 i = 0; i < kMmuCount; i++) {
		if ((read32(device->core, kMmuOffsets[i] + kMmuStatus) & bit) == 0)
			return false;
	}
	return true;
}


/*!	Whether the IOMMU stopped the core at an address that is not mapped. */
static bool
mmu_fault_pending(Device* device, uint32* _address)
{
	bool pending = false;
	for (uint32 i = 0; i < kMmuCount; i++) {
		if ((read32(device->core, kMmuOffsets[i] + kMmuStatus)
				& kMmuStatusPageFault) == 0
			&& read32(device->core, kMmuOffsets[i] + kMmuInterruptRawStatus)
				== 0) {
			continue;
		}

		pending = true;
		*_address = read32(device->core, kMmuOffsets[i] + kMmuFaultAddress);
	}
	return pending;
}


/*!	Has the IOMMU forget what it cached of the tables; the core must be
	idle. */
static void
mmu_zap(Device* device)
{
	if (device->powered && device->attached != NULL)
		mmu_command(device, kMmuZapCache);
}


/*!	Points the IOMMU at a domain's tables and switches translation on;
	with NULL, switches it off. The core must be powered and idle. */
static status_t
attach_domain(Device* device, Domain* domain)
{
	if (device->attached == domain)
		return B_OK;

	// Commands other than paging on and off are only taken while the
	// IOMMU is stalled, and it can only be stalled while it translates.
	if (mmu_all(device, kMmuStatusPaging)
		&& !mmu_all(device, kMmuStatusStall)) {
		mmu_command(device, kMmuEnableStall);
		if (!mmu_wait(device, kMmuStatusStall, true, 10000)) {
			TRACE("the IOMMU does not stall (status %#" B_PRIx32 ")\n",
				read32(device->core, kMmuOffsets[0] + kMmuStatus));
			return B_IO_ERROR;
		}
	}

	// A reset clears the directory register; that it can be written at
	// all is checked first.
	for (uint32 i = 0; i < kMmuCount; i++) {
		write32(device->core, kMmuOffsets[i] + kMmuDirectory, 0xcafeb000);
		if (read32(device->core, kMmuOffsets[i] + kMmuDirectory)
				!= 0xcafeb000) {
			TRACE("IOMMU %" B_PRIu32 " does not answer\n", i);
			return B_IO_ERROR;
		}
	}
	mmu_command(device, kMmuForceReset);
	for (uint32 i = 0; i < kMmuCount; i++) {
		if (!wait_for_bits(device->core, kMmuOffsets[i] + kMmuDirectory,
				0xffffffff, false, 100000)) {
			TRACE("IOMMU %" B_PRIu32 " does not reset\n", i);
			return B_IO_ERROR;
		}
	}

	device->attached = NULL;
	if (domain == NULL)
		return B_OK;

	for (uint32 i = 0; i < kMmuCount; i++) {
		write32(device->core, kMmuOffsets[i] + kMmuDirectory,
			make_entry(domain->directoryPhysical));
		write32(device->core, kMmuOffsets[i] + kMmuCommand, kMmuZapCache);
		write32(device->core, kMmuOffsets[i] + kMmuInterruptMask,
			kMmuInterrupts);
	}

	mmu_command(device, kMmuEnablePaging);
	if (!mmu_wait(device, kMmuStatusPaging, true, 10000)) {
		TRACE("the IOMMU does not start translating (status %#" B_PRIx32
			")\n", read32(device->core, kMmuOffsets[0] + kMmuStatus));
		return B_IO_ERROR;
	}

	if (mmu_all(device, kMmuStatusStall)) {
		mmu_command(device, kMmuDisableStall);
		mmu_wait(device, kMmuStatusStall, false, 10000);
	}

	device->attached = domain;
	return B_OK;
}


// #pragma mark - power


static void
reset_core(Device* device)
{
	write_masked(device->cru, kCruSoftReset30, kReset30Bits, true);
	spin(20);
	write_masked(device->cru, kCruSoftReset30, kReset30Bits, false);
	spin(20);

	// the IOMMU went with it
	device->attached = NULL;
}


/*!	Brings the first core up from whatever state it is in. The device's
	lock must be held. */
static status_t
power_on(Device* device)
{
	if (device->powered)
		return B_OK;

	// The clocks of the domain have to run while it changes state.
	write_masked(device->cru, kCruClockGate29, kGate29Bits, false);
	write_masked(device->cru, kCruClockGate30, kGate30Bits, false);

	// the cores' clock: GPLL by the divider
	write32(device->cru, kCruClockSelect73, (0xffu << 2) << 16
		| (device->clockDivider - 1) << 2);

	// The outer domain is on wherever the firmware leaves the board's
	// supply for it on; it is left as it is found afterwards, too.
	if ((read32(device->pmu, kPmuPowerStatus) & kNpuPowerBit) != 0) {
		write_masked(device->pmu, kPmuPowerGate, kNpuPowerBit, false);
		if (!wait_for_bits(device->pmu, kPmuPowerStatus, kNpuPowerBit, false,
				20000)) {
			TRACE("the NPU power domain does not come up (is its supply "
				"on?)\n");
			return B_IO_ERROR;
		}
	}

	if ((read32(device->pmu, kPmuRepairStatus) & kTopRepairBit) == 0) {
		// Memories that kept their power while the logic was off have to
		// be power cycled with it.
		bool memoryOn = (read32(device->pmu, kPmuMemoryStatus)
			& kTopMemoryBit) == 0;

		write_masked(device->pmu, kPmuPowerGate, kTopPowerBit, false);

		if (memoryOn) {
			if (!wait_for_bits(device->pmu, kPmuChainStatus, kTopMemoryBit,
					true, 10000)) {
				TRACE("no power chain status for the core\n");
				return B_IO_ERROR;
			}
			spin(20);
			write_masked(device->pmu, kPmuMemoryPower, kTopPowerBit, true);
			if (!wait_for_bits(device->pmu, kPmuMemoryStatus, kTopMemoryBit,
					true, 10000)) {
				TRACE("the core's memories do not switch off\n");
				return B_IO_ERROR;
			}
			write_masked(device->pmu, kPmuMemoryPower, kTopPowerBit, false);
			if (!wait_for_bits(device->pmu, kPmuMemoryStatus, kTopMemoryBit,
					false, 10000)) {
				TRACE("the core's memories do not switch on\n");
				return B_IO_ERROR;
			}
		}

		if (!wait_for_bits(device->pmu, kPmuRepairStatus, kTopRepairBit,
				true, 20000)) {
			TRACE("the core's power domain does not come up (status %#"
				B_PRIx32 ")\n", read32(device->pmu, kPmuRepairStatus));
			write_masked(device->pmu, kPmuPowerGate, kTopPowerBit, true);
			return B_IO_ERROR;
		}
	}

	// let its bus interface go
	write_masked(device->pmu, kPmuIdleRequest, kTopIdleBit, false);
	if (!wait_for_bits(device->pmu, kPmuIdleAck, kTopIdleBit, false, 10000)
		|| !wait_for_bits(device->pmu, kPmuIdleStatus, kTopIdleBit, false,
			10000)) {
		TRACE("the core's bus interface stays idle\n");
		return B_IO_ERROR;
	}

	reset_core(device);

	device->hardwareVersion = read32(device->core, kPcVersion)
		+ (read32(device->core, kPcVersionNumber) & 0xffff);

	write32(device->core, kPcInterruptMask, 0);
	write32(device->core, kPcInterruptClear, kInterruptAll);

	InterruptsSpinLocker locker(device->jobSpin);
	device->powered = true;
	return B_OK;
}


static void
power_off(Device* device)
{
	if (!device->powered)
		return;

	{
		InterruptsSpinLocker locker(device->jobSpin);
		write32(device->core, kPcInterruptMask, 0);
		for (uint32 i = 0; i < kMmuCount; i++)
			write32(device->core, kMmuOffsets[i] + kMmuInterruptMask, 0);
		device->powered = false;
	}
	device->attached = NULL;

	// A core that was left in the middle of something does not let go
	// of the bus; a reset makes it.
	bool idle = false;
	for (int32 attempt = 0; !idle && attempt < 2; attempt++) {
		if (attempt > 0)
			reset_core(device);

		write_masked(device->pmu, kPmuIdleRequest, kTopIdleBit, true);
		idle = wait_for_bits(device->pmu, kPmuIdleAck, kTopIdleBit, true,
				10000)
			&& wait_for_bits(device->pmu, kPmuIdleStatus, kTopIdleBit, true,
				10000);
	}
	if (!idle) {
		// Taking the power away under a transfer would be worse than
		// leaving it on.
		TRACE("the core's bus interface does not go idle, the core stays "
			"powered\n");
		write_masked(device->pmu, kPmuIdleRequest, kTopIdleBit, false);
		return;
	}

	write_masked(device->pmu, kPmuPowerGate, kTopPowerBit, true);
	if (!wait_for_bits(device->pmu, kPmuRepairStatus, kTopRepairBit, false,
			20000)) {
		TRACE("the core's power domain does not go down\n");
	}

	// as the firmware had it
	write_masked(device->pmu, kPmuIdleRequest, kTopIdleBit, false);
}


// #pragma mark - jobs


/*!	Starts the next task of the job; the job spinlock is held. */
static void
start_task(Device* device)
{
	const rk3588_npu_task& task = device->tasks[device->nextTask++];

	write32(device->core, kPcBaseAddress, 1);
	write32(device->core, kCnaPointer, kPointerSetup);
	write32(device->core, kCorePointer, kPointerSetup);

	write32(device->core, kPcBaseAddress, task.regcmd);
	write32(device->core, kPcRegisterAmounts,
		(task.regcmd_count + 1) / 2 - 1);

	write32(device->core, kPcInterruptMask, kInterruptDone);
	write32(device->core, kPcInterruptClear, kInterruptDone);

	write32(device->core, kPcTaskControl, kTaskControl);
	write32(device->core, kPcTaskDmaBase, 0);

	write32(device->core, kPcOperationEnable, 1);
}


static int32
handle_interrupt(void* data)
{
	Device* device = (Device*)data;

	SpinLocker locker(device->jobSpin);

	// The registers are only there while the core has power.
	if (!device->powered)
		return B_UNHANDLED_INTERRUPT;

	uint32 mmuStatus = 0;
	for (uint32 i = 0; i < kMmuCount; i++) {
		mmuStatus |= read32(device->core,
			kMmuOffsets[i] + kMmuInterruptStatus);
	}
	uint32 status = read32(device->core, kPcInterruptRawStatus);

	if (mmuStatus != 0) {
		// The job reached for memory it does not have. The core is stuck
		// on that access now; whoever waits for the job resets it.
		for (uint32 i = 0; i < kMmuCount; i++) {
			uint32 fault = read32(device->core,
				kMmuOffsets[i] + kMmuFaultAddress);
			if (fault != 0)
				device->faultAddress = fault;
			write32(device->core, kMmuOffsets[i] + kMmuInterruptMask, 0);
		}
		write32(device->core, kPcInterruptMask, 0);

		if (!device->active)
			return B_HANDLED_INTERRUPT;
		device->active = false;
		device->failed = true;
		device->faulted = true;
		device->interruptStatus = status;
		release_sem_etc(device->done, 1, B_DO_NOT_RESCHEDULE);
		return B_INVOKE_SCHEDULER;
	}

	if ((status & (kInterruptDone | kInterruptDmaError)) == 0)
		return B_UNHANDLED_INTERRUPT;

	write32(device->core, kPcOperationEnable, 0);
	write32(device->core, kPcInterruptClear, kInterruptAll);

	if (!device->active) {
		write32(device->core, kPcInterruptMask, 0);
		return B_HANDLED_INTERRUPT;
	}

	if ((status & kInterruptDmaError) != 0) {
		device->failed = true;
		device->interruptStatus = status;
	} else {
		device->tasksDone++;
		if (device->nextTask < device->taskCount) {
			start_task(device);
			return B_HANDLED_INTERRUPT;
		}
	}

	write32(device->core, kPcInterruptMask, 0);
	device->active = false;
	release_sem_etc(device->done, 1, B_DO_NOT_RESCHEDULE);
	return B_INVOKE_SCHEDULER;
}


static Buffer*
find_buffer(Client* client, uint32 handle)
{
	BufferList::Iterator iterator = client->buffers.GetIterator();
	while (Buffer* buffer = iterator.Next()) {
		if (buffer->handle == handle)
			return buffer;
	}
	return NULL;
}


/*!	Whether a stretch of the device's address space is all in one of the
	client's buffers. */
static bool
in_buffer(Client* client, uint32 deviceAddress, uint64 size)
{
	BufferList::Iterator iterator = client->buffers.GetIterator();
	while (Buffer* buffer = iterator.Next()) {
		if (deviceAddress >= buffer->deviceAddress
			&& (uint64)deviceAddress + size
				<= (uint64)buffer->deviceAddress + buffer->size) {
			return true;
		}
	}
	return false;
}


static status_t
submit(Client* client, void* userArgs)
{
	Device* device = client->device;

	rk3588_npu_submit args;
	if (!IS_USER_ADDRESS(userArgs)
		|| user_memcpy(&args, userArgs, sizeof(args)) != B_OK) {
		return B_BAD_ADDRESS;
	}
	if (args.task_count == 0 || args.task_count > kMaxTasks
		|| args.timeout < 0 || args.timeout > kMaxTimeout) {
		return B_BAD_VALUE;
	}

	rk3588_npu_task* tasks = (rk3588_npu_task*)malloc(
		args.task_count * sizeof(rk3588_npu_task));
	if (tasks == NULL)
		return B_NO_MEMORY;
	MemoryDeleter tasksDeleter(tasks);

	if (!IS_USER_ADDRESS(args.tasks) || user_memcpy(tasks, args.tasks,
			args.task_count * sizeof(rk3588_npu_task)) != B_OK) {
		return B_BAD_ADDRESS;
	}

	// The IOMMU keeps a job within its client's buffers whatever its
	// command lists say; that the lists themselves are in one is checked
	// here, so that a mistake is an error instead of a stuck core.
	{
		MutexLocker locker(client->lock);
		for (uint32 i = 0; i < args.task_count; i++) {
			if (tasks[i].regcmd_count == 0
				|| tasks[i].regcmd_count > 0x1fffe
				|| !in_buffer(client, tasks[i].regcmd,
					(uint64)tasks[i].regcmd_count * 8)) {
				return B_BAD_VALUE;
			}
		}
	}

	bigtime_t timeout = args.timeout != 0 ? args.timeout : kDefaultTimeout;

	MutexLocker jobLocker(device->jobLock);

	status_t status = attach_domain(device, &client->domain);
	if (status != B_OK) {
		reset_core(device);
		return status;
	}

	// a wake-up left over from a job that was given up
	while (acquire_sem_etc(device->done, 1, B_RELATIVE_TIMEOUT, 0) == B_OK)
		;

	bigtime_t start = system_time();
	{
		InterruptsSpinLocker locker(device->jobSpin);
		device->tasks = tasks;
		device->taskCount = args.task_count;
		device->nextTask = 0;
		device->tasksDone = 0;
		device->interruptStatus = 0;
		device->faultAddress = 0;
		device->failed = false;
		device->faulted = false;
		device->active = true;
		start_task(device);
	}

	// The interrupt handler wakes us when the job is done. A job that
	// reaches for memory its client does not have is stopped by the
	// IOMMU, which says so in its registers; its interrupt, though, does
	// not arrive (on the ROCK 5 ITX at least, whatever the device tree of
	// Linux says about the line), so the wait looks for itself now and
	// then. Jobs that are done sooner never get to that.
	bigtime_t deadline = start + timeout;
	while (true) {
		bigtime_t slice = std::min(deadline - system_time(),
			kFaultPollInterval);
		if (slice <= 0) {
			status = B_TIMED_OUT;
			break;
		}

		status = acquire_sem_etc(device->done, 1, B_RELATIVE_TIMEOUT, slice);
		if (status != B_TIMED_OUT)
			break;

		uint32 address;
		if (!mmu_fault_pending(device, &address))
			continue;

		InterruptsSpinLocker locker(device->jobSpin);
		if (device->active) {
			device->active = false;
			device->failed = true;
			device->faulted = true;
			device->faultAddress = address;
			device->interruptStatus = read32(device->core,
				kPcInterruptRawStatus);
			write32(device->core, kPcInterruptMask, 0);
		}
		status = B_OK;
		break;
	}
	args.duration = system_time() - start;

	bool failed;
	bool faulted;
	uint32 faultAddress;
	{
		InterruptsSpinLocker locker(device->jobSpin);
		device->active = false;
		device->tasks = NULL;
		failed = device->failed;
		faulted = device->faulted;
		faultAddress = device->faultAddress;
		args.tasks_done = device->tasksDone;
		args.interrupt_status = device->interruptStatus;
		if (status != B_OK || failed) {
			write32(device->core, kPcInterruptMask, 0);
			write32(device->core, kPcOperationEnable, 0);
		}
	}

	if (status != B_OK || failed) {
		if (faulted) {
			TRACE("job of team %" B_PRId32 " failed in task %" B_PRIu32
				" of %" B_PRIu32 ": it reached for device address %#"
				B_PRIx32 ", where the team has no buffer\n", client->team,
				args.tasks_done, args.task_count, faultAddress);
			status = B_BAD_ADDRESS;
		} else if (failed) {
			TRACE("job of team %" B_PRId32 " failed in task %" B_PRIu32
				" of %" B_PRIu32 ": bus error (interrupt status %#" B_PRIx32
				")\n", client->team, args.tasks_done, args.task_count,
				args.interrupt_status);
			status = B_IO_ERROR;
		} else {
			TRACE("job of team %" B_PRId32 " timed out in task %" B_PRIu32
				" of %" B_PRIu32 " (interrupt status %#" B_PRIx32 ")\n",
				client->team, args.tasks_done, args.task_count,
				read32(device->core, kPcInterruptRawStatus));
			status = B_TIMED_OUT;
		}

		// The core is stuck where it was stopped; this is what frees it
		// for the next job.
		reset_core(device);
	}

	jobLocker.Unlock();

	if (user_memcpy(userArgs, &args, sizeof(args)) != B_OK)
		return B_BAD_ADDRESS;
	return status;
}


// #pragma mark - buffers


/*!	A free stretch of the client's device address space; 0 if there is
	none. The buffers are kept in the order of their addresses. */
static uint32
find_device_space(Client* client, size_t size, Buffer** _insertBefore)
{
	uint64 candidate = kDeviceSpaceBase;

	BufferList::Iterator iterator = client->buffers.GetIterator();
	while (Buffer* buffer = iterator.Next()) {
		if (candidate + size <= buffer->deviceAddress) {
			*_insertBefore = buffer;
			return candidate;
		}
		// a page is left free between buffers, to stop overruns
		candidate = (uint64)buffer->deviceAddress + buffer->size
			+ B_PAGE_SIZE;
	}

	*_insertBefore = NULL;
	if (candidate + size > kDeviceSpaceEnd)
		return 0;
	return candidate;
}


static void
delete_buffer(Client* client, Buffer* buffer)
{
	Device* device = client->device;

	// No job may be using it, and the IOMMU may not remember it.
	{
		MutexLocker jobLocker(device->jobLock);
		unmap_in_domain(client->domain, buffer->deviceAddress, buffer->size);
		if (device->attached == &client->domain)
			mmu_zap(device);
	}

	if (buffer->userArea >= 0)
		vm_delete_area(client->team, buffer->userArea, true);
	delete_area(buffer->kernelArea);
	delete buffer;
}


static status_t
create_buffer(Client* client, void* userArgs)
{
	Device* device = client->device;

	rk3588_npu_create_buffer args;
	if (!IS_USER_ADDRESS(userArgs)
		|| user_memcpy(&args, userArgs, sizeof(args)) != B_OK) {
		return B_BAD_ADDRESS;
	}
	if (args.size == 0 || args.size > kMaxBufferSize)
		return B_BAD_VALUE;

	size_t size = ROUNDUP(args.size, B_PAGE_SIZE);

	Buffer* buffer = new(std::nothrow) Buffer;
	if (buffer == NULL)
		return B_NO_MEMORY;
	buffer->size = size;
	buffer->userArea = -1;

	// The memory belongs to the kernel, so that it stays where it is
	// whatever the client does with its view of it.
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	void* address;
	buffer->kernelArea = create_area_etc(B_SYSTEM_TEAM, "rk3588_npu buffer",
		size, B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &address);
	if (buffer->kernelArea < 0) {
		status_t status = buffer->kernelArea;
		delete buffer;
		return status;
	}
	buffer->kernelAddress = (uint8*)address;

	// what another client computed must not show
	memset(address, 0, size);
	sync_cache(address, size, true);

	MutexLocker locker(client->lock);

	Buffer* insertBefore;
	buffer->deviceAddress = find_device_space(client, size, &insertBefore);
	status_t status = buffer->deviceAddress != 0 ? B_OK : B_NO_MEMORY;

	if (status == B_OK) {
		MutexLocker jobLocker(device->jobLock);
		status = map_in_domain(client->domain, buffer->deviceAddress,
			buffer->kernelAddress, size);
		if (status != B_OK)
			unmap_in_domain(client->domain, buffer->deviceAddress, size);
		// It may have looked this up before and found nothing.
		if (device->attached == &client->domain)
			mmu_zap(device);
	}

	void* userAddress = NULL;
	if (status == B_OK) {
		buffer->userArea = vm_clone_area(client->team, "rk3588_npu buffer",
			&userAddress, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA,
			REGION_NO_PRIVATE_MAP, buffer->kernelArea, true);
		if (buffer->userArea < 0)
			status = buffer->userArea;
	}

	if (status != B_OK) {
		if (buffer->deviceAddress != 0) {
			MutexLocker jobLocker(device->jobLock);
			unmap_in_domain(client->domain, buffer->deviceAddress, size);
			if (device->attached == &client->domain)
				mmu_zap(device);
		}
		locker.Unlock();
		delete_area(buffer->kernelArea);
		delete buffer;
		return status;
	}

	buffer->handle = client->nextHandle++;
	if (insertBefore != NULL)
		client->buffers.InsertBefore(insertBefore, buffer);
	else
		client->buffers.Add(buffer);

	args.size = size;
	args.handle = buffer->handle;
	args.device_address = buffer->deviceAddress;
	args.area = buffer->userArea;
	args.address = userAddress;

	if (user_memcpy(userArgs, &args, sizeof(args)) != B_OK) {
		client->buffers.Remove(buffer);
		locker.Unlock();
		delete_buffer(client, buffer);
		return B_BAD_ADDRESS;
	}
	return B_OK;
}


static status_t
sync_buffer(Client* client, void* userArgs)
{
	rk3588_npu_sync_buffer args;
	if (!IS_USER_ADDRESS(userArgs)
		|| user_memcpy(&args, userArgs, sizeof(args)) != B_OK) {
		return B_BAD_ADDRESS;
	}
	if (args.direction != RK3588_NPU_SYNC_FOR_DEVICE
		&& args.direction != RK3588_NPU_SYNC_FOR_CPU) {
		return B_BAD_VALUE;
	}

	MutexLocker locker(client->lock);

	Buffer* buffer = find_buffer(client, args.handle);
	if (buffer == NULL)
		return B_BAD_VALUE;
	if (args.offset > buffer->size
		|| args.size > buffer->size - args.offset) {
		return B_BAD_VALUE;
	}

	size_t size = args.size != 0 ? args.size : buffer->size - args.offset;
	sync_cache(buffer->kernelAddress + args.offset, size,
		args.direction == RK3588_NPU_SYNC_FOR_DEVICE);
	return B_OK;
}


// #pragma mark - device


static float
supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0;
	}

	// The clock and reset unit says that this is an RK3588 and is in
	// every device tree for it.
	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible",
			&compatible, false) != B_OK
		|| strcmp(compatible, "rockchip,rk3588-cru") != 0) {
		return 0;
	}

	return 0.8f;
}


static status_t
register_device(device_node* parent)
{
	device_attr attributes[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Rockchip RK3588 neural processor" } },
		{ NULL }
	};
	return sDeviceManager->register_node(parent, DRIVER_NAME, attributes,
		NULL, NULL);
}


static status_t
map_registers(const char* name, phys_addr_t base, size_t size,
	area_id* _area, volatile uint8** _address)
{
	void* address;
	area_id area = map_physical_memory(name, base, size,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&address);
	if (area < 0)
		return area;

	*_area = area;
	*_address = (volatile uint8*)address;
	return B_OK;
}


static void
uninit_driver(void* cookie)
{
	Device* device = (Device*)cookie;

	remove_io_interrupt_handler(kCoreInterrupt, &handle_interrupt, device);

	if (device->cruArea >= 0)
		delete_area(device->cruArea);
	if (device->pmuArea >= 0)
		delete_area(device->pmuArea);
	if (device->coreArea >= 0)
		delete_area(device->coreArea);
	if (device->done >= 0)
		delete_sem(device->done);
	mutex_destroy(&device->lock);
	mutex_destroy(&device->jobLock);
	free(device);
}


static status_t
init_driver(device_node* node, void** _cookie)
{
	Device* device = (Device*)calloc(1, sizeof(Device));
	if (device == NULL)
		return B_NO_MEMORY;

	device->node = node;
	device->cruArea = -1;
	device->pmuArea = -1;
	device->coreArea = -1;
	device->clockDivider = kDefaultClockDivider;
	mutex_init(&device->lock, "rk3588_npu");
	mutex_init(&device->jobLock, "rk3588_npu job");
	B_INITIALIZE_SPINLOCK(&device->jobSpin);

	void* settings = load_driver_settings("rk3588_npu");
	if (settings != NULL) {
		// "clock_divider 3": 396 MHz. Higher frequencies need a higher
		// supply voltage than some boards start with.
		const char* divider = get_driver_parameter(settings, "clock_divider",
			NULL, NULL);
		if (divider != NULL) {
			uint32 value = strtoul(divider, NULL, 10);
			if (value >= 2 && value <= 32)
				device->clockDivider = value;
		}
		unload_driver_settings(settings);
	}

	device->done = create_sem(0, "rk3588_npu job done");
	status_t status = device->done >= 0 ? B_OK : device->done;

	// Nothing is touched yet: the registers of the core are only there
	// once it has power, which it gets when it is opened.
	if (status == B_OK) {
		status = map_registers("rk3588_npu cru", kCruBase, kCruSize,
			&device->cruArea, &device->cru);
	}
	if (status == B_OK) {
		status = map_registers("rk3588_npu pmu", kPmuBase, kPmuSize,
			&device->pmuArea, &device->pmu);
	}
	if (status == B_OK) {
		status = map_registers("rk3588_npu core", kCoreBase, kCoreSize,
			&device->coreArea, &device->core);
	}
	if (status == B_OK) {
		status = install_io_interrupt_handler(kCoreInterrupt,
			&handle_interrupt, device, 0);
	}

	if (status != B_OK) {
		TRACE("cannot set up: %s\n", strerror(status));
		if (device->cruArea >= 0)
			delete_area(device->cruArea);
		if (device->pmuArea >= 0)
			delete_area(device->pmuArea);
		if (device->coreArea >= 0)
			delete_area(device->coreArea);
		if (device->done >= 0)
			delete_sem(device->done);
		mutex_destroy(&device->lock);
		mutex_destroy(&device->jobLock);
		free(device);
		return status;
	}

	*_cookie = device;
	return B_OK;
}


static status_t
register_child_devices(void* cookie)
{
	Device* device = (Device*)cookie;
	return sDeviceManager->publish_device(device->node, "npu/rk3588/0",
		DEVICE_NAME);
}


static status_t
init_device(void* driverCookie, void** _deviceCookie)
{
	*_deviceCookie = driverCookie;
	return B_OK;
}


static void
uninit_device(void* deviceCookie)
{
}


static status_t
device_open(void* deviceCookie, const char* path, int openMode,
	void** _cookie)
{
	Device* device = (Device*)deviceCookie;

	Client* client = new(std::nothrow) Client;
	if (client == NULL)
		return B_NO_MEMORY;

	client->device = device;
	client->team = team_get_current_team_id();
	client->nextHandle = 1;
	memset(&client->domain, 0, sizeof(client->domain));
	mutex_init(&client->lock, "rk3588_npu client");

	status_t status = init_domain(client->domain);
	if (status == B_OK) {
		MutexLocker locker(device->lock);
		status = power_on(device);
		if (status == B_OK) {
			if (device->clients++ == 0) {
				TRACE("core powered, version %" B_PRIx32 ", clock %" B_PRIu32
					" MHz\n", device->hardwareVersion,
					kGpllRate / device->clockDivider / 1000000);
			}
		} else if (device->clients == 0)
			power_off(device);
	}

	if (status != B_OK) {
		destroy_domain(client->domain);
		mutex_destroy(&client->lock);
		delete client;
		return status;
	}

	*_cookie = client;
	return B_OK;
}


static status_t
device_close(void* cookie)
{
	return B_OK;
}


static status_t
device_free(void* cookie)
{
	Client* client = (Client*)cookie;
	Device* device = client->device;

	while (Buffer* buffer = client->buffers.RemoveHead())
		delete_buffer(client, buffer);

	// The core has power as long as there are clients, and this one
	// still counts.
	{
		MutexLocker jobLocker(device->jobLock);
		if (device->attached == &client->domain) {
			attach_domain(device, NULL);
			device->attached = NULL;
		}
	}

	{
		MutexLocker locker(device->lock);
		if (--device->clients == 0) {
			MutexLocker jobLocker(device->jobLock);
			power_off(device);
		}
	}

	destroy_domain(client->domain);
	mutex_destroy(&client->lock);
	delete client;
	return B_OK;
}


static status_t
device_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
device_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_ALLOWED;
}


static status_t
device_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	Client* client = (Client*)cookie;
	Device* device = client->device;

	switch (op) {
		case RK3588_NPU_GET_INFO:
		{
			rk3588_npu_info info;
			memset(&info, 0, sizeof(info));
			info.interface_version = RK3588_NPU_INTERFACE_VERSION;
			info.hardware_version = device->hardwareVersion;
			info.core_count = 1;
			info.clock_rate = kGpllRate / device->clockDivider;
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(buffer, &info, sizeof(info)) != B_OK) {
				return B_BAD_ADDRESS;
			}
			return B_OK;
		}

		case RK3588_NPU_CREATE_BUFFER:
			return create_buffer(client, buffer);

		case RK3588_NPU_DELETE_BUFFER:
		{
			rk3588_npu_delete_buffer args;
			if (!IS_USER_ADDRESS(buffer)
				|| user_memcpy(&args, buffer, sizeof(args)) != B_OK) {
				return B_BAD_ADDRESS;
			}

			MutexLocker locker(client->lock);
			Buffer* found = find_buffer(client, args.handle);
			if (found == NULL)
				return B_BAD_VALUE;
			client->buffers.Remove(found);
			locker.Unlock();

			delete_buffer(client, found);
			return B_OK;
		}

		case RK3588_NPU_SYNC_BUFFER:
			return sync_buffer(client, buffer);

		case RK3588_NPU_SUBMIT:
			return submit(client, buffer);
	}

	return B_DEV_INVALID_IOCTL;
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};

static device_module_info sDevice = {
	{ DEVICE_NAME, 0, NULL },
	init_device, uninit_device, NULL,
	device_open, device_close, device_free, device_read, device_write, NULL,
	device_control, NULL, NULL
};

static driver_module_info sDriver = {
	{ DRIVER_NAME, 0, NULL },
	supports_device, register_device, init_driver, uninit_driver,
	register_child_devices, NULL, NULL, NULL, NULL
};

module_info* modules[] = {
	(module_info*)&sDriver,
	(module_info*)&sDevice,
	NULL
};
