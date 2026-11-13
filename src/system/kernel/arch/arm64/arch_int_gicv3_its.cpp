/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "arch_int_gicv3_its.h"

#include <arch/arm64/arch_cpu.h>
#include <arch/arm64/cache_line_size.h>
#include <arch/arm64/rk3588_its.h>
#include <cpu.h>
#include <debug.h>
#include <interrupts.h>
#include <smp.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

using namespace Gicv3Its;

static const uint64 kValid = UINT64_C(1) << 63;
static const uint64 kNormalNonCacheable = UINT64_C(1) << 59;
static const uint64 kBaserReadOnly = UINT64_C(0x071f000000000000);
static const size_t kTablePage = 65536;

static uint64
Counter()
{
	// Timer services are not initialized when the GIC constructor runs.
	return READ_SPECIALREG(cntvct_el0);
}


status_t
GICv3Its::Memory::Allocate(const char* name, size_t bytes, size_t alignment,
	phys_addr_t minimumAddress)
{
	virtual_address_restrictions virtualRestrictions{};
	physical_address_restrictions physicalRestrictions{};
	physicalRestrictions.low_address = minimumAddress;
	physicalRestrictions.high_address = UINT64_C(1) << 35;
	physicalRestrictions.alignment = alignment;
	area = create_area_etc(B_SYSTEM_TEAM, name, bytes, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &address);
	if (area < 0)
		return area;
	physical_entry entry;
	status_t status = get_memory_map(address, bytes, &entry, 1);
	if (status != B_OK || entry.size < bytes
		|| !ValidTableRange(entry.address, bytes, alignment, minimumAddress)) {
		delete_area(area);
		area = -1;
		address = nullptr;
		return B_BAD_ADDRESS;
	}
	physical = entry.address;
	size = bytes;
	// The allocator zeroed these private pages through a cached mapping.
	// Remove those lines before retyping, as in the qualified NVMe DMA path.
	size_t line = arm64_data_cache_line_size(READ_SPECIALREG(ctr_el0));
	for (addr_t p = (addr_t)address; p < (addr_t)address + bytes; p += line)
		asm volatile("dc civac, %0" :: "r"(p) : "memory");
	memory_full_barrier();
	status = vm_set_area_memory_type(area, physical, B_WRITE_COMBINING_MEMORY);
	if (status != B_OK) {
		delete_area(area);
		area = -1;
		address = nullptr;
		return status;
	}
	memory_full_barrier();
	dprintf("GICv3 ITS: %s physical=%#" B_PRIxPHYSADDR " bytes=%zu Normal-NC\n",
		name, physical, bytes);
	return B_OK;
}


uint32
GICv3Its::_Read32(const Instance& instance, size_t offset) const
{
	uint32 value = *(volatile uint32*)(instance.base + offset);
	memory_full_barrier();
	return value;
}


uint64
GICv3Its::_Read64(const Instance& instance, size_t offset) const
{
	uint64 value = *(volatile uint64*)(instance.base + offset);
	memory_full_barrier();
	return value;
}


void
GICv3Its::_Write32(Instance& instance, size_t offset, uint32 value)
{
	*(volatile uint32*)(instance.base + offset) = value;
	memory_full_barrier();
}


void
GICv3Its::_Write64(Instance& instance, size_t offset, uint64 value)
{
	*(volatile uint64*)(instance.base + offset) = value;
	memory_full_barrier();
}


bool
GICv3Its::_WriteChecked(Instance& instance, size_t offset, uint64 value)
{
	_Write64(instance, offset, value);
	uint64 readback = _Read64(instance, offset);
	if (readback == value)
		return true;
	dprintf("GICv3 ITS: register %#zx wanted=%#" B_PRIx64 " read=%#" B_PRIx64 "\n",
		offset, value, readback);
	return false;
}


status_t
GICv3Its::_PrepareSharedTables()
{
	status_t status;
	if ((status = fProperties.Allocate("LPI properties", kTablePage, kTablePage,
			fMinimumTableAddress)) != B_OK
		|| (status = fPending.Allocate("CPU0 LPI pending", kTablePage, kTablePage,
			fMinimumTableAddress)) != B_OK) {
		return status;
	}
	memset(fProperties.address, 0xa2, fProperties.size); // priority, RES1, disabled
	memory_full_barrier();
	return B_OK;
}


