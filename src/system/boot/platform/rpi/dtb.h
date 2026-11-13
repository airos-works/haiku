/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef DTB_H
#define DTB_H


#include <SupportDefs.h>

#include <boot/addr_range.h>


#define MAX_FDT_CPUS	8

struct fdt_cpu {
	uint64	mpidr;
	uint64	releaseAddress;
		// spin table slot (Pi 4), 0 if the CPU has none
	bool	psci;
		// started with PSCI CPU_ON (Pi 5)
};

extern void* gFDT;
extern addr_range gBootArchive;
	// the "initramfs" the firmware loaded
extern fdt_cpu gFDTCpus[MAX_FDT_CPUS];
extern uint32 gFDTCpuCount;
extern bool gBCM2712;
	// a Raspberry Pi 5 (or 500, CM5): the SoC is a BCM2712
extern uint32 gPsciConduit;
	// ARM64_PSCI_*, from /psci

void fdt_init(void* fdt);
	// memory ranges, reservations, the boot archive, UART, GIC, CPUs
void fdt_set_kernel_args();
void fdt_set_watchdog(phys_addr_t base);
	// the kernel keeps the watchdog from firing (/chosen/airos,watchdog)
const char* fdt_boot_arguments();
bool fdt_get_reg(int node, size_t index, addr_range& range);
bool fdt_find_compatible_reg(const char* compatible, addr_range& range);


#endif	/* DTB_H */
