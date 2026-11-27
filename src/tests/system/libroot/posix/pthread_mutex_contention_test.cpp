/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Contended user mutexes: correctness of pthread mutexes, libroot's private
	mutex, condition variables and rwlocks built on them, timed locks, waiters
	that are killed, process-shared mutexes, and that no waiter starves while
	another thread keeps taking the mutex (#226: unlocking releases the mutex
	and wakes a waiter to compete for it instead of handing it over).
	Every phase has a deadline; the test prints what it checked and exits 1
	on the first failure.
*/


#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <OS.h>

#include <locks.h>


static const char* sPhase = "start";
static volatile bigtime_t sDeadline;


static void
fail(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	printf("FAIL (%s): ", sPhase);
	vprintf(format, args);
	printf("\n");
	va_end(args);
	fflush(stdout);
	_exit(1);
}


static void
crash_handler(int signal)
{
	printf("FAIL (%s): signal %d\n", sPhase, signal);
	fflush(stdout);
	_exit(1);
}


static status_t
watchdog(void*)
{
	while (true) {
		snooze(100000);
		if (sDeadline != 0 && system_time() > sDeadline) {
			printf("FAIL (%s): deadline passed, a thread hangs\n", sPhase);
			fflush(stdout);
			_exit(1);
		}
	}
	return B_OK;
}


static void
begin(const char* phase, int seconds)
{
	sPhase = phase;
	sDeadline = system_time() + seconds * 1000000LL;
}


static void
pass(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	printf("ok   %-24s ", sPhase);
	vprintf(format, args);
	printf("\n");
	va_end(args);
	fflush(stdout);
	sDeadline = 0;
}


static void
start_threads(pthread_t* threads, int count, void* (*function)(void*),
	void* (*argument)(int))
{
	for (int i = 0; i < count; i++) {
		if (pthread_create(&threads[i], NULL, function, argument(i)) != 0)
			fail("pthread_create");
	}
}


static void
join_threads(pthread_t* threads, int count)
{
	for (int i = 0; i < count; i++)
		pthread_join(threads[i], NULL);
}


static void*
index_argument(int index)
{
	return (void*)(addr_t)index;
}


// #pragma mark - counters


static pthread_mutex_t sMutex;
static mutex sLibrootMutex = MUTEX_INITIALIZER("contention test");
static volatile long sCounter;
static volatile long sShadow;
static int sMutexType;
static const long kRounds = 200000;
static int32 sTrylocks;


static void*
pthread_counter_thread(void*)
{
	for (long i = 0; i < kRounds; i++) {
		if (pthread_mutex_lock(&sMutex) != 0)
			fail("lock failed");
		if (sMutexType == PTHREAD_MUTEX_RECURSIVE
			&& pthread_mutex_lock(&sMutex) != 0)
			fail("recursive lock failed");
		long value = sCounter;
		sShadow = value;
		sCounter = value + 1;
		if (sShadow != value)
			fail("two threads in the critical section");
		if (sMutexType == PTHREAD_MUTEX_RECURSIVE
			&& pthread_mutex_unlock(&sMutex) != 0)
			fail("recursive unlock failed");
		if (pthread_mutex_unlock(&sMutex) != 0)
			fail("unlock failed");
		if ((i & 1023) == 0 && pthread_mutex_trylock(&sMutex) == 0) {
			sCounter++;
			pthread_mutex_unlock(&sMutex);
			atomic_add(&sTrylocks, 1);
		}
	}
	return NULL;
}


