/*
 * Copyright 2019-2022 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *   Alexander von Gluck IV <kallisti5@unixzen.com>
 */


// TODO: split arch-depending code to per-arch source

#include <arch_cpu_defs.h>
#include <arch_dtb.h>
#include <arch_smp.h>
#include <arch/generic/debug_uart_8250.h>
#if defined(__riscv)
#	include <arch/riscv64/arch_uart_sifive.h>
#elif defined(__ARM__)
#	include <arch/arm/arch_uart_pl011.h>
#elif defined(__aarch64__)
#	include <arch/arm/arch_uart_pl011.h>
#	include <arch/arm64/arch_uart_linflex.h>
#	include <arch/arm64/arch_uart_samsung.h>
#	include <arch/arm64/arch_uart_geni.h>
#endif


#include <boot/addr_range.h>
#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/uart.h>
#include <string.h>
#include <kernel/kernel.h>

#include <ByteOrder.h>

extern "C" {
#include <libfdt.h>
}

#include "efi_platform.h"
#include "serial.h"


#define GIC_INTERRUPT_CELL_TYPE		0
#define GIC_INTERRUPT_CELL_ID		1
#define GIC_INTERRUPT_CELL_FLAGS	2
#define GIC_INTERRUPT_TYPE_SPI		0
#define GIC_INTERRUPT_TYPE_PPI		1
#define GIC_INTERRUPT_BASE_SPI		32
#define GIC_INTERRUPT_BASE_PPI		16


//#define TRACE_DUMP_FDT


#define INFO(x...) dprintf("efi/fdt: " x)
#define ERROR(x...) dprintf("efi/fdt: " x)


static void* sDtbTable = NULL;
static uint32 sDtbSize = 0;

template <typename T> DebugUART*
get_uart(addr_t base, int64 clock) {
	static char buffer[sizeof(T)];
	return new(buffer) T(base, clock);
}


const struct supported_uarts {
	const char*	dtb_compat;
	const char*	kind;
	DebugUART*	(*uart_driver_init)(addr_t base, int64 clock);
} kSupportedUarts[] = {
	{ "ns16550a", UART_KIND_8250, &get_uart<DebugUART8250> },
	{ "ns16550", UART_KIND_8250, &get_uart<DebugUART8250> },
	{ "snps,dw-apb-uart", UART_KIND_8250, &get_uart<DebugUART8250> },
#if defined(__riscv)
	{ "sifive,uart0", UART_KIND_SIFIVE, &get_uart<ArchUARTSifive> },
#elif defined(__ARM__)
	{ "arm,pl011", UART_KIND_PL011, &get_uart<ArchUARTPL011> },
	{ "brcm,bcm2835-aux-uart", UART_KIND_8250, &get_uart<DebugUART8250> },
#elif defined(__aarch64__)
	{ "arm,pl011", UART_KIND_PL011, &get_uart<ArchUARTPL011> },
	{ "fsl,s32-linflexuart", UART_KIND_LINFLEX, &get_uart<ArchUARTlinflex> },
	{ "brcm,bcm2835-aux-uart", UART_KIND_8250, &get_uart<DebugUART8250> },
	{ "apple,s5l-uart", UART_KIND_SAMSUNG, &get_uart<ArchUARTSamsung> },
	// Only used when its clocks are known to run, see kGeniUartClocks.
	{ "qcom,geni-debug-uart", UART_KIND_GENI, &get_uart<ArchUARTGeni> },
#endif
};


#if defined(__aarch64__)
// A GENI serial engine whose clocks are off ends any access to it in a bus
// error that hangs the SoC. The firmware runs them while it owns the console,
// but may drop its votes in ExitBootServices: so the engine is only used on
// SoCs whose clock controller is known here, and its votes are cast again
// once boot services are gone.
static const struct geni_uart_clocks {
	const char*	soc_compat;
	phys_addr_t	uart;
	phys_addr_t	vote;			// branch enable vote register
	uint32		vote_mask;
	phys_addr_t	branches[5];	// CBCRs: bit 31 = clock off
} kGeniUartClocks[] = {
	// SC8280XP debug UART (QUP2 SE1): core, s_ahb, m_ahb, core_2x, se
	{ "qcom,sc8280xp", 0x884000, 0x152010, 0x2f,
		{ 0x11e00c, 0x11e008, 0x11e004, 0x11e014, 0x11e274 } },
};

