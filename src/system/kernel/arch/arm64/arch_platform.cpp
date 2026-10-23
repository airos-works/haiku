/*
 * Copyright 2019-2022 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include <KernelExport.h>
#include <arch/platform.h>
#include <boot/kernel_args.h>
#include <boot_item.h>
#include <safemode.h>

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


status_t
arch_platform_init_post_thread(struct kernel_args *kernelArgs)
{
	return B_OK;
}
