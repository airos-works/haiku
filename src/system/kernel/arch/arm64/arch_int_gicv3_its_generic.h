/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARCH_ARM64_GICV3_ITS_GENERIC_H
#define ARCH_ARM64_GICV3_ITS_GENERIC_H

#include "arch_int_gicv3_its.h"

#include <smp.h>


// A GICv3 ITS set up from scratch, for firmware that leaves LPIs off: every
// Redistributor gets the LPI tables and a collection of its own (the
// collection ID is the Redistributor's index), every ITS the device tree
// lists gets its device, collection and command tables, and each requester
// (PCI function) that asks for MSIs is mapped on demand. New LPIs target
// the boot CPU; AssignToCpu() moves each to its CPU's collection (MOVI).
// Tables and leases live for the kernel lifetime.
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
	int32 AssignToCpu(uint32 vector, int32 cpu) override;
	void DumpRouting() override;

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
		bool physicalTargets = false;
		bool ready = false;
		spinlock lock = B_SPINLOCK_INITIALIZER;
	};
	struct Lease {
		Controller* controller = nullptr;
		uint32 device = 0;
		Memory table;
		uint32 count = 0;
		bool used = false;
		uint16 collection[Gicv3Its::kEventCount] = {};
	};
	struct Redistributor {
		uint32 affinity = 0;
		uint64 processorTarget = 0;	// for an ITS with GITS_TYPER.PTA 0
		uint64 addressTarget = 0;	// and 1
		Memory pending;
		bool ready = false;
	};
	static const uint32 kMaxLeases = 64;
	static const uint32 kMaxRedistributors = SMP_MAX_CPUS;

	status_t _InitController(Controller& controller, phys_addr_t physical);
	bool _Submit(Controller& controller, const Gicv3Its::Command& command,
		uint16 collection);
	uint64 _Target(const Controller& controller, uint16 collection) const;
	int32 _CollectionForCpu(int32 cpu) const;
	int32 _CpuForCollection(uint16 collection) const;
	bool _WaitReadOffset(Controller& controller, uint32 offset);
	uint64 _Read64(const Controller& controller, size_t offset) const;
	void _Write64(Controller& controller, size_t offset, uint64 value);
	Lease* _LeaseForVector(uint32 vector);
	uint32 _FirstVector(const Lease* lease) const;

	Memory fProperties;
	Redistributor fRedistributors[kMaxRedistributors];
	uint32 fRedistributorCount = 0;
	uint16 fBootCollection = 0;
	int32 fMoves = 0;
	Controller fControllers[kMaxControllers];
	uint32 fControllerCount = 0;
	Lease fLeases[kMaxLeases];
	mutex fLeaseLock = MUTEX_INITIALIZER("GICv3 ITS leases");
};

#endif
