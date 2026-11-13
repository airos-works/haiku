/*
 * Copyright 2021-2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <string.h>

#include <interrupts.h>
#include <interrupt_controller.h>
#include <kernel.h>
#include <vm/vm.h>
#include <smp.h>
#include <KernelExport.h>
#include <debug.h>
#include <util/AutoLock.h>

#include "arch_int_gicv2.h"
#include "gicv2_regs.h"

extern "C" {
#include <libfdt.h>
}


extern void* gFDT;


#define ICI_IRQ 0


GICv2InterruptController::GICv2InterruptController(phys_addr_t gicd_addr, phys_addr_t gicc_addr)
: InterruptController()
{
	// map memory area for distributor (GICD) and CPU interface (GICC)

	area_id gicd_area = vm_map_physical_memory(B_SYSTEM_TEAM, "intc-gicv2-gicd",
		(void**)&fGicdRegs, B_ANY_KERNEL_ADDRESS, GICD_REG_SIZE,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		gicd_addr ? gicd_addr : GICD_REG_START, false);
	if (gicd_area < 0) {
		panic("not able to map the memory area for gicd\n");
	}

	area_id gicc_area = vm_map_physical_memory(B_SYSTEM_TEAM, "intc-gicv2-gicc",
		(void**)&fGiccRegs, B_ANY_KERNEL_ADDRESS, GICC_REG_SIZE,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		gicc_addr ? gicc_addr : GICC_REG_START, false);
	if (gicc_area < 0) {
		panic("not able to map the memory area for gicc\n");
	}

	// Configure the GIC. Refer to chapter 4, Programmers' Model,
	// in ARM GIC Architecture Spec v2.0
	// https://support.arm.com/documentation/ihi0048/latest/

	// disable GICC and GICD before configuring
	fGiccRegs[GICC_REG_CTLR] = 0x00;
	fGicdRegs[GICD_REG_CTLR] = 0x00;

	// how many registers cover all IRQs?
	uint32 typer = fGicdRegs[GICD_REG_TYPER];
	uint32 lineRegs = (typer & 0x1f) + 1;
	fMaxInt = 32 * lineRegs;
	// SGIs and PPIs (the timers) are banked per CPU: they are not routed
	reserve_io_interrupt_vectors(32, 0, INTERRUPT_TYPE_LOCAL_IRQ);
	reserve_io_interrupt_vectors(fMaxInt - 32, 32, INTERRUPT_TYPE_IRQ);
	LoadRoutingSetting();

	for (uint32 i = 1; i < lineRegs; i++) { // 1 bit per IRQ
		// mask and clear state before enabling
		fGicdRegs[GICD_REG_ICENABLER + i] = 0xffffffff;
		fGicdRegs[GICD_REG_ICPENDR + i] = 0xffffffff;
	}

	// route SPIs to the boot CPU: the first ITARGETSR bytes are read-only
	// and name the CPU interface of the CPU reading them
	uint32 bootTarget = _CurrentCpuInterface();
	for (uint32 i = 8; i < lineRegs * 8; i++) // 8 bits per IRQ
		fGicdRegs[GICD_REG_ITARGETSR + i] = bootTarget * 0x01010101;

	// all SPIs level-sensitive, but those the device tree says otherwise of
	for (uint32 i = 2; i < lineRegs * 2; i++) // 2 bits per IRQ
		fGicdRegs[GICD_REG_ICFGR + i] = 0x0;
	_ConfigureEdgeTriggered();

	// GICD configured, enable (GICC follows per-CPU)
	fGicdRegs[GICD_REG_CTLR] = 0x03;

	// PerCpuInit is not called separately for the boot CPU
	PerCpuInit();
}


/*!	Makes the SPIs that the device tree describes as edge triggered so in
	the GIC. On the Raspberry Pi 5 the RP1's interrupts arrive as message
	signalled interrupts, which the MIP turns into pulses on SPIs: a
	level-sensitive input would lose them.
*/
void
GICv2InterruptController::_ConfigureEdgeTriggered()
{
	if (gFDT == NULL)
		return;

	static const char* const kCompatible[]
		= {"arm,gic-400", "arm,cortex-a15-gic", "arm,cortex-a9-gic"};
	int gic = -1;
	for (size_t i = 0; i < B_COUNT_OF(kCompatible) && gic < 0; i++)
		gic = fdt_node_offset_by_compatible(gFDT, -1, kCompatible[i]);
	if (gic < 0)
		return;
	uint32 gicPhandle = fdt_get_phandle(gFDT, gic);

	int node = -1;
	while ((node = fdt_next_node(gFDT, node, NULL)) >= 0) {
		int length;
		const fdt32_t* interrupts = (const fdt32_t*)fdt_getprop(gFDT, node,
			"interrupts", &length);
		if (interrupts == NULL || length < 12 || length % 12 != 0)
			continue;
		const char* status = (const char*)fdt_getprop(gFDT, node, "status",
			NULL);
		if (status != NULL && strcmp(status, "okay") != 0
			&& strcmp(status, "ok") != 0) {
			continue;
		}

		// the node's interrupt parent, inherited from its ancestors
		uint32 parent = 0;
		for (int current = node; current >= 0 && parent == 0;
				current = fdt_parent_offset(gFDT, current)) {
			const fdt32_t* phandle = (const fdt32_t*)fdt_getprop(gFDT, current,
				"interrupt-parent", NULL);
			if (phandle != NULL)
				parent = fdt32_to_cpu(*phandle);
		}
		if (parent != gicPhandle)
			continue;

		for (int i = 0; i < length / 12; i++) {
			uint32 type = fdt32_to_cpu(interrupts[i * 3]);
			uint32 irq = fdt32_to_cpu(interrupts[i * 3 + 1]) + 32;
			uint32 flags = fdt32_to_cpu(interrupts[i * 3 + 2]);
			if (type != 0 || (flags & 0x3) == 0 || irq >= (uint32)fMaxInt)
				continue;
			// 2 bits per IRQ, the upper one: edge triggered
			fGicdRegs[GICD_REG_ICFGR + irq / 16] |= 2u << (irq % 16 * 2);
		}
	}
}


