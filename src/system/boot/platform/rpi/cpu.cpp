/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "cpu.h"

#include <OS.h>

#include <arch/cpu.h>
#include <arch_cpu.h>
#include <boot/kernel_args.h>
#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <boot/vfs.h>
#include <kernel/arch/arm64/arm_registers.h>

#include "dtb.h"
#include "mailbox.h"


// what the armstub programs; used if CNTFRQ_EL0 was left unset
static const uint64 kDefaultTimerFrequency = 54000000;

static uint64 sTimerFrequency;
static uint64 sTimerBase;

// The power manager's watchdog (BCM2835 style, Pi 4 and Pi 5), armed with
// "airos.watchdog" on the command line: a lab board whose power nobody can
// switch restarts by itself when the loader or the kernel hangs.
#define PM_PASSWORD				0x5a000000
#define PM_RSTC					0x1c
#define PM_WDOG					0x24
#define PM_RSTC_WRCFG_CLEAR		0xffffffcf
#define PM_RSTC_WRCFG_FULL_RESET	0x00000020
#define PM_WDOG_TIMEOUT			(15 << 16)
	// 15 s, about the longest the 20-bit counter of 1/65536 s takes

static volatile uint32* sWatchdog;
static uint64 sWatchdogPetted;


bigtime_t
system_time()
{
	if (sTimerFrequency == 0) {
		sTimerFrequency = READ_SPECIALREG(CNTFRQ_EL0);
		if (sTimerFrequency == 0)
			sTimerFrequency = kDefaultTimerFrequency;
		sTimerBase = READ_SPECIALREG(CNTPCT_EL0);
	}

	uint64 now = READ_SPECIALREG(CNTPCT_EL0);
	if (sWatchdog != NULL && now - sWatchdogPetted > sTimerFrequency) {
		sWatchdogPetted = now;
		watchdog_pet();
	}

	uint64 ticks = now - sTimerBase;
	return ticks / sTimerFrequency * 1000000
		+ ticks % sTimerFrequency * 1000000 / sTimerFrequency;
}


void
spin(bigtime_t microseconds)
{
	bigtime_t end = system_time() + microseconds;
	while (system_time() < end)
		;
}


void
cpu_init()
{
	gKernelArgs.num_cpus = 1;

	// The firmware starts the ARM cores at their lowest clock and leaves
	// raising it to the operating system.
	uint32 rate = 0;
	uint32 maximum = 0;
	if (mailbox_get_clock_rate(MAILBOX_CLOCK_ARM, true, maximum) == B_OK
		&& mailbox_get_clock_rate(MAILBOX_CLOCK_ARM, false, rate) == B_OK) {
		if (rate < maximum
			&& mailbox_set_clock_rate(MAILBOX_CLOCK_ARM, maximum) == B_OK) {
			dprintf("cpu: ARM clock raised from %" B_PRIu32 " to %" B_PRIu32
				" MHz\n", rate / 1000000, maximum / 1000000);
		} else {
			dprintf("cpu: ARM clock at %" B_PRIu32 " MHz\n", rate / 1000000);
		}
	}

	boot_arch_cpu_init();
}


void
watchdog_init(phys_addr_t base)
{
	sWatchdog = (volatile uint32*)base;
	watchdog_pet();
	uint32 control = sWatchdog[PM_RSTC / 4];
	sWatchdog[PM_RSTC / 4] = PM_PASSWORD | (control & PM_RSTC_WRCFG_CLEAR)
		| PM_RSTC_WRCFG_FULL_RESET;
	dprintf("watchdog: armed at %#" B_PRIxPHYSADDR ", 15 s\n", base);
}


void
watchdog_pet()
{
	if (sWatchdog != NULL)
		sWatchdog[PM_WDOG / 4] = PM_PASSWORD | PM_WDOG_TIMEOUT;
}


void
cpu_reset_after(bigtime_t timeout)
{
	if (gPsciConduit == ARM64_PSCI_NONE)
		return;

	spin(timeout);

	// PSCI SYSTEM_RESET
	register uint64 x0 asm("x0") = 0x84000009;
	if (gPsciConduit == ARM64_PSCI_HVC)
		asm volatile("hvc #0" : "+r" (x0) : : "memory");
	else
		asm volatile("smc #0" : "+r" (x0) : : "memory");
}


void
platform_load_ucode(BootVolume& volume)
{
}