static const geni_uart_clocks* sGeniUartClocks = NULL;


static bool
geni_uart_clocks_running(const geni_uart_clocks& clocks)
{
	for (uint32 i = 0; i < B_COUNT_OF(clocks.branches); i++) {
		// A branch under hardware control (bit 1) reads as off while idle,
		// and wakes up on access.
		uint32 branch = *(volatile uint32*)clocks.branches[i];
		if ((branch & (1u << 31)) != 0 && (branch & (1u << 1)) == 0)
			return false;
	}
	return true;
}


static bool
geni_uart_usable(const void* fdt, addr_t base)
{
	for (uint32 i = 0; i < B_COUNT_OF(kGeniUartClocks); i++) {
		const geni_uart_clocks& clocks = kGeniUartClocks[i];
		if (clocks.uart != base
			|| fdt_node_check_compatible(fdt, 0, clocks.soc_compat) != 0) {
			continue;
		}
		if (!geni_uart_clocks_running(clocks)) {
			dprintf("GENI UART %#" B_PRIxADDR ": clocks off, not used (vote %#"
				B_PRIx32 ";", base, *(volatile uint32*)clocks.vote);
			for (uint32 j = 0; j < B_COUNT_OF(clocks.branches); j++)
				dprintf(" %#" B_PRIx32, *(volatile uint32*)clocks.branches[j]);
			dprintf(")\n");
			return false;
		}
		sGeniUartClocks = &clocks;
		return true;
	}
	dprintf("GENI UART %#" B_PRIxADDR ": unknown SoC, not used\n", base);
	return false;
}
#endif


#ifdef TRACE_DUMP_FDT
static void
write_string_list(const char* prop, size_t size)
{
	bool first = true;
	const char* propEnd = prop + size;
	while (propEnd - prop > 0) {
		if (first)
			first = false;
		else
			dprintf(", ");
		int curLen = strlen(prop);
		dprintf("'%s'", prop);
		prop += curLen + 1;
	}
}