static void
test_pthread_counter(int type, const char* phase, int threads)
{
	begin(phase, 120);
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, type);
	pthread_mutex_init(&sMutex, &attr);
	pthread_mutexattr_destroy(&attr);
	sMutexType = type;
	sCounter = 0;
	sTrylocks = 0;

	pthread_t ids[32];
	bigtime_t start = system_time();
	start_threads(ids, threads, pthread_counter_thread, index_argument);
	join_threads(ids, threads);
	bigtime_t time = system_time() - start;

	if (sCounter != (long)threads * kRounds + sTrylocks)
		fail("counter %ld, expected %ld", sCounter,
			(long)threads * kRounds + sTrylocks);
	if (pthread_mutex_trylock(&sMutex) != 0)
		fail("mutex not free at the end");
	pthread_mutex_unlock(&sMutex);
	pthread_mutex_destroy(&sMutex);
	pass("%d threads x %ld (+%d trylocks): %.0f ns per lock", threads,
		kRounds, (int)sTrylocks, time * 1000.0 / ((double)threads * kRounds));
}


static void*
librootmutex_thread(void*)
{
	for (long i = 0; i < kRounds; i++) {
		mutex_lock(&sLibrootMutex);
		long value = sCounter;
		sShadow = value;
		sCounter = value + 1;
		if (sShadow != value)
			fail("two threads in the critical section");
		mutex_unlock(&sLibrootMutex);
	}
	return NULL;
}


static void
test_librootmutex_counter(int threads)
{
	begin("libroot mutex", 120);
	sCounter = 0;
	pthread_t ids[32];
	bigtime_t start = system_time();
	start_threads(ids, threads, librootmutex_thread, index_argument);
	join_threads(ids, threads);
	bigtime_t time = system_time() - start;
	if (sCounter != (long)threads * kRounds)
		fail("counter %ld, expected %ld", sCounter, (long)threads * kRounds);
	pass("%d threads x %ld: counter %ld, %.0f ns per lock", threads, kRounds,
		sCounter, time * 1000.0 / ((double)threads * kRounds));
}


// #pragma mark - many mutexes


static const int kMutexCount = 64;
static pthread_mutex_t sMutexes[kMutexCount];
static long sCounters[kMutexCount];
static long sExpected[16][kMutexCount];


static void*
many_mutexes_thread(void* data)
{
	int index = (int)(addr_t)data;
	unsigned seed = index * 7919 + 1;
	for (long i = 0; i < 100000; i++) {
		seed = seed * 1103515245 + 12345;
		int a = (seed >> 8) % kMutexCount;
		int b = (seed >> 20) % kMutexCount;
		if (a > b) {
			int t = a;
			a = b;
			b = t;
		}
		pthread_mutex_lock(&sMutexes[a]);
		if (b != a)
			pthread_mutex_lock(&sMutexes[b]);
		sCounters[a]++;
		sExpected[index][a]++;
		if (b != a) {
			sCounters[b]++;
			sExpected[index][b]++;
			pthread_mutex_unlock(&sMutexes[b]);
		}
		pthread_mutex_unlock(&sMutexes[a]);
	}
	return NULL;
}


static void
test_many_mutexes()
{
	begin("many mutexes", 120);
	for (int i = 0; i < kMutexCount; i++)
		pthread_mutex_init(&sMutexes[i], NULL);
	memset(sCounters, 0, sizeof(sCounters));
	memset(sExpected, 0, sizeof(sExpected));
	pthread_t ids[16];
	start_threads(ids, 16, many_mutexes_thread, index_argument);
	join_threads(ids, 16);
	long total = 0;
	for (int m = 0; m < kMutexCount; m++) {
		long expected = 0;
		for (int t = 0; t < 16; t++)
			expected += sExpected[t][m];
		if (expected != sCounters[m])
			fail("mutex %d: counter %ld, expected %ld", m, sCounters[m],
				expected);
		total += expected;
		pthread_mutex_destroy(&sMutexes[m]);
	}
	pass("16 threads, 64 mutexes, pairs: %ld increments", total);
}


// #pragma mark - condition variables


static pthread_mutex_t sQueueLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sNotEmpty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t sNotFull = PTHREAD_COND_INITIALIZER;
static const int kQueueSize = 8;
static long sQueue[kQueueSize];
static int sQueueCount;
static int sQueueHead;
static const long kItemsPerProducer = 50000;
static long sConsumedSum;
static long sConsumedCount;
static bool sBroadcast;


