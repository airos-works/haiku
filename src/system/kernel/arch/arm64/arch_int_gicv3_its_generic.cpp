/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "arch_int_gicv3_its_generic.h"

#include <arch/arm64/arch_cpu.h>
#include <arch/arm64/cache_line_size.h>
#include <debug.h>
#include <interrupts.h>
#include <smp.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

using namespace Gicv3Its;


static const uint64 kValid = UINT64_C(1) << 63;
static const uint64 kInnerNonCacheable = UINT64_C(1) << 59;
static const uint64 kBaserTypeAndEntrySize = UINT64_C(0x071f000000000000);
static const size_t kTablePage = 65536;
static const uint32 kEventBits = 5;		// kEventCount events per device
static const uint32 kMaxDeviceBits = 20;

// GITS registers
static const size_t kControl = 0x0;
static const size_t kType = 0x8;
static const size_t kCommandBase = 0x80;
static const size_t kCommandWrite = 0x88;
static const size_t kCommandRead = 0x90;
static const size_t kTableBase = 0x100;
static const size_t kTranslater = 0x10040;

// GICR registers (RD_base frame)
static const size_t kRedistributorControl = 0x0;
static const size_t kRedistributorType = 0x8;
static const size_t kPropertiesBase = 0x70;
static const size_t kPendingBase = 0x78;


static uint64
Counter()
{
	return READ_SPECIALREG(cntvct_el0);
}


status_t
GICv3ItsGeneric::Memory::Allocate(const char* name, size_t bytes, size_t alignment)
{
	virtual_address_restrictions virtualRestrictions{};
	physical_address_restrictions physicalRestrictions{};
	physicalRestrictions.alignment = alignment;
	area = create_area_etc(B_SYSTEM_TEAM, name, bytes, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &address);
	if (area < 0)
		return area;
	physical_entry entry;
	status_t status = get_memory_map(address, bytes, &entry, 1);
	if (status != B_OK || entry.size < bytes || entry.address % alignment != 0) {
		delete_area(area);
		area = -1;
		address = nullptr;
		return B_BAD_ADDRESS;
	}
	physical = entry.address;
	size = bytes;
	// The tables are shared with the GIC as Normal non-cacheable memory: drop
	// the lines the allocator left from zeroing them before retyping.
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
	memset(address, 0, bytes);
	memory_full_barrier();
	return B_OK;
}


uint64
GICv3ItsGeneric::_Read64(const Controller& controller, size_t offset) const
{
	uint64 value = *(volatile uint64*)(controller.base + offset);
	memory_full_barrier();
	return value;
}


void
GICv3ItsGeneric::_Write64(Controller& controller, size_t offset, uint64 value)
{
	*(volatile uint64*)(controller.base + offset) = value;
	memory_full_barrier();
}