static void
dump_fdt(const void *fdt)
{
	if (!fdt)
		return;

	int err = fdt_check_header(fdt);
	if (err) {
		dprintf("fdt error: %s\n", fdt_strerror(err));
		return;
	}

	dprintf("fdt tree:\n");

	int node = -1;
	int depth = -1;
	while ((node = fdt_next_node(fdt, node, &depth)) >= 0 && depth >= 0) {
		for (int i = 0; i < depth; i++)
			dprintf("  ");
		// WriteInt(node); WriteString(", "); WriteInt(depth); WriteString(": ");
		dprintf("node('%s')\n", fdt_get_name(fdt, node, NULL));
		depth++;
		for (int prop = fdt_first_property_offset(fdt, node); prop >= 0; prop = fdt_next_property_offset(fdt, prop)) {
			int len;
			const struct fdt_property *property = fdt_get_property_by_offset(fdt, prop, &len);
			if (property == NULL) {
				for (int i = 0; i < depth; i++)
					dprintf("  ");
				dprintf("getting prop at %d: %s\n", prop, fdt_strerror(len));
				break;
			}
			for (int i = 0; i < depth; i++)
				dprintf("  ");
			dprintf("prop('%s'): ", fdt_string(fdt, fdt32_to_cpu(property->nameoff)));
			if (
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "compatible") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "model") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "serial-number") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "status") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "device_type") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "riscv,isa") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "mmu-type") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "format") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "bootargs") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "stdout-path") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "reg-names") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "reset-names") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "clock-names") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "clock-output-names") == 0
			) {
				write_string_list((const char*)property->data, fdt32_to_cpu(property->len));
			} else if (strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "reg") == 0) {
				for (uint64_t *it = (uint64_t*)property->data; (uint8_t*)it - (uint8_t*)property->data < fdt32_to_cpu(property->len); it += 2) {
					if (it != (uint64_t*)property->data)
						dprintf(", ");
					dprintf("(0x%08" B_PRIx64 ", 0x%08" B_PRIx64 ")",
						fdt64_to_cpu(*it), fdt64_to_cpu(*(it + 1)));
				}
			} else if (
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "phandle") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "clock-frequency") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "timebase-frequency") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "#address-cells") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "#size-cells") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "#interrupt-cells") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "interrupts") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "interrupt-parent") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "boot-hartid") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "riscv,ndev") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "value") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "offset") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "regmap") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "bank-width") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "width") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "height") == 0 ||
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "stride") == 0
			) {
				dprintf("%" B_PRId32, fdt32_to_cpu(*(uint32_t*)property->data));
			} else if (
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "interrupts-extended") == 0
			) {
				for (uint32_t *it = (uint32_t*)property->data; (uint8_t*)it - (uint8_t*)property->data < fdt32_to_cpu(property->len); it += 2) {
					if (it != (uint32_t*)property->data)
						dprintf(", ");
					dprintf("(%" B_PRId32 ", %" B_PRId32 ")",
						fdt32_to_cpu(*it), fdt32_to_cpu(*(it + 1)));
				}
			} else if (
				strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "ranges") == 0
			) {
				dprintf("\n");
				depth++;
				// kind
				// child address
				// parent address
				// size
				for (uint32_t *it = (uint32_t*)property->data; (uint8_t*)it - (uint8_t*)property->data < fdt32_to_cpu(property->len); it += 7) {
					for (int i = 0; i < depth; i++)
						dprintf("  ");
					uint32_t kind = fdt32_to_cpu(*(it + 0));
					switch (kind & 0x03000000) {
					case 0x00000000: dprintf("CONFIG"); break;
					case 0x01000000: dprintf("IOPORT"); break;
					case 0x02000000: dprintf("MMIO"); break;
					case 0x03000000: dprintf("MMIO_64BIT"); break;
					}
					dprintf(" (0x%08" PRIx32 "), child: 0x%08" PRIx64 ", parent: 0x%08" PRIx64 ", len: 0x%08" PRIx64 "\n",
						kind, fdt64_to_cpu(*(uint64_t*)(it + 1)), fdt64_to_cpu(*(uint64_t*)(it + 3)), fdt64_to_cpu(*(uint64_t*)(it + 5)));
				}
				for (int i = 0; i < depth; i++)
					dprintf("  ");
				depth--;
			} else if (strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "bus-range") == 0) {
				uint32_t *it = (uint32_t*)property->data;
				dprintf("%" PRId32 ", %" PRId32, fdt32_to_cpu(*it), fdt32_to_cpu(*(it + 1)));
			} else if (strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "interrupt-map-mask") == 0) {
				dprintf("\n");
				depth++;
				for (uint32_t *it = (uint32_t*)property->data; (uint8_t*)it - (uint8_t*)property->data < fdt32_to_cpu(property->len); it++) {
					for (int i = 0; i < depth; i++)
						dprintf("  ");
					dprintf("0x%08" PRIx32 "\n", fdt32_to_cpu(*(uint32_t*)it));
				}
				for (int i = 0; i < depth; i++)
					dprintf("  ");
				depth--;
			} else if (strcmp(fdt_string(fdt, fdt32_to_cpu(property->nameoff)), "interrupt-map") == 0) {
				dprintf("\n");
				depth++;

				int addressCells = 3;
				int interruptCells = 1;
				int phandleCells = 1;

				uint32 *prop;

				prop = (uint32*)fdt_getprop(fdt, node, "#address-cells", NULL);
				if (prop != NULL)
					addressCells = fdt32_to_cpu(*prop);

				prop = (uint32*)fdt_getprop(fdt, node, "#interrupt-cells", NULL);
				if (prop != NULL)
					interruptCells = fdt32_to_cpu(*prop);

				uint32_t *it = (uint32_t*)property->data;
				while ((uint8_t*)it - (uint8_t*)property->data < fdt32_to_cpu(property->len)) {
					for (int i = 0; i < depth; i++)
						dprintf("  ");

					uint32 childAddr = fdt32_to_cpu(*it);
					dprintf("childAddr: ");
					for (int i = 0; i < addressCells; i++) {
						dprintf("0x%08" PRIx32 " ", fdt32_to_cpu(*it));
						it++;
					}

					uint8 bus = childAddr / (1 << 16) % (1 << 8);
					uint8 dev = childAddr / (1 << 11) % (1 << 5);
					uint8 func = childAddr % (1 << 3);
					dprintf("(bus: %" PRId32 ", dev: %" PRId32 ", fn: %" PRId32 "), ",
						bus, dev, func);

					dprintf("childIrq: ");
					for (int i = 0; i < interruptCells; i++) {
						dprintf("%" PRIu32 " ", fdt32_to_cpu(*it));
						it++;
					}

					uint32 parentPhandle = fdt32_to_cpu(*it);
					it += phandleCells;
					dprintf("parentPhandle: %" PRId32 ", ", parentPhandle);

					int parentAddressCells = 0;
					int parentInterruptCells = 1;

					int parentNode = fdt_node_offset_by_phandle(fdt, parentPhandle);
					if (parentNode >= 0) {
						prop = (uint32*)fdt_getprop(fdt, parentNode, "#address-cells", NULL);
						if (prop != NULL)
							parentAddressCells = fdt32_to_cpu(*prop);

						prop = (uint32*)fdt_getprop(fdt, parentNode, "#interrupt-cells", NULL);
						if (prop != NULL)
							parentInterruptCells = fdt32_to_cpu(*prop);
					}

					dprintf("parentAddress: ");
					for (int i = 0; i < parentAddressCells; i++) {
						dprintf("%" PRIu32 " ", fdt32_to_cpu(*it));
						it++;
					}

					dprintf("parentIrq: ");
					for (int i = 0; i < parentInterruptCells; i++) {
						dprintf("%" PRIu32 " ", fdt32_to_cpu(*it));
						it++;
					}

					dprintf("\n");
				}
				for (int i = 0; i < depth; i++)
					dprintf("  ");
				depth--;
			} else {
				dprintf("?");
			}
			dprintf(" (len %" PRId32 ")\n", fdt32_to_cpu(property->len));
