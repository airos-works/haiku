/* SPDX-License-Identifier: MIT */
/*
 * Many threads that are alive at the same time each use thread-local storage
 * for the first time (haiku#161, second part).
 *
 * A thread's first access allocates its TLS vector with realloc() from the
 * runtime loader's heap. That realloc() did not enlarge the heap as malloc()
 * does: once the heap (64 KiB at first) was full the allocation failed,
 * __tls_get_addr() returned a null block and the thread crashed at the
 * address of its variable, as did every later thread until some malloc()
 * happened to enlarge the heap.
 *
 * Build: gcc -O2 -Wall tls_many_threads_test.c -o tls_many_threads_test
 */
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { kThreadCount = 3000 };

static __thread unsigned long sValue = 17;
static sem_t sDone;
static sem_t sExit;
static int sFailures;
static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;


static void
crashed(int signal)
{
	static const char kMessage[]
		= "FAIL a thread crashed at its first use of thread-local storage\n";
	write(STDOUT_FILENO, kMessage, sizeof(kMessage) - 1);
	_exit(1);
}


static void*
worker(void* cookie)
{
	unsigned long expected = (unsigned long)cookie;
	unsigned long* address = &sValue;
	int failed = address == NULL || *address != 17;
	if (!failed) {
		*address = expected;
		failed = sValue != expected;
	}
	if (failed) {
		pthread_mutex_lock(&sLock);
		sFailures++;
		pthread_mutex_unlock(&sLock);
	}
	sem_post(&sDone);

	/* stay alive, so that the vector and the block stay allocated */
	sem_wait(&sExit);
	return NULL;
}


int
main(void)
{
	static pthread_t threads[kThreadCount];
	pthread_attr_t attr;
	int count = 0;

	signal(SIGSEGV, crashed);
	signal(SIGBUS, crashed);
	alarm(120);
	sem_init(&sDone, 0, 0);
	sem_init(&sExit, 0, 0);
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 64 * 1024);

	for (int i = 0; i < kThreadCount; i++) {
		if (pthread_create(&threads[i], &attr, worker,
				(void*)(unsigned long)(1000 + i)) != 0) {
			break;
		}
		count++;
		sem_wait(&sDone);
	}

	for (int i = 0; i < count; i++)
		sem_post(&sExit);
	for (int i = 0; i < count; i++)
		pthread_join(threads[i], NULL);

	printf("%s threads=%d failures=%d\n",
		sFailures == 0 && count > 1000 ? "PASS" : "FAIL", count, sFailures);
	return sFailures == 0 && count > 1000 ? 0 : 1;
}
