/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Idle cores power down (PSCI CPU_SUSPEND with the device tree's
	power-down idle state) instead of only waiting in WFI, which leaves them
	powered at full voltage: the arm64 counterpart of x86's deep C-states.

	The device tree's state stops the core's timer ("local-timer-stop"), and
	nothing else would wake it for one of its timers, so a core powers down
	only while no timer is due on it; interrupts and inter-processor
	messages still wake it. It also takes a while to enter and leave the
	state, so a core powers down only when its recent idle periods were long
	(longer in low latency mode). CPU 0, which takes the device interrupts,
	stays in WFI.
*/


#include <KernelExport.h>
#include <driver_settings.h>
#include <libfdt.h>

#include <arch/cpu.h>
#include <cpu.h>
#include <generic_syscall.h>
#include <kscheduler.h>
#include <smp.h>
#include <timer.h>
#include <vm/vm.h>

#include <new>
#include <scheduler.h>
#include <string.h>

#include "arch_cpu_suspend.h"


extern void* gFDT;
int arm64_fdt_cpu_node(const void* fdt, uint64 mpidr);
uint32 arm64_psci_conduit();


#define ARM64_IDLE_SYSCALLS		"arm64_idle"
#define ARM64_IDLE_GET_STATS	1
	// buffer: uint32 count, uint32 enabled, then count x
	// { uint64 powered down, uint64 refused, uint64 microseconds down }
#define ARM64_IDLE_SET_ENABLED	2
	// buffer: uint32 enable

#define ARM64_PSCI_SMC			1


struct idle_stats {
	int64		poweredDown;
	int64		refused;
	int64		timeDown;
	bigtime_t	averageIdle;
	bigtime_t	downSince;
		// while powered down: since when (for the statistics)
} __attribute__((aligned(64)));


static bool sAvailable;
static bool sEnabled;
static uint64 sPowerState;
static bigtime_t sMinResidency;
static uint64 sEntryPhysical;
static phys_addr_t sContextsPhysical;
static arm64_suspend_context* sContexts;
static bool sUseSMC;
static idle_stats* sStats;
static int32 sCPUCount;


static inline void
clean_to_poc(void* address, size_t size)
{
	addr_t line = (addr_t)address & ~(addr_t)63;
	addr_t end = (addr_t)address + size;
	for (; line < end; line += 64)
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	asm volatile("dsb sy" : : : "memory");
}


/*!	The device tree's deepest idle state for the boot CPU, if it is entered
	through PSCI.
*/
static bool
find_idle_state()
{
	if (gFDT == NULL || fdt_check_header(gFDT) != 0)
		return false;

	int cpuNode = arm64_fdt_cpu_node(gFDT, gCPU[0].arch.mpidr);
	if (cpuNode < 0)
		return false;

	int length;
	const fdt32_t* states = (const fdt32_t*)fdt_getprop(gFDT, cpuNode,
		"cpu-idle-states", &length);
	if (states == NULL || length < 4)
		return false;

	// the last one listed is the deepest
	int node = fdt_node_offset_by_phandle(gFDT,
		fdt32_to_cpu(states[length / 4 - 1]));
	if (node < 0 || fdt_node_check_compatible(gFDT, node, "arm,idle-state") != 0)
		return false;

	const fdt32_t* parameter = (const fdt32_t*)fdt_getprop(gFDT, node,
		"arm,psci-suspend-param", &length);
	if (parameter == NULL || length != 4)
		return false;
	sPowerState = fdt32_to_cpu(*parameter);

	const fdt32_t* residency = (const fdt32_t*)fdt_getprop(gFDT, node,
		"min-residency-us", &length);
	sMinResidency = residency != NULL && length == 4
		? fdt32_to_cpu(*residency) : 1000;

	// only power-down states (PSCI StateType, bit 16) need all of this
	return (sPowerState & (1 << 16)) != 0;
}