status_t
GICv3ItsGeneric::Init(volatile uint8* redistributors,
	phys_addr_t redistributorsPhysical, size_t redistributorStride,
	uint32 redistributorCount, const phys_addr_t* controllers,
	uint32 controllerCount)
{
	if (smp_get_current_cpu() != 0 || controllerCount == 0
		|| controllerCount > kMaxControllers) {
		return B_NOT_SUPPORTED;
	}

	// Find this CPU's Redistributor by its affinity.
	uint64 mpidr = READ_SPECIALREG(MPIDR_EL1);
	uint32 affinity = (uint32)(((mpidr >> 32) & 0xff) << 24) | (uint32)(mpidr & 0xffffff);
	volatile uint8* redistributor = nullptr;
	for (uint32 i = 0; i < redistributorCount; i++) {
		volatile uint8* frame = redistributors + i * redistributorStride;
		uint64 type = *(volatile uint64*)(frame + kRedistributorType);
		if ((uint32)(type >> 32) == affinity) {
			redistributor = frame;
			break;
		}
		if ((type & (1 << 4)) != 0)
			break;
	}
	if (redistributor == nullptr) {
		dprintf("GICv3 ITS: no Redistributor for affinity %#" B_PRIx32 "\n", affinity);
		return B_ERROR;
	}
	uint64 redistributorType = *(volatile uint64*)(redistributor + kRedistributorType);
	uint32 redistributorControl
		= *(volatile uint32*)(redistributor + kRedistributorControl);
	dprintf("GICv3 ITS: CPU 0 Redistributor control %#" B_PRIx32 " type %#" B_PRIx64
		"\n", redistributorControl, redistributorType);
	if ((redistributorType & 1) == 0)
		return B_NOT_SUPPORTED;
	if ((redistributorControl & 1) != 0) {
		// The tables of live LPIs must not be replaced.
		dprintf("GICv3 ITS: the firmware left LPIs enabled\n");
		return B_BUSY;
	}

	// LPI configuration (8 KiB for LPIs 8192..16383) and pending tables.
	status_t status;
	if ((status = fProperties.Allocate("LPI properties", kTablePage, kTablePage))
			!= B_OK
		|| (status = fPending.Allocate("CPU 0 LPI pending", kTablePage, kTablePage))
			!= B_OK) {
		return status;
	}
	memset(fProperties.address, 0xa2, fProperties.size); // priority, RES1, off
	memory_full_barrier();
	uint64 properties = fProperties.physical | 0x80 | 13;	// non-cacheable, 14 bits
	uint64 pending = fPending.physical | 0x80;
	*(volatile uint64*)(redistributor + kPropertiesBase) = properties;
	*(volatile uint64*)(redistributor + kPendingBase) = pending;
	memory_full_barrier();
	*(volatile uint32*)(redistributor + kRedistributorControl) |= 1;
	memory_full_barrier();
	if ((*(volatile uint32*)(redistributor + kRedistributorControl) & 1) == 0) {
		dprintf("GICv3 ITS: LPIs do not enable\n");
		return B_ERROR;
	}

	// The collection targets CPU 0: by its Redistributor's processor number,
	// or, for an ITS with physical target addresses, the Redistributor's
	// address.
	uint64 processorTarget = ((redistributorType >> 8) & 0xffff) << 16;
	uint64 addressTarget = redistributorsPhysical + (redistributor - redistributors);

	for (uint32 i = 0; i < controllerCount; i++) {
		Controller& controller = fControllers[fControllerCount];
		status = _InitController(controller, controllers[i]);
		if (status != B_OK) {
			dprintf("GICv3 ITS: controller %#" B_PRIxPHYSADDR " failed: %" B_PRId32
				"\n", controllers[i], status);
			continue;
		}
		controller.target = (_Read64(controller, kType) & (1 << 19)) != 0
			? addressTarget : processorTarget;
		InterruptsSpinLocker locker(controller.lock);
		if (!_Submit(controller, MapCollection(0, controller.target, true))
			|| !_Submit(controller, InvalidateAll(0))) {
			dprintf("GICv3 ITS: controller %#" B_PRIxPHYSADDR
				" did not take its collection\n", controllers[i]);
			continue;
		}
		controller.ready = true;
		fControllerCount++;
		dprintf("GICv3 ITS: controller %#" B_PRIxPHYSADDR " ready, %" B_PRIu32
			" DeviceID bits, ITT entries of %" B_PRIu32 " bytes\n", controllers[i],
			controller.deviceBits, controller.ittEntrySize);
	}
	if (fControllerCount == 0)
		return B_ERROR;

	return reserve_io_interrupt_vectors(kMaxLeases * kEventCount, kFirstLpi,
		INTERRUPT_TYPE_IRQ);
}


