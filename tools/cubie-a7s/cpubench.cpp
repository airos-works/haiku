/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	cubie_cpubench: a fixed integer workload (hashing a 256 KiB buffer that
	stays in the caches, with multiplies, shifts and loads) on N threads for
	some seconds; prints each thread's rate and the total, in millions of
	64-bit words a second. For the thermal tests: the cost of a throttled
	CPU, cool against hot.

		cubie_cpubench [threads] [seconds]
*/


#include <OS.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static const size_t kWords = 256 * 1024 / 8;

struct worker {
	thread_id	thread;
	bigtime_t	until;
	uint64		words;
	uint64		hash;
	int32		cpu;
};


static status_t
work(void* data)
{
	worker* self = (worker*)data;
	uint64* buffer = (uint64*)malloc(kWords * 8);
	if (buffer == NULL)
		return B_NO_MEMORY;
	uint64 x = 0x9e3779b97f4a7c15ull ^ (uint64)self->thread;
	for (size_t i = 0; i < kWords; i++) {
		x ^= x << 13;
		x ^= x >> 7;
		x ^= x << 17;
		buffer[i] = x;
	}

	uint64 hash = 0;
	uint64 words = 0;
	while (system_time() < self->until) {
		for (size_t i = 0; i < kWords; i++) {
			hash = (hash ^ buffer[i]) * 0x100000001b3ull;
			hash ^= hash >> 29;
		}
		words += kWords;
	}
	self->words = words;
	self->hash = hash;
	free(buffer);
	return B_OK;
}


int
main(int argc, char** argv)
{
	system_info info;
	get_system_info(&info);
	int32 threads = argc > 1 ? atoi(argv[1]) : info.cpu_count;
	int32 seconds = argc > 2 ? atoi(argv[2]) : 10;
	if (threads < 1 || threads > 64 || seconds < 1) {
		fprintf(stderr, "usage: %s [threads] [seconds]\n", argv[0]);
		return 1;
	}

	worker* workers = new worker[threads];
	bigtime_t start = system_time();
	for (int32 i = 0; i < threads; i++) {
		workers[i].until = start + seconds * 1000000LL;
		workers[i].words = 0;
		workers[i].thread = spawn_thread(work, "cpubench", B_NORMAL_PRIORITY,
			&workers[i]);
	}
	for (int32 i = 0; i < threads; i++)
		resume_thread(workers[i].thread);

	double total = 0;
	for (int32 i = 0; i < threads; i++) {
		status_t result;
		wait_for_thread(workers[i].thread, &result);
	}
	double elapsed = (system_time() - start) / 1e6;
	printf("cpubench: %" B_PRId32 " threads, %.1f s:", threads, elapsed);
	for (int32 i = 0; i < threads; i++) {
		double rate = workers[i].words / elapsed / 1e6;
		total += rate;
		printf(" %.0f", rate);
	}
	printf(" = %.0f Mwords/s\n", total);
	delete[] workers;
	return 0;
}