/*!	The CPU interface of the calling CPU as an ITARGETSR byte. It reads as
	zero on a uniprocessor GIC, which has only interface 0.
*/
uint32
GICv2InterruptController::_CurrentCpuInterface()
{
	uint32 mask = fGicdRegs[GICD_REG_ITARGETSR] & 0xff;
	return mask != 0 ? mask : 0x01;
}


status_t
GICv2InterruptController::PerCpuInit()
{
	int32 cpu = smp_get_current_cpu();
	if (cpu >= 0 && cpu < SMP_MAX_CPUS)
		fCpuInterface[cpu] = _CurrentCpuInterface();

	// configure IRQs 0-31 (SGIs/PPIs) which are banked per CPU interface
	fGicdRegs[GICD_REG_ICENABLER] = 0xffffffff;
	fGicdRegs[GICD_REG_ICPENDR] = 0xffffffff;

	// disable priority masking
	fGiccRegs[GICC_REG_PMR] = 0xff;
	// disable interrupt preemption
	fGiccRegs[GICC_REG_BPR] = 0x07;

	// enable GICC
	fGiccRegs[GICC_REG_CTLR] = 0x01;

	EnableInterrupt(ICI_IRQ);

	return B_OK;
}


void
GICv2InterruptController::EnableInterrupt(int32 irq)
{
	uint32_t ena_reg = GICD_REG_ISENABLER + irq / 32;
	uint32_t ena_val = 1 << (irq % 32);
	fGicdRegs[ena_reg] = ena_val;

	InterruptsSpinLocker locker(fLock);
	uint32_t prio_reg = GICD_REG_IPRIORITYR + irq / 4;
	uint32_t prio_val = fGicdRegs[prio_reg];
	prio_val |= 0x80 << (irq % 4 * 8);
	fGicdRegs[prio_reg] = prio_val;
}


/*!	Routes the SPI \a irq to \a cpu by its byte in ITARGETSR. A pending
	interrupt follows the new target; an active one finishes where it is.
*/
int32
GICv2InterruptController::AssignToCpu(int32 irq, int32 cpu)
{
	if (irq < 32 || irq >= fMaxInt)
		return cpu;
	if (!RoutingEnabled())
		return 0;
	cpu = UsableCpu(cpu);
	uint32 target = cpu < SMP_MAX_CPUS ? fCpuInterface[cpu] : 0;
	if (target == 0) {
		// that CPU has not come up (yet): leave the interrupt where it is
		uint32 current = (fGicdRegs[GICD_REG_ITARGETSR + irq / 4]
			>> (irq % 4 * 8)) & 0xff;
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (fCpuInterface[i] != 0 && (current & fCpuInterface[i]) != 0)
				return i;
		}
		return 0;
	}

	InterruptsSpinLocker locker(fLock);
	uint32 reg = GICD_REG_ITARGETSR + irq / 4;
	uint32 shift = irq % 4 * 8;
	uint32 value = fGicdRegs[reg];
	if (((value >> shift) & 0xff) != target) {
		fGicdRegs[reg] = (value & ~(0xffu << shift)) | (target << shift);
		CountMove((fGicdRegs[GICD_REG_ISENABLER + irq / 32] & (1u << (irq % 32))) != 0);
	}
	return cpu;
}


void
GICv2InterruptController::DumpRouting()
{
	DumpDeviceInterrupts();
	for (int32 irq = 32; irq < fMaxInt; irq++) {
		if ((fGicdRegs[GICD_REG_ISENABLER + irq / 32] & (1u << (irq % 32))) == 0)
			continue;
		uint32 target = (fGicdRegs[GICD_REG_ITARGETSR + irq / 4]
			>> (irq % 4 * 8)) & 0xff;
		kprintf("SPI %3" B_PRId32 ": ITARGETSR %#04" B_PRIx32 " ->", irq, target);
		for (int32 i = 0; i < smp_get_num_cpus(); i++) {
			if (fCpuInterface[i] != 0 && (target & fCpuInterface[i]) != 0)
				kprintf(" cpu%" B_PRId32, i);
		}
		kprintf("\n");
	}
}


void
GICv2InterruptController::DisableInterrupt(int32 irq)
{
	fGicdRegs[GICD_REG_ICENABLER + irq / 32] = 1 << (irq % 32);
}


void GICv2InterruptController::HandleInterrupt()
{
	uint32_t iar = fGiccRegs[GICC_REG_IAR];
	uint32_t irqnr = iar & 0x3FF;

	if ((irqnr == 1022) || (irqnr == 1023)) {
		dprintf("spurious interrupt (%d)\n", irqnr);
	} else if (irqnr == ICI_IRQ) {
		smp_intercpu_interrupt_handler(smp_get_current_cpu());
	} else {
		if (irqnr >= 32)
			CountDeviceInterrupt();
		io_interrupt_handler(irqnr, B_LEVEL_TRIGGERED);
	}

	fGiccRegs[GICC_REG_EOIR] = iar;
}


void GICv2InterruptController::SendMulticastIci(CPUSet& cpuSet)
{
	fGicdRegs[GICD_REG_SGIR] = ((cpuSet.Bits(0) & 0xff) << 16);
}


void GICv2InterruptController::SendBroadcastIci()
{
	fGicdRegs[GICD_REG_SGIR] = (0b01 << 24);
}
