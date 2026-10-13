/* SPDX-License-Identifier: MIT */
/*
 * Build two independent copies of tls_generation_module.c as shared libraries,
 * then run this test with their paths. All dlclose calls occur after joining
 * the workers; no thread executes code from an unloaded image.
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef void* (*store_function)(unsigned long);
typedef unsigned long (*read_function)(void**);

enum { kThreadCount = 8, kRoundCount = 100 };
static store_function sStore;
static read_function sRead;
static pthread_barrier_t sBarrier;

struct result {
	unsigned long expected;
	unsigned long actual;
	void* first;
	void* second;
};

static void*
worker(void* cookie)
{
	struct result* result = cookie;
	// This write must be the thread's first access to dynamic TLS. A read
	// first would hide the bug by consuming the erroneous reinitialization.
	result->first = sStore(result->expected);
	pthread_barrier_wait(&sBarrier);
	result->actual = sRead(&result->second);
	return NULL;
}

static void*
open_module(const char* path)
{
	void* image = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (image == NULL) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		exit(2);
	}
	return image;
}

int
main(int argc, char** argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s liba.so libb.so\n", argv[0]);
		return 2;
	}
	void* a = open_module(argv[1]);
	store_function storeA = (store_function)dlsym(a, "store_value");
	read_function readA = (read_function)dlsym(a, "read_value");
	if (storeA == NULL || readA == NULL)
		return 2;
	void* addressA = storeA(987654);
	void* b = open_module(argv[2]);
	int checks = 0;
	int failures = 0;
	if (pthread_barrier_init(&sBarrier, NULL, kThreadCount) != 0)
		return 2;
	for (int round = 0; round < kRoundCount; round++) {
		// Advance the generation and reuse a DSO slot before first TLS access
		// in each new worker. The main thread keeps its existing vector.
		if (dlclose(b) != 0)
			return 2;
		b = open_module(argv[2]);
		sStore = (store_function)dlsym(b, "store_value");
		sRead = (read_function)dlsym(b, "read_value");
		if (sStore == NULL || sRead == NULL)
			return 2;
		pthread_t threads[kThreadCount];
		struct result results[kThreadCount];
		for (int i = 0; i < kThreadCount; i++) {
			results[i].expected = 1234567 + round * kThreadCount + i;
			if (pthread_create(&threads[i], NULL, worker, &results[i]) != 0)
				return 2;
		}
		for (int i = 0; i < kThreadCount; i++) {
			if (pthread_join(threads[i], NULL) != 0)
				return 2;
			checks += 2;
			failures += results[i].actual != results[i].expected;
			failures += results[i].first != results[i].second;
		}
		void* address;
		// A retained DSO's contents must survive other images' unloads.
		unsigned long valueA = readA(&address);
		checks += 2;
		failures += valueA != 987654;
		failures += address != addressA;
		// Reusing B's slot must invalidate the main thread's previous value.
		checks++;
		failures += sRead(&address) != 17;
		sStore(7654321);
		checks++;
		failures += sRead(&address) != 7654321;
	}

	pthread_barrier_destroy(&sBarrier);
	dlclose(b);
	dlclose(a);
	printf("%s checks=%d failures=%d rounds=%d threads=%d\n",
		failures == 0 ? "PASS" : "FAIL", checks, failures, kRoundCount,
		kThreadCount);
	return failures != 0;
}