status_t
GICv3Its::_PrepareInstanceTables(Instance& instance, const char* name)
{
	char devices[40], collections[40], commands[40], interrupts[40];
	snprintf(devices, sizeof(devices), "%s devices", name);
	snprintf(collections, sizeof(collections), "%s collections", name);
	snprintf(commands, sizeof(commands), "%s commands", name);
	snprintf(interrupts, sizeof(interrupts), "%s entries", name);
	status_t status;
	if ((status = instance.devices.Allocate(devices, 8 * 65536, kTablePage,
			fMinimumTableAddress)) != B_OK
		|| (status = instance.collections.Allocate(collections, kTablePage, kTablePage,
			fMinimumTableAddress)) != B_OK
		|| (status = instance.commands.Allocate(commands, kQueueBytes, kTablePage,
			fMinimumTableAddress)) != B_OK
		|| (status = instance.interrupts.Allocate(interrupts, B_PAGE_SIZE, B_PAGE_SIZE,
			fMinimumTableAddress)) != B_OK) {
		return status;
	}
	return B_OK;
}


status_t
GICv3Its::Init(volatile uint8* redistributors, uint32 count, bool trace,
	bool forceHighTables, bool lpiRouting)
{
	fRedistributors = redistributors;
	fTrace = trace;
	fLpiRouting = lpiRouting;
	fMinimumTableAddress = forceHighTables ? UINT64_C(1) << 32 : 0;
	if (count != 8 || smp_get_current_cpu() != 0)
		return B_NOT_SUPPORTED;
	// Never replace a live Redistributor's pending/property tables. CommonLPIAff
	// also requires agreement with any other Redistributor with LPIs enabled.
	for (uint32 i = 0; i < count; i++) {
		uint32 control = *(volatile uint32*)(redistributors + i * 0x20000);
		uint64 typer = *(volatile uint64*)(redistributors + i * 0x20000 + 8);
		memory_full_barrier();
		dprintf("GICv3 ITS: CPU%u RD control=%#x typer=%#" B_PRIx64 "\n", i, control, typer);
		if ((control & 9) != 0 || (typer & 1) == 0)
			return B_BUSY;
		if (i == 0 && typer != 0x21)
			return B_NOT_SUPPORTED;
		// GICR_TYPER: Affinity_Value, Processor_Number (the target, PTA=0)
		fAffinity[i] = (uint32)(typer >> 32);
		fTargets[i] = ((typer >> 8) & 0xffff) << 16;
	}
	status_t status = _PrepareSharedTables();
	if (status != B_OK)
		return status;
	uint64 property = fProperties.physical | 0x8d;
	uint64 pending = fPending.physical | 0x80;
	*(volatile uint64*)(redistributors + 0x70) = property;
	*(volatile uint64*)(redistributors + 0x78) = pending;
	memory_full_barrier();
	if (*(volatile uint64*)(redistributors + 0x70) != property
		|| *(volatile uint64*)(redistributors + 0x78) != pending) {
		dprintf("GICv3 ITS: Redistributor table readback failed\n");
		return B_ERROR;
	}
	*(volatile uint32*)redistributors |= 1;
	memory_full_barrier();
	if ((*(volatile uint32*)redistributors & 1) == 0) {
		dprintf("GICv3 ITS: LPI enable readback failed\n");
		return B_ERROR;
	}
	// PTA=0: use GICR_TYPER.Processor_Number, which is zero for this CPU.
	fTarget = 0;
	fCollectionReady[0] = true;
	for (uint32 i = 1; fLpiRouting && i < count; i++) {
		// the same configuration table, a pending table of its own
		volatile uint8* frame = redistributors + i * 0x20000;
		if (fCpuPending[i].Allocate("LPI pending", kTablePage, kTablePage,
				fMinimumTableAddress) != B_OK) {
			continue;
		}
		uint64 cpuPending = fCpuPending[i].physical | 0x80;
		*(volatile uint64*)(frame + 0x70) = property;
		*(volatile uint64*)(frame + 0x78) = cpuPending;
		memory_full_barrier();
		if (*(volatile uint64*)(frame + 0x70) != property
			|| *(volatile uint64*)(frame + 0x78) != cpuPending) {
			dprintf("GICv3 ITS: CPU%u Redistributor table readback failed\n", i);
			continue;
		}
		*(volatile uint32*)frame |= 1;
		memory_full_barrier();
		fCollectionReady[i] = (*(volatile uint32*)frame & 1) != 0;
	}
	status = _InitInstance(fNvme, Rk3588Its::kController, Rk3588Its::kDevice,
		kFirstLpi, "ITS1/NVMe");
	if (status != B_OK)
		return status;
	status = _InitInstance(fWifi, Rk3588Its::kWifiController, Rk3588Its::kWifiDevice,
		kFirstLpi + kEventCount, "ITS0/AX210");
	if (status != B_OK) {
		dprintf("GICv3 ITS: AX210 controller initialization failed: %" B_PRId32 "\n",
			status);
	}
	return B_OK;
}


