/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARCH_ARM64_BCM2836_H
#define ARCH_ARM64_BCM2836_H


#include <SupportDefs.h>
#include <lock.h>

#include "soc.h"


/*!	The interrupt controllers of the BCM2836 and BCM2837 (Raspberry Pi 2
	v1.2, Pi 3, Zero 2): the ARM's per-core "local" controller (the cores'
	timers, four mailboxes per core for the inter-processor interrupts, the
	VideoCore's interrupts as one input) and the VideoCore's ARM control
	block with the device interrupts in three banks.

	The kernel numbers them the way it numbers a GIC's, so that the device
	tree (the loader converts its two-cell specifiers) and the timer code
	stay as they are:
	  0          the inter-processor interrupt (mailbox 0 of each core)
	  16-31      per-core inputs as GIC PPIs: 26 hypervisor physical timer,
	             27 virtual timer, 29 secure and 30 non-secure physical
	             timer, 23 performance monitors
	  32-95      the VideoCore's interrupts 0-63 (banks 1 and 2)
	  96-103     the ARM's own interrupts in the basic bank 0
*/
class BCM2836InterruptController : public InterruptController {
public:
	BCM2836InterruptController(phys_addr_t armctrl, phys_addr_t local);
	void EnableInterrupt(int32 irq);
	void DisableInterrupt(int32 irq);
	void HandleInterrupt();
	void SendMulticastIci(CPUSet& cpuSet);
	void SendBroadcastIci();
	status_t PerCpuInit();
	int32 AssignToCpu(int32 irq, int32 cpu);
	void DumpRouting();

private:
	static uint32 _CurrentCore();
	static int32 _TimerBit(int32 irq);
	void _SendIci(int32 cpu);

	volatile uint32* fArmctrl;
	volatile uint32* fLocal;
	uint8 fCore[SMP_MAX_CPUS] = {};
	int32 fCpuOfCore[4] = {0, 1, 2, 3};
	uint32 fEnabled[3] = {};	// banks 1, 2 and basic
	spinlock fLock = B_SPINLOCK_INITIALIZER;
};


#endif /* ARCH_ARM64_BCM2836_H */
