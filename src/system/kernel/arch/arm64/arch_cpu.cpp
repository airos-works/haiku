/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


#include <KernelExport.h>

extern "C" {
#include <libfdt.h>
}


#include <arch/cpu.h>
#include <arch/arm64/arch_cpu_type.h>
#include <arch/arm64/cache_line_size.h>
#include <boot/kernel_args.h>
#include <commpage.h>
#include <elf.h>
#include <vm/vm.h>


extern "C" void _exception_vectors(void);

extern void* gFDT;
int arm64_fdt_cpu_node(const void* fdt, uint64 mpidr);
uint64 arm64_fdt_cpu_frequency(const void* fdt, int node);


static const uint32 kPsciVersion = 0x84000000;
static const uint32 kPsciSystemOff = 0x84000008;
static const uint32 kPsciSystemReset = 0x84000009;

static uint32 sPsciConduit = ARM64_PSCI_NONE;


static volatile uint32* sBcm2835Watchdog;


static int32
psci_call(uint32 function)
{
	register uint64 x0 asm("x0") = function;
	register uint64 x1 asm("x1") = 0;
	register uint64 x2 asm("x2") = 0;
	register uint64 x3 asm("x3") = 0;

	// These PSCI functions use the SMC32/HVC32 convention. Conservatively
	// clobber the caller-saved registers for older SMCCC implementations.
	if (sPsciConduit == ARM64_PSCI_SMC) {
		asm volatile("smc #0"
			: "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
			:
			: "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			  "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
	} else if (sPsciConduit == ARM64_PSCI_HVC) {
		asm volatile("hvc #0"
			: "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
			:
			: "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			  "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
	} else
		return -1;

	return (int32)x0;
}


status_t
arch_cpu_preboot_init_percpu(kernel_args *args, int curr_cpu)
{
	WRITE_SPECIALREG(VBAR_EL1, _exception_vectors);
	return B_OK;
}


/*!	The AT_HWCAP and AT_HWCAP2 bits (numbered as on Linux, see <sys/auxv.h>)
	of the features all CPUs have: a big.LITTLE system can only report what
	its smallest cores support. User land reads them from the commpage
	(getauxval()); libgcc picks its atomics by HWCAP_ATOMICS, for one.
*/
int64 gARM64HWCaps[2] = { -1, -1 };


static void
arm64_cpu_hwcaps(uint64 caps[2])
{
	const uint64 pfr0 = READ_SPECIALREG(ID_AA64PFR0_EL1);
	const uint64 isar0 = READ_SPECIALREG(ID_AA64ISAR0_EL1);
	const uint64 isar1 = READ_SPECIALREG(ID_AA64ISAR1_EL1);
	auto field = [](uint64 reg, int shift) { return (reg >> shift) & 0xf; };

	uint64 hwcap = 0;
	uint64 hwcap2 = 0;

	// FP and AdvSIMD: 0 present, 1 with half precision, 0xf absent
	if (field(pfr0, 16) != 0xf)
		hwcap |= 1UL << 0;
	if (field(pfr0, 16) == 1)
		hwcap |= 1UL << 9;
	if (field(pfr0, 20) != 0xf)
		hwcap |= 1UL << 1;
	if (field(pfr0, 20) == 1)
		hwcap |= 1UL << 10;
	if (field(pfr0, 48) >= 1)
		hwcap |= 1UL << 24;		// DIT

	if (field(isar0, 4) >= 1)
		hwcap |= 1UL << 3;		// AES
	if (field(isar0, 4) >= 2)
		hwcap |= 1UL << 4;		// PMULL
	if (field(isar0, 8) >= 1)
		hwcap |= 1UL << 5;		// SHA1
	if (field(isar0, 12) >= 1)
		hwcap |= 1UL << 6;		// SHA2
	if (field(isar0, 12) >= 2)
		hwcap |= 1UL << 21;		// SHA512
	if (field(isar0, 16) >= 1)
		hwcap |= 1UL << 7;		// CRC32
	if (field(isar0, 20) >= 2)
		hwcap |= 1UL << 8;		// ATOMICS (LSE)
	if (field(isar0, 28) >= 1)
		hwcap |= 1UL << 12;		// ASIMDRDM
	if (field(isar0, 32) >= 1)
		hwcap |= 1UL << 17;		// SHA3
	if (field(isar0, 36) >= 1)
		hwcap |= 1UL << 18;		// SM3
	if (field(isar0, 40) >= 1)
		hwcap |= 1UL << 19;		// SM4
	if (field(isar0, 44) >= 1)
		hwcap |= 1UL << 20;		// ASIMDDP
	if (field(isar0, 48) >= 1)
		hwcap |= 1UL << 23;		// ASIMDFHM
	if (field(isar0, 52) >= 1)
		hwcap |= 1UL << 27;		// FLAGM
	if (field(isar0, 52) >= 2)
		hwcap2 |= 1UL << 7;		// FLAGM2
	if (field(isar0, 60) >= 1)
		hwcap2 |= 1UL << 16;	// RNG

	if (field(isar1, 0) >= 1)
		hwcap |= 1UL << 16;		// DCPOP
	if (field(isar1, 0) >= 2)
		hwcap2 |= 1UL << 0;		// DCPODP
	if (field(isar1, 12) >= 1)
		hwcap |= 1UL << 13;		// JSCVT
	if (field(isar1, 16) >= 1)
		hwcap |= 1UL << 14;		// FCMA
	if (field(isar1, 20) >= 1)
		hwcap |= 1UL << 15;		// LRCPC
	if (field(isar1, 20) >= 2)
		hwcap |= 1UL << 26;		// ILRCPC
	if (field(isar1, 32) >= 1)
		hwcap2 |= 1UL << 8;		// FRINT
	if (field(isar1, 36) >= 1)
		hwcap |= 1UL << 29;		// SB

	caps[0] = hwcap;
	caps[1] = hwcap2;
}


/*!	Work per clock of a core design, against 1024 for the big out-of-order
	cores of its time (the scale of the device tree's capacity-dmips-mhz),
	for device trees without that property; 0 when the design is unknown.
	Rough classes only: a table that cannot tell cores apart is no worse
	than knowing nothing, the device tree's own figures win.
*/
static uint32
arm64_core_capacity(uint64 midr)
{
	uint32 part = CPU_PART(midr);
	switch (CPU_IMPL(midr)) {
		case CPU_IMPL_ARM:
			switch (part) {
				case CPU_PART_CORTEX_A34:
				case CPU_PART_CORTEX_A35:
				case CPU_PART_CORTEX_A53:
				case CPU_PART_CORTEX_A55:
				case CPU_PART_CORTEX_A510:
				case CPU_PART_CORTEX_A520:
				case CPU_PART_C1_NANO:
					return 400;
				case CPU_PART_CORTEX_A57:
				case CPU_PART_CORTEX_A72:
				case CPU_PART_CORTEX_A73:
					return 650;
				case CPU_PART_CORTEX_X1:
				case CPU_PART_CORTEX_X1C:
				case CPU_PART_CORTEX_X2:
				case CPU_PART_CORTEX_X3:
				case CPU_PART_CORTEX_X4:
				case CPU_PART_CORTEX_X925:
				case CPU_PART_C1_ULTRA:
				case CPU_PART_C1_PREMIUM:
				case CPU_PART_NEOVERSE_V1:
				case CPU_PART_NEOVERSE_V2:
				case CPU_PART_NEOVERSE_V3:
				case CPU_PART_NEOVERSE_V3AE:
					return 1300;
				default:
					return 1024;
			}
		case CPU_IMPL_QUALCOMM:
			return part == CPU_PART_KRYO400_SILVER ? 400
				: part == CPU_PART_KRYO400_GOLD ? 1024 : 0;
		case CPU_IMPL_APPLE:
			switch (part) {
				case CPU_PART_M1_ICESTORM:
				case CPU_PART_M1_ICESTORM_PRO:
				case CPU_PART_M1_ICESTORM_MAX:
				case CPU_PART_M2_BLIZZARD:
				case CPU_PART_M2_BLIZZARD_PRO:
					return 500;
				case CPU_PART_M1_FIRESTORM:
				case CPU_PART_M1_FIRESTORM_PRO:
				case CPU_PART_M1_FIRESTORM_MAX:
				case CPU_PART_M2_AVALANCHE:
					return 1300;
				default:
					return 0;
			}
		default:
			return 0;
	}
}


/*!	The CPU's capacity for the scheduler: the device tree's
	capacity-dmips-mhz (or the core design's class) times its top frequency
	when the device tree has one.
*/
static uint32
arm64_cpu_capacity(uint64 mpidr, uint64 midr)
{
	int node = -1;
	if (gFDT != NULL && fdt_check_header(gFDT) == 0)
		node = arm64_fdt_cpu_node(gFDT, mpidr);

	uint32 perClock = 0;
	if (node >= 0) {
		int length;
		const fdt32_t* dmips = (const fdt32_t*)fdt_getprop(gFDT, node,
			"capacity-dmips-mhz", &length);
		if (dmips != NULL && length == 4)
			perClock = fdt32_to_cpu(*dmips);
	}
	if (perClock == 0)
		perClock = arm64_core_capacity(midr);
	if (perClock == 0)
		return 0;

	uint64 frequency = node >= 0 ? arm64_fdt_cpu_frequency(gFDT, node) : 0;
	uint32 megahertz = frequency != 0 ? frequency / 1000000 : 1000;
	return perClock * megahertz;
}


status_t
arch_cpu_init_percpu(kernel_args *args, int curr_cpu)
{
	uint64_t tcr = READ_SPECIALREG(TCR_EL1);
	uint64_t mmfr1 = READ_SPECIALREG(ID_AA64MMFR1_EL1);

	uint64_t hafdbs = ID_AA64MMFR1_HAFDBS(mmfr1);
	if (hafdbs == ID_AA64MMFR1_HAFDBS_AF) {
		tcr |= (1UL << 39);
	}
	if (hafdbs == ID_AA64MMFR1_HAFDBS_AF_DBS) {
		tcr |= (1UL << 40) | (1UL << 39);
	}

	WRITE_SPECIALREG(TCR_EL1, tcr);

	gCPU[curr_cpu].arch.mpidr = READ_SPECIALREG(MPIDR_EL1);
	gCPU[curr_cpu].arch.midr = READ_SPECIALREG(MIDR_EL1);
	gCPU[curr_cpu].capacity = arm64_cpu_capacity(gCPU[curr_cpu].arch.mpidr,
		gCPU[curr_cpu].arch.midr);

	uint64 caps[2];
	arm64_cpu_hwcaps(caps);
	atomic_and64(&gARM64HWCaps[0], caps[0]);
	atomic_and64(&gARM64HWCaps[1], caps[1]);

	return 0;
}


status_t
arch_cpu_init(kernel_args *args)
{
	sPsciConduit = args->arch_args.psci_conduit;
	if (sPsciConduit == ARM64_PSCI_SMC || sPsciConduit == ARM64_PSCI_HVC) {
		int32 version = psci_call(kPsciVersion);
		if (version >= 2) {
			dprintf("PSCI: version %" B_PRId32 ".%" B_PRId32 " via %s\n",
				version >> 16, version & 0xffff,
				sPsciConduit == ARM64_PSCI_SMC ? "SMC" : "HVC");
		} else {
			dprintf("PSCI: unsupported version response %" B_PRId32 "\n", version);
			sPsciConduit = ARM64_PSCI_NONE;
		}
	} else
		sPsciConduit = ARM64_PSCI_NONE;

	for (uint32 i = 0; i < args->num_cpus; i++) {
		cpu_ent* cpu = &gCPU[i];

		cpu->topology_id[CPU_TOPOLOGY_PACKAGE] = 0;
		cpu->topology_id[CPU_TOPOLOGY_CORE] = i;
		cpu->topology_id[CPU_TOPOLOGY_SMT] = 0;
	}
	return B_OK;
}


static uint32
fdt_cells(const void* fdt, int node, const char* name)
{
	const fdt32_t* cells = (const fdt32_t*)fdt_getprop(fdt, node, name, NULL);
	return cells != NULL ? fdt32_to_cpu(*cells) : 0;
}


/*!	Boards without PSCI need another way to restart. The Raspberry Pi 4's
	firmware provides none, but its power management block has a watchdog
	that resets the SoC (Linux: bcm2835_wdt.c).
*/
static void
init_bcm2835_watchdog(kernel_args* args)
{
	const void* fdt = args->arch_args.fdt;
	if (fdt == NULL || fdt_check_header(fdt) != 0)
		return;

	int node = fdt_node_offset_by_compatible(fdt, -1, "brcm,bcm2835-pm-wdt");
	if (node < 0)
		return;

	int bus = fdt_parent_offset(fdt, node);
	int length;
	const fdt32_t* reg = (const fdt32_t*)fdt_getprop(fdt, node, "reg", &length);
	const fdt32_t* ranges = bus >= 0
		? (const fdt32_t*)fdt_getprop(fdt, bus, "ranges", &length) : NULL;
	if (reg == NULL || ranges == NULL || fdt_cells(fdt, bus, "#address-cells") != 1
		|| fdt_cells(fdt, bus, "#size-cells") != 1
		|| fdt_cells(fdt, fdt_parent_offset(fdt, bus), "#address-cells") != 2) {
		return;
	}

	// <child address, parent address (two cells), size>
	uint64 address = fdt32_to_cpu(reg[0]);
	for (int i = 0; i + 4 <= length / 4; i += 4) {
		uint64 child = fdt32_to_cpu(ranges[i]);
		uint64 parent = (uint64)fdt32_to_cpu(ranges[i + 1]) << 32
			| fdt32_to_cpu(ranges[i + 2]);
		uint64 size = fdt32_to_cpu(ranges[i + 3]);
		if (address >= child && address - child < size) {
			address = address - child + parent;
			break;
		}
	}

	void* mapped;
	area_id area = map_physical_memory("bcm2835 watchdog",
		address & ~(phys_addr_t)(B_PAGE_SIZE - 1), B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&mapped);
	if (area < 0)
		return;

	sBcm2835Watchdog = (volatile uint32*)((addr_t)mapped
		+ (address & (B_PAGE_SIZE - 1)));
	dprintf("restart and shutdown through the BCM2835 watchdog at %#" B_PRIx64
		"\n", address);
}


static void
bcm2835_watchdog_shutdown(bool reboot)
{
	const uint32 kPassword = 0x5a000000;
	const uint32 kResetControl = 0x1c / 4;
	const uint32 kResetStatus = 0x20 / 4;
	const uint32 kWatchdog = 0x24 / 4;

	if (!reboot) {
		// Boot partition 63 tells the firmware to stay off after the reset.
		sBcm2835Watchdog[kResetStatus]
			= kPassword | sBcm2835Watchdog[kResetStatus] | 0x555;
	}

	// a full reset when the watchdog runs out, which is in ten ticks
	sBcm2835Watchdog[kWatchdog] = kPassword | 10;
	sBcm2835Watchdog[kResetControl]
		= kPassword | (sBcm2835Watchdog[kResetControl] & ~0x30u) | 0x20;
}


status_t
arch_cpu_init_post_vm(kernel_args *args)
{
	if (sPsciConduit == ARM64_PSCI_NONE)
		init_bcm2835_watchdog(args);

	return B_OK;
}


status_t
arch_cpu_init_post_modules(kernel_args *args)
{
	return B_OK;
}


status_t
arch_cpu_shutdown(bool reboot)
{
	if (sPsciConduit == ARM64_PSCI_NONE) {
		if (sBcm2835Watchdog == NULL)
			return B_NOT_SUPPORTED;

		dprintf("watchdog: system %s\n", reboot ? "reset" : "off");
		disable_interrupts();
		bcm2835_watchdog_shutdown(reboot);
		while (true)
			asm volatile("wfi");
	}

	dprintf("PSCI: requesting system %s\n", reboot ? "reset" : "off");
	cpu_status state = disable_interrupts();
	int32 result = psci_call(reboot ? kPsciSystemReset : kPsciSystemOff);
	restore_interrupts(state);

	// A successful SYSTEM_RESET or SYSTEM_OFF never returns.
	dprintf("PSCI: system %s returned %" B_PRId32 "\n",
		reboot ? "reset" : "off", result);
	return B_ERROR;
}


void
arch_cpu_sync_icache(void *address, size_t len)
{
	uint64_t ctr_el0 = 0;
	asm volatile ("mrs\t%0, ctr_el0":"=r" (ctr_el0));

	const uint64_t dcache_line_size = arm64_data_cache_line_size(ctr_el0);
	uint64_t addr = (uint64_t)address;
	uint64_t end = addr + len;

	for (uint64_t address_dcache = ROUNDDOWN(addr, dcache_line_size);
	     address_dcache < end; address_dcache += dcache_line_size) {
		asm volatile ("dc cvau, %0" : : "r"(address_dcache) : "memory");
	}

	asm volatile("dsb ish" : : : "memory");

	// Executable mappings are synchronized through the physical map, whose
	// virtual address can have a different I-cache index from the execution
	// address. Invalidate every alias in the inner-shareable domain. Another
	// CPU can have an aliasing VIPT I-cache even when this CPU uses PIPT.
	asm volatile("ic ialluis" : : : "memory");
	asm volatile("dsb ish" : : : "memory");
	asm volatile("isb" : : : "memory");
}


void
arch_cpu_invalidate_tlb_range(intptr_t, addr_t start, addr_t end)
{
	arch_cpu_global_tlb_invalidate();
}


void
arch_cpu_invalidate_tlb_list(intptr_t, addr_t pages[], int num_pages)
{
	arch_cpu_global_tlb_invalidate();
}


void
arch_cpu_global_tlb_invalidate()
{
	asm(
		"dsb ishst\n"
		"tlbi vmalle1\n"
		"dsb ish\n"
		"isb\n"
	);
}


void
arch_cpu_user_tlb_invalidate(intptr_t)
{
	arch_cpu_global_tlb_invalidate();
}