status_t
GICv3ItsGeneric::_InitController(Controller& controller, phys_addr_t physical)
{
	controller.physical = physical;
	area_id area = vm_map_physical_memory(B_SYSTEM_TEAM, "GICv3 ITS",
		(void**)&controller.base, B_ANY_KERNEL_ADDRESS, 0x10000,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, physical, false);
	if (area < 0)
		return area;

	uint32 control = *(volatile uint32*)(controller.base + kControl);
	uint64 type = _Read64(controller, kType);
	dprintf("GICv3 ITS: controller %#" B_PRIxPHYSADDR " control %#" B_PRIx32
		" type %#" B_PRIx64 "\n", physical, control, type);
	if ((control & 1) != 0) {
		*(volatile uint32*)(controller.base + kControl) = 0;
		memory_full_barrier();
		uint64 start = Counter(), ticks = READ_SPECIALREG(cntfrq_el0) / 10;
		while ((*(volatile uint32*)(controller.base + kControl) & (1u << 31)) == 0) {
			if (Counter() - start > ticks)
				return B_BUSY;
		}
	}
	controller.ittEntrySize = ((type >> 4) & 0xf) + 1;
	controller.deviceBits = ((type >> 13) & 0x1f) + 1;
	if (controller.deviceBits > kMaxDeviceBits)
		controller.deviceBits = kMaxDeviceBits;

	for (uint32 i = 0; i < 8; i++) {
		size_t offset = kTableBase + i * 8;
		uint64 table = _Read64(controller, offset);
		uint32 tableType = (table >> 56) & 7;
		size_t entrySize = ((table >> 48) & 0x1f) + 1;
		Memory* memory = nullptr;
		size_t bytes = 0;
		if (tableType == 1) {
			memory = &controller.devices;
			bytes = entrySize << controller.deviceBits;
		} else if (tableType == 4) {
			memory = &controller.collections;
			bytes = kTablePage;
		} else {
			_Write64(controller, offset, 0);
			continue;
		}
		bytes = ROUNDUP(bytes, kTablePage);
		if (bytes / kTablePage > 256)
			return B_NOT_SUPPORTED;
		status_t status = memory->Allocate(tableType == 1 ? "GICv3 ITS devices"
			: "GICv3 ITS collections", bytes, kTablePage);
		if (status != B_OK)
			return status;
		uint64 value = kValid | kInnerNonCacheable | (table & kBaserTypeAndEntrySize)
			| memory->physical | (2 << 8) | (bytes / kTablePage - 1);
		_Write64(controller, offset, value);
		uint64 readback = _Read64(controller, offset);
		if ((readback & ~UINT64_C(0x0000000000000c00)) != (value & ~UINT64_C(0x0000000000000c00))) {
			// shareability may be read-only; anything else is not ours
			dprintf("GICv3 ITS: table %" B_PRIu32 " wanted %#" B_PRIx64 " read %#"
				B_PRIx64 "\n", i, value, readback);
			return B_NOT_SUPPORTED;
		}
	}

	status_t status = controller.commands.Allocate("GICv3 ITS commands", kQueueBytes,
		kTablePage);
	if (status != B_OK)
		return status;
	_Write64(controller, kCommandBase, kValid | kInnerNonCacheable
		| controller.commands.physical | (kQueueBytes / 4096 - 1));
	_Write64(controller, kCommandWrite, 0);
	if (_Read64(controller, kCommandRead) != 0)
		return B_ERROR;
	*(volatile uint32*)(controller.base + kControl) = 1;
	memory_full_barrier();
	if ((*(volatile uint32*)(controller.base + kControl) & 1) == 0)
		return B_ERROR;
	return B_OK;
}


bool
GICv3ItsGeneric::_WaitReadOffset(Controller& controller, uint32 offset)
{
	uint64 start = Counter();
	uint64 ticks = READ_SPECIALREG(cntfrq_el0) / 10; // at most 100 ms
	while (true) {
		uint64 reader = _Read64(controller, kCommandRead);
		if (reader == offset)
			return true;
		if ((reader & 1) != 0 || Counter() - start >= ticks) {
			dprintf("GICv3 ITS: command queue stalled, reader %#" B_PRIx64
				" wanted %#" B_PRIx32 "\n", reader, offset);
			controller.ready = false;
			return false;
		}
		asm volatile("yield" ::: "memory");
	}
}


bool
GICv3ItsGeneric::_Submit(Controller& controller, const Command& command)
{
	// Callers hold the controller's lock. Two entries never fill the queue.
	if (!_WaitReadOffset(controller, controller.writeOffset))
		return false;
	const Command commands[] = {command, Sync(controller.target)};
	for (const Command& current : commands) {
		volatile uint64* entry = (volatile uint64*)((uint8*)controller.commands.address
			+ controller.writeOffset);
		for (unsigned word = 0; word < 4; word++)
			entry[word] = current.words[word];
		controller.writeOffset = NextCommand(controller.writeOffset);
	}
	memory_full_barrier();
	_Write64(controller, kCommandWrite, controller.writeOffset);
	return _WaitReadOffset(controller, controller.writeOffset);
}


uint32
GICv3ItsGeneric::_FirstVector(const Lease* lease) const
{
	return kFirstLpi + (lease - fLeases) * kEventCount;
}


GICv3ItsGeneric::Lease*
GICv3ItsGeneric::_LeaseForVector(uint32 vector)
{
	if (vector < kFirstLpi || vector >= kFirstLpi + kMaxLeases * kEventCount)
		return nullptr;
	Lease* lease = &fLeases[(vector - kFirstLpi) / kEventCount];
	return lease->used ? lease : nullptr;
}