/*
			dump_hex(property->data, fdt32_to_cpu(property->len), depth);
*/
		}
		depth--;
	}
}
#endif


bool
dtb_has_fdt_string(const char* prop, int size, const char* pattern)
{
	int patternLen = strlen(pattern);
	const char* propEnd = prop + size;
	while (propEnd - prop > 0) {
		int curLen = strlen(prop);
		if (curLen == patternLen && memcmp(prop, pattern, curLen + 1) == 0)
			return true;
		prop += curLen + 1;
	}
	return false;
}


uint32
dtb_get_address_cells(const void* fdt, int node)
{
	uint32 res = 2;

	int parent = fdt_parent_offset(fdt, node);
	if (parent < 0)
		return res;

	uint32 *prop = (uint32*)fdt_getprop(fdt, parent, "#address-cells", NULL);
	if (prop == NULL)
		return res;

	res = fdt32_to_cpu(*prop);
	return res;
}


uint32
dtb_get_size_cells(const void* fdt, int node)
{
	uint32 res = 1;

	int parent = fdt_parent_offset(fdt, node);
	if (parent < 0)
		return res;

	uint32 *prop = (uint32*)fdt_getprop(fdt, parent, "#size-cells", NULL);
	if (prop == NULL)
		return res;

	res = fdt32_to_cpu(*prop);
	return res;
}


