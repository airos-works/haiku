/*
 * Copyright 2026, air/OS. All rights reserved.
 * Copyright 2019-2022 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	What the loader takes from the device tree that the firmware hands over:
	the memory layout, the boot archive, the kernel command line, and the
	devices the kernel needs before it can read the tree itself. */


#include "dtb.h"

#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>

#include <arch/arm/arch_uart_pl011.h>
#include <boot/kernel_args.h>
#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <kernel.h>

extern "C" {
#include <libfdt.h>
}

#include "mailbox.h"
#include "serial.h"


#define GIC_INTERRUPT_TYPE_SPI		0
#define GIC_INTERRUPT_TYPE_PPI		1
#define GIC_INTERRUPT_BASE_SPI		32
#define GIC_INTERRUPT_BASE_PPI		16

// what the firmware clocks the PL011 with unless config.txt says otherwise
#define PL011_DEFAULT_CLOCK			48000000


void* gFDT = NULL;
addr_range gBootArchive = {0, 0};
fdt_cpu gFDTCpus[MAX_FDT_CPUS];
uint32 gFDTCpuCount = 0;
bool gBCM2712 = false;
uint32 gPsciConduit = ARM64_PSCI_NONE;


static uint64
read_cells(const uint32*& cells, uint32 count)
{
	uint64 value = 0;
	for (uint32 i = 0; i < count; i++)
		value = (value << 32) | fdt32_to_cpu(*cells++);
	return value;
}


static uint32
get_cell_count(int node, const char* property, uint32 defaultCount)
{
	if (node < 0)
		return defaultCount;
	const uint32* cells = (const uint32*)fdt_getprop(gFDT, node, property,
		NULL);
	return cells != NULL ? fdt32_to_cpu(*cells) : defaultCount;
}


/*!	Translates \a address of a child of \a parent into a CPU address, through
	the "ranges" of all buses on the way up.
*/
static uint64
translate_address(int parent, uint64 address)
{
	while (parent >= 0) {
		int grandParent = fdt_parent_offset(gFDT, parent);
		if (grandParent < 0)
			break;

		int length;
		const uint32* ranges = (const uint32*)fdt_getprop(gFDT, parent,
			"ranges", &length);
		if (ranges != NULL && length > 0) {
			uint32 childCells = get_cell_count(parent, "#address-cells", 2);
			uint32 sizeCells = get_cell_count(parent, "#size-cells", 1);
			uint32 parentCells = get_cell_count(grandParent, "#address-cells",
				2);
			const uint32* end = ranges + length / 4;
			while (ranges + childCells + parentCells + sizeCells <= end) {
				uint64 childBase = read_cells(ranges, childCells);
				uint64 parentBase = read_cells(ranges, parentCells);
				uint64 size = read_cells(ranges, sizeCells);
				if (address >= childBase && address - childBase < size) {
					address = address - childBase + parentBase;
					break;
				}
			}
		}

		parent = grandParent;
	}

	return address;
}


bool
fdt_get_reg(int node, size_t index, addr_range& range)
{
	int parent = fdt_parent_offset(gFDT, node);
	uint32 addressCells = get_cell_count(parent, "#address-cells", 2);
	uint32 sizeCells = get_cell_count(parent, "#size-cells", 1);

	int length;
	const uint32* reg = (const uint32*)fdt_getprop(gFDT, node, "reg", &length);
	if (reg == NULL)
		return false;
	if ((index + 1) * (addressCells + sizeCells) * 4 > (size_t)length)
		return false;

	reg += index * (addressCells + sizeCells);
	range.start = translate_address(parent, read_cells(reg, addressCells));
	range.size = read_cells(reg, sizeCells);
	return true;
}


bool
fdt_find_compatible_reg(const char* compatible, addr_range& range)
{
	int node = fdt_node_offset_by_compatible(gFDT, -1, compatible);
	if (node < 0)
		return false;
	return fdt_get_reg(node, 0, range);
}


static bool
is_enabled(int node)
{
	const char* status = (const char*)fdt_getprop(gFDT, node, "status", NULL);
	return status == NULL || strcmp(status, "okay") == 0
		|| strcmp(status, "ok") == 0;
}


static uint32
get_gic_interrupt(int node)
{
	int length;
	const uint32* interrupts = (const uint32*)fdt_getprop(gFDT, node,
		"interrupts", &length);
	if (interrupts == NULL || length < 12)
		return 0;

	uint32 type = fdt32_to_cpu(interrupts[0]);
	uint32 number = fdt32_to_cpu(interrupts[1]);
	if (type == GIC_INTERRUPT_TYPE_SPI)
		return number + GIC_INTERRUPT_BASE_SPI;
	return number + GIC_INTERRUPT_BASE_PPI;
}


static void
reserve_range(uint64 start, uint64 size)
{
	if (size == 0)
		return;

	uint64 end = ROUNDUP(start + size, B_PAGE_SIZE);
	start = ROUNDDOWN(start, B_PAGE_SIZE);

	// only what lies in RAM is of interest
	for (uint32 i = 0; i < gKernelArgs.num_physical_memory_ranges; i++) {
		const addr_range& range = gKernelArgs.physical_memory_range[i];
		uint64 from = start > range.start ? start : range.start;
		uint64 to = end < range.start + range.size
			? end : range.start + range.size;
		if (from < to)
			insert_physical_allocated_range(from, to - from);
	}
}