/*!	An identity mapping of the 1 GiB around the trampoline, for the moment
	the MMU comes back on: TTBR0 then points at \a table0 (and \a table1).
*/
static bool
build_identity_map(uint64* table0, phys_addr_t table0Physical, uint64* table1,
	phys_addr_t table1Physical, phys_addr_t codePhysical, uint64* _ttbr0)
{
	uint64 tcr = READ_SPECIALREG(TCR_EL1);
	uint32 t0sz = tcr & 0x3f;
	uint32 tg0 = (tcr >> 14) & 0x3;
	uint32 vaBits = 64 - t0sz;
	if (tg0 != 0 || vaBits > 48 || vaBits <= 30
		|| codePhysical >= (1ULL << vaBits)) {
		dprintf("arm64 idle: no identity mapping (TCR %#" B_PRIx64 ")\n", tcr);
		return false;
	}

	uint64 mair = READ_SPECIALREG(MAIR_EL1);
	int attrIndex = -1;
	for (int i = 0; i < 8; i++) {
		if (((mair >> (i * 8)) & 0xff) == 0xff) {
			attrIndex = i;
			break;
		}
	}
	if (attrIndex < 0)
		return false;

	memset(table0, 0, B_PAGE_SIZE);
	memset(table1, 0, B_PAGE_SIZE);

	uint64 block = (codePhysical & ~((1ULL << 30) - 1))
		| (1ULL << 54)				// not executable at EL0
		| (1ULL << 10)				// access flag
		| (3ULL << 8)				// inner shareable
		| ((uint64)attrIndex << 2)
		| 0x1;						// block

	if (vaBits > 39) {
		table0[(codePhysical >> 39) & 0x1ff] = table1Physical | 0x3;
		table1[(codePhysical >> 30) & 0x1ff] = block;
		*_ttbr0 = table0Physical;
	} else {
		table0[(codePhysical >> 30) & 0x1ff] = block;
		*_ttbr0 = table0Physical;
	}

	clean_to_poc(table0, B_PAGE_SIZE);
	clean_to_poc(table1, B_PAGE_SIZE);
	return true;
}


static status_t
idle_syscall(const char* subsystem, uint32 function, void* buffer,
	size_t bufferSize)
{
	if (buffer == NULL || !IS_USER_ADDRESS(buffer))
		return B_BAD_VALUE;

	if (function == ARM64_IDLE_SET_ENABLED) {
		uint32 enable;
		if (bufferSize < sizeof(enable)
			|| user_memcpy(&enable, buffer, sizeof(enable)) != B_OK) {
			return B_BAD_ADDRESS;
		}
		sEnabled = sAvailable && enable != 0;
		return B_OK;
	}
	if (function != ARM64_IDLE_GET_STATS)
		return B_BAD_VALUE;

	size_t size = 8 + (size_t)sCPUCount * 24;
	if (bufferSize < size)
		return B_BAD_VALUE;
	uint32 header[2] = { (uint32)sCPUCount, sEnabled ? 1u : 0u };
	if (user_memcpy(buffer, header, sizeof(header)) != B_OK)
		return B_BAD_ADDRESS;
	bigtime_t now = system_time();
	for (int32 i = 0; i < sCPUCount; i++) {
		bigtime_t since = atomic_get64(&sStats[i].downSince);
		int64 values[3] = { sStats[i].poweredDown, sStats[i].refused,
			sStats[i].timeDown + (since != 0 ? now - since : 0) };
		if (user_memcpy((uint8*)buffer + 8 + i * 24, values, 24) != B_OK)
			return B_BAD_ADDRESS;
	}
	return B_OK;
}


