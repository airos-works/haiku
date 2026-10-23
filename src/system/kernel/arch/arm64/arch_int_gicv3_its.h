/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARCH_ARM64_GICV3_ITS_H
#define ARCH_ARM64_GICV3_ITS_H

#include <arch/arm64/gicv3_its_commands.h>
#include <arch/generic/msi.h>
#include <lock.h>

// What the GICv3 driver needs from an LPI provider besides MSI allocation.
class GICv3LpiProvider : public MSIInterface {
public:
	virtual bool Contains(uint32 vector) const = 0;
	virtual void SetEnabled(uint32 vector, bool enabled) = 0;
	virtual void TraceInterrupt(uint32 vector) {}
};


// RK3588 firmware profile: ITS1/NVMe and ITS0/AX210, targeting CPU 0.
// Tables and reservations live for the kernel lifetime, including on failure.
class GICv3Its : public GICv3LpiProvider {
public:
	status_t Init(volatile uint8* redistributors, uint32 count, bool trace,
		bool forceHighTables);
	status_t AllocateVectors(uint32, uint32&, uint64&, uint32&) override
		{ return B_NOT_SUPPORTED; }
	status_t AllocateVectorsForDevice(const msi_requester* requester, uint32 count,
		uint32& startVector, uint64& address, uint32& data) override;
	void FreeVectors(uint32 count, uint32 startVector) override;
	void SetEnabled(uint32 vector, bool enabled) override;
	void TraceInterrupt(uint32 vector) override;
	bool Contains(uint32 vector) const override;

private:
	struct Memory {
		area_id area = -1;
		void* address = nullptr;
		phys_addr_t physical = 0;
		size_t size = 0;
		status_t Allocate(const char* name, size_t bytes, size_t alignment,
			phys_addr_t minimumAddress);
	};
	struct Instance {
		volatile uint8* base = nullptr;
		Memory devices;
		Memory collections;
		Memory commands;
		Memory interrupts;
		uint64 deviceBaser = 0;
		uint64 collectionBaser = 0;
		uint64 controller = 0;
		uint32 device = 0;
		uint32 firstLpi = 0;
		uint32 writeOffset = 0;
		uint32 count = 0;
		uint32 enabledMask = 0;
		int32 interruptCounts[Gicv3Its::kEventCount]{};
		bool ready = false;
		bool faulted = false;
		spinlock lock = B_SPINLOCK_INITIALIZER;
	};
	status_t _PrepareSharedTables();
	status_t _InitInstance(Instance& instance, uint64 controller, uint32 device,
		uint32 firstLpi, const char* name);
	status_t _PrepareInstanceTables(Instance& instance, const char* name);
	bool _WriteChecked(Instance& instance, size_t offset, uint64 value);
	bool _Submit(Instance& instance, const Gicv3Its::Command& command);
	bool _WaitReadOffset(Instance& instance, uint32 offset);
	uint32 _Read32(const Instance& instance, size_t offset) const;
	uint64 _Read64(const Instance& instance, size_t offset) const;
	void _Write32(Instance& instance, size_t offset, uint32 value);
	void _Write64(Instance& instance, size_t offset, uint64 value);
	void _Quarantine(Instance& instance, const char* reason);
	Instance* _InstanceForRequester(const msi_requester* requester);
	Instance* _InstanceForVector(uint32 vector);

	volatile uint8* fRedistributors = nullptr;
	Memory fProperties;
	Memory fPending;
	Instance fNvme;
	Instance fWifi;
	uint64 fTarget = 0;
	phys_addr_t fMinimumTableAddress = 0;
	bool fTrace = false;
};

#endif
