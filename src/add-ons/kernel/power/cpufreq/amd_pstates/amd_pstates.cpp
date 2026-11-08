/*
 * Copyright 2020-2022, Jérôme Duval, jerome.duval@gmail.com.
 * Copyright 2013, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Paweł Dziepak, <pdziepak@quarnos.org>
 */


#include <cpufreq.h>
#include <KernelExport.h>

#include <arch_cpu.h>
#include <cpu.h>
#include <generic_syscall.h>
#include <kscheduler.h>
#include <smp.h>
#include <util/AutoLock.h>

#include <stdlib.h>


#define AMD_PSTATES_MODULE_NAME	CPUFREQ_MODULES_PREFIX "/amd_pstates/v1"


static uint32 sHWPLowest;
static uint32 sHWPGuaranteed;
static uint32 sHWPEfficient;
static uint32 sHWPHighest;

static bool sSavePower = true;


// Zen 1 and Zen+ have no CPPC: the operating system picks one of a few
// P-states itself (PStateDef MSRs; the hardware boosts above P0 on its own).
#define MSR_AMD_PSTATE_CURRENT_LIMIT	0xc0010061
#define MSR_AMD_PSTATE_CONTROL			0xc0010062
#define AMD_PSTATE_DEFINITIONS			8

// the processor's energy counters (CPUID 0x80000007 EDX bit 14)
#define AMD_FEATURE_RAPL				(1 << 14)
#define MSR_AMD_RAPL_POWER_UNIT			0xc0010299
#define MSR_AMD_PACKAGE_ENERGY_STATUS	0xc001029b

#define AMD_ENERGY_SYSCALLS				"amd_energy"
#define AMD_ENERGY_GET					1
#define AMD_ENERGY_SET_PSTATE_SCALING	2
	// uint32 0: Zen 1 P-states stay at P0 (to compare), else scale them

static bool sLegacy;
// Off by default: measured on a Threadripper 1950X (package energy counter),
// scaling saved 0.4-1 W at idle and with light periodic jobs in low latency
// mode and cost more energy in power saving mode, while a 4 ms job took
// 6.7 instead of 4.4 ms and a busy thread reached 90 % of its speed after
// 10-40 instead of 1 ms: the scheduler asks for more only when it switches
// threads. Idle cores power down in CC6 at any P-state, and boosting from
// P0 and going idle sooner costs less. AMD_ENERGY_SET_PSTATE_SCALING turns
// it on, to measure again.
static bool sLegacyScaling = false;
static uint32 sLegacyFastest;
static uint32 sLegacySlowest;
static uint32 sLegacyMHz[AMD_PSTATE_DEFINITIONS];

// don't go down again sooner than this after a change (us)
static const bigtime_t kLegacyMinimalInterval = 20000;

struct legacy_cpu {
	uint32		pstate;
	bigtime_t	changed;
} CACHE_LINE_ALIGN;
static legacy_cpu* sLegacyCPUs;

static bool sEnergySyscalls;


static void set_normal_pstate(void* /* dummy */, int cpu);


static void
pstates_set_scheduler_mode(scheduler_mode mode)
{
	sSavePower = mode == SCHEDULER_MODE_POWER_SAVING;
	call_all_cpus(set_normal_pstate, NULL);
}


static void
set_legacy_pstate(uint32 pstate)
{
	legacy_cpu* entry = &sLegacyCPUs[smp_get_current_cpu()];
	if (entry->pstate == pstate)
		return;
	x86_write_msr(MSR_AMD_PSTATE_CONTROL, pstate);
	entry->pstate = pstate;
	entry->changed = system_time();
}


/*!	The scheduler asks for more when a processor's load goes above its
	target (55 %): straight to P0, from where the hardware boosts, as a
	thread that needs the processor should not wait for it to ramp up.
	Saving power, only a load well above the target gets P0.
*/
static status_t
pstates_increase_performance(int delta)
{
	if (!sLegacy || !sLegacyScaling)
		return B_NOT_SUPPORTED;

	uint32 pstate = sLegacyFastest;
	if (sSavePower && delta < kCPUPerformanceScaleMax / 10
		&& sLegacySlowest > sLegacyFastest) {
		pstate = sLegacyFastest + 1;
	}
	legacy_cpu* entry = &sLegacyCPUs[smp_get_current_cpu()];
	if (pstate < entry->pstate)
		set_legacy_pstate(pstate);
	return B_OK;
}


