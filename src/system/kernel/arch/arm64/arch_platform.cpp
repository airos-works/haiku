/*
 * Copyright 2019-2022 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include <KernelExport.h>
#include <arch/platform.h>
#include <boot/kernel_args.h>
#include <boot_item.h>
#include <safemode.h>
#include <timer.h>
#include <vm/vm.h>

extern "C" {
#include <libfdt.h>
}


void *gFDT = NULL;
phys_addr_t sACPIRootPointer = 0;


status_t
arch_platform_init(struct kernel_args *kernelArgs)
{
	gFDT = kernelArgs->arch_args.fdt;
	return B_OK;
}


// Like Linux, use the device tree when the firmware offers both a complete
// one and ACPI tables: the Qualcomm UEFI of the Radxa Dragon Q8B has ACPI
// tables for Windows whose devices need vendor drivers, and probing their
// PCI segments without the platform's power management stops the machine.
// The "prefer_acpi" safe mode setting keeps ACPI.
static bool
use_acpi(struct kernel_args *kernelArgs)
{
	if (!kernelArgs->arch_args.acpi_root)
		return false;
	if (gFDT == NULL || get_safemode_boolean_early(kernelArgs, "prefer_acpi", false))
		return true;
	return fdt_path_offset(gFDT, "/cpus") < 0
		|| fdt_getprop(gFDT, 0, "compatible", NULL) == NULL;
}


status_t
arch_platform_init_post_vm(struct kernel_args *kernelArgs)
{
	if (use_acpi(kernelArgs)) {
		sACPIRootPointer = kernelArgs->arch_args.acpi_root.Get();
		add_boot_item("ACPI_ROOT_POINTER", &sACPIRootPointer, sizeof(sACPIRootPointer));
	}

	return B_OK;
}


// The Raspberry Pi loader's lab watchdog ("airos.watchdog" in cmdline.txt):
// the BCM2835-style power manager restarts the board unless its counter is
// reloaded within 15 s. A timer interrupt does that for as long as the
// kernel runs; a kernel that stops taking interrupts gets restarted.
#define PM_PASSWORD		0x5a000000
#define PM_WDOG			0x24
#define PM_WDOG_TIMEOUT	(15 << 16)

static volatile uint32* sWatchdog;
static timer sWatchdogTimer;


static int32
pet_watchdog(timer*)
{
	sWatchdog[PM_WDOG / 4] = PM_PASSWORD | PM_WDOG_TIMEOUT;
	return B_HANDLED_INTERRUPT;
}


static void
init_lab_watchdog()
{
	int chosen = gFDT != NULL ? fdt_path_offset(gFDT, "/chosen") : -1;
	int length;
	const void* property = chosen >= 0
		? fdt_getprop(gFDT, chosen, "airos,watchdog", &length) : NULL;
	if (property == NULL || length != 8)
		return;

	phys_addr_t base = fdt64_to_cpu(*(const fdt64_t*)property);
	void* address;
	area_id area = map_physical_memory("lab watchdog", base, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&address);
	if (area < 0)
		return;

	sWatchdog = (volatile uint32*)address;
	pet_watchdog(NULL);
	add_timer(&sWatchdogTimer, &pet_watchdog, 1000000, B_PERIODIC_TIMER);
	dprintf("lab watchdog at %#" B_PRIxPHYSADDR ": the board restarts if "
		"the kernel stops for 15 s\n", base);
}


status_t
arch_platform_init_post_thread(struct kernel_args *kernelArgs)
{
	init_lab_watchdog();
	return B_OK;
}