static void
init_memory()
{
	int node = -1;
	while ((node = fdt_node_offset_by_prop_value(gFDT, node, "device_type",
			"memory", 7)) >= 0) {
		addr_range range;
		for (size_t i = 0; fdt_get_reg(node, i, range); i++) {
			if (range.size == 0)
				continue;
			dprintf("memory: %#" B_PRIx64 " - %#" B_PRIx64 "\n", range.start,
				range.start + range.size);
			insert_physical_memory_range(range.start, range.size);
		}
	}

	if (gKernelArgs.num_physical_memory_ranges == 0)
		panic("The device tree describes no memory");

	// the tree itself
	reserve_range((addr_t)gFDT, fdt_totalsize(gFDT));

	int count = fdt_num_mem_rsv(gFDT);
	for (int i = 0; i < count; i++) {
		uint64 start, size;
		if (fdt_get_mem_rsv(gFDT, i, &start, &size) == 0) {
			dprintf("reserved: %#" B_PRIx64 " - %#" B_PRIx64 "\n", start,
				start + size);
			reserve_range(start, size);
		}
	}

	int reserved = fdt_path_offset(gFDT, "/reserved-memory");
	if (reserved >= 0) {
		int child;
		fdt_for_each_subnode(child, gFDT, reserved) {
			addr_range range;
			for (size_t i = 0; fdt_get_reg(child, i, range); i++) {
				dprintf("reserved: %#" B_PRIx64 " - %#" B_PRIx64 " (%s)\n",
					range.start, range.start + range.size,
					fdt_get_name(gFDT, child, NULL));
				reserve_range(range.start, range.size);
			}
		}
	}
}


static void
init_chosen()
{
	int chosen = fdt_path_offset(gFDT, "/chosen");
	if (chosen < 0)
		return;

	int startLength, endLength;
	const uint32* start = (const uint32*)fdt_getprop(gFDT, chosen,
		"linux,initrd-start", &startLength);
	const uint32* end = (const uint32*)fdt_getprop(gFDT, chosen,
		"linux,initrd-end", &endLength);
	if (start != NULL && end != NULL) {
		gBootArchive.start = read_cells(start, startLength / 4);
		gBootArchive.size = read_cells(end, endLength / 4) - gBootArchive.start;
		dprintf("boot archive: %#" B_PRIx64 ", %" B_PRIu64 " bytes\n",
			gBootArchive.start, gBootArchive.size);
		reserve_range(gBootArchive.start, gBootArchive.size);
	}
}


const char*
fdt_boot_arguments()
{
	int chosen = fdt_path_offset(gFDT, "/chosen");
	if (chosen < 0)
		return "";
	const char* arguments = (const char*)fdt_getprop(gFDT, chosen, "bootargs",
		NULL);
	return arguments != NULL ? arguments : "";
}


/*!	The rate of the fixed clock that is the first of \a node's "clocks", or 0.
*/
static uint32
get_fixed_clock_rate(int node)
{
	const uint32* clocks = (const uint32*)fdt_getprop(gFDT, node, "clocks",
		NULL);
	if (clocks == NULL)
		return 0;
	int clock = fdt_node_offset_by_phandle(gFDT, fdt32_to_cpu(*clocks));
	if (clock < 0 || fdt_node_check_compatible(gFDT, clock, "fixed-clock") != 0)
		return 0;
	const uint32* frequency = (const uint32*)fdt_getprop(gFDT, clock,
		"clock-frequency", NULL);
	return frequency != NULL ? fdt32_to_cpu(*frequency) : 0;
}


// The Pi 5's PCIe x4 controller (brcmstb) with the RP1 behind it
#define BCM2712_PCIE_STATUS				0x4068
#define BCM2712_PCIE_LINK_UP			0x30	// PHYLINKUP, DL_ACTIVE
#define BCM2712_PCIE_WIN_PCI_LO(w)		(0x400c + (w) * 8)
#define BCM2712_PCIE_WIN_PCI_HI(w)		(0x4010 + (w) * 8)
#define BCM2712_PCIE_WIN_BASE_LIMIT(w)	(0x4070 + (w) * 4)
#define BCM2712_PCIE_WIN_BASE_HI(w)		(0x4080 + (w) * 8)
#define BCM2712_PCIE_WIN_LIMIT_HI(w)	(0x4084 + (w) * 8)
#define BCM2712_PCIE_EXT_CFG_DATA		0x8000
#define BCM2712_PCIE_EXT_CFG_INDEX		0x9000

static uint64 sRP1Base = 0;
static uint64 sRP1Bar = 0;
static uint64 sRP1Controller = 0;
static uint32 sRP1BusNumbers = 0;
static uint32 sRP1Id = 0;


static inline uint32
read_register(uint64 base, uint32 offset)
{
	return *(volatile uint32*)(base + offset);
}