static void*
producer_thread(void*)
{
	for (long i = 1; i <= kItemsPerProducer; i++) {
		pthread_mutex_lock(&sQueueLock);
		while (sQueueCount == kQueueSize)
			pthread_cond_wait(&sNotFull, &sQueueLock);
		sQueue[(sQueueHead + sQueueCount) % kQueueSize] = i;
		sQueueCount++;
		if (sBroadcast)
			pthread_cond_broadcast(&sNotEmpty);
		else
			pthread_cond_signal(&sNotEmpty);
		pthread_mutex_unlock(&sQueueLock);
	}
	return NULL;
}


static void*
consumer_thread(void*)
{
	while (true) {
		pthread_mutex_lock(&sQueueLock);
		while (sQueueCount == 0 && sQueue[0] != -1)
			pthread_cond_wait(&sNotEmpty, &sQueueLock);
		if (sQueueCount == 0) {
			// sQueue[0] == -1: done
			pthread_mutex_unlock(&sQueueLock);
			return NULL;
		}
		long item = sQueue[sQueueHead];
		sQueueHead = (sQueueHead + 1) % kQueueSize;
		sQueueCount--;
		sConsumedSum += item;
		sConsumedCount++;
		if (sBroadcast)
			pthread_cond_broadcast(&sNotFull);
		else
			pthread_cond_signal(&sNotFull);
		pthread_mutex_unlock(&sQueueLock);
	}
}


static void
test_condition_variables(bool broadcast)
{
	begin(broadcast ? "cond broadcast" : "cond signal", 120);
	sBroadcast = broadcast;
	sQueueCount = 0;
	sQueueHead = 0;
	sConsumedSum = 0;
	sConsumedCount = 0;
	memset(sQueue, 0, sizeof(sQueue));

	pthread_t producers[4];
	pthread_t consumers[4];
	start_threads(consumers, 4, consumer_thread, index_argument);
	start_threads(producers, 4, producer_thread, index_argument);
	join_threads(producers, 4);

	// wait for the queue to drain, then tell the consumers to quit
	pthread_mutex_lock(&sQueueLock);
	while (sQueueCount > 0)
		pthread_cond_wait(&sNotFull, &sQueueLock);
	sQueue[0] = -1;
	sQueueHead = 0;
	pthread_cond_broadcast(&sNotEmpty);
	pthread_mutex_unlock(&sQueueLock);
	join_threads(consumers, 4);

	long expectedSum = 4 * (kItemsPerProducer * (kItemsPerProducer + 1) / 2);
	if (sConsumedCount != 4 * kItemsPerProducer || sConsumedSum != expectedSum)
		fail("consumed %ld items, sum %ld; expected %ld, %ld", sConsumedCount,
			sConsumedSum, 4 * kItemsPerProducer, expectedSum);
	pass("4 producers, 4 consumers: %ld items", sConsumedCount);
}


static volatile bool sGenerationDone;
static pthread_cond_t sGenerationCond = PTHREAD_COND_INITIALIZER;
static int32 sTimedWaitTimeouts;


static void*
timed_waiter_thread(void*)
{
	pthread_mutex_lock(&sQueueLock);
	while (!sGenerationDone) {
		struct timespec until;
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_nsec += 1000000;
		if (until.tv_nsec >= 1000000000) {
			until.tv_nsec -= 1000000000;
			until.tv_sec++;
		}
		if (pthread_cond_timedwait(&sGenerationCond, &sQueueLock, &until)
				== ETIMEDOUT) {
			atomic_add(&sTimedWaitTimeouts, 1);
		}
	}
	pthread_mutex_unlock(&sQueueLock);
	return NULL;
}