bool
dtb_get_reg(const void* fdt, int node, size_t idx, addr_range& range)
{
	uint32 addressCells = dtb_get_address_cells(fdt, node);
	uint32 sizeCells = dtb_get_size_cells(fdt, node);

	int propSize;
	const uint8* prop = (const uint8*)fdt_getprop(fdt, node, "reg", &propSize);
	if (prop == NULL)
		return false;

	size_t entrySize = 4 * (addressCells + sizeCells);
	if ((idx + 1) * entrySize > (size_t)propSize)
		return false;

	prop += idx * entrySize;

	switch (addressCells) {
		case 1: range.start = fdt32_to_cpu(*(uint32*)prop); prop += 4; break;
		case 2: range.start = fdt64_to_cpu(*(uint64*)prop); prop += 8; break;
		default: panic("unsupported addressCells");
	}
	switch (sizeCells) {
		case 1: range.size = fdt32_to_cpu(*(uint32*)prop); prop += 4; break;
		case 2: range.size = fdt64_to_cpu(*(uint64*)prop); prop += 8; break;
		default: panic("unsupported sizeCells");
	}

	int parent = fdt_parent_offset(fdt, node);
	if (parent >= 0) {
		uint32 parentAddressCells = dtb_get_address_cells(fdt, parent);

		uint32 rangesSize = 0;
		uint32 *ranges = (uint32 *)fdt_getprop(fdt, parent, "ranges", (int *)&rangesSize);
		if (ranges == NULL)
			return true;

		uint32 rangesPos = 0;
		while (rangesSize >= (rangesPos + parentAddressCells + addressCells + sizeCells)) {
			addr_t childAddress;
			addr_t parentAddress;
			size_t rangeSize;

			if (addressCells == 1) {
				childAddress = fdt32_to_cpu(*(uint32*)(ranges+rangesPos));
			} else {
				childAddress = fdt64_to_cpu(*(uint64*)(ranges+rangesPos));
			}
			rangesPos += addressCells;

			if (parentAddressCells == 1) {
				parentAddress = fdt32_to_cpu(*(uint32*)(ranges+rangesPos));
			} else {
				parentAddress = fdt64_to_cpu(*(uint64*)(ranges+rangesPos));
			}
			rangesPos += parentAddressCells;

			if (sizeCells == 1) {
				rangeSize = fdt32_to_cpu(*(uint32*)(ranges+rangesPos));
			} else {
				rangeSize = fdt64_to_cpu(*(uint64*)(ranges+rangesPos));
			}
			rangesPos += sizeCells;

			if ((range.start >= childAddress) && (range.start <= childAddress + rangeSize)) {
				range.start -= childAddress;
				range.start += parentAddress;
				break;
			}
		}
	}

	return true;
}


static int
dtb_get_interrupt_parent(const void* fdt, int node)
{
	while (node >= 0) {
		uint32* prop;
		prop = (uint32*)fdt_getprop(fdt, node, "interrupt-parent", NULL);
		if (prop != NULL) {
			uint32_t phandle = fdt32_to_cpu(*prop);
			return fdt_node_offset_by_phandle(fdt, phandle);
		}

		node = fdt_parent_offset(fdt, node);
	}

	return -1;
}


static uint32
dtb_get_interrupt_cells(const void* fdt, int node)
{
	int intc_node = dtb_get_interrupt_parent(fdt, node);
	if (intc_node >= 0) {
		uint32* prop = (uint32*)fdt_getprop(fdt, intc_node, "#interrupt-cells", NULL);
		if (prop != NULL) {
			return fdt32_to_cpu(*prop);
		}
	}

	return 1;
}


uint32
dtb_get_interrupt(const void* fdt, int node)
{
	uint32 interruptCells = dtb_get_interrupt_cells(fdt, node);

	if (uint32* prop = (uint32*)fdt_getprop(fdt, node, "interrupts-extended", NULL)) {
		return fdt32_to_cpu(*(prop + 1));
	}
	int length;
	if (uint32* prop = (uint32*)fdt_getprop(fdt, node, "interrupts", &length)) {
		if (interruptCells == 0 || interruptCells > (uint32)length / sizeof(uint32))
			panic("invalid interrupts property length");

		if ((interruptCells == 1) || (interruptCells == 2)) {
			return fdt32_to_cpu(*prop);
		} else if (interruptCells == 3 || (interruptCells == 4
			&& fdt_node_check_compatible(fdt, dtb_get_interrupt_parent(fdt, node),
				"arm,gic-v3") == 0)) {
			// GICv3 adds an optional PPI affinity phandle after the usual
			// type, number and flags. Zero denotes an unpartitioned interrupt.
			if (interruptCells == 4 && fdt32_to_cpu(prop[3]) != 0)
				panic("GIC PPI affinity partitions are not supported");

			uint32 interruptType = fdt32_to_cpu(prop[GIC_INTERRUPT_CELL_TYPE]);
			uint32 interruptNumber = fdt32_to_cpu(prop[GIC_INTERRUPT_CELL_ID]);
			if (interruptType == GIC_INTERRUPT_TYPE_SPI)
				interruptNumber += GIC_INTERRUPT_BASE_SPI;
			else if (interruptType == GIC_INTERRUPT_TYPE_PPI)
				interruptNumber += GIC_INTERRUPT_BASE_PPI;

			return interruptNumber;
		} else {
			panic("unsupported interruptCells");
		}
	}
	dprintf("%s: no interrupt field on node %d\n", __func__, node);
	return 0;
}