/*!	The CPU address at which the controller's outbound windows show the PCI
	address \a pci, or 0.
*/
static uint64
pci_to_cpu(uint64 regs, uint64 pci)
{
	for (uint32 window = 0; window < 4; window++) {
		uint64 pciBase = read_register(regs, BCM2712_PCIE_WIN_PCI_LO(window))
			| (uint64)read_register(regs, BCM2712_PCIE_WIN_PCI_HI(window))
				<< 32;
		uint32 baseLimit = read_register(regs,
			BCM2712_PCIE_WIN_BASE_LIMIT(window));
		uint64 baseMB = ((baseLimit >> 4) & 0xfff)
			| (uint64)(read_register(regs, BCM2712_PCIE_WIN_BASE_HI(window))
				& 0xff) << 12;
		uint64 limitMB = (baseLimit >> 20)
			| (uint64)(read_register(regs, BCM2712_PCIE_WIN_LIMIT_HI(window))
				& 0xff) << 12;
		if (baseMB == 0 || limitMB < baseMB)
			continue;
		uint64 size = (limitMB - baseMB + 1) << 20;
		if (pci >= pciBase && pci - pciBase < size)
			return (baseMB << 20) + (pci - pciBase);
	}
	return 0;
}


static inline uint32
rp1_config_read(uint32 offset)
{
	*(volatile uint32*)(sRP1Controller + BCM2712_PCIE_EXT_CFG_INDEX) = 1 << 20;
	return read_register(sRP1Controller, BCM2712_PCIE_EXT_CFG_DATA + offset);
}


static inline void
rp1_config_write(uint32 offset, uint32 value)
{
	*(volatile uint32*)(sRP1Controller + BCM2712_PCIE_EXT_CFG_INDEX) = 1 << 20;
	*(volatile uint32*)(sRP1Controller + BCM2712_PCIE_EXT_CFG_DATA + offset)
		= value;
}


/*!	The firmware puts the RP1 where it likes when it leaves the PCIe link up
	for the operating system ("pciex4_reset=0"): at 0x1c_0000_0000, where
	the device tree's "ranges" say 0x1f_0000_0000. Find out from the
	controller's outbound windows and the RP1's BAR 1 (its peripherals) where
	it really is, and correct the RP1's "ranges" in the tree, which the
	kernel gets too.
*/
static void
fixup_rp1()
{
	int pcie = -1;
	while ((pcie = fdt_node_offset_by_compatible(gFDT, pcie,
			"brcm,bcm2712-pcie")) >= 0) {
		int rp1 = fdt_subnode_offset(gFDT, pcie, "rp1");
		addr_range controller;
		if (rp1 < 0 || !fdt_get_reg(pcie, 0, controller))
			continue;
		uint64 regs = controller.start;
		if ((read_register(regs, BCM2712_PCIE_STATUS) & BCM2712_PCIE_LINK_UP)
				!= BCM2712_PCIE_LINK_UP) {
			continue;
		}
		sRP1Controller = regs;

		// The root port's own configuration space is at the start of the
		// registers. Configuration accesses to the RP1 only go out with the
		// bridge's bus numbers set: primary 0, secondary and subordinate 1.
		sRP1BusNumbers = read_register(regs, 0x18);
		if (((sRP1BusNumbers >> 8) & 0xff) != 1) {
			*(volatile uint32*)(regs + 0x18)
				= (sRP1BusNumbers & 0xff000000) | 0x010100;
		}

		// bus 1, device 0, function 0: the RP1
		*(volatile uint32*)(regs + BCM2712_PCIE_EXT_CFG_INDEX) = 1 << 20;
		sRP1Id = read_register(regs, BCM2712_PCIE_EXT_CFG_DATA);
		uint64 bar;
		if (sRP1Id != 0xffffffff) {
			uint32 low = read_register(regs, BCM2712_PCIE_EXT_CFG_DATA + 0x14);
			bar = low & ~(uint64)0xf;
			if ((low & 0x6) == 0x4) {
				bar |= (uint64)read_register(regs,
					BCM2712_PCIE_EXT_CFG_DATA + 0x18) << 32;
			}
		} else {
			// The firmware puts BAR 1 at the start of its first window.
			bar = read_register(regs, BCM2712_PCIE_WIN_PCI_LO(0))
				| (uint64)read_register(regs, BCM2712_PCIE_WIN_PCI_HI(0)) << 32;
		}
		sRP1Bar = bar;

		uint64 cpu = pci_to_cpu(regs, bar);
		if (cpu == 0)
			continue;
		sRP1Base = cpu;

		// The PCI address that the controller's "ranges" turn into that CPU
		// address; the RP1's "ranges" are <child (2), PCI (3), size (2)>.
		int length;
		const uint32* ranges = (const uint32*)fdt_getprop(gFDT, pcie, "ranges",
			&length);
		const uint32* end = ranges != NULL ? ranges + length / 4 : NULL;
		while (ranges != NULL && ranges + 7 <= end) {
			uint32 flags = fdt32_to_cpu(ranges[0]);
			const uint32* cells = ranges + 1;
			uint64 child = read_cells(cells, 2);
			uint64 parent = read_cells(cells, 2);
			uint64 size = read_cells(cells, 2);
			ranges = cells;
			if (cpu < parent || cpu - parent >= size)
				continue;

			uint64 pci = cpu - parent + child;
			const uint32* rp1Ranges = (const uint32*)fdt_getprop(gFDT, rp1,
				"ranges", &length);
			if (rp1Ranges == NULL || length < 7 * 4)
				break;
			uint32 patched[7];
			memcpy(patched, rp1Ranges, sizeof(patched));
			patched[2] = cpu_to_fdt32(flags);
			patched[3] = cpu_to_fdt32((uint32)(pci >> 32));
			patched[4] = cpu_to_fdt32((uint32)pci);
			fdt_setprop_inplace_namelen_partial(gFDT, rp1, "ranges", 6, 0,
				patched, sizeof(patched));
			break;
		}
	}
}