/*!	And less when the load is below the target: delta grows from 0 at 55 %
	to 302 at no load. Under about 27 % the slowest P-state, under about
	44 % the one in the middle; not sooner than 20 ms after a change.
*/
static status_t
pstates_decrease_performance(int delta)
{
	if (!sLegacy || !sLegacyScaling)
		return B_NOT_SUPPORTED;

	legacy_cpu* entry = &sLegacyCPUs[smp_get_current_cpu()];
	if (system_time() - entry->changed < kLegacyMinimalInterval)
		return B_OK;

	uint32 pstate;
	if (delta >= 150)
		pstate = sLegacySlowest;
	else if (delta >= 60)
		pstate = (sLegacyFastest + sLegacySlowest + 1) / 2;
	else
		return B_OK;
	if (pstate > entry->pstate)
		set_legacy_pstate(pstate);
	return B_OK;
}


static void
read_package_energy(void* _counters, int cpu)
{
	((uint64*)_counters)[cpu]
		= x86_read_msr(MSR_AMD_PACKAGE_ENERGY_STATUS) & 0xffffffff;
}


/*!	AMD_ENERGY_GET: the buffer gets the energy unit (1 / 2^unit joules) and
	the package energy counter (32 bits, wrapping) as each processor reads
	it: uint32 unit, uint32 count, uint64 counter[count].
*/
static status_t
energy_syscall(const char* subsystem, uint32 function, void* buffer,
	size_t bufferSize)
{
	if (function == AMD_ENERGY_SET_PSTATE_SCALING) {
		uint32 enable;
		if (!sLegacy || buffer == NULL || bufferSize < sizeof(enable)
			|| !IS_USER_ADDRESS(buffer)
			|| user_memcpy(&enable, buffer, sizeof(enable)) != B_OK) {
			return B_BAD_VALUE;
		}
		sLegacyScaling = enable != 0;
		call_all_cpus_sync(set_normal_pstate, NULL);
		// the scheduler asks for performance levels only when it can
		scheduler_update_policy();
		return B_OK;
	}
	if (function != AMD_ENERGY_GET)
		return B_BAD_VALUE;

	int32 count = smp_get_num_cpus();
	size_t size = 8 + 8 * (size_t)count;
	if (buffer == NULL || bufferSize < size || !IS_USER_ADDRESS(buffer))
		return B_BAD_VALUE;

	uint64* counters = (uint64*)malloc(8 * (size_t)count);
	if (counters == NULL)
		return B_NO_MEMORY;
	call_all_cpus_sync(read_package_energy, counters);
	uint32 header[2] = {
		(uint32)((x86_read_msr(MSR_AMD_RAPL_POWER_UNIT) >> 8) & 0x1f),
		(uint32)count
	};
	status_t status = user_memcpy(buffer, header, sizeof(header));
	if (status == B_OK)
		status = user_memcpy((uint8*)buffer + 8, counters, 8 * (size_t)count);
	free(counters);
	return status;
}


static bool
is_cpu_model_supported(cpu_ent* cpu)
{
	if (cpu->arch.vendor != VENDOR_AMD)
		return false;

	return true;
}


static void
set_normal_pstate(void* /* dummy */, int cpu)
{
	if (sLegacy) {
		// P0 until the scheduler asks for less
		x86_write_msr(MSR_AMD_PSTATE_CONTROL, sLegacyFastest);
		sLegacyCPUs[cpu].pstate = sLegacyFastest;
		sLegacyCPUs[cpu].changed = system_time();
		return;
	}

	x86_write_msr(MSR_AMD_CPPC_ENABLE, 1);

	uint64 cap1 = x86_read_msr(MSR_AMD_CPPC_CAP1);
	sHWPLowest = AMD_CPPC_LOWEST_PERF(cap1);
	sHWPEfficient = AMD_CPPC_LOWNONLIN_PERF(cap1);
	sHWPGuaranteed = AMD_CPPC_NOMINAL_PERF(cap1);
	sHWPHighest = AMD_CPPC_HIGHEST_PERF(cap1);

	// The hardware chooses the clock between the efficient and the highest
	// level, in both modes. Power saving asks it to lean towards efficiency
	// ("balance_performance", Linux's default when balanced), and no longer
	// forbids boosting: capped at the guaranteed level, a busy thread on
	// battery ran at 2.0 instead of up to 4.8 GHz (Ryzen AI 5 340). Against
	// "balance_power" (0xbf) that drew the same (idle 4.0 W, one busy thread
	// 5.7 W, a 2 ms job every frame 4.25 W) while the job took 3.5 instead of
	// 4.3 ms and a thread reached half its speed after 1-2 instead of 13-23 ms.
	uint64 request = AMD_CPPC_MIN_PERF(sHWPEfficient);
	request |= AMD_CPPC_MAX_PERF(sHWPHighest);
	request |= AMD_CPPC_EPP_PERF(
		sSavePower ? AMD_CPPC_EPP_BALANCE_PERFORMANCE : AMD_CPPC_EPP_PERFORMANCE);
	x86_write_msr(MSR_AMD_CPPC_REQ, request & 0xffffffff);
}