static int64
dtb_get_clock_frequency(const void* fdt, int node)
{
	uint32* prop;
	int len = 0;

	prop = (uint32*)fdt_getprop(fdt, node, "clock-frequency", NULL);
	if (prop != NULL) {
		return fdt32_to_cpu(*prop);
	}

	prop = (uint32*)fdt_getprop(fdt, node, "clocks", &len);
	if (prop != NULL) {
		uint32_t phandle = fdt32_to_cpu(*prop);
		int offset = fdt_node_offset_by_phandle(fdt, phandle);
		if (offset > 0) {
			uint32* prop2 = (uint32*)fdt_getprop(fdt, offset, "clock-frequency", NULL);
			if (prop2 != NULL) {
				return fdt32_to_cpu(*prop2);
			}
		}
	}

	dprintf("%s: no clock-frequency field on node %d\n", __func__, node);
	return -1;
}


static void
dtb_handle_fdt(const void* fdt, int node)
{
	arch_handle_fdt(fdt, node);

	int compatibleLen;
	const char* compatible = (const char*)fdt_getprop(fdt, node,
		"compatible", &compatibleLen);

	if (compatible == NULL)
		return;

	// check for a uart if we don't have one
	uart_info &uart = gKernelArgs.arch_args.uart;
	if (uart.kind[0] == 0) {
		for (uint32 i = 0; i < B_COUNT_OF(kSupportedUarts); i++) {
			if (dtb_has_fdt_string(compatible, compatibleLen,
					kSupportedUarts[i].dtb_compat)) {

				addr_range regs;
				dtb_get_reg(fdt, node, 0, regs);
#if defined(__aarch64__)
				if (strcmp(kSupportedUarts[i].kind, UART_KIND_GENI) == 0
					&& !geni_uart_usable(fdt, regs.start)) {
					continue;
				}
#endif

				memcpy(uart.kind, kSupportedUarts[i].kind,
					sizeof(uart.kind));
				uart.regs = regs;
				uart.irq = dtb_get_interrupt(fdt, node);
				uart.clock = dtb_get_clock_frequency(fdt, node);

				gUART = kSupportedUarts[i].uart_driver_init(uart.regs.start,
					uart.clock);
				gUARTSkipInit = fdt_getprop(fdt, node, "skip-init", NULL) != NULL;
			}
		}
	}
}


static void
dtb_handle_chosen_node(const void *fdt)
{
	int chosen = fdt_path_offset(fdt, "/chosen");
	if (chosen < 0)
		return;

	int len;
	const char *stdoutPath = (const char *)fdt_getprop(fdt, chosen, "stdout-path", &len);
	if (stdoutPath == NULL)
		return;

	// stdout-path can optionally contain a ":" separator character
	// The part after the ":" character specifies the UART configuration
	// We can ignore it here as the UART should be already initialized
	// by the UEFI firmware (e.g. U-Boot or TianoCore)

	char *separator = strchr(stdoutPath, ':');
	int namelen = (separator == NULL) ? len - 1 : separator - stdoutPath;

	int stdoutNode = fdt_path_offset_namelen(fdt, stdoutPath, namelen);
	if (stdoutNode < 0)
		return;

	dtb_handle_fdt(fdt, stdoutNode);
}


void
dtb_init()
{
	efi_configuration_table *table = kSystemTable->ConfigurationTable;
	size_t entries = kSystemTable->NumberOfTableEntries;

	INFO("Probing for device trees from UEFI...\n");

	// Try to find an FDT
	for (uint32 i = 0; i < entries; i++) {
		if (!table[i].VendorGuid.equals(DEVICE_TREE_GUID))
			continue;

		void* dtbPtr = (void*)(table[i].VendorTable);

		int res = fdt_check_header(dtbPtr);
		if (res != 0) {
			ERROR("Invalid FDT from UEFI table %d: %s\n", i, fdt_strerror(res));
			continue;
		}

		sDtbTable = dtbPtr;
		sDtbSize = fdt_totalsize(dtbPtr);
	
		INFO("Valid FDT from UEFI table %d, size: %" B_PRIu32 "\n", i, sDtbSize);

#ifdef TRACE_DUMP_FDT
		dump_fdt(sDtbTable);
#endif

		dtb_handle_chosen_node(sDtbTable);

		int node = -1;
		int depth = -1;
		while ((node = fdt_next_node(sDtbTable, node, &depth)) >= 0 && depth >= 0) {
			dtb_handle_fdt(sDtbTable, node);
		}
		break;
	}
}


