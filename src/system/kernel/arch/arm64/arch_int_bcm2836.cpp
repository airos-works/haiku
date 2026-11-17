/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "arch_int_bcm2836.h"

#include <arch_cpu.h>
#include <debug.h>
#include <interrupts.h>
#include <smp.h>
#include <util/AutoLock.h>
#include <vm/vm.h>


#define ICI_IRQ					0
#define PMU_IRQ					23
#define HYP_TIMER_IRQ			26
#define VIRTUAL_TIMER_IRQ		27
#define SECURE_TIMER_IRQ		29
#define PHYSICAL_TIMER_IRQ		30
#define GPU_IRQ_BASE			32
#define BASIC_IRQ_BASE			96
#define IRQ_COUNT				104

// the ARM control block (Linux: irq-bcm2835.c), 32-bit registers
#define ARMCTRL_PENDING_BASIC	(0x00 / 4)
#define ARMCTRL_PENDING_1		(0x04 / 4)
#define ARMCTRL_PENDING_2		(0x08 / 4)
#define ARMCTRL_ENABLE_1		(0x10 / 4)
#define ARMCTRL_ENABLE_2		(0x14 / 4)
#define ARMCTRL_ENABLE_BASIC	(0x18 / 4)
#define ARMCTRL_DISABLE_1		(0x1c / 4)
#define ARMCTRL_DISABLE_2		(0x20 / 4)
#define ARMCTRL_DISABLE_BASIC	(0x24 / 4)

// the local controller (Linux: irq-bcm2836.c)
#define LOCAL_GPU_ROUTING		(0x0c / 4)
#define LOCAL_PMU_ROUTING_SET	(0x10 / 4)
#define LOCAL_PMU_ROUTING_CLEAR	(0x14 / 4)
#define LOCAL_TIMER_CONTROL(c)	((0x40 + 4 * (c)) / 4)
#define LOCAL_MAILBOX_CONTROL(c) ((0x50 + 4 * (c)) / 4)
#define LOCAL_IRQ_SOURCE(c)		((0x60 + 4 * (c)) / 4)
#define LOCAL_MAILBOX_SET(c, m)	((0x80 + 16 * (c) + 4 * (m)) / 4)
#define LOCAL_MAILBOX_CLEAR(c, m) ((0xc0 + 16 * (c) + 4 * (m)) / 4)

#define SOURCE_MAILBOX_0		(1 << 4)
#define SOURCE_GPU				(1 << 8)
#define SOURCE_PMU				(1 << 9)


BCM2836InterruptController::BCM2836InterruptController(phys_addr_t armctrl,
	phys_addr_t local)
	:
	InterruptController()
{
	area_id area = vm_map_physical_memory(B_SYSTEM_TEAM, "intc-bcm2835-armctrl",
		(void**)&fArmctrl, B_ANY_KERNEL_ADDRESS, 0x200,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, armctrl, false);
	if (area < 0)
		panic("BCM2836: cannot map the ARM control block");
	area = vm_map_physical_memory(B_SYSTEM_TEAM, "intc-bcm2836-local",
		(void**)&fLocal, B_ANY_KERNEL_ADDRESS, 0x100,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, local, false);
	if (area < 0)
		panic("BCM2836: cannot map the local interrupt controller");

	// everything off; the VideoCore's interrupts go to core 0
	fArmctrl[ARMCTRL_DISABLE_1] = ~0u;
	fArmctrl[ARMCTRL_DISABLE_2] = ~0u;
	fArmctrl[ARMCTRL_DISABLE_BASIC] = ~0u;
	fLocal[LOCAL_GPU_ROUTING] = 0;
	for (uint32 core = 0; core < 4; core++) {
		fLocal[LOCAL_TIMER_CONTROL(core)] = 0;
		fLocal[LOCAL_MAILBOX_CONTROL(core)] = 0;
		for (uint32 mailbox = 0; mailbox < 4; mailbox++)
			fLocal[LOCAL_MAILBOX_CLEAR(core, mailbox)] = ~0u;
	}

	// the per-core inputs are not routed; the device interrupts all arrive
	// on one core (one routing register for all of them)
	reserve_io_interrupt_vectors(GPU_IRQ_BASE, 0, INTERRUPT_TYPE_LOCAL_IRQ);
	reserve_io_interrupt_vectors(IRQ_COUNT - GPU_IRQ_BASE, GPU_IRQ_BASE,
		INTERRUPT_TYPE_IRQ);
	LoadRoutingSetting();

	dprintf("BCM2836 interrupt controller: ARM control at %#" B_PRIxPHYSADDR
		", local at %#" B_PRIxPHYSADDR "\n", armctrl, local);

	// PerCpuInit is not called separately for the boot CPU
	PerCpuInit();
}