static void
test_cond_timedwait()
{
	begin("cond timedwait", 60);
	sGenerationDone = false;
	sTimedWaitTimeouts = 0;
	pthread_t ids[8];
	start_threads(ids, 8, timed_waiter_thread, index_argument);
	for (int i = 0; i < 300; i++) {
		pthread_mutex_lock(&sQueueLock);
		if ((i & 1) != 0)
			pthread_cond_signal(&sGenerationCond);
		else
			pthread_cond_broadcast(&sGenerationCond);
		pthread_mutex_unlock(&sQueueLock);
		snooze(1000 + (i % 7) * 100);
	}
	pthread_mutex_lock(&sQueueLock);
	sGenerationDone = true;
	pthread_cond_broadcast(&sGenerationCond);
	pthread_mutex_unlock(&sQueueLock);
	join_threads(ids, 8);
	pass("8 waiters with 1 ms timeouts: %d time-outs, all woke up",
		(int)sTimedWaitTimeouts);
}


// #pragma mark - rwlock


static pthread_rwlock_t sRWLock = PTHREAD_RWLOCK_INITIALIZER;
static volatile long sA;
static volatile long sB;
static int32 sReaders;


static void*
rwlock_thread(void* data)
{
	int index = (int)(addr_t)data;
	for (long i = 0; i < 50000; i++) {
		if (((i + index) & 7) == 0) {
			pthread_rwlock_wrlock(&sRWLock);
			if (sReaders != 0)
				fail("writer runs with %d readers", (int)sReaders);
			sA++;
			sB = sA;
			pthread_rwlock_unlock(&sRWLock);
		} else {
			pthread_rwlock_rdlock(&sRWLock);
			atomic_add(&sReaders, 1);
			if (sA != sB)
				fail("reader sees a writer's half-done update");
			atomic_add(&sReaders, -1);
			pthread_rwlock_unlock(&sRWLock);
		}
	}
	return NULL;
}


static void
test_rwlock()
{
	begin("rwlock", 120);
	sA = sB = 0;
	pthread_t ids[8];
	start_threads(ids, 8, rwlock_thread, index_argument);
	join_threads(ids, 8);
	if (sA != 8 * 50000 / 8)
		fail("%ld writes, expected %d", sA, 8 * 50000 / 8);
	pass("8 threads, 1/8 writes: %ld writes", sA);
}


// #pragma mark - timed locks


static int32 sTimedOut;
static int32 sAcquired;


static void
absolute_in(struct timespec* until, bigtime_t microseconds)
{
	clock_gettime(CLOCK_REALTIME, until);
	bigtime_t nanoseconds = until->tv_nsec + microseconds * 1000;
	until->tv_sec += nanoseconds / 1000000000;
	until->tv_nsec = nanoseconds % 1000000000;
}


static void*
timedlock_thread(void*)
{
	struct timespec until;
	absolute_in(&until, 20000);
	bigtime_t start = system_time();
	int result = pthread_mutex_timedlock(&sMutex, &until);
	bigtime_t waited = system_time() - start;
	if (result != ETIMEDOUT)
		fail("timedlock of a held mutex returned %d", result);
	if (waited < 15000)
		fail("timedlock timed out after %lld us", (long long)waited);
	atomic_add(&sTimedOut, 1);

	// now block until the holder releases the mutex
	if (pthread_mutex_lock(&sMutex) != 0)
		fail("lock failed");
	atomic_add(&sAcquired, 1);
	pthread_mutex_unlock(&sMutex);
	return NULL;
}


static void*
timedlock_mix_thread(void* data)
{
	int index = (int)(addr_t)data;
	for (long i = 0; i < 20000; i++) {
		if (((i + index) & 3) == 0) {
			struct timespec until;
			absolute_in(&until, 50 + (i % 5) * 100);
			if (pthread_mutex_timedlock(&sMutex, &until) != 0) {
				atomic_add(&sTimedOut, 1);
				continue;
			}
		} else if (pthread_mutex_lock(&sMutex) != 0)
			fail("lock failed");
		sCounter++;
		if ((i & 255) == 0)
			snooze(50);
		pthread_mutex_unlock(&sMutex);
	}
	return NULL;
}


