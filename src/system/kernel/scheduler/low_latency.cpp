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

	// The package lists lag behind (a core leaves them only when it runs
	// what it was given): ask the cores themselves.
	return FastestIdleCore(threadData, 0);
}


/*!	A thread that is not using up its quanta (it waits for something
	between bursts of work: user interface, audio, a server answering) on
	cores of several speeds: the fastest core where it would run at once ahead
	of a thread of lower priority, when that is at least half as fast again
	as the \a capacity it would get otherwise. The thread it takes the core
	from moves to an idle one (see reschedule()).
	Busy threads that kept the fast cores otherwise left it a slow one, where
	a 3 ms job took 14 ms (ROCK 5: Cortex-A76 against Cortex-A55). Between
	cores that differ less (Zen 5 and Zen 5c: 4 to 3) the move costs the
	other thread about what it gains, and short wake-ups then kept bouncing
	busy threads between the two kinds of core.
*/
static CoreEntry*
faster_core_to_preempt(const ThreadData* threadData, uint32 capacity)
{
	if (!gHeterogeneousCores
		|| (uint64)capacity * 3 / 2 >= gMaxCoreCapacity) {
		return NULL;
	}
	if (threadData->IsPenalized())
		return NULL;
	return FastestPreemptibleCore(threadData,
		(uint32)((uint64)capacity * 3 / 2) + 1,
		threadData->GetEffectivePriority());
}


/*!	Cores of several speeds, none of them idle: the fastest one faster than
	\a capacity with a processor that has nothing to do (the other half of
	a core with two threads). Nobody has to make room there.
*/
static CoreEntry*
faster_core_with_idle_cpu(const ThreadData* threadData, uint32 capacity)
{
	if (!gHeterogeneousCores || capacity >= gMaxCoreCapacity)
		return NULL;
	return FastestPreemptibleCore(threadData, capacity + 1,
		B_IDLE_PRIORITY + 1);
}


static CoreEntry*
choose_core(const ThreadData* threadData)
{
	SCHEDULER_ENTER_FUNCTION();

	CPUSet mask = threadData->GetCPUMask();
	const bool useMask = !mask.IsEmpty();

	CoreEntry* core = find_idle_core(threadData);
	CoreEntry* faster = faster_core_to_preempt(threadData,
		core != NULL ? core->Capacity() : 0);
	if (faster == NULL && core == NULL)
		faster = faster_core_with_idle_cpu(threadData, 0);
	if (faster != NULL)
		return faster;
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

	// Cores of several speeds: where the thread would run at once without
	// taking anyone's place - its own core when that has nothing to do, else
	// the fastest idle one - unless a faster core has nothing to do either,
	// or runs a thread of lower priority on a much faster core (it took a
	// 3 ms job of a thread that woke up on its busy big core to an idle
	// little one, where half of it ran at a third of the speed).
	if (gHeterogeneousCores) {
		CoreEntry* free = core->ThreadCount() == 0
			? core : find_idle_core(threadData);
		uint32 capacity = free != NULL ? free->Capacity() : 0;
		CoreEntry* faster = NULL;
		if (capacity < gMaxCoreCapacity) {
			faster = FastestIdleCore(threadData, capacity + 1);
			if (faster == NULL)
				faster = faster_core_to_preempt(threadData, capacity);
			if (faster == NULL && free == NULL)
				faster = faster_core_with_idle_cpu(threadData, 0);
		}
		if (faster != NULL)
			return faster;
		if (free != NULL) {
			// Not from a faster core to a slower one only because something
			// else is there for a moment (a thread that woke up and took the
			// processor): only when the rest of what that core has to do
			// keeps it busy on its own. Busy threads otherwise went from big
			// cores to little ones at every wake-up of another thread, and
			// four of them on four big cores did 5 % less.
			if (free->Capacity() < core->Capacity()) {
				int32 rest = core->GetRequestedLoad() - threadData->GetLoad()
					/ std::max(core->CPUCount(), (int32)1);
				if (rest < kHighLoad)
					return core;
			}
			return free;
		}
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

	int32 threadLoad = threadData->GetLoad()
		/ std::max(core->CPUCount(), (int32)1);
		// (0 while its last CPU is being disabled)
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