uint32
BCM2836InterruptController::_CurrentCore()
{
	return READ_SPECIALREG(MPIDR_EL1) & 3;
}


int32
BCM2836InterruptController::_TimerBit(int32 irq)
{
	switch (irq) {
		case SECURE_TIMER_IRQ:
			return 0;
		case PHYSICAL_TIMER_IRQ:
			return 1;
		case HYP_TIMER_IRQ:
			return 2;
		case VIRTUAL_TIMER_IRQ:
			return 3;
	}
	return -1;
}


status_t
BCM2836InterruptController::PerCpuInit()
{
	int32 cpu = smp_get_current_cpu();
	uint32 core = _CurrentCore();
	if (cpu >= 0 && cpu < SMP_MAX_CPUS) {
		fCore[cpu] = core;
		fCpuOfCore[core] = cpu;
	}

	fLocal[LOCAL_MAILBOX_CLEAR(core, 0)] = ~0u;
	fLocal[LOCAL_MAILBOX_CONTROL(core)] |= 1;
	return B_OK;
}


void
BCM2836InterruptController::EnableInterrupt(int32 irq)
{
	if (irq < GPU_IRQ_BASE) {
		int32 bit = _TimerBit(irq);
		uint32 core = _CurrentCore();
		if (bit >= 0)
			fLocal[LOCAL_TIMER_CONTROL(core)] |= 1u << bit;
		else if (irq == PMU_IRQ)
			fLocal[LOCAL_PMU_ROUTING_SET] = 1u << core;
		return;
	}

	InterruptsSpinLocker locker(fLock);
	if (irq < BASIC_IRQ_BASE) {
		uint32 bank = (irq - GPU_IRQ_BASE) / 32;
		uint32 bit = 1u << ((irq - GPU_IRQ_BASE) % 32);
		fEnabled[bank] |= bit;
		fArmctrl[bank == 0 ? ARMCTRL_ENABLE_1 : ARMCTRL_ENABLE_2] = bit;
	} else if (irq < IRQ_COUNT) {
		uint32 bit = 1u << (irq - BASIC_IRQ_BASE);
		fEnabled[2] |= bit;
		fArmctrl[ARMCTRL_ENABLE_BASIC] = bit;
	}
}


void
BCM2836InterruptController::DisableInterrupt(int32 irq)
{
	if (irq < GPU_IRQ_BASE) {
		int32 bit = _TimerBit(irq);
		uint32 core = _CurrentCore();
		if (bit >= 0)
			fLocal[LOCAL_TIMER_CONTROL(core)] &= ~(1u << bit);
		else if (irq == PMU_IRQ)
			fLocal[LOCAL_PMU_ROUTING_CLEAR] = 1u << core;
		return;
	}

	InterruptsSpinLocker locker(fLock);
	if (irq < BASIC_IRQ_BASE) {
		uint32 bank = (irq - GPU_IRQ_BASE) / 32;
		uint32 bit = 1u << ((irq - GPU_IRQ_BASE) % 32);
		fEnabled[bank] &= ~bit;
		fArmctrl[bank == 0 ? ARMCTRL_DISABLE_1 : ARMCTRL_DISABLE_2] = bit;
	} else if (irq < IRQ_COUNT) {
		uint32 bit = 1u << (irq - BASIC_IRQ_BASE);
		fEnabled[2] &= ~bit;
		fArmctrl[ARMCTRL_DISABLE_BASIC] = bit;
	}
}


