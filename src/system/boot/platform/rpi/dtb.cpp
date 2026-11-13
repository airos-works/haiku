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


static inline uint32
read_register(uint64 base, uint32 offset)
{
	return *(volatile uint32*)(base + offset);
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

		// bus 1, device 0, function 0: the RP1
		*(volatile uint32*)(regs + BCM2712_PCIE_EXT_CFG_INDEX) = 1 << 20;
		uint32 low = read_register(regs, BCM2712_PCIE_EXT_CFG_DATA + 0x14);
		uint64 bar = low & ~(uint64)0xf;
		if ((low & 0x6) == 0x4) {
			bar |= (uint64)read_register(regs, BCM2712_PCIE_EXT_CFG_DATA + 0x18)
				<< 32;
		}
		sRP1Bar = bar;

		uint64 cpu = 0;
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
				| (uint64)(read_register(regs,
					BCM2712_PCIE_WIN_LIMIT_HI(window)) & 0xff) << 12;
			if (baseMB == 0 || limitMB < baseMB)
				continue;
			uint64 size = (limitMB - baseMB + 1) << 20;
			if (bar >= pciBase && bar - pciBase < size) {
				cpu = (baseMB << 20) + (bar - pciBase);
				break;
			}
		}
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
		dprintf("RP1: BAR 1 at PCI %#" B_PRIx64 ", CPU %#" B_PRIx64 "\n",
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
	uint32 size = fdt_totalsize(gFDT) + 1024;
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
