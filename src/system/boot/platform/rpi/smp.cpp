/*
 * Copyright 2026, air/OS. All rights reserved.
 * Copyright 2021-2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Pi 4: cores 1-3 wait in the firmware's armstub, each polling its slot of
	the spin table (the device tree's cpu-release-addr). Writing an address
	there sends the core to rpi_secondary_entry in entry.S.
	Pi 5: the firmware's TF-A holds them; PSCI CPU_ON sends a core to
	rpi_psci_secondary_entry with its index as the context ID. */


#include "smp.h"

#include <string.h>

#include <KernelExport.h>

#include <boot/kernel_args.h>
#include <boot/menu.h>
#include <boot/platform.h>
#include <boot/stage2.h>
#include <kernel.h>
#include <safemode.h>

#include "dtb.h"
#include "mmu.h"


extern uint8 _start;
extern uint8 _image_end;

extern "C" void rpi_secondary_entry();
extern "C" void rpi_psci_secondary_entry();
extern "C" void rpi_psci_secondary_main(uint64 cpu);
extern "C" void arch_enter_kernel(struct kernel_args* kernelArgs,
	addr_t kernelEntry, addr_t kernelStackTop, uint32 cpu);
extern "C" void rpi_secondary_main(uint64 affinity);

// indexed by MPIDR affinity level 0; read by rpi_secondary_entry
uint64 gSecondaryStacks[256];
// indexed by CPU; read by rpi_psci_secondary_entry
uint64 gPsciSecondaryStacks[MAX_FDT_CPUS];

#define PSCI_CPU_ON_64		0xc4000003

static uint32 sCpuIndex[256];
static addr_t sKernelEntry;
static kernel_args* sKernelArgs;


void
smp_init()
{
	gKernelArgs.num_cpus = 1;

	// Only CPUs behind the boot CPU that can be started count.
	for (uint32 i = 1; i < gFDTCpuCount; i++) {
		if (gFDTCpus[i].psci) {
			gKernelArgs.num_cpus++;
			continue;
		}
		if (gFDTCpus[i].releaseAddress == 0 || gFDTCpus[i].mpidr > 0xff)
			break;
		gKernelArgs.num_cpus++;
	}

	if (gKernelArgs.num_cpus > SMP_MAX_CPUS)
		gKernelArgs.num_cpus = SMP_MAX_CPUS;
}


void
smp_init_other_cpus()
{
	if (get_safemode_boolean(B_SAFEMODE_DISABLE_SMP, false))
		gKernelArgs.num_cpus = 1;

	for (uint32 i = 1; i < gKernelArgs.num_cpus; i++) {
		void* stack = NULL;
		const size_t size = KERNEL_STACK_SIZE
			+ KERNEL_STACK_GUARD_PAGES * B_PAGE_SIZE;
		if (platform_allocate_region(&stack, size, 0) != B_OK)
			panic("Unable to allocate AP stack");

		memset(stack, 0, size);
		gKernelArgs.cpu_kstack[i].start = fix_address((addr_t)stack);
		gKernelArgs.cpu_kstack[i].size = size;

		if (gFDTCpus[i].psci) {
			gPsciSecondaryStacks[i] = (addr_t)stack + size;
			continue;
		}
		uint64 affinity = gFDTCpus[i].mpidr & 0xff;
		gSecondaryStacks[affinity] = (addr_t)stack + size;
		sCpuIndex[affinity] = i;
	}
}


static int64
psci_call(uint64 function, uint64 argument1, uint64 argument2,
	uint64 argument3)
{
	register uint64 x0 asm("x0") = function;
	register uint64 x1 asm("x1") = argument1;
	register uint64 x2 asm("x2") = argument2;
	register uint64 x3 asm("x3") = argument3;
	if (gPsciConduit == ARM64_PSCI_HVC) {
		asm volatile("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
			: : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
				"x13", "x14", "x15", "x16", "x17", "memory");
	} else {
		asm volatile("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
			: : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12",
				"x13", "x14", "x15", "x16", "x17", "memory");
	}
	return (int64)x0;
}


extern "C" void
rpi_secondary_main(uint64 affinity)
{
	// Still on the loader's stack at its physical address; the identity map
	// keeps it valid once the MMU is on.
	mmu_enable_on_this_cpu();

	uint32 cpu = sCpuIndex[affinity];
	arch_enter_kernel(sKernelArgs, sKernelEntry,
		gKernelArgs.cpu_kstack[cpu].start + gKernelArgs.cpu_kstack[cpu].size,
		cpu);
}


extern "C" void
rpi_psci_secondary_main(uint64 cpu)
{
	mmu_enable_on_this_cpu();

	arch_enter_kernel(sKernelArgs, sKernelEntry,
		gKernelArgs.cpu_kstack[cpu].start + gKernelArgs.cpu_kstack[cpu].size,
		cpu);
}


void
smp_boot_other_cpus(addr_t kernelEntry)
{
	sKernelEntry = kernelEntry;
	sKernelArgs = &gKernelArgs;

	// The other cores run uncached until they turn their MMU on: what they
	// read has to be in memory by then, and nothing they write may sit in
	// this core's cache, or it would come back as it was before.
	mmu_flush_data_cache(&_start, &_image_end - &_start);
	for (uint32 i = 1; i < gKernelArgs.num_cpus; i++) {
		const size_t size = KERNEL_STACK_SIZE
			+ KERNEL_STACK_GUARD_PAGES * B_PAGE_SIZE;
		uint64 top = gFDTCpus[i].psci
			? gPsciSecondaryStacks[i] : gSecondaryStacks[gFDTCpus[i].mpidr & 0xff];
		mmu_flush_data_cache((void*)(top - size), size);
	}

	bool spinTable = false;
	for (uint32 i = 1; i < gKernelArgs.num_cpus; i++) {
		if (gFDTCpus[i].psci) {
			int64 result = psci_call(PSCI_CPU_ON_64, gFDTCpus[i].mpidr,
				(uint64)&rpi_psci_secondary_entry, i);
			if (result != 0)
				panic("PSCI CPU_ON for CPU %" B_PRIu32 " failed: %" B_PRId64,
					i, result);
			continue;
		}
		uint64* release = (uint64*)gFDTCpus[i].releaseAddress;
		*release = (uint64)&rpi_secondary_entry;
		mmu_flush_data_cache(release, sizeof(*release));
		spinTable = true;
	}
	if (spinTable)
		asm("sev");
}


void
smp_add_safemode_menus(Menu* menu)
{
	if (gKernelArgs.num_cpus < 2)
		return;

	MenuItem* item = new(std::nothrow) MenuItem("Disable SMP");
	menu->AddItem(item);
	item->SetData(B_SAFEMODE_DISABLE_SMP);
	item->SetType(MENU_ITEM_MARKABLE);
	item->SetHelpText("Disables all but one CPU core.");
}