/*!	Whether the registers of \a node can be read. The Pi 5's UARTs on the GPIO
	header are in the RP1, behind the PCIe x4 link: with the link down, or the
	RP1 not where the tree says, an access stalls the CPU. The firmware
	leaves the link up with "pciex4_reset=0" in config.txt.
*/
static bool
is_reachable(int node)
{
	for (int parent = fdt_parent_offset(gFDT, node); parent >= 0;
			parent = fdt_parent_offset(gFDT, parent)) {
		if (fdt_node_check_compatible(gFDT, parent, "brcm,bcm2712-pcie") == 0)
			return sRP1Base != 0;
	}
	return true;
}


static void
dump_registers(const char* name, uint64 base, uint32 from, uint32 to)
{
	for (uint32 offset = from; offset < to; offset += 16) {
		dprintf("%s %04" B_PRIx32 ": %08" B_PRIx32 " %08" B_PRIx32 " %08"
			B_PRIx32 " %08" B_PRIx32 "\n", name, offset,
			read_register(base, offset), read_register(base, offset + 4),
			read_register(base, offset + 8), read_register(base, offset + 12));
	}
}


/*!	"airos.rp1dump": what the firmware left in the PCIe controller of the
	RP1, the RP1's configuration space and the MSI controller (MIP), for
	working out the RP1's interrupts and DMA.
*/
void
fdt_dump_rp1_state()
{
	if (sRP1Controller == 0)
		return;
	uint64 regs = sRP1Controller;
	dump_registers("pcie", regs, 0, 0x40);
	dump_registers("pcie", regs, 0x4000, 0x4180);
	dump_registers("pcie", regs, 0x4300, 0x4320);
	dump_registers("pcie", regs, 0x4500, 0x4520);
	*(volatile uint32*)(regs + BCM2712_PCIE_EXT_CFG_INDEX) = 1 << 20;
	dump_registers("rp1cfg", regs + BCM2712_PCIE_EXT_CFG_DATA, 0, 0x100);
	addr_range mip;
	if (fdt_find_compatible_reg("brcm,bcm2712-mip", mip))
		dump_registers("mip", mip.start, 0, 0xc0);
	// the RP1's own MSI-X configuration and interrupt status (PCIE_APBS)
	dump_registers("rp1apbs", sRP1Base + 0x108000, 0, 0x110);
}


static bool
init_uart_node(int node)
{
	if (node < 0 || !is_enabled(node))
		return false;

	uart_info& uart = gKernelArgs.arch_args.uart;
	const char* kind;
	if (fdt_node_check_compatible(gFDT, node, "arm,pl011") == 0
		|| fdt_node_check_compatible(gFDT, node, "arm,pl011-axi") == 0) {
		kind = UART_KIND_PL011;
	} else
		return false;

	addr_range regs;
	if (!fdt_get_reg(node, 0, regs) || !is_reachable(node))
		return false;

	uart.regs = regs;
	strlcpy(uart.kind, kind, sizeof(uart.kind));
	uart.irq = get_gic_interrupt(node);
	uart.clock = PL011_DEFAULT_CLOCK;

	const uint32* frequency = (const uint32*)fdt_getprop(gFDT, node,
		"clock-frequency", NULL);
	uint32 rate;
	if (frequency != NULL)
		uart.clock = fdt32_to_cpu(*frequency);
	else if ((rate = get_fixed_clock_rate(node)) != 0)
		uart.clock = rate;
	else if (!gBCM2712
		&& mailbox_get_clock_rate(MAILBOX_CLOCK_UART, false, rate) == B_OK)
		uart.clock = rate;
	else if (gBCM2712) {
		// The RP1's UART clock is the RP1's own; the firmware set the port
		// up at 115200 baud ("enable_rp1_uart=1"): the divisor tells.
		volatile uint32* pl011 = (volatile uint32*)regs.start;
		uint32 integer = pl011[0x24 / 4];
		uint32 fraction = pl011[0x28 / 4] & 0x3f;
		if (integer != 0) {
			uart.clock = (uint32)(((uint64)integer * 64 + fraction)
				* 16 * 115200 / 64);
		} else
			uart.clock = 50000000;
	}

	return true;
}


static void
init_uart()
{
	// On the Pi 5 the "console" alias follows dtparam=uart0_console: the
	// RP1's UART on GPIO 14/15 instead of the debug connector.
	if (gBCM2712 && init_uart_node(fdt_path_offset(gFDT, "console")))
		return;

	// The console the firmware names, then the usual alias.
	int chosen = fdt_path_offset(gFDT, "/chosen");
	if (chosen >= 0) {
		int length;
		const char* path = (const char*)fdt_getprop(gFDT, chosen,
			"stdout-path", &length);
		if (path != NULL) {
			const char* separator = strchr(path, ':');
			int nameLength = separator != NULL
				? separator - path : (int)strlen(path);
			if (init_uart_node(fdt_path_offset_namelen(gFDT, path,
					nameLength))) {
				return;
			}
		}
	}

	// Only a PL011 is taken, and only as the console: without the disable-bt
	// overlay the PL011 talks to the Bluetooth chip and the console is the
	// mini UART, which the kernel has no driver for.
	init_uart_node(fdt_path_offset(gFDT, "serial0"));
}