status_t
GICv3Its::_InitInstance(Instance& instance, uint64 controller, uint32 device,
	uint32 firstLpi, const char* name)
{
	instance.controller = controller;
	instance.device = device;
	instance.firstLpi = firstLpi;
	area_id area = vm_map_physical_memory(B_SYSTEM_TEAM, name,
		(void**)&instance.base, B_ANY_KERNEL_ADDRESS, 0x10000,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, controller, false);
	if (area < 0)
		return area;
	uint32 control = _Read32(instance, 0), iidr = _Read32(instance, 4);
	uint64 typer = _Read64(instance, 8);
	dprintf("GICv3 ITS: %s control=%#x IIDR=%#x TYPER=%#" B_PRIx64 "\n",
		name, control, iidr, typer);
	if (iidr != Rk3588Its::kIidr || typer != Rk3588Its::kTyper)
		return B_NOT_SUPPORTED;
	if ((control & 0x80000003) != 0x80000000)
		return B_BUSY;
	instance.deviceBaser = _Read64(instance, 0x100) & kBaserReadOnly;
	instance.collectionBaser = _Read64(instance, 0x108) & kBaserReadOnly;
	if (instance.deviceBaser != UINT64_C(0x0107000000000000)
		|| instance.collectionBaser != UINT64_C(0x0401000000000000))
		return B_NOT_SUPPORTED;
	for (unsigned i = 2; i < 8; i++) {
		if ((_Read64(instance, 0x100 + i * 8) & kBaserReadOnly) != 0)
			return B_NOT_SUPPORTED;
	}
	status_t status = _PrepareInstanceTables(instance, name);
	if (status != B_OK)
		return status;
	status = reserve_io_interrupt_vectors(kEventCount, firstLpi, INTERRUPT_TYPE_IRQ);
	if (status != B_OK)
		return status;
	if (!_WriteChecked(instance, 0x100, instance.deviceBaser | kValid
			| kNormalNonCacheable | instance.devices.physical | 0x207)
		|| !_WriteChecked(instance, 0x108, instance.collectionBaser | kValid
			| kNormalNonCacheable | instance.collections.physical | 0x200)
		|| !_WriteChecked(instance, 0x80, kValid | kNormalNonCacheable
			| instance.commands.physical | 15)) {
		_Quarantine(instance, "table descriptor readback failed");
		return B_ERROR;
	}
	_Write64(instance, 0x88, 0);
	if (_Read64(instance, 0x90) != 0) {
		_Quarantine(instance, "command reader did not reset");
		return B_ERROR;
	}
	_Write32(instance, 0, 1);
	if ((_Read32(instance, 0) & 1) == 0
		|| !_Submit(instance, MapCollection(0, fTarget, true))
		|| !_Submit(instance, InvalidateAll(0))) {
		_Quarantine(instance, "collection initialization failed");
		return B_ERROR;
	}
	for (uint16 i = 1; i < kRedistributors; i++) {
		if (!fCollectionReady[i])
			continue;
		if (!_Submit(instance, MapCollection(i, fTargets[i], true), i)
			|| !_Submit(instance, InvalidateAll(i), i)) {
			_Quarantine(instance, "CPU collection initialization failed");
			return B_ERROR;
		}
	}
	instance.ready = true;
	dprintf("GICv3 ITS: enabled %s, CPU 0, LPI %u..%u, DeviceID %#x\n",
		name, firstLpi, firstLpi + kEventCount - 1, device);
	return B_OK;
}


