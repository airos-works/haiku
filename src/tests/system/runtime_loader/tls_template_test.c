// SPDX-License-Identifier: MIT
// Grow the loader's TLS template vector while fresh threads copy a large
// template. All libraries stay loaded until the workers have finished.
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#define THREADS 12
#define ITERATIONS 48
#define MODULES 256

static atomic_int sReady, sGo, sFailures, sChecks;
static void* (*sAddress)(void);
static unsigned sSize;


static void*
touch_tls(void* unused)
{
	(void)unused;
	volatile unsigned char* data = sAddress();
	if (data == NULL || data[0] != 37 || data[sSize / 2] != 0
		|| data[sSize - 1] != 91) {
		atomic_fetch_add(&sFailures, 1);
		fprintf(stderr, "TLS initialization mismatch: %u,%u,%u; expected 37,0,91\n",
			data != NULL ? data[0] : 0, data != NULL ? data[sSize / 2] : 0,
			data != NULL ? data[sSize - 1] : 0);
	} else {
		data[sSize / 2] = 77;
		if (((volatile unsigned char*)sAddress())[sSize / 2] != 77)
			atomic_fetch_add(&sFailures, 1);
	}
	atomic_fetch_add(&sChecks, 1);
	return NULL;
}


static void*
worker(void* unused)
{
	(void)unused;
	atomic_fetch_add(&sReady, 1);
	while (!atomic_load(&sGo))
		usleep(50);
	for (int i = 0; i < ITERATIONS; i++) {
		pthread_t child;
		if (pthread_create(&child, NULL, touch_tls, NULL) != 0
			|| pthread_join(child, NULL) != 0) {
			atomic_fetch_add(&sFailures, 1);
			break;
		}
	}
	return NULL;
}


int
main(void)
{
	alarm(20);
	void* base = dlopen("./base.so", RTLD_NOW | RTLD_LOCAL);
	if (base == NULL) {
		fprintf(stderr, "%s\n", dlerror());
		return 2;
	}
	sAddress = dlsym(base, "tls_address");
	unsigned (*size)(void) = dlsym(base, "tls_size");
	if (sAddress == NULL || size == NULL || (sSize = size()) < 3)
		return 2;
	pthread_t threads[THREADS];
	for (int i = 0; i < THREADS; i++) {
		if (pthread_create(&threads[i], NULL, worker, NULL) != 0)
			return 2;
	}
	while (atomic_load(&sReady) != THREADS)
		usleep(50);
	atomic_store(&sGo, 1);
	void* modules[MODULES];
	for (int i = 0; i < MODULES; i++) {
		char path[80];
		snprintf(path, sizeof(path), "./modules/module%03d.so", i);
		modules[i] = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		if (modules[i] == NULL) {
			fprintf(stderr, "%s\n", dlerror());
			return 2;
		}
	}
	for (int i = 0; i < THREADS; i++) {
		if (pthread_join(threads[i], NULL) != 0)
			return 2;
	}
	for (int i = MODULES - 1; i >= 0; i--)
		dlclose(modules[i]);
	dlclose(base);
	int failed = atomic_load(&sFailures) != 0
		|| atomic_load(&sChecks) != THREADS * ITERATIONS;
	printf("%s checks=%d failures=%d modules=%d threads=%d\n",
		failed ? "FAIL" : "PASS", atomic_load(&sChecks), atomic_load(&sFailures),
		MODULES, THREADS);
	return failed;
}