static void
test_timed_locks()
{
	begin("timed locks", 60);
	pthread_mutex_init(&sMutex, NULL);
	sTimedOut = 0;
	sAcquired = 0;
	pthread_mutex_lock(&sMutex);
	pthread_t ids[8];
	start_threads(ids, 4, timedlock_thread, index_argument);
	while (atomic_get(&sTimedOut) < 4)
		snooze(1000);
	snooze(20000);
	if (atomic_get(&sAcquired) != 0)
		fail("a waiter got a held mutex");
	pthread_mutex_unlock(&sMutex);
	join_threads(ids, 4);
	if (sAcquired != 4)
		fail("%d of 4 waiters got the mutex", (int)sAcquired);

	// many timed and untimed lockers; time-outs must not lose wake-ups
	sTimedOut = 0;
	sCounter = 0;
	start_threads(ids, 8, timedlock_mix_thread, index_argument);
	join_threads(ids, 8);
	if (sCounter + sTimedOut != 8 * 20000)
		fail("%ld locked + %d timed out != %d", sCounter, (int)sTimedOut,
			8 * 20000);
	if (pthread_mutex_trylock(&sMutex) != 0)
		fail("mutex not free at the end");
	pthread_mutex_unlock(&sMutex);
	pthread_mutex_destroy(&sMutex);
	pass("4 timed out and then locked; mix: %ld locked, %d timed out",
		sCounter, (int)sTimedOut);
}


// #pragma mark - killed waiters


static int32 sKillAcquired;


static status_t
kill_waiter_thread(void*)
{
	pthread_mutex_lock(&sMutex);
	atomic_add(&sKillAcquired, 1);
	pthread_mutex_unlock(&sMutex);
	return B_OK;
}


static bool
is_waiting(thread_id thread)
{
	thread_info info;
	return get_thread_info(thread, &info) == B_OK
		&& info.state == B_THREAD_WAITING;
}


static void
test_killed_waiters()
{
	begin("killed waiters", 120);
	pthread_mutex_init(&sMutex, NULL);
	int killedTotal = 0;
	for (int round = 0; round < 100; round++) {
		sKillAcquired = 0;
		pthread_mutex_lock(&sMutex);
		thread_id threads[6];
		for (int i = 0; i < 6; i++) {
			threads[i] = spawn_thread(kill_waiter_thread, "waiter",
				B_NORMAL_PRIORITY, NULL);
			resume_thread(threads[i]);
		}
		for (int i = 0; i < 6; i++) {
			while (!is_waiting(threads[i]))
				snooze(100);
		}
		// kill one to four of them while they wait in the kernel
		int killCount = 1 + round % 4;
		for (int i = 0; i < killCount; i++) {
			int index = (round * 5 + i * 3) % 6;
			if (threads[index] < 0)
				continue;
			kill_thread(threads[index]);
			status_t result;
			wait_for_thread(threads[index], &result);
			threads[index] = -1;
			killedTotal++;
		}
		int survivors = 0;
		for (int i = 0; i < 6; i++) {
			if (threads[i] >= 0)
				survivors++;
		}
		pthread_mutex_unlock(&sMutex);
		for (int i = 0; i < 6; i++) {
			if (threads[i] >= 0) {
				status_t result;
				wait_for_thread(threads[i], &result);
			}
		}
		if (sKillAcquired != survivors)
			fail("round %d: %d of %d surviving waiters got the mutex", round,
				(int)sKillAcquired, survivors);
		if (pthread_mutex_trylock(&sMutex) != 0)
			fail("round %d: mutex not free at the end", round);
		pthread_mutex_unlock(&sMutex);
	}
	pthread_mutex_destroy(&sMutex);
	pass("100 rounds, %d waiters killed, the others got the mutex",
		killedTotal);
}


// #pragma mark - starvation


static volatile bool sStopHog;


static void*
hog_thread(void*)
{
	while (!sStopHog) {
		pthread_mutex_lock(&sMutex);
		sCounter++;
		pthread_mutex_unlock(&sMutex);
	}
	return NULL;
}