static void
init_interrupt_controller()
{
	static const char* const kCompatible[]
		= {"arm,gic-400", "arm,cortex-a15-gic"};

	intc_info& controller = gKernelArgs.arch_args.interrupt_controller;
	for (size_t i = 0; i < B_COUNT_OF(kCompatible); i++) {
		int node = fdt_node_offset_by_compatible(gFDT, -1, kCompatible[i]);
		if (node < 0)
			continue;

		strlcpy(controller.kind, INTC_KIND_GICV2, sizeof(controller.kind));
		fdt_get_reg(node, 0, controller.regs1);
		fdt_get_reg(node, 1, controller.regs2);
		return;
	}
}


static void
init_cpus()
{
	int cpus = fdt_path_offset(gFDT, "/cpus");
	if (cpus < 0)
		return;

	uint32 addressCells = get_cell_count(cpus, "#address-cells", 1);

	int node;
	fdt_for_each_subnode(node, gFDT, cpus) {
		const char* type = (const char*)fdt_getprop(gFDT, node, "device_type",
			NULL);
		if (type == NULL || strcmp(type, "cpu") != 0)
			continue;
		if (gFDTCpuCount == MAX_FDT_CPUS)
			break;

		fdt_cpu& cpu = gFDTCpus[gFDTCpuCount++];
		const uint32* reg = (const uint32*)fdt_getprop(gFDT, node, "reg",
			NULL);
		cpu.mpidr = reg != NULL ? read_cells(reg, addressCells) : 0;
		cpu.releaseAddress = 0;
		cpu.psci = false;

		const char* method = (const char*)fdt_getprop(gFDT, node,
			"enable-method", NULL);
		int length;
		const uint32* release = (const uint32*)fdt_getprop(gFDT, node,
			"cpu-release-addr", &length);
		if (method != NULL && strcmp(method, "spin-table") == 0
			&& release != NULL) {
			cpu.releaseAddress = read_cells(release, length / 4);
		} else if (method != NULL && strcmp(method, "psci") == 0
			&& gPsciConduit != ARM64_PSCI_NONE) {
			cpu.psci = true;
		}
	}
}


static void
init_psci()
{
	int node = fdt_path_offset(gFDT, "/psci");
	if (node < 0 || !is_enabled(node))
		return;
	const char* method = (const char*)fdt_getprop(gFDT, node, "method", NULL);
	if (method == NULL)
		return;
	if (strcmp(method, "smc") == 0)
		gPsciConduit = ARM64_PSCI_SMC;
	else if (strcmp(method, "hvc") == 0)
		gPsciConduit = ARM64_PSCI_HVC;
}


void
fdt_init(void* fdt)
{
	if (fdt == NULL || fdt_check_header(fdt) != 0)
		panic("No device tree from the firmware");
	gFDT = fdt;
	gBCM2712 = fdt_node_check_compatible(fdt, 0, "brcm,bcm2712") == 0;

	if (gBCM2712)
		fixup_rp1();
	mailbox_init();
	init_uart();
	serial_init();
		// from here on dprintf() reaches the UART
	if (sRP1Bar != 0) {
		dprintf("RP1: ID %#" B_PRIx32 ", bus numbers %#" B_PRIx32 ", BAR 1 at "
			"PCI %#" B_PRIx64 ", CPU %#" B_PRIx64 "\n", sRP1Id, sRP1BusNumbers,
			sRP1Bar, sRP1Base);
	}

	dprintf("device tree at %p, %u bytes: %s\n", fdt,
		(unsigned)fdt_totalsize(fdt),
		(const char*)fdt_getprop(fdt, 0, "model", NULL));

	init_memory();
	init_chosen();
	init_interrupt_controller();
	init_psci();
	init_cpus();

	// The kernel restarts and powers off through PSCI where there is one.
	gKernelArgs.arch_args.psci_conduit = gPsciConduit;
}


// The RP1's interrupts (Linux: drivers/mfd/rp1.c, irq-bcm2712-mip.c)
#define RP1_PCIE_APBS			0x108000
#define RP1_REG_SET				0x800
#define RP1_MSIX_CFG(irq)		(0x8 + (irq) * 4)
#define RP1_MSIX_CFG_ENABLE		(1 << 0)
#define RP1_MSIX_CFG_IACK_EN	(1 << 3)
#define RP1_IRQ_COUNT			61
#define MIP_INT_CFGL_HOST		0x20
#define MIP_INT_CFGH_HOST		0x30
#define MIP_INT_MASKL_HOST		0x40
#define MIP_INT_MASKH_HOST		0x50
#define MIP_INT_MASKL_VPU		0x60
#define MIP_INT_MASKH_VPU		0x70
#define BCM2712_PCIE_RC_BAR(n)	((n) <= 3 ? 0x402c + ((n) - 1) * 8 \
	: 0x40d4 + ((n) - 4) * 8)
#define BCM2712_PCIE_UBUS_BAR(n) ((n) <= 3 ? 0x40ac + ((n) - 1) * 8 \
	: 0x410c + ((n) - 4) * 8)