void
GICv3Its::_Quarantine(Instance& instance, const char* reason)
{
	instance.faulted = true;
	// Hardware may still hold these physical addresses. Keep all allocations
	// and IDs until reboot, and refuse subsequent commands or leases.
	dprintf("GICv3 ITS: controller %#" B_PRIx64 " quarantined until reboot: %s\n",
		instance.controller, reason);
}


bool
GICv3Its::_WaitReadOffset(Instance& instance, uint32 offset)
{
	uint64 start = Counter();
	uint64 ticks = READ_SPECIALREG(cntfrq_el0) / 10; // at most 100 ms
	for (unsigned attempt = 0; attempt < 1000000; attempt++) {
		uint64 reader = _Read64(instance, 0x90);
		if (reader == offset)
			return true;
		if ((reader & 1) != 0 || Counter() - start >= ticks) {
			dprintf("GICv3 ITS: command timeout/stall reader=%#" B_PRIx64 " wanted=%#x\n",
				reader, offset);
			break;
		}
		asm volatile("yield" ::: "memory");
	}
	_Quarantine(instance, "command queue did not drain");
	return false;
}


bool
GICv3Its::_Submit(Instance& instance, const Command& command, uint16 collection)
{
	// The caller serializes the entire submission/completion under the instance lock, or
	// runs during single-CPU initialization. Two entries can never fill 64 KiB.
	if (instance.faulted || !_WaitReadOffset(instance, instance.writeOffset))
		return false;
	// the SYNC goes to the Redistributor the command affects
	const Command commands[] = {command, Sync(fTargets[collection])};
	for (const Command& current : commands) {
		volatile uint64* entry = (volatile uint64*)((uint8*)instance.commands.address
			+ instance.writeOffset);
		for (unsigned word = 0; word < 4; word++)
			entry[word] = current.words[word];
		instance.writeOffset = NextCommand(instance.writeOffset);
	}
	memory_full_barrier();
	_Write64(instance, 0x88, instance.writeOffset);
	return _WaitReadOffset(instance, instance.writeOffset);
}


GICv3Its::Instance*
GICv3Its::_InstanceForRequester(const msi_requester* requester)
{
	if (requester == nullptr)
		return nullptr;
	Instance* instances[] = {&fNvme, &fWifi};
	for (Instance* instance : instances) {
		if (requester->controller_address == instance->controller
			&& requester->device_id == instance->device)
			return instance;
	}
	return nullptr;
}


GICv3Its::Instance*
GICv3Its::_InstanceForVector(uint32 vector)
{
	Instance* instances[] = {&fNvme, &fWifi};
	for (Instance* instance : instances) {
		if (vector >= instance->firstLpi
			&& vector < instance->firstLpi + kEventCount)
			return instance;
	}
	return nullptr;
}


bool
GICv3Its::Contains(uint32 vector) const
{
	return (fNvme.ready && vector >= fNvme.firstLpi
			&& vector < fNvme.firstLpi + kEventCount)
		|| (fWifi.ready && vector >= fWifi.firstLpi
			&& vector < fWifi.firstLpi + kEventCount);
}