void
BCM2836InterruptController::HandleInterrupt()
{
	uint32 core = _CurrentCore();
	uint32 source = fLocal[LOCAL_IRQ_SOURCE(core)];

	if ((source & SOURCE_MAILBOX_0) != 0) {
		uint32 pending = fLocal[LOCAL_MAILBOX_CLEAR(core, 0)];
		fLocal[LOCAL_MAILBOX_CLEAR(core, 0)] = pending;
		if (pending != 0)
			smp_intercpu_interrupt_handler(smp_get_current_cpu());
	}

	// the timers: CNTPS, CNTPNS, CNTHP, CNTV
	static const int32 kTimerIrq[4] = {SECURE_TIMER_IRQ, PHYSICAL_TIMER_IRQ,
		HYP_TIMER_IRQ, VIRTUAL_TIMER_IRQ};
	for (uint32 bit = 0; bit < 4; bit++) {
		if ((source & (1u << bit)) != 0)
			io_interrupt_handler(kTimerIrq[bit], B_LEVEL_TRIGGERED);
	}
	if ((source & SOURCE_PMU) != 0)
		io_interrupt_handler(PMU_IRQ, B_LEVEL_TRIGGERED);

	if ((source & SOURCE_GPU) == 0)
		return;

	// The shortcut bits of the basic register do not show in its bits 8
	// and 9: read all three banks.
	uint32 pending[3] = {
		fArmctrl[ARMCTRL_PENDING_1] & fEnabled[0],
		fArmctrl[ARMCTRL_PENDING_2] & fEnabled[1],
		fArmctrl[ARMCTRL_PENDING_BASIC] & fEnabled[2] & 0xff
	};
	static const int32 kBase[3] = {GPU_IRQ_BASE, GPU_IRQ_BASE + 32,
		BASIC_IRQ_BASE};
	for (uint32 bank = 0; bank < 3; bank++) {
		uint32 bits = pending[bank];
		while (bits != 0) {
			uint32 bit = __builtin_ctz(bits);
			bits &= bits - 1;
			CountDeviceInterrupt();
			io_interrupt_handler(kBase[bank] + bit, B_LEVEL_TRIGGERED);
		}
	}
}


void
BCM2836InterruptController::_SendIci(int32 cpu)
{
	if (cpu < 0 || cpu >= SMP_MAX_CPUS)
		return;
	fLocal[LOCAL_MAILBOX_SET(fCore[cpu], 0)] = 1;
}


void
BCM2836InterruptController::SendMulticastIci(CPUSet& cpuSet)
{
	int32 count = smp_get_num_cpus();
	for (int32 cpu = 0; cpu < count; cpu++) {
		if (cpuSet.GetBit(cpu))
			_SendIci(cpu);
	}
}


void
BCM2836InterruptController::SendBroadcastIci()
{
	int32 count = smp_get_num_cpus();
	int32 current = smp_get_current_cpu();
	for (int32 cpu = 0; cpu < count; cpu++) {
		if (cpu != current)
			_SendIci(cpu);
	}
}


int32
BCM2836InterruptController::AssignToCpu(int32 irq, int32 cpu)
{
	// One routing register takes all of the VideoCore's interrupts to one
	// core: they stay on the boot CPU.
	return fCpuOfCore[fLocal[LOCAL_GPU_ROUTING] & 3];
}


void
BCM2836InterruptController::DumpRouting()
{
	kprintf("BCM2836: device interrupts go to core %" B_PRIu32 "\n",
		fLocal[LOCAL_GPU_ROUTING] & 3);
	kprintf("enabled: bank 1 %#" B_PRIx32 ", bank 2 %#" B_PRIx32
		", basic %#" B_PRIx32 "\n", fEnabled[0], fEnabled[1], fEnabled[2]);
	kprintf("pending: bank 1 %#" B_PRIx32 ", bank 2 %#" B_PRIx32
		", basic %#" B_PRIx32 "\n", fArmctrl[ARMCTRL_PENDING_1],
		fArmctrl[ARMCTRL_PENDING_2], fArmctrl[ARMCTRL_PENDING_BASIC]);
	for (uint32 core = 0; core < 4; core++) {
		kprintf("core %" B_PRIu32 ": timers %#" B_PRIx32 ", mailboxes %#"
			B_PRIx32 ", source %#" B_PRIx32 "\n", core,
			fLocal[LOCAL_TIMER_CONTROL(core)],
			fLocal[LOCAL_MAILBOX_CONTROL(core)], fLocal[LOCAL_IRQ_SOURCE(core)]);
	}
	DumpDeviceInterrupts();
}