/*!	Makes sure that one of the PCIe controller's inbound windows (RC BARs)
	maps PCI address \a pci to CPU address \a cpu, \a sizeCode (the
	controller's encoding: 0x1c-0x1f 4-32 KB, 1-21 64 KB-64 GB) long.
*/
static bool
add_inbound_window(uint64 pci, uint64 cpu, uint32 sizeCode, const char* what)
{
	uint32 freeBar = 0;
	for (uint32 bar = 1; bar <= 10; bar++) {
		uint32 low = read_register(sRP1Controller, BCM2712_PCIE_RC_BAR(bar));
		uint32 high = read_register(sRP1Controller,
			BCM2712_PCIE_RC_BAR(bar) + 4);
		uint32 code = low & 0x1f;
		if (code == 0) {
			if (freeBar == 0)
				freeBar = bar;
			continue;
		}
		uint64 base = (uint64)high << 32 | (low & ~(uint32)0xfff);
		uint32 log2 = code >= 0x1c ? code - 0x1c + 12 : code + 15;
		if (pci >= base && pci - base < (1ull << log2))
			return true;
	}
	if (freeBar == 0) {
		dprintf("RP1: no inbound window free for %s\n", what);
		return false;
	}

	*(volatile uint32*)(sRP1Controller + BCM2712_PCIE_RC_BAR(freeBar))
		= (uint32)pci | sizeCode;
	*(volatile uint32*)(sRP1Controller + BCM2712_PCIE_RC_BAR(freeBar) + 4)
		= (uint32)(pci >> 32);
	*(volatile uint32*)(sRP1Controller + BCM2712_PCIE_UBUS_BAR(freeBar))
		= ((uint32)cpu & ~(uint32)0xfff) | 1;
	*(volatile uint32*)(sRP1Controller + BCM2712_PCIE_UBUS_BAR(freeBar) + 4)
		= (uint32)(cpu >> 32);
	dprintf("RP1: inbound window %" B_PRIu32 " for %s\n", freeBar, what);
	return true;
}


/*!	Makes the RP1's interrupts plain GIC interrupts for the kernel, the way
	Linux sets them up: interrupt N of the RP1 is MSI-X vector N, a write of
	N to the MIP, which raises SPI <msi base> + N, edge triggered. Then the
	RP1's devices in \a tree (the kernel's copy) get those SPIs as their
	interrupts, and drivers need to know nothing about the RP1. Level
	triggered sources are acknowledged through the RP1's MSIX_CFG IACK bit
	("airos,rp1-iack" names the register; the driver writes 1 << 2 there).
*/
static void
setup_rp1_interrupts(void* tree)
{
	if (sRP1Base == 0 || sRP1Controller == 0)
		return;

	int mip = fdt_node_offset_by_compatible(tree, -1, "brcm,bcm2712-mip");
	int length;
	const uint32* ranges = mip >= 0
		? (const uint32*)fdt_getprop(tree, mip, "msi-ranges", &length) : NULL;
	if (ranges == NULL || length != 5 * 4)
		return;
	uint32 gicPhandle = fdt32_to_cpu(ranges[0]);
	uint32 spiBase = fdt32_to_cpu(ranges[2]);
	uint32 vectors = fdt32_to_cpu(ranges[4]);

	// reg 0: the MIP's registers, reg 1: the address MSIs are written to
	const uint32* reg = (const uint32*)fdt_getprop(tree, mip, "reg", &length);
	if (reg == NULL || length < 8 * 4)
		return;
	uint64 mipRegs = (uint64)fdt32_to_cpu(reg[0]) << 32 | fdt32_to_cpu(reg[1]);
	uint64 message = (uint64)fdt32_to_cpu(reg[4]) << 32 | fdt32_to_cpu(reg[5]);
	uint32 count = vectors < RP1_IRQ_COUNT ? vectors : RP1_IRQ_COUNT;

	// The controller has to pass the RP1's MSI writes on to the MIP: an
	// inbound window (RC BAR) for the message address.
	if (!add_inbound_window(message, mipRegs, 0x1c, "the MIP"))
		return;

	// The MIP: everything to the host, nothing to the VPU, edge triggered.
	volatile uint32* mipRegisters = (volatile uint32*)mipRegs;
	mipRegisters[MIP_INT_MASKL_HOST / 4] = 0;
	mipRegisters[MIP_INT_MASKH_HOST / 4] = 0;
	mipRegisters[MIP_INT_MASKL_VPU / 4] = ~0u;
	mipRegisters[MIP_INT_MASKH_VPU / 4] = ~0u;
	mipRegisters[MIP_INT_CFGL_HOST / 4] = ~0u;
	mipRegisters[MIP_INT_CFGH_HOST / 4] = ~0u;

	// memory space and bus mastering (MSIs are memory writes)
	rp1_config_write(0x04, rp1_config_read(0x04) | 0x6);

	// the MSI-X capability
	uint32 capability = rp1_config_read(0x34) & 0xfc;
	while (capability != 0 && (rp1_config_read(capability) & 0xff) != 0x11)
		capability = (rp1_config_read(capability) >> 8) & 0xfc;
	if (capability == 0) {
		dprintf("RP1: no MSI-X capability\n");
		return;
	}
	uint32 control = rp1_config_read(capability) >> 16;
	uint32 tableInfo = rp1_config_read(capability + 4);
	uint32 tableBar = tableInfo & 0x7;
	uint32 barLow = rp1_config_read(0x10 + tableBar * 4);
	uint64 barAddress = barLow & ~(uint64)0xf;
	if ((barLow & 0x6) == 0x4)
		barAddress |= (uint64)rp1_config_read(0x14 + tableBar * 4) << 32;
	uint64 table = pci_to_cpu(sRP1Controller, barAddress);
	if (table == 0) {
		dprintf("RP1: MSI-X table BAR %" B_PRIu32 " not mapped\n", tableBar);
		return;
	}
	table += tableInfo & ~(uint32)0x7;
	if ((control & 0x7ff) + 1 < count)
		count = (control & 0x7ff) + 1;

	volatile uint32* entries = (volatile uint32*)table;
	for (uint32 i = 0; i < count; i++) {
		entries[i * 4 + 0] = (uint32)message;
		entries[i * 4 + 1] = (uint32)(message >> 32);
		entries[i * 4 + 2] = i;
		entries[i * 4 + 3] = 0;	// unmasked
	}
	// MSI-X on, the function's mask off
	rp1_config_write(capability, (rp1_config_read(capability)
		| (1u << 31)) & ~(1u << 30));

	// The RP1's devices: GIC SPIs instead of the RP1's numbers.
	int rp1 = -1;
	int pcie = fdt_node_offset_by_compatible(tree, -1, "brcm,bcm2712-pcie");
	while (pcie >= 0 && rp1 < 0) {
		rp1 = fdt_subnode_offset(tree, pcie, "rp1");
		if (rp1 < 0) {
			pcie = fdt_node_offset_by_compatible(tree, pcie,
				"brcm,bcm2712-pcie");
		}
	}
	if (rp1 < 0)
		return;

	volatile uint32* apbs = (volatile uint32*)(sRP1Base + RP1_PCIE_APBS);
	uint32 converted = 0;
	int node;
	fdt_for_each_subnode(node, tree, rp1) {
		const uint32* interrupts = (const uint32*)fdt_getprop(tree, node,
			"interrupts", &length);
		if (interrupts == NULL || length != 8)
			continue;
		uint32 irq = fdt32_to_cpu(interrupts[0]);
		uint32 type = fdt32_to_cpu(interrupts[1]);
		if (irq >= count)
			continue;

		bool level = (type & 0xc) != 0;
		apbs[(RP1_REG_SET + RP1_MSIX_CFG(irq)) / 4] = RP1_MSIX_CFG_ENABLE
			| (level ? RP1_MSIX_CFG_IACK_EN : 0);

		fdt32_t cells[3] = {cpu_to_fdt32(0), cpu_to_fdt32(spiBase + irq),
			cpu_to_fdt32(1)};
		fdt_setprop(tree, node, "interrupts", cells, sizeof(cells));
		fdt_setprop_u32(tree, node, "interrupt-parent", gicPhandle);
		if (level) {
			fdt_setprop_u64(tree, node, "airos,rp1-iack",
				sRP1Base + RP1_PCIE_APBS + RP1_REG_SET + RP1_MSIX_CFG(irq));
		}
		converted++;
	}
	dprintf("RP1: %" B_PRIu32 " MSI-X vectors to SPI %" B_PRIu32 "+, %"
		B_PRIu32 " devices\n", count, spiBase, converted);
}


