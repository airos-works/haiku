/*
 * Copyright 2019-2026 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */
#include <OS.h>

#include <arch/arm64/arch_cpu_type.h>
#include <arch/system_info.h>
#include <arch_cpu.h>
#include <boot/kernel_args.h>

#include "cpu.h"

#include <libfdt.h>


static cpu_vendor sCPUVendor = B_CPU_VENDOR_UNKNOWN;

extern void* gFDT;

static uint64 sCPUFrequency[SMP_MAX_CPUS];


int
arm64_fdt_cpu_node(const void* fdt, uint64 mpidr)
{
	int cpus = fdt_path_offset(fdt, "/cpus");
	if (cpus < 0)
		return -1;

	int length;
	const fdt32_t* addressCells = (const fdt32_t*)fdt_getprop(fdt, cpus,
		"#address-cells", &length);
	uint32 cells = addressCells != NULL && length == 4
		? fdt32_to_cpu(*addressCells) : 1;
	if (cells != 1 && cells != 2)
		return -1;

	const uint64 affinityMask = UINT64_C(0x000000ff00ffffff);
	for (int node = fdt_first_subnode(fdt, cpus); node >= 0;
			node = fdt_next_subnode(fdt, node)) {
		const char* type = (const char*)fdt_getprop(fdt, node, "device_type",
			&length);
		if (type == NULL || length < 4 || strcmp(type, "cpu") != 0)
			continue;
		const fdt32_t* reg = (const fdt32_t*)fdt_getprop(fdt, node, "reg",
			&length);
		if (reg == NULL || length != (int)(cells * sizeof(fdt32_t)))
			continue;
		uint64 id = fdt32_to_cpu(reg[0]);
		if (cells == 2)
			id = (id << 32) | fdt32_to_cpu(reg[1]);
		if ((id & affinityMask) == (mpidr & affinityMask))
			return node;
	}
	return -1;
}


uint64
arm64_fdt_cpu_frequency(const void* fdt, int node)
{
	if (node < 0)
		return 0;

	int length;
	const fdt32_t* opp = (const fdt32_t*)fdt_getprop(fdt, node,
		"operating-points-v2", &length);
	if (opp != NULL && length == 4) {
		int table = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(*opp));
		if (table >= 0) {
			uint64 maximum = 0;
			for (int entry = fdt_first_subnode(fdt, table); entry >= 0;
					entry = fdt_next_subnode(fdt, entry)) {
				const char* status = (const char*)fdt_getprop(fdt, entry,
					"status", NULL);
				if (status != NULL && strcmp(status, "disabled") == 0)
					continue;
				const fdt64_t* hz = (const fdt64_t*)fdt_getprop(fdt, entry,
					"opp-hz", &length);
				if (hz != NULL && length == 8) {
					uint64 frequency = fdt64_to_cpu(*hz);
					if (frequency > maximum && frequency < UINT64_C(10000000000))
						maximum = frequency;
				}
			}
			if (maximum != 0)
				return maximum;
		}
	}

	const fdt32_t* clock = (const fdt32_t*)fdt_getprop(fdt, node,
		"clock-frequency", &length);
	return clock != NULL && length == 4 ? fdt32_to_cpu(*clock) : 0;
}


void
arch_fill_topology_node(cpu_topology_node_info* node, int32 cpu)
{
	switch (node->type) {
		case B_TOPOLOGY_ROOT:
			node->data.root.platform = B_CPU_ARM_64;
			break;
		case B_TOPOLOGY_PACKAGE:
			node->data.package.vendor = sCPUVendor;
			node->data.package.cache_line_size = CACHE_LINE_SIZE;
			break;
		case B_TOPOLOGY_CORE:
			node->data.core.model = static_cast<uint32>(gCPU[cpu].arch.midr);
			// The maximum from the device tree's operating points or
			// clock-frequency; 0 when it has neither.
			node->data.core.default_frequency = sCPUFrequency[cpu];
			break;
		default:
			break;
	}
}


status_t
arch_system_info_init(struct kernel_args *args)
{
	cpu_ent* cpu = get_cpu_struct();

	// TODO: It *is* possible for the package vendors to be heterogeneous,
	//       (Tegra X2 has NVIDIA and ARM-designed cores on the same package)
	//       but as vendor field is set for the whole package currently,
	//       it would have to be reworked for it to be supported.
	switch (CPU_IMPL(cpu->arch.midr)) {
		case CPU_IMPL_ARM:
			sCPUVendor = B_CPU_VENDOR_ARM;
			break;
		case CPU_IMPL_BROADCOM:
			sCPUVendor = B_CPU_VENDOR_BROADCOM;
			break;
		case CPU_IMPL_CAVIUM:
			sCPUVendor = B_CPU_VENDOR_CAVIUM;
			break;
		case CPU_IMPL_DEC:
			sCPUVendor = B_CPU_VENDOR_DEC;
			break;
		case CPU_IMPL_FUJITSU:
			sCPUVendor = B_CPU_VENDOR_FUJITSU;
			break;
		case CPU_IMPL_HISILICON:
			sCPUVendor = B_CPU_VENDOR_HISILICON;
			break;
		case CPU_IMPL_INFINEON:
			sCPUVendor = B_CPU_VENDOR_INFINEON;
			break;
		case CPU_IMPL_FREESCALE:
			sCPUVendor = B_CPU_VENDOR_FREESCALE;
			break;
		case CPU_IMPL_NVIDIA:
			sCPUVendor = B_CPU_VENDOR_NVIDIA;
			break;
		case CPU_IMPL_APM:
			sCPUVendor = B_CPU_VENDOR_APM;
			break;
		case CPU_IMPL_QUALCOMM:
			sCPUVendor = B_CPU_VENDOR_QUALCOMM;
			break;
		case CPU_IMPL_MARVELL:
			sCPUVendor = B_CPU_VENDOR_MARVELL;
			break;
		case CPU_IMPL_APPLE:
			sCPUVendor = B_CPU_VENDOR_APPLE;
			break;
		case CPU_IMPL_INTEL:
			sCPUVendor = B_CPU_VENDOR_INTEL;
			break;
		case CPU_IMPL_AMPERE:
			sCPUVendor = B_CPU_VENDOR_AMPERE;
			break;
		case CPU_IMPL_MICROSOFT:
			sCPUVendor = B_CPU_VENDOR_MICROSOFT;
			break;
		default:
			break;
	}

	if (gFDT != NULL && fdt_check_header(gFDT) == 0) {
		for (uint32 i = 0; i < args->num_cpus && i < SMP_MAX_CPUS; i++) {
			sCPUFrequency[i] = arm64_fdt_cpu_frequency(gFDT,
				arm64_fdt_cpu_node(gFDT, gCPU[i].arch.mpidr));
		}
	}

	return B_OK;
}


status_t
arch_get_frequency(uint64 *frequency, int32 cpu)
{
	// HACKME: This is a tricky one. There are a few options with wildly varying availability:
	// * ARMv8.4 Activity Monitors (AMEVCNTR0 counter)
	// * PMU cycle counter sampling
	// * on Apple M-series, p-state registers
	// * ACPI CPPC feedback counters
	// In QEMU, the only one that "works" is PMU, but it returns constant 1 GHz.
	// Until then, report the maximum the device tree gives.

	if (frequency == NULL || cpu < 0 || cpu >= SMP_MAX_CPUS)
		return B_BAD_VALUE;
	*frequency = sCPUFrequency[cpu];
	return B_OK;
}