/*!	Zen 1 and Zen+: the P-states the firmware enabled, between the current
	limit (fastest) and the last one.
*/
static bool
init_legacy_pstates()
{
	cpu_ent* cpu = &gCPU[0];
	if (cpu->arch.family + cpu->arch.extended_family < 0x17
		|| !x86_check_feature(IA32_FEATURE_AMD_HW_PSTATE, FEATURE_EXT_7_EDX)) {
		return false;
	}

	uint64 limit = x86_read_msr(MSR_AMD_PSTATE_CURRENT_LIMIT);
	sLegacyFastest = limit & 0x7;
	sLegacySlowest = (limit >> 4) & 0x7;
	if (sLegacySlowest <= sLegacyFastest)
		return false;

	for (uint32 i = sLegacyFastest; i <= sLegacySlowest; i++) {
		uint64 definition = x86_read_msr(MSR_F10H_PSTATEDEF(i));
		uint32 fid = definition & 0xff;
		uint32 dfsID = (definition >> 8) & 0x3f;
		if ((definition & PSTATEDEF_EN) == 0 || dfsID == 0) {
			// a hole: only the states before it
			if (i == sLegacyFastest)
				return false;
			sLegacySlowest = i - 1;
			break;
		}
		sLegacyMHz[i] = fid * 200 / dfsID;
	}
	if (sLegacySlowest <= sLegacyFastest)
		return false;

	sLegacyCPUs = (legacy_cpu*)calloc(smp_get_num_cpus(), sizeof(legacy_cpu));
	if (sLegacyCPUs == NULL)
		return false;

	dprintf("using AMD P-states P%" B_PRIu32 "-P%" B_PRIu32 ":",
		sLegacyFastest, sLegacySlowest);
	for (uint32 i = sLegacyFastest; i <= sLegacySlowest; i++)
		dprintf(" %" B_PRIu32 " MHz", sLegacyMHz[i]);
	dprintf(", staying at P%" B_PRIu32 "\n", sLegacyFastest);
	return true;
}


static status_t
init_pstates()
{
	int32 cpuCount = smp_get_num_cpus();
	for (int32 i = 0; i < cpuCount; i++) {
		if (!is_cpu_model_supported(&gCPU[i]))
			return B_ERROR;
	}

	if (gCPU[0].arch.family + gCPU[0].arch.extended_family >= 0x17
		&& x86_check_feature(AMD_FEATURE_RAPL, FEATURE_EXT_7_EDX)) {
		sEnergySyscalls = register_generic_syscall(AMD_ENERGY_SYSCALLS,
			energy_syscall, 1, 0) == B_OK;
	}

	if (!x86_check_feature(IA32_FEATURE_CPPC, FEATURE_EXT_8_EBX)) {
		sLegacy = init_legacy_pstates();
		if (!sLegacy) {
			if (sEnergySyscalls)
				unregister_generic_syscall(AMD_ENERGY_SYSCALLS, 1);
			return B_ERROR;
		}
		call_all_cpus_sync(set_normal_pstate, NULL);
		return B_OK;
	}

	dprintf("using AMD P-States (capabilities: 0x%08" B_PRIx64 ")\n",
		x86_read_msr(MSR_AMD_CPPC_CAP1));

	pstates_set_scheduler_mode(SCHEDULER_MODE_LOW_LATENCY);

	call_all_cpus_sync(set_normal_pstate, NULL);
	return B_OK;
}


static status_t
uninit_pstates()
{
	call_all_cpus_sync(set_normal_pstate, NULL);
	if (sEnergySyscalls)
		unregister_generic_syscall(AMD_ENERGY_SYSCALLS, 1);
	free(sLegacyCPUs);
	sLegacyCPUs = NULL;
	sLegacy = false;

	return B_OK;
}


static status_t
std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			return init_pstates();

		case B_MODULE_UNINIT:
			uninit_pstates();
			return B_OK;
	}

	return B_ERROR;
}


static cpufreq_module_info sAMDPStates = {
	{
		AMD_PSTATES_MODULE_NAME,
		0,
		std_ops,
	},

	1.0f,

	pstates_set_scheduler_mode,

	pstates_increase_performance,
	pstates_decrease_performance,
};


module_info* modules[] = {
	(module_info*)&sAMDPStates,
	NULL
};