status_t
GICv3Its::AllocateVectorsForDevice(const msi_requester* requester, uint32 count,
	uint32& startVector, uint64& address, uint32& data)
{
	Instance* instance = _InstanceForRequester(requester);
	if (instance == nullptr || !Rk3588Its::RequesterMatches(
			requester->controller_address, requester->device_id))
		return B_NOT_SUPPORTED;
	if (!ValidCount(count))
		return B_BAD_VALUE;
	// The trace reporter needs the kernel daemons, which do not run yet when
	// the interrupt controller is set up; drivers allocate vectors later.
	if (fTrace && atomic_get_and_set(&fTraceReporter, 1) == 0)
		register_kernel_daemon(&_ReportTrace, this, 10);
	InterruptsSpinLocker locker(instance->lock);
	if (!instance->ready || instance->faulted)
		return B_NO_INIT;
	if (instance->count != 0)
		return B_BUSY;
	// Each device mapping is invalid between leases. Never zero a table
	// while a valid MAPD may still let hardware access it.
	memset(instance->interrupts.address, 0, instance->interrupts.size);
	memory_full_barrier();
	if (!_Submit(*instance, MapDevice(instance->device,
			instance->interrupts.physical, 5, true)))
		return B_ERROR;
	for (uint32 event = 0; event < count; event++) {
		if (!_Submit(*instance, MapInterrupt(instance->device, event,
				instance->firstLpi + event, 0)))
			return B_ERROR;
		instance->collection[event] = 0;
	}
	instance->count = count;
	instance->enabledMask = 0;
	startVector = instance->firstLpi;
	address = instance->controller + 0x10040;
	data = 0; // event IDs are per-device, so the MSI base is naturally aligned
	dprintf("GICv3 ITS: allocated DeviceID=%#x count=%u vector=%u address=%#"
		B_PRIx64 " data=0\n", instance->device, count, startVector, address);
	return B_OK;
}


void
GICv3Its::SetEnabled(uint32 vector, bool enabled)
{
	Instance* instance = _InstanceForVector(vector);
	if (instance == nullptr)
		return;
	InterruptsSpinLocker locker(instance->lock);
	uint32 event = vector - instance->firstLpi;
	if (!instance->ready || instance->faulted || event >= instance->count)
		return;
	uint32 mask = uint32(1) << event;
	volatile uint8* properties = (volatile uint8*)fProperties.address;
	properties[vector - kFirstLpi] = enabled ? 0xa3 : 0xa2;
	memory_full_barrier();
	// A CPU cache flush alone cannot invalidate the GIC's property cache.
	if (!_Submit(*instance, EventCommand(0x0c, instance->device, event),
			instance->collection[event])) {
		return;
	}
	if (enabled)
		instance->enabledMask |= mask;
	else
		instance->enabledMask &= ~mask;
	if (fTrace)
		dprintf("GICv3 ITS: vector=%u %s\n", vector, enabled ? "enabled" : "disabled");
}


void
GICv3Its::FreeVectors(uint32 count, uint32 startVector)
{
	Instance* instance = _InstanceForVector(startVector);
	if (instance == nullptr)
		return;
	InterruptsSpinLocker locker(instance->lock);
	if (!instance->ready || instance->faulted || !ValidCount(count)
		|| startVector != instance->firstLpi || instance->count != count
		|| instance->enabledMask != 0) {
		dprintf("GICv3 ITS: retaining invalid/live lease count=%u vector=%u\n",
			count, startVector);
		return;
	}
	// The caller must disable the PCI message source and synchronize removal
	// of all handlers first. CLEAR then DISCARD, each followed by SYNC, removes
	// pending state and the event translations before the device is unmapped.
	for (uint32 event = 0; event < count; event++) {
		uint16 collection = instance->collection[event];
		if (!_Submit(*instance, EventCommand(0x04, instance->device, event),
				collection)
			|| !_Submit(*instance, EventCommand(0x0f, instance->device, event),
				collection)) {
			return;
		}
		instance->collection[event] = 0;
	}
	if (!_Submit(*instance, MapDevice(instance->device,
			instance->interrupts.physical, 5, false)))
		return;
	instance->count = 0;
	dprintf("GICv3 ITS: freed count=%u vector=%u\n", count, startVector);
}


/*!	Counts the interrupts of a vector when tracing is on. Nothing is printed
	here: debug output from this interrupt handler held up the boot on the
	ROCK 5 ITX for seconds at a time and sometimes for good (the NVMe probe
	never went on). The counts are reported from a kernel daemon instead.
*/
void
GICv3Its::TraceInterrupt(uint32 vector)
{
	if (!fTrace)
		return;
	Instance* instance = _InstanceForVector(vector);
	if (instance == nullptr || !instance->ready)
		return;
	atomic_add(&instance->interruptCounts[vector - instance->firstLpi], 1);
}


