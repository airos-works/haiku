/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_INTERRUPTS_H
#define AMDGPU_INTERRUPTS_H

#include <KernelExport.h>
#include <PCI.h>
#include <amdgpu_haiku.h>

struct GpuInterrupts {
	static const uint32 kRingBytes = 65536;
	static const uint32 kMask = kRingBytes - 1;
	static const uint32 kAllocationBytes = kRingBytes + 8192;
	static const uint32 kVMInterrupts = 0x8 | 0x40 | 0x200 | 0x1000 | 0x8000 | 0x40000 | 0x200000;
	static const uint32 kCPInterrupts = (1 << 26) | (1 << 23) | (1 << 22);
	volatile uint32* regs;
	volatile uint32* memory;
	pci_module_info* pci;
	uint8 bus, slot, function;
	uint32 vector, rptr;
	volatile uint32* fence;
	uint32 expectedFence;
	bool pendingFence;
	area_id area;
	sem_id signal;
	spinlock lock;
	int32 error;
	bool attempted, configured, handler, enabled, programmed, resetSuspended;
	amdgpu_irq_info stats;

	status_t Initialize(volatile uint32* r, const amdgpu_info& info, pci_module_info* module);
	status_t SuspendForReset();
	status_t RearmAfterReset();
	void Uninitialize();
	status_t Error() { return atomic_get(&error); }
	void RecordError(status_t status) { atomic_test_and_set(&error, status, B_OK); }
	uint64 Ticket(volatile uint32* completion, uint32 sequence);
	status_t Wait(uint64 ticket, bigtime_t deadline);
	void Snapshot(amdgpu_irq_info& result);
	static int32 Handle(void* cookie);
};
#endif