static void
test_starvation()
{
	// Two threads take the mutex in a loop as fast as they can; a third one
	// has to get it 2000 times too (each time after a short pause, so that it
	// queues up behind running threads every time).
	begin("starvation", 60);
	pthread_mutex_init(&sMutex, NULL);
	sStopHog = false;
	sCounter = 0;
	pthread_t hogs[2];
	start_threads(hogs, 2, hog_thread, index_argument);
	snooze(10000);
	bigtime_t start = system_time();
	bigtime_t worst = 0;
	for (int i = 0; i < 2000; i++) {
		bigtime_t before = system_time();
		pthread_mutex_lock(&sMutex);
		bigtime_t waited = system_time() - before;
		if (waited > worst)
			worst = waited;
		pthread_mutex_unlock(&sMutex);
		snooze(100);
	}
	bigtime_t time = system_time() - start;
	sStopHog = true;
	join_threads(hogs, 2);
	pthread_mutex_destroy(&sMutex);
	pass("2000 locks next to 2 busy threads (%ld locks) in %lld ms, "
		"longest wait %lld us", sCounter, (long long)time / 1000,
		(long long)worst);
}


// #pragma mark - process-shared


struct SharedData {
	pthread_mutex_t	mutex;
	long			counter;
};


static void
shared_loop(SharedData* data, long rounds)
{
	for (long i = 0; i < rounds; i++) {
		pthread_mutex_lock(&data->mutex);
		data->counter++;
		pthread_mutex_unlock(&data->mutex);
	}
}


static void
test_process_shared()
{
	begin("process-shared", 120);
	SharedData* data = (SharedData*)mmap(NULL, B_PAGE_SIZE,
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (data == MAP_FAILED)
		fail("mmap");
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
	pthread_mutex_init(&data->mutex, &attr);
	pthread_mutexattr_destroy(&attr);
	data->counter = 0;

	const long kSharedRounds = 100000;
	pid_t children[3];
	for (int i = 0; i < 3; i++) {
		children[i] = fork();
		if (children[i] == 0) {
			shared_loop(data, kSharedRounds);
			_exit(0);
		}
		if (children[i] < 0)
			fail("fork");
	}
	shared_loop(data, kSharedRounds);
	for (int i = 0; i < 3; i++) {
		int status;
		if (waitpid(children[i], &status, 0) != children[i]
			|| !WIFEXITED(status) || WEXITSTATUS(status) != 0)
			fail("child %d failed", i);
	}
	if (data->counter != 4 * kSharedRounds)
		fail("counter %ld, expected %ld", data->counter, 4 * kSharedRounds);
	pass("4 teams x %ld: counter %ld", kSharedRounds, data->counter);
	munmap(data, B_PAGE_SIZE);
}


int
main()
{
	signal(SIGSEGV, crash_handler);
	signal(SIGBUS, crash_handler);
	signal(SIGILL, crash_handler);
	resume_thread(spawn_thread(watchdog, "watchdog", B_NORMAL_PRIORITY, NULL));

	system_info info;
	get_system_info(&info);
	printf("%d CPUs\n", (int)info.cpu_count);

	test_pthread_counter(PTHREAD_MUTEX_NORMAL, "normal mutex", 1);
	test_pthread_counter(PTHREAD_MUTEX_NORMAL, "normal mutex", 2);
	test_pthread_counter(PTHREAD_MUTEX_NORMAL, "normal mutex", 8);
	test_pthread_counter(PTHREAD_MUTEX_DEFAULT, "default mutex", 4);
	test_pthread_counter(PTHREAD_MUTEX_ERRORCHECK, "errorcheck mutex", 4);
	test_pthread_counter(PTHREAD_MUTEX_RECURSIVE, "recursive mutex", 4);
	test_librootmutex_counter(4);
	test_many_mutexes();
	test_condition_variables(false);
	test_condition_variables(true);
	test_cond_timedwait();
	test_rwlock();
	test_timed_locks();
	test_killed_waiters();
	test_starvation();
	test_process_shared();

	printf("all passed\n");
	return 0;
}