/*static*/ void
GICv3Its::_ReportTrace(void* data, int /*iteration*/)
{
	GICv3Its* self = (GICv3Its*)data;
	Instance* instances[] = { &self->fNvme, &self->fWifi };
	for (Instance* instance : instances) {
		if (!instance->ready)
			continue;
		for (uint32 event = 0; event < Gicv3Its::kEventCount; event++) {
			uint32 count = (uint32)atomic_get(&instance->interruptCounts[event]);
			uint32 reported = instance->reportedCounts[event];
			if (count == reported)
				continue;
			// the first eight, then every power of two that was passed
			uint32 next = reported < 8 ? reported + 1 : reported * 2;
			if (count < next)
				continue;
			instance->reportedCounts[event] = count;
			dprintf("GICv3 ITS: received vector=%" B_PRIu32 " count=%" B_PRIu32
				"\n", instance->firstLpi + event, count);
		}
	}
}


/*!	The collection of \a cpu's Redistributor, -1 when it has none. */
int32
GICv3Its::_CollectionForCpu(int32 cpu) const
{
	if (cpu < 0 || cpu >= smp_get_num_cpus())
		return -1;
	uint64 mpidr = gCPU[cpu].arch.mpidr;
	if (cpu != 0 && (mpidr & CPU_AFF_MASK) == 0)
		return -1;
	uint32 affinity = (uint32)(((mpidr >> 32) & 0xff) << 24) | (uint32)(mpidr & 0xffffff);
	for (uint32 i = 0; i < kRedistributors; i++) {
		if (fAffinity[i] == affinity)
			return fCollectionReady[i] ? (int32)i : -1;
	}
	return -1;
}


int32
GICv3Its::_CpuForCollection(uint16 collection) const
{
	for (int32 cpu = 0; cpu < smp_get_num_cpus(); cpu++) {
		uint64 mpidr = gCPU[cpu].arch.mpidr;
		uint32 affinity = (uint32)(((mpidr >> 32) & 0xff) << 24)
			| (uint32)(mpidr & 0xffffff);
		if (affinity == fAffinity[collection])
			return cpu;
	}
	return 0;
}


/*!	Moves an LPI to \a cpu's collection (MOVI, then a SYNC to the new
	Redistributor), with "lpi_routing true" only: otherwise all stay on
	CPU 0. Runs with interrupts disabled.
*/
int32
GICv3Its::AssignToCpu(uint32 vector, int32 cpu)
{
	Instance* instance = _InstanceForVector(vector);
	if (!fLpiRouting || instance == nullptr)
		return 0;
	InterruptsSpinLocker locker(instance->lock);
	uint32 event = vector - instance->firstLpi;
	if (!instance->ready || instance->faulted || event >= instance->count)
		return _CpuForCollection(instance->collection[event]);
	uint16 current = instance->collection[event];
	int32 collection = _CollectionForCpu(cpu);
	if (collection < 0)
		return _CpuForCollection(current);
	if (collection == current)
		return cpu;
	if (!_Submit(*instance, MoveInterrupt(instance->device, event, collection),
			collection)) {
		return _CpuForCollection(current);
	}
	instance->collection[event] = collection;
	return cpu;
}


void
GICv3Its::DumpRouting()
{
	Instance* instances[] = { &fNvme, &fWifi };
	for (Instance* instance : instances) {
		if (!instance->ready)
			continue;
		for (uint32 event = 0; event < instance->count; event++) {
			kprintf("LPI %5" B_PRIu32 ": ITS %#" B_PRIx64 " DeviceID %#" B_PRIx32
				" event %2" B_PRIu32 " -> collection %u (cpu%" B_PRId32 ")%s\n",
				instance->firstLpi + event, instance->controller, instance->device,
				event, instance->collection[event],
				_CpuForCollection(instance->collection[event]),
				(instance->enabledMask & (1u << event)) != 0 ? "" : ", disabled");
		}
	}
}