void
arm64_cpu_idle_init()
{
	sCPUCount = smp_get_num_cpus();
	sStats = new(std::nothrow) idle_stats[sCPUCount];
	if (sStats == NULL)
		return;
	memset(sStats, 0, sizeof(idle_stats) * sCPUCount);

	// "cpu_power_down false" in the kernel settings turns it off
	void* settings = load_driver_settings("kernel");
	bool wanted = get_driver_boolean_parameter(settings, "cpu_power_down",
		true, true);
	unload_driver_settings(settings);
	if (!wanted) {
		dprintf("arm64 idle: power-down off in the kernel settings\n");
		return;
	}

	uint32 conduit = arm64_psci_conduit();
	if (conduit == 0 || sCPUCount < 2 || !find_idle_state())
		return;
	sUseSMC = conduit == ARM64_PSCI_SMC;

	// one page of trampoline, two of identity mapping, then the contexts
	size_t contextsSize = ROUNDUP(sCPUCount * sizeof(arm64_suspend_context),
		B_PAGE_SIZE);
	size_t size = 3 * B_PAGE_SIZE + contextsSize;
	void* base;
	area_id area = create_area("arm64 cpu suspend", &base, B_ANY_KERNEL_ADDRESS,
		size, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (area < 0)
		return;
	physical_entry entry;
	if (get_memory_map(base, size, &entry, 1) != B_OK) {
		delete_area(area);
		return;
	}
	phys_addr_t physical = entry.address;

	size_t codeSize = (addr_t)arm64_cpu_resume_end - (addr_t)arm64_cpu_resume;
	if (codeSize > B_PAGE_SIZE) {
		delete_area(area);
		return;
	}
	memcpy(base, (void*)arm64_cpu_resume, codeSize);
	clean_to_poc(base, codeSize);
	asm volatile("ic iallu; dsb sy; isb" : : : "memory");

	uint64 ttbr0;
	if (!build_identity_map((uint64*)((addr_t)base + B_PAGE_SIZE),
			physical + B_PAGE_SIZE, (uint64*)((addr_t)base + 2 * B_PAGE_SIZE),
			physical + 2 * B_PAGE_SIZE, physical, &ttbr0)) {
		delete_area(area);
		return;
	}

	sEntryPhysical = physical;
	sContexts = (arm64_suspend_context*)((addr_t)base + 3 * B_PAGE_SIZE);
	sContextsPhysical = physical + 3 * B_PAGE_SIZE;
	bool el2 = (READ_SPECIALREG(CurrentEL) >> 2) == 2;
	for (int32 i = 0; i < sCPUCount; i++) {
		uint64* values = sContexts[i].values;
		memset(values, 0, sizeof(arm64_suspend_context));
		values[CTX_IS_EL2 / 8] = el2 ? 1 : 0;
		values[CTX_IDMAP_TTBR0 / 8] = ttbr0;
		values[CTX_RESUME_VA / 8] = (uint64)arm64_cpu_resume_virtual;
		values[CTX_VA / 8] = (uint64)&sContexts[i];
	}

	register_generic_syscall(ARM64_IDLE_SYSCALLS, idle_syscall, 1, 0);

	sAvailable = true;
	sEnabled = true;
	dprintf("arm64 idle: cores power down when idle (PSCI state %#" B_PRIx64
		", residency %" B_PRId64 " us, %s)\n", sPowerState, sMinResidency,
		el2 ? "EL2" : "EL1");
}


extern "C" void
arm64_cpu_idle()
{
	int32 cpu = smp_get_current_cpu();
	if (!sEnabled || cpu == 0) {
		arm64_wfi();
		return;
	}

	idle_stats& stats = sStats[cpu];
	// how long idle periods have to be: twice the state's residency, ten
	// times that in low latency mode (a thread waking up on a powered-down
	// core waits the exit latency)
	bigtime_t needed = 2 * sMinResidency;
	if (_user_get_scheduler_mode() != SCHEDULER_MODE_POWER_SAVING)
		needed *= 10;

	cpu_status state = disable_interrupts();
	bigtime_t start = system_time();
	bool deep = stats.averageIdle >= needed && !timer_events_pending();
	if (deep) {
		arm64_suspend_context* context = &sContexts[cpu];
		context->values[CTX_IDMAP_TCR / 8]
			= READ_SPECIALREG(TCR_EL1) & ~(1ULL << 7);
			// (EPD0 off: the identity mapping is in TTBR0)
		atomic_set64(&stats.downSince, start);
		int64 result = arm64_cpu_suspend(context, sPowerState,
			sEntryPhysical, sContextsPhysical
				+ cpu * sizeof(arm64_suspend_context),
			sUseSMC ? 1 : 0);
		atomic_set64(&stats.downSince, 0);
		if (result == 1) {
			stats.poweredDown++;
			stats.timeDown += system_time() - start;
		} else
			stats.refused++;
	} else
		arm64_wfi();
	restore_interrupts(state);

	bigtime_t idle = system_time() - start;
	stats.averageIdle = (stats.averageIdle + idle) / 2;
}
