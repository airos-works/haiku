/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARCH_ARM64_GICV3_ITS_GENERIC_H
#define ARCH_ARM64_GICV3_ITS_GENERIC_H

#include "arch_int_gicv3_its.h"


// A GICv3 ITS set up from scratch, for firmware that leaves LPIs off: the
// Redistributor of CPU 0 gets the LPI tables, every ITS the device tree
// lists gets its device, collection and command tables, and each requester
// (PCI function) that asks for MSIs is mapped on demand. All LPIs target
// CPU 0. Tables and leases live for the kernel lifetime.
class GICv3ItsGeneric : public GICv3LpiProvider {
public:
	static const uint32 kMaxControllers = 4;

	status_t Init(volatile uint8* redistributors, phys_addr_t redistributorsPhysical,
		size_t redistributorStride, uint32 redistributorCount,
		const phys_addr_t* controllers,
		uint32 controllerCount);

	status_t AllocateVectors(uint32, uint32&, uint64&, uint32&) override
		{ return B_NOT_SUPPORTED; }
	status_t AllocateVectorsForDevice(const msi_requester* requester, uint32 count,
		uint32& startVector, uint64& address, uint32& data) override;
	void FreeVectors(uint32 count, uint32 startVector) override;
	bool Contains(uint32 vector) const override;
	void SetEnabled(uint32 vector, bool enabled) override;

private:
	struct Memory {
		area_id area = -1;
		void* address = nullptr;
		phys_addr_t physical = 0;
		size_t size = 0;
		status_t Allocate(const char* name, size_t bytes, size_t alignment);
	};
	struct Controller {
		phys_addr_t physical = 0;
		volatile uint8* base = nullptr;
		Memory devices;
		Memory collections;
		Memory commands;
		uint32 deviceBits = 0;
		uint32 ittEntrySize = 0;
		uint32 writeOffset = 0;
		uint64 target = 0;
		bool ready = false;
		spinlock lock = B_SPINLOCK_INITIALIZER;
	};
	struct Lease {
		Controller* controller = nullptr;
		uint32 device = 0;
		Memory table;
		uint32 count = 0;
		bool used = false;
	};
	static const uint32 kMaxLeases = 64;

	status_t _InitController(Controller& controller, phys_addr_t physical);
	bool _Submit(Controller& controller, const Gicv3Its::Command& command);
	bool _WaitReadOffset(Controller& controller, uint32 offset);
	uint64 _Read64(const Controller& controller, size_t offset) const;
	void _Write64(Controller& controller, size_t offset, uint64 value);
	Lease* _LeaseForVector(uint32 vector);
	uint32 _FirstVector(const Lease* lease) const;

	Memory fProperties;
	Memory fPending;
	Controller fControllers[kMaxControllers];
	uint32 fControllerCount = 0;
	Lease fLeases[kMaxLeases];
	mutex fLeaseLock = MUTEX_INITIALIZER("GICv3 ITS leases");
};

#endif
