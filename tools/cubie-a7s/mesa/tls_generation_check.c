/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Thread-local storage of a library loaded with dlopen(), as Mesa's drivers
// are, in a new thread, after a library with TLS has been unloaded. On air/OS
// the runtime loader starts a thread's dynamic thread vector at generation 0.
// Once any library with TLS has been unloaded (the generation is above 0),
// the thread's second TLS access frees and re-creates the blocks of every
// library registered since. That loses what the first access wrote. Summit's
// WebProcess crashed so: zink's cache thread wrote util_call_once_data()'s
// context, and the call_once() callback read it back as NULL and called it.
//   tls_generation_check [library]
// (default: libtls_generation_check.so next to the program)
// 1. dlopen() and dlclose() the library: the generation moves on;
// 2. dlopen() it again: it is registered at the new generation;
// 3. a new thread writes the library's thread-local variable (its first TLS
//    access) and reads it back (its second): the runtime loader check;
// 4. another new thread reads twice first, as Mesa's own threads now do on
//    air/OS, then writes and reads: the workaround check.
// The library is this file built with -DTLS_GENERATION_LIBRARY.


#ifdef TLS_GENERATION_LIBRARY

static __thread int sValue;

void
tls_check_set(int value)
{
	sValue = value;
}


int
tls_check_get(void)
{
	return sValue;
}

#else	// the program

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef void (*set_function)(int value);
typedef int (*get_function)(void);

static set_function sSet;
static get_function sGet;

struct check {
	int		prime;
	int		written;
	int		read;
};


static void*
check_thread(void* data)
{
	struct check* check = data;
	if (check->prime) {
		sGet();
		sGet();
	}
	sSet(check->written);
	check->read = sGet();
	return NULL;
}


static int
run_check(int prime, int value)
{
	struct check check = { prime, value, -1 };
	pthread_t thread;
	if (pthread_create(&thread, NULL, check_thread, &check) != 0) {
		printf("FAIL: pthread_create\n");
		exit(1);
	}
	pthread_join(thread, NULL);
	int ok = check.read == check.written;
	printf("%s: %s thread wrote %d, read back %d\n", ok ? "ok" : "FAIL",
		prime ? "primed" : "new", check.written, check.read);
	return ok;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	char path[1024];
	if (argc > 1)
		snprintf(path, sizeof(path), "%s", argv[1]);
	else {
		const char* slash = strrchr(argv[0], '/');
		int length = slash != NULL ? (int)(slash - argv[0]) : 1;
		snprintf(path, sizeof(path), "%.*s/libtls_generation_check.so",
			length, slash != NULL ? argv[0] : ".");
	}

	void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (library == NULL) {
		printf("FAIL: dlopen %s: %s\n", path, dlerror());
		return 1;
	}
	dlclose(library);
	library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (library == NULL) {
		printf("FAIL: dlopen %s again: %s\n", path, dlerror());
		return 1;
	}
	sSet = (set_function)dlsym(library, "tls_check_set");
	sGet = (get_function)dlsym(library, "tls_check_get");
	if (sSet == NULL || sGet == NULL) {
		printf("FAIL: tls_check_set/tls_check_get not in %s\n", path);
		return 1;
	}
	printf("%s loaded, unloaded and loaded again\n", path);

	int loaderOk = run_check(0, 42);
	int primedOk = run_check(1, 43);
	if (!loaderOk) {
		printf("the runtime loader lost the new thread's first TLS write "
			"(its vector started at generation 0)\n");
	}
	if (!primedOk)
		printf("priming the thread's TLS did not help\n");

	dlclose(library);
	printf("%s\n", loaderOk && primedOk ? "PASS" : "FAIL");
	return loaderOk && primedOk ? 0 : 1;
}

#endif	// TLS_GENERATION_LIBRARY
