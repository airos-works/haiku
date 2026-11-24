/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	scheduler_pinned_power_saving_test [seconds]: a thread with a CPU
	affinity mask that wakes up in power-saving mode while no core is idle
	(airos-works/haiku#168).

	The power-saving scheduler's choose_core() asked for an idle core when
	the cores the mask allows were all loaded, and looked at the answer
	without checking that there was one: with every core busy, a NULL
	dereference in the scheduler.

	One busy thread per CPU, bound to it (left to itself the power-saving
	mode does not use every core); a few threads, each bound to one CPU, that
	sleep 200 ms (longer than the scheduler takes a thread's cache for warm)
	and then compute for 50 ms. The test passes when the machine is still
	there at the end. The scheduler mode is set back.
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>
#include <scheduler.h>


extern "C" status_t _kern_set_thread_affinity(thread_id id, const void* mask,
	size_t size);

static volatile bool sStop;
static int32 sWakeups;


static status_t
bind_to_cpu(int32 cpu)
{
	uint64 mask[4] = {};
	mask[cpu / 64] = (uint64)1 << (cpu % 64);
	status_t status = _kern_set_thread_affinity(find_thread(NULL), mask,
		sizeof(mask));
	if (status != B_OK) {
		printf("setting the affinity to CPU %" B_PRId32 " failed: %s\n", cpu,
			strerror(status));
	}
	return status;
}


static status_t
busy_thread(void* _cpu)
{
	if (bind_to_cpu((int32)(addr_t)_cpu) != B_OK)
		return B_ERROR;

	volatile uint64 count = 0;
	while (!sStop)
		count++;
	return B_OK;
}


static status_t
bound_thread(void* _cpu)
{
	if (bind_to_cpu((int32)(addr_t)_cpu) != B_OK)
		return B_ERROR;

	while (!sStop) {
		snooze(200000);
		atomic_add(&sWakeups, 1);
		volatile uint64 count = 0;
		bigtime_t end = system_time() + 50000;
		while (system_time() < end)
			count++;
	}
	return B_OK;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	const int seconds = argc > 1 ? atoi(argv[1]) : 20;

	system_info info;
	get_system_info(&info);
	const int32 cpus = info.cpu_count;

	const int32 oldMode = get_scheduler_mode();
	status_t status = set_scheduler_mode(SCHEDULER_MODE_POWER_SAVING);
	if (status != B_OK) {
		printf("cannot set the power-saving mode: %s\n", strerror(status));
		return 1;
	}
	printf("power-saving mode (was mode %" B_PRId32 "), %" B_PRId32
		" CPUs, %d s\n", oldMode, cpus, seconds);

	thread_id threads[256];
	int32 count = 0;
	for (int32 i = 0; i < cpus && count < 200; i++) {
		threads[count] = spawn_thread(busy_thread, "busy", B_NORMAL_PRIORITY,
			(void*)(addr_t)i);
		resume_thread(threads[count++]);
	}
	// let the load of every core build up
	snooze(1000000);

	const int32 boundCount = cpus < 4 ? cpus : 4;
	for (int32 i = 0; i < boundCount; i++) {
		threads[count] = spawn_thread(bound_thread, "bound",
			B_NORMAL_PRIORITY, (void*)(addr_t)(cpus - 1 - i));
		resume_thread(threads[count++]);
	}

	for (int i = 0; i < seconds; i++) {
		snooze(1000000);
		printf("  %d s, %" B_PRId32 " wakeups of bound threads\n", i + 1,
			atomic_get(&sWakeups));
	}

	sStop = true;
	for (int32 i = 0; i < count; i++)
		wait_for_thread(threads[i], NULL);

	set_scheduler_mode(oldMode);

	bool ok = atomic_get(&sWakeups) >= seconds;
	printf("%s\n", ok ? "PASSED" : "FAILED (the bound threads did not run)");
	return ok ? 0 : 1;
}