/*!	DMA from the RP1's devices (USB, Ethernet) reaches memory at the CPU's
	physical addresses. The firmware maps PCI address 0 to memory 0; Linux,
	whose device tree this is, maps PCI 0x10_0000_0000 there, which the RP1
	may emit for its devices' low addresses. Both are there then, and the
	kernel's tree says the identity ("dma-ranges" empty), which the drivers
	that only know physical addresses rely on.
*/
static void
setup_rp1_dma(void* tree)
{
	if (sRP1Controller == 0)
		return;
	// 64 GB (size code 21)
	if (!add_inbound_window(0, 0, 21, "memory at PCI 0")
		|| !add_inbound_window(0x1000000000ull, 0, 21,
			"memory at PCI 0x10_0000_0000")) {
		return;
	}

	int pcie = -1;
	while ((pcie = fdt_node_offset_by_compatible(tree, pcie,
			"brcm,bcm2712-pcie")) >= 0) {
		int rp1 = fdt_subnode_offset(tree, pcie, "rp1");
		if (rp1 < 0)
			continue;
		fdt_setprop(tree, rp1, "dma-ranges", NULL, 0);
		fdt_setprop(tree, pcie, "dma-ranges", NULL, 0);
	}
}


/*!	Takes the Ethernet PHY out of reset: its reset line is an RP1 GPIO
	("phy-reset-gpios", ETH_RST_N on GPIO 32, active low), which Linux pulses
	when it probes the GEM and the firmware leaves asserted. The RP1's GPIO
	block (Linux: pinctrl-rp1.c) has three register areas, the pins' control
	("io"), the outputs ("rio", with set/clear aliases) and the pads, in
	three banks: GPIO 0-27, 28-33 and 34-53.
*/
static void
release_rp1_phy()
{
	int ethernet = fdt_node_offset_by_compatible(gFDT, -1,
		"raspberrypi,rp1-gem");
	int length;
	const uint32* gpios = ethernet >= 0 ? (const uint32*)fdt_getprop(gFDT,
		ethernet, "phy-reset-gpios", &length) : NULL;
	if (gpios == NULL || length != 12)
		return;
	int controller = fdt_node_offset_by_phandle(gFDT, fdt32_to_cpu(gpios[0]));
	uint32 pin = fdt32_to_cpu(gpios[1]);
	bool activeLow = (fdt32_to_cpu(gpios[2]) & 1) != 0;
	addr_range io, rio, pads;
	if (controller < 0
		|| fdt_node_check_compatible(gFDT, controller, "raspberrypi,rp1-gpio")
			!= 0
		|| !fdt_get_reg(controller, 0, io) || !fdt_get_reg(controller, 1, rio)
		|| !fdt_get_reg(controller, 2, pads) || pin >= 54) {
		return;
	}

	static const uint32 kBankFirst[3] = {0, 28, 34};
	uint32 bank = pin >= 34 ? 2 : pin >= 28 ? 1 : 0;
	uint32 index = pin - kBankFirst[bank];
	uint64 control = io.start + bank * 0x4000 + index * 8 + 4;
	uint64 outputs = rio.start + bank * 0x4000;
	uint64 pad = pads.start + bank * 0x4000 + 4 + index * 4;

	volatile uint32* outSet = (volatile uint32*)(outputs + 0x2000);
	volatile uint32* outClear = (volatile uint32*)(outputs + 0x3000);
	volatile uint32* enableSet = (volatile uint32*)(outputs + 0x2000 + 4);
	uint32 bit = 1u << index;

	// asserted: low for an active low line
	if (activeLow)
		*outClear = bit;
	else
		*outSet = bit;
	*enableSet = bit;
	// the pad: input on, output not disabled
	*(volatile uint32*)pad = (*(volatile uint32*)pad & ~(1u << 7)) | (1u << 6);
	// the pin's function: the output registers (SYS_RIO)
	*(volatile uint32*)control = (*(volatile uint32*)control & ~0x1fu) | 5;
	spin(10000);

	if (activeLow)
		*outSet = bit;
	else
		*outClear = bit;
	spin(20000);
	dprintf("RP1: Ethernet PHY out of reset (GPIO %" B_PRIu32 ")\n", pin);
}


