/*
 * Copyright 2013, Paweł Dziepak, pdziepak@quarnos.org.
 * Distributed under the terms of the MIT License.
 */


#include <util/AutoLock.h>

#include "scheduler_common.h"
#include "scheduler_cpu.h"
#include "scheduler_modes.h"
#include "scheduler_profiler.h"
#include "scheduler_thread.h"


using namespace Scheduler;


const bigtime_t kCacheExpire = 100000;


static void
switch_to_mode()
{
}


static void
set_cpu_enabled(int32 /* cpu */, bool /* enabled */)
{
}


static bool
has_cache_expired(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();
	if (threadData->WentSleepActive() == 0)
		return false;
	CoreEntry* core = threadData->Core();
	bigtime_t activeTime = core->GetActiveTime();
	return activeTime - threadData->WentSleepActive() > kCacheExpire;
}


/*!	A core with nothing to do and nothing on its way. A core stays in its
	package's list of idle ones until it gets to run the thread it was given,
	so the list alone sent two threads woken together to the same core, where
	the second waited for the first while other cores sat idle.
*/
static CoreEntry*
find_idle_core(const ThreadData* threadData)
{
	// Cores of several speeds: the fastest idle one.
	if (gHeterogeneousCores)
		return FastestIdleCore(threadData, 0);

	// wake new package
	PackageEntry* package = gIdlePackageList.Last();
	if (package == NULL) {
		// wake new core
		package = PackageEntry::GetMostIdlePackage();
	}
	if (package == NULL)
		return NULL;

	CPUSet mask = threadData->GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	for (int32 index = 0; index < smp_get_num_cpus(); index++) {
		CoreEntry* core = package->GetIdleCore(index);
		if (core == NULL)
			break;
		if (useMask && !core->CPUMask().Matches(mask))
			continue;
		if (core->ThreadCount() == 0)
			return core;
	}
	return NULL;
}


static CoreEntry*
choose_core(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();

	CPUSet mask = threadData->GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	CoreEntry* core = find_idle_core(threadData);
	if (core == NULL) {
		ReadSpinLocker coreLocker(gCoreHeapsLock);
		int32 index = 0;
		// no idle cores, use least occupied core
		do {
			core = gCoreLoadHeap.PeekMinimum(index++);
		} while (useMask && core != NULL && !core->CPUMask().Matches(mask));
		if (core == NULL) {
			index = 0;
			do {
				core = gCoreHighLoadHeap.PeekMinimum(index++);
			} while (useMask && core != NULL && !core->CPUMask().Matches(mask));
		}
	}

	ASSERT(core != NULL);
	return core;
}


static CoreEntry*
rebalance(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();

	CoreEntry* core = threadData->Core();
	ASSERT(core != NULL);

	// A faster core has nothing to do: there the thread runs now.
	if (gHeterogeneousCores && core->Capacity() < gMaxCoreCapacity) {
		CoreEntry* faster = FastestIdleCore(threadData, core->Capacity());
		if (faster != NULL)
			return faster;
	}

	// The thread's core has something to run already and another has not:
	// there the thread runs now.
	if (core->ThreadCount() > 0) {
		CoreEntry* idle = find_idle_core(threadData);
		if (idle != NULL)
			return idle;
	}

	// Get the least loaded core.
	ReadSpinLocker coreLocker(gCoreHeapsLock);
	CPUSet mask = threadData->GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	int32 index = 0;
	CoreEntry* other;
	do {
		other = gCoreLoadHeap.PeekMinimum(index++);
		if (other != NULL && (useMask && other->CPUMask().IsEmpty()))
			panic("other->CPUMask().IsEmpty()\n");
	} while (useMask && other != NULL && !other->CPUMask().Matches(mask));

	if (other == NULL) {
		index = 0;
		do {
			other = gCoreHighLoadHeap.PeekMinimum(index++);
		} while (useMask && other != NULL && !other->CPUMask().Matches(mask));
	}
	coreLocker.Unlock();
	ASSERT(other != NULL);

	// Check if the least loaded core is significantly less loaded than
	// the current one. For the current one that is what its threads ask
	// for: GetLoad() stops at "all of it", and a core with two threads that
	// each want a processor to themselves looked no fuller than a core with
	// one. The second thread then never left (moving it "would not bring
	// the two loads closer"), while another core sat idle.
	int32 coreLoad = core->GetRequestedLoad();
	int32 otherLoad = other->GetLoad();
	if (other == core || otherLoad + kLoadDifference >= coreLoad)
		return core;

	// Check whether migrating the current thread would result in both core
	// loads become closer to the average.
	int32 difference = coreLoad - otherLoad - kLoadDifference;
	ASSERT(difference > 0);

	int32 threadLoad = threadData->GetLoad() / core->CPUCount();
	return difference >= threadLoad ? other : core;
}


static void
rebalance_irqs(bool idle)
{
	SCHEDULER_ENTER_FUNCTION();

	if (idle)
		return;

	cpu_ent* cpu = get_cpu_struct();
	SpinLocker locker(cpu->irqs_lock);

	irq_assignment* chosen = NULL;
	irq_assignment* irq = cpu->irqs.First();

	int32 totalLoad = 0;
	while (irq != NULL) {
		if (chosen == NULL || chosen->load < irq->load)
			chosen = irq;
		totalLoad += irq->load;
		irq = cpu->irqs.GetNext(irq);
	}

	locker.Unlock();

	if (chosen == NULL || totalLoad < kLowLoad)
		return;

	ReadSpinLocker coreLocker(gCoreHeapsLock);
	CoreEntry* other = gCoreLoadHeap.PeekMinimum();
	if (other == NULL)
		other = gCoreHighLoadHeap.PeekMinimum();
	coreLocker.Unlock();

	int32 newCPU = other->CPUHeap()->PeekRoot()->ID();

	ASSERT(other != NULL);

	CoreEntry* core = CoreEntry::GetCore(cpu->cpu_num);
	if (other == core)
		return;
	if (other->GetLoad() + kLoadDifference >= core->GetLoad())
		return;

	assign_io_interrupt_to_cpu(chosen->irq, newCPU);
}


scheduler_mode_operations gSchedulerLowLatencyMode = {
	"low latency",

	1000,
	100,
	{ 2, 5 },

	5000,

	switch_to_mode,
	set_cpu_enabled,
	has_cache_expired,
	choose_core,
	rebalance,
	rebalance_irqs,
};

