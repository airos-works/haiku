/*
 * Copyright 2021-2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARCH_ARM_GICV2_H
#define ARCH_ARM_GICV2_H

#include <SupportDefs.h>
#include <lock.h>

#include "soc.h"

class GICv2InterruptController : public InterruptController {
public:
	GICv2InterruptController(phys_addr_t gicd_regs = 0, phys_addr_t gicc_regs = 0);
	void EnableInterrupt(int32 irq);
	void DisableInterrupt(int32 irq);
	void HandleInterrupt();
	void SendMulticastIci(CPUSet& cpuSet);
	void SendBroadcastIci();
	status_t PerCpuInit();
	int32 AssignToCpu(int32 irq, int32 cpu);
	void DumpRouting();
private:
	uint32 _CurrentCpuInterface();

	volatile uint32_t *fGicdRegs;
	volatile uint32_t *fGiccRegs;
	int32 fMaxInt = 0;
	uint8 fCpuInterface[SMP_MAX_CPUS] = {};
	spinlock fLock = B_SPINLOCK_INITIALIZER;
};

#endif /* ARCH_ARM_GICV2_H */