static phys_addr_t sWatchdog = 0;


void
fdt_set_watchdog(phys_addr_t base)
{
	sWatchdog = base;
}


void
fdt_set_kernel_args()
{
	// The kernel gets a copy with room for what is added below; libfdt wants
	// the tree 8-byte aligned.
	uint32 size = fdt_totalsize(gFDT) + 16384;
	void* copy = kernel_args_malloc(size, 8);
	if (copy == NULL || fdt_open_into(gFDT, copy, size) != 0)
		panic("No memory for the device tree");
	gKernelArgs.arch_args.fdt = copy;

	// The firmware owns the clocks. Until the kernel can ask it, tell the
	// SD controller's driver what its clock runs at.
	int node = fdt_node_offset_by_compatible(copy, -1, "brcm,bcm2711-emmc2");
	uint32 rate;
	if (node >= 0
		&& mailbox_get_clock_rate(MAILBOX_CLOCK_EMMC2, false, rate) == B_OK) {
		fdt_setprop_u32(copy, node, "clock-frequency", rate);
		dprintf("EMMC2 clock: %" B_PRIu32 " Hz\n", rate);
	}

	// The Pi 5's SD controller runs from a fixed clock that the kernel's
	// device tree bus cannot follow by its phandle.
	node = -1;
	while ((node = fdt_node_offset_by_compatible(copy, node,
			"brcm,bcm2712-sdhci")) >= 0) {
		const uint32* clocks = (const uint32*)fdt_getprop(copy, node, "clocks",
			NULL);
		int clock = clocks != NULL
			? fdt_node_offset_by_phandle(copy, fdt32_to_cpu(*clocks)) : -1;
		const uint32* frequency = clock >= 0 ? (const uint32*)fdt_getprop(copy,
			clock, "clock-frequency", NULL) : NULL;
		if (frequency != NULL) {
			fdt_setprop_u32(copy, node, "clock-frequency",
				fdt32_to_cpu(*frequency));
		}
	}

	// The firmware's tree has no operating points for the cores either; the
	// kernel reports a core's "clock-frequency" as its maximum speed.
	int cpus = fdt_path_offset(copy, "/cpus");
	if (cpus >= 0
		&& mailbox_get_clock_rate(MAILBOX_CLOCK_ARM, true, rate) == B_OK) {
		fdt_for_each_subnode(node, copy, cpus) {
			const char* type = (const char*)fdt_getprop(copy, node,
				"device_type", NULL);
			if (type != NULL && strcmp(type, "cpu") == 0)
				fdt_setprop_u32(copy, node, "clock-frequency", rate);
		}
		dprintf("ARM clock: up to %" B_PRIu32 " Hz\n", rate);
	}

	if (gBCM2712) {
		setup_rp1_interrupts(copy);
		setup_rp1_dma(copy);
		release_rp1_phy();
	}

	int chosen = fdt_path_offset(copy, "/chosen");
	if (sWatchdog != 0 && chosen >= 0)
		fdt_setprop_u64(copy, chosen, "airos,watchdog", sWatchdog);

	const uart_info& uart = gKernelArgs.arch_args.uart;
	dprintf("UART: %s at %#" B_PRIx64 ", irq %" B_PRIu32 "\n",
		uart.kind[0] != 0 ? uart.kind : "none", uart.regs.start, uart.irq);

	const intc_info& controller = gKernelArgs.arch_args.interrupt_controller;
	dprintf("interrupt controller: %s at %#" B_PRIx64 ", %#" B_PRIx64 "\n",
		controller.kind[0] != 0 ? controller.kind : "none",
		controller.regs1.start, controller.regs2.start);
}