bool
GICv3ItsGeneric::Contains(uint32 vector) const
{
	return vector >= kFirstLpi && vector < kFirstLpi + kMaxLeases * kEventCount
		&& fLeases[(vector - kFirstLpi) / kEventCount].used;
}


status_t
GICv3ItsGeneric::AllocateVectorsForDevice(const msi_requester* requester,
	uint32 count, uint32& startVector, uint64& address, uint32& data)
{
	if (requester == nullptr)
		return B_NOT_SUPPORTED;
	if (!ValidCount(count))
		return B_BAD_VALUE;
	Controller* controller = nullptr;
	for (uint32 i = 0; i < fControllerCount; i++) {
		if (fControllers[i].physical == requester->controller_address)
			controller = &fControllers[i];
	}
	if (controller == nullptr || !controller->ready) {
		dprintf("GICv3 ITS: no ITS at %#" B_PRIx64 " for DeviceID %#" B_PRIx32 "\n",
			requester->controller_address, requester->device_id);
		return B_NOT_SUPPORTED;
	}
	if (requester->device_id >= (uint32(1) << controller->deviceBits))
		return B_BAD_VALUE;

	MutexLocker leaseLocker(fLeaseLock);
	Lease* lease = nullptr;
	for (uint32 i = 0; i < kMaxLeases; i++) {
		if (fLeases[i].used && fLeases[i].controller == controller
			&& fLeases[i].device == requester->device_id) {
			return B_BUSY;
		}
		if (!fLeases[i].used && lease == nullptr)
			lease = &fLeases[i];
	}
	if (lease == nullptr)
		return B_NO_MEMORY;
	if (lease->table.area < 0) {
		status_t status = lease->table.Allocate("GICv3 ITS translation table",
			B_PAGE_SIZE, B_PAGE_SIZE);
		if (status != B_OK)
			return status;
	}

	InterruptsSpinLocker locker(controller->lock);
	memset(lease->table.address, 0, lease->table.size);
	memory_full_barrier();
	uint32 first = _FirstVector(lease);
	if (!_Submit(*controller, MapDevice(requester->device_id,
			lease->table.physical, kEventBits, true))) {
		return B_ERROR;
	}
	for (uint32 event = 0; event < count; event++) {
		if (!_Submit(*controller, MapInterrupt(requester->device_id, event,
				first + event, 0))) {
			return B_ERROR;
		}
	}
	lease->controller = controller;
	lease->device = requester->device_id;
	lease->count = count;
	lease->used = true;

	startVector = first;
	address = controller->physical + kTranslater;
	data = 0;
	dprintf("GICv3 ITS: DeviceID %#" B_PRIx32 ": %" B_PRIu32 " vectors from %"
		B_PRIu32 "\n", requester->device_id, count, first);
	return B_OK;
}


void
GICv3ItsGeneric::FreeVectors(uint32 count, uint32 startVector)
{
	MutexLocker leaseLocker(fLeaseLock);
	Lease* lease = _LeaseForVector(startVector);
	if (lease == nullptr || startVector != _FirstVector(lease) || count != lease->count)
		return;
	Controller& controller = *lease->controller;
	InterruptsSpinLocker locker(controller.lock);
	for (uint32 event = 0; event < count; event++) {
		if (!_Submit(controller, EventCommand(0x0f, lease->device, event)))
			return;
	}
	if (!_Submit(controller, MapDevice(lease->device, lease->table.physical,
			kEventBits, false))) {
		return;
	}
	lease->used = false;
	lease->count = 0;
}


void
GICv3ItsGeneric::SetEnabled(uint32 vector, bool enabled)
{
	Lease* lease = _LeaseForVector(vector);
	if (lease == nullptr)
		return;
	uint32 event = vector - _FirstVector(lease);
	if (event >= lease->count)
		return;
	InterruptsSpinLocker locker(lease->controller->lock);
	((volatile uint8*)fProperties.address)[vector - kFirstLpi] = enabled ? 0xa3 : 0xa2;
	memory_full_barrier();
	// The Redistributor caches the configuration: invalidate it.
	_Submit(*lease->controller, EventCommand(0x0c, lease->device, event));
}