void
dtb_exit_boot_services()
{
#if defined(__aarch64__)
	if (sGeniUartClocks == NULL)
		return;

	// Vote the UART clocks on again, and give up on the UART if they still
	// do not run.
	volatile uint32* vote = (volatile uint32*)sGeniUartClocks->vote;
	*vote = *vote | sGeniUartClocks->vote_mask;
	for (int i = 0; i < 1000000; i++) {
		if (geni_uart_clocks_running(*sGeniUartClocks))
			return;
	}
	gUART = NULL;
	memset(&gKernelArgs.arch_args.uart, 0, sizeof(gKernelArgs.arch_args.uart));
#endif
}


// Days since 1970-01-01 of a proleptic Gregorian date.
static int64
days_from_civil(int64 year, uint32 month, uint32 day)
{
	year -= month <= 2;
	int64 era = (year >= 0 ? year : year - 399) / 400;
	uint32 yearOfEra = (uint32)(year - era * 400);
	uint32 dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	uint32 dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100
		+ dayOfYear;
	return era * 146097 + dayOfEra - 719468;
}


/*!	The firmware's clock in seconds since 1970, as its fields read (the
	kernel applies its own GMT or local time setting), or 0 if it has none.
*/
static uint64
firmware_clock_seconds()
{
	efi_time time;
	if (kRuntimeServices->GetTime(&time, NULL) != EFI_SUCCESS
		|| time.Year < 2000 || time.Month < 1 || time.Month > 12
		|| time.Day < 1 || time.Day > 31) {
		return 0;
	}

	return days_from_civil(time.Year, time.Month, time.Day) * 86400
		+ time.Hour * 3600 + time.Minute * 60 + time.Second;
}


void
dtb_set_kernel_args()
{
	// pack into proper location if the architecture cares
	if (sDtbTable != NULL) {
		// The kernel has no driver for most boards' real-time clock, so the
		// firmware's clock goes along as "haiku,firmware-clock" in /chosen.
		uint64 clock = firmware_clock_seconds();
		uint32 size = sDtbSize + (clock != 0 ? 64 : 0);

		// libfdt requires 8-byte alignment
		gKernelArgs.arch_args.fdt = (void*)(addr_t)kernel_args_malloc(size, 8);

		if (gKernelArgs.arch_args.fdt == NULL)
			ERROR("unable to malloc for fdt!\n");
		else if (clock == 0)
			memcpy(gKernelArgs.arch_args.fdt, sDtbTable, sDtbSize);
		else {
			void* fdt = gKernelArgs.arch_args.fdt;
			int chosen = -1;
			if (fdt_open_into(sDtbTable, fdt, size) == 0)
				chosen = fdt_path_offset(fdt, "/chosen");
			if (chosen < 0 || fdt_setprop_u64(fdt, chosen,
					"haiku,firmware-clock", clock) != 0) {
				memcpy(fdt, sDtbTable, sDtbSize);
			} else
				dprintf("firmware clock: %" B_PRIu64 "\n", clock);
		}
	}

	uart_info &uart = gKernelArgs.arch_args.uart;
	dprintf("Chosen UART:\n");
	if (uart.kind[0] == 0) {
		dprintf("kind: None!\n");
	} else {
		dprintf("  kind: %s\n", uart.kind);
		dprintf("  regs: %#" B_PRIx64 ", %#" B_PRIx64 "\n", uart.regs.start, uart.regs.size);
		dprintf("  irq: %" B_PRIu32 "\n", uart.irq);
		dprintf("  clock: %" B_PRId64 "\n", uart.clock);
	}

	arch_dtb_set_kernel_args();
}
