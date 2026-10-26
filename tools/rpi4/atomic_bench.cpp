/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
// Cost of atomic operations, locks and a kernel round trip, to compare builds
// that differ in atomic code generation (outlined libgcc helpers or inline
// exclusive load/store loops; see SetCPUProfile in build/jam/ArchitectureRules).
// The program itself is built with the build's flags, as is the libroot it
// runs against. Prints the median of the rounds in nanoseconds per operation:
//	rpi4_atomic_bench [operations per round] [rounds]
#include <OS.h>
#include <SupportDefs.h>
#include <algorithm>
#include <atomic>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

static void require(bool ok, const char* message)
{
	if (!ok) {
		fprintf(stderr, "ATOMIC_BENCH_FAIL %s\n", message);
		exit(1);
	}
}

static uint32 sOperations;
static int32 sShared;
static int32 sPerThread[4][16];
	// one cache line per thread
static pthread_mutex_t sMutex = PTHREAD_MUTEX_INITIALIZER;
static sem_id sStartSem;

static int32 contended_adder(void* data)
{
	int32* counter = (int32*)data;
	require(acquire_sem(sStartSem) == B_OK, "start semaphore");
	for (uint32 i = 0; i < sOperations; i++)
		atomic_add(counter, 1);
	return 0;
}

static int32 contended_locker(void*)
{
	require(acquire_sem(sStartSem) == B_OK, "start semaphore");
	for (uint32 i = 0; i < sOperations; i++) {
		pthread_mutex_lock(&sMutex);
		sShared++;
		pthread_mutex_unlock(&sMutex);
	}
	return 0;
}

// Wall time of four threads running "function" at once.
static bigtime_t run_four(thread_func function, bool separateCounters)
{
	thread_id threads[4];
	sStartSem = create_sem(0, "atomic bench start");
	require(sStartSem >= 0, "create semaphore");
	for (int i = 0; i < 4; i++) {
		threads[i] = spawn_thread(function, "atomic bench worker",
			B_NORMAL_PRIORITY, separateCounters ? sPerThread[i] : &sShared);
		require(threads[i] >= 0, "spawn thread");
		require(resume_thread(threads[i]) == B_OK, "resume thread");
	}
	snooze(20000);
	bigtime_t start = system_time();
	require(release_sem_etc(sStartSem, 4, 0) == B_OK, "release semaphore");
	for (int i = 0; i < 4; i++) {
		status_t result;
		require(wait_for_thread(threads[i], &result) == B_OK, "wait for thread");
	}
	bigtime_t elapsed = system_time() - start;
	delete_sem(sStartSem);
	return elapsed;
}

template<typename Function>
static double time_one(Function function)
{
	bigtime_t start = system_time();
	function();
	return (system_time() - start) * 1000.0 / sOperations;
}

static void report(const char* name, std::vector<double>& values)
{
	std::sort(values.begin(), values.end());
	printf("ATOMIC_BENCH %s ns_per_op=%.3f min=%.3f max=%.3f\n", name,
		values[values.size() / 2], values.front(), values.back());
}

int main(int argc, char** argv)
{
	sOperations = argc > 1 ? strtoul(argv[1], NULL, 0) : 4000000;
	const unsigned rounds = argc > 2 ? strtoul(argv[2], NULL, 0) : 9;
	require(sOperations >= 1000 && sOperations <= 100000000, "operation count");
	require(rounds > 0 && rounds <= 100, "round count");
	setvbuf(stdout, NULL, _IOLBF, 0);

	std::vector<double> add, testAndSet, cxxFetchAdd, cxxCompareExchange,
		mutex, semaphore, contendedAdd, separateAdd, contendedMutex;
	sem_id sem = create_sem(0, "atomic bench semaphore");
	require(sem >= 0, "create semaphore");

	for (unsigned round = 0; round < rounds; round++) {
		int32 value = 0;
		add.push_back(time_one([&] {
			for (uint32 i = 0; i < sOperations; i++)
				atomic_add(&value, 1);
		}));
		require(value == (int32)sOperations, "atomic_add total");

		testAndSet.push_back(time_one([&] {
			for (uint32 i = 0; i < sOperations; i++)
				atomic_test_and_set(&value, (int32)i, (int32)i - 1);
		}));

		std::atomic<int64_t> counter(0);
		cxxFetchAdd.push_back(time_one([&] {
			for (uint32 i = 0; i < sOperations; i++)
				counter.fetch_add(1, std::memory_order_acq_rel);
		}));
		require(counter == (int64_t)sOperations, "std::atomic total");

		cxxCompareExchange.push_back(time_one([&] {
			for (uint32 i = 0; i < sOperations; i++) {
				int64_t expected = counter.load(std::memory_order_relaxed);
				counter.compare_exchange_strong(expected, expected + 1);
			}
		}));
		require(counter == 2 * (int64_t)sOperations, "compare exchange total");

		mutex.push_back(time_one([&] {
			for (uint32 i = 0; i < sOperations; i++) {
				pthread_mutex_lock(&sMutex);
				pthread_mutex_unlock(&sMutex);
			}
		}));

		// a kernel round trip per operation: fewer of them
		const uint32 operations = sOperations;
		sOperations = std::max<uint32>(operations / 16, 1000);
		semaphore.push_back(time_one([&] {
			for (uint32 i = 0; i < sOperations; i++) {
				release_sem_etc(sem, 1, B_DO_NOT_RESCHEDULE);
				acquire_sem(sem);
			}
		}));

		sShared = 0;
		contendedAdd.push_back(run_four(contended_adder, false) * 1000.0
			/ (4.0 * sOperations));
		require(sShared == 4 * (int32)sOperations, "contended total");

		separateAdd.push_back(run_four(contended_adder, true) * 1000.0
			/ (4.0 * sOperations));

		sShared = 0;
		contendedMutex.push_back(run_four(contended_locker, false) * 1000.0
			/ (4.0 * sOperations));
		require(sShared == 4 * (int32)sOperations, "mutex total");
		sOperations = operations;
	}
	delete_sem(sem);

	report("atomic_add", add);
	report("atomic_test_and_set", testAndSet);
	report("std_atomic_fetch_add64", cxxFetchAdd);
	report("std_atomic_compare_exchange64", cxxCompareExchange);
	report("pthread_mutex_uncontended", mutex);
	report("semaphore_release_acquire", semaphore);
	report("atomic_add_4_threads_shared", contendedAdd);
	report("atomic_add_4_threads_separate", separateAdd);
	report("pthread_mutex_4_threads", contendedMutex);
	return 0;
}
