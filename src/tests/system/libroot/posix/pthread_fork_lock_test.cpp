/*
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */


/*!	pthread locks held across fork(), and the error of the timed rwlock
	functions (haiku#175).

	A mutex locked by the thread that calls fork() belongs to the child's
	thread as well (this is what pthread_atfork(lock, unlock, unlock) relies
	on). The mutex names its owner by thread ID and the child's thread has a
	new one: pthread_mutex_unlock() returned EPERM and the next
	pthread_mutex_lock() never came back. A write-locked rwlock was released
	as if it were read-locked and stayed write-locked.

	pthread_rwlock_timedrdlock()/timedwrlock() returned EBUSY, the error of
	the "try" functions, when the time ran out.

	Every child ends itself with alarm() if it hangs.
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


static int sFailures;


static void
check(bool ok, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	printf("%s: ", ok ? "ok" : "FAILED");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	if (!ok)
		sFailures++;
}


static void
check_result(int result, int expected, const char* what)
{
	check(result == expected, "%s: %s (expected: %s)", what,
		result == 0 ? "0" : strerror(result),
		expected == 0 ? "0" : strerror(expected));
}


/*!	Waits for a child that returns its number of failures. */
static void
wait_for_child(pid_t child, const char* what)
{
	int status = 0;
	while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
	}
	if (WIFSIGNALED(status)) {
		check(false, "%s: the child was killed by signal %d%s", what,
			WTERMSIG(status),
			WTERMSIG(status) == SIGALRM ? " (SIGALRM: it hung)" : "");
	} else
		sFailures += WEXITSTATUS(status);
}


static timespec
deadline(int milliseconds)
{
	timespec time;
	clock_gettime(CLOCK_REALTIME, &time);
	time.tv_nsec += (long)milliseconds * 1000000;
	time.tv_sec += time.tv_nsec / 1000000000;
	time.tv_nsec %= 1000000000;
	return time;
}


static void*
unlock_thread(void* _mutex)
{
	return (void*)(addr_t)pthread_mutex_unlock((pthread_mutex_t*)_mutex);
}


static const char*
type_name(int type)
{
	switch (type) {
		case PTHREAD_MUTEX_DEFAULT:
			return "default";
		case PTHREAD_MUTEX_NORMAL:
			return "normal";
		case PTHREAD_MUTEX_ERRORCHECK:
			return "errorcheck";
		case PTHREAD_MUTEX_RECURSIVE:
			return "recursive";
	}
	return "?";
}


/*!	What the child of a fork() does with a mutex its parent's thread locked
	\a depth times.
*/
static void
use_inherited_mutex(pthread_mutex_t* mutex, int type, int depth)
{
	char what[128];
	const char* name = type_name(type);

	// Another thread of the child does not own it.
	pthread_t thread;
	void* result = NULL;
	if (pthread_create(&thread, NULL, unlock_thread, mutex) == 0
		&& pthread_join(thread, &result) == 0) {
		snprintf(what, sizeof(what), "%s mutex: unlock by another thread of "
			"the child", name);
		check_result((int)(addr_t)result, EPERM, what);
	}

	if (type == PTHREAD_MUTEX_RECURSIVE) {
		snprintf(what, sizeof(what), "%s mutex: child locks it once more",
			name);
		check_result(pthread_mutex_lock(mutex), 0, what);
		depth++;
	} else if (type != PTHREAD_MUTEX_NORMAL) {
		snprintf(what, sizeof(what), "%s mutex: child tries to lock what it "
			"holds", name);
		check_result(pthread_mutex_trylock(mutex), EBUSY, what);
	}

	for (int i = 0; i < depth; i++) {
		snprintf(what, sizeof(what), "%s mutex: child unlocks what was "
			"locked before the fork", name);
		check_result(pthread_mutex_unlock(mutex), 0, what);
	}

	snprintf(what, sizeof(what), "%s mutex: child locks it again", name);
	check_result(pthread_mutex_lock(mutex), 0, what);
	snprintf(what, sizeof(what), "%s mutex: child unlocks it again", name);
	check_result(pthread_mutex_unlock(mutex), 0, what);
}


static void
test_mutex_across_fork(int type)
{
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, type);
	pthread_mutex_t mutex;
	pthread_mutex_init(&mutex, &attr);
	pthread_mutex_t second;
	pthread_mutex_init(&second, &attr);
	pthread_mutexattr_destroy(&attr);

	int depth = type == PTHREAD_MUTEX_RECURSIVE ? 2 : 1;
	for (int i = 0; i < depth; i++)
		pthread_mutex_lock(&mutex);

	pid_t child = fork();
	if (child == 0) {
		alarm(10);
		sFailures = 0;

		// A fork of the fork: one mutex held since the first team, one
		// locked in the second.
		pthread_mutex_lock(&second);
		pid_t grandchild = fork();
		if (grandchild == 0) {
			alarm(5);
			sFailures = 0;
			use_inherited_mutex(&mutex, type, depth);
			use_inherited_mutex(&second, type, 1);
			_exit(sFailures);
		}
		wait_for_child(grandchild, "mutex in the fork of a fork");

		use_inherited_mutex(&mutex, type, depth);
		use_inherited_mutex(&second, type, 1);
		_exit(sFailures);
	}
	wait_for_child(child, type_name(type));

	char what[128];
	snprintf(what, sizeof(what), "%s mutex: parent unlocks its own",
		type_name(type));
	for (int i = 0; i < depth; i++)
		check_result(pthread_mutex_unlock(&mutex), 0, what);

	pthread_mutex_destroy(&mutex);
	pthread_mutex_destroy(&second);
}


/*!	A process-shared mutex in shared memory is one object for both teams:
	what the parent's thread holds is not the child's.
*/
static void
test_shared_mutex_across_fork()
{
	pthread_mutex_t* mutex = (pthread_mutex_t*)mmap(NULL, B_PAGE_SIZE,
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (mutex == MAP_FAILED) {
		check(false, "mmap: %s", strerror(errno));
		return;
	}

	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
	pthread_mutex_init(mutex, &attr);
	pthread_mutexattr_destroy(&attr);

	pthread_mutex_lock(mutex);
	pid_t child = fork();
	if (child == 0) {
		alarm(5);
		sFailures = 0;
		check_result(pthread_mutex_unlock(mutex), EPERM,
			"process-shared mutex: child unlocks the parent's lock");
		check_result(pthread_mutex_trylock(mutex), EBUSY,
			"process-shared mutex: child tries to lock it");
		_exit(sFailures);
	}
	wait_for_child(child, "process-shared mutex");
	check_result(pthread_mutex_unlock(mutex), 0,
		"process-shared mutex: parent unlocks its own");

	pthread_mutex_destroy(mutex);
	munmap(mutex, B_PAGE_SIZE);
}


static void
test_condition_across_fork()
{
	pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t condition = PTHREAD_COND_INITIALIZER;

	pthread_mutex_lock(&mutex);
	pid_t child = fork();
	if (child == 0) {
		alarm(5);
		sFailures = 0;
		timespec time = deadline(50);
		check_result(pthread_cond_timedwait(&condition, &mutex, &time),
			ETIMEDOUT, "condition: child waits with the mutex locked before "
			"the fork");
		check_result(pthread_mutex_unlock(&mutex), 0,
			"condition: child unlocks the mutex after the wait");
		_exit(sFailures);
	}
	wait_for_child(child, "condition");
	pthread_mutex_unlock(&mutex);
}


static void
test_rwlock_across_fork()
{
	pthread_rwlock_t lock;
	pthread_rwlock_init(&lock, NULL);

	pthread_rwlock_wrlock(&lock);
	pid_t child = fork();
	if (child == 0) {
		alarm(5);
		sFailures = 0;
		check_result(pthread_rwlock_unlock(&lock), 0,
			"rwlock: child unlocks the write lock taken before the fork");
		timespec time = deadline(200);
		check_result(pthread_rwlock_timedrdlock(&lock, &time), 0,
			"rwlock: child read-locks it");
		check_result(pthread_rwlock_unlock(&lock), 0, "rwlock: child unlocks");
		time = deadline(200);
		check_result(pthread_rwlock_timedwrlock(&lock, &time), 0,
			"rwlock: child write-locks it");
		check_result(pthread_rwlock_unlock(&lock), 0, "rwlock: child unlocks");
		_exit(sFailures);
	}
	wait_for_child(child, "rwlock");
	check_result(pthread_rwlock_unlock(&lock), 0,
		"rwlock: parent unlocks its own");
	pthread_rwlock_destroy(&lock);
}


static FILE* sStream;
static volatile bool sStopWriter;


static void*
stream_writer(void*)
{
	while (!sStopWriter)
		fprintf(sStream, "%d %s\n", 12345, "some text to format");
	return NULL;
}


/*!	A stream that another thread is writing to while this one forks: its
	lock is held by a thread the child does not have. What the forking thread
	has locked itself stays locked for the child.
*/
static void
test_stream_across_fork()
{
	sStream = fopen("/dev/null", "w");
	if (sStream == NULL) {
		check(false, "fopen: %s", strerror(errno));
		return;
	}
	FILE* own = fopen("/dev/null", "w");

	pthread_t thread;
	pthread_create(&thread, NULL, stream_writer, NULL);

	const int kForks = 100;
	int hung = 0;
	int failed = 0;
	flockfile(own);
	for (int i = 0; i < kForks && hung < 3; i++) {
		pid_t child = fork();
		if (child == 0) {
			alarm(2);
			fprintf(sStream, "child\n");
			// the forking thread's own lock: recursive for it, and its to
			// release
			fprintf(own, "child\n");
			funlockfile(own);
			_exit(ftrylockfile(own) == 0 ? 0 : 1);
		}
		int status = 0;
		while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
		}
		if (WIFSIGNALED(status))
			hung++;
		else if (WEXITSTATUS(status) != 0)
			failed++;
	}
	funlockfile(own);

	sStopWriter = true;
	pthread_join(thread, NULL);
	fclose(sStream);
	fclose(own);

	check(hung == 0, "stream written to by another thread during fork(): %d "
		"children hung at their first use of it", hung);
	check(failed == 0, "stream locked by the forking thread: %d children "
		"could not unlock and lock it", failed);
}


struct timed_rwlock_results {
	pthread_rwlock_t*	lock;
	int					tryRead;
	int					tryWrite;
	int					timedRead;
	int					timedWrite;
	int					expiredRead;
	int					expiredWrite;
};


static void*
timed_rwlock_thread(void* _results)
{
	timed_rwlock_results* results = (timed_rwlock_results*)_results;

	results->tryRead = pthread_rwlock_tryrdlock(results->lock);
	results->tryWrite = pthread_rwlock_trywrlock(results->lock);

	timespec time = deadline(100);
	results->timedRead = pthread_rwlock_timedrdlock(results->lock, &time);
	time = deadline(100);
	results->timedWrite = pthread_rwlock_timedwrlock(results->lock, &time);

	// a deadline that has passed already
	time = deadline(0);
	time.tv_sec -= 10;
	results->expiredRead = pthread_rwlock_timedrdlock(results->lock, &time);
	results->expiredWrite = pthread_rwlock_timedwrlock(results->lock, &time);
	return NULL;
}


static void
test_timed_rwlock(bool shared)
{
	pthread_rwlockattr_t attr;
	pthread_rwlockattr_init(&attr);
	if (shared)
		pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
	pthread_rwlock_t lock;
	pthread_rwlock_init(&lock, &attr);
	pthread_rwlockattr_destroy(&attr);

	pthread_rwlock_wrlock(&lock);

	timed_rwlock_results results;
	memset(&results, 0, sizeof(results));
	results.lock = &lock;
	pthread_t thread;
	pthread_create(&thread, NULL, timed_rwlock_thread, &results);
	pthread_join(thread, NULL);
		// ends by itself: every call in it has a deadline

	pthread_rwlock_unlock(&lock);
	pthread_rwlock_destroy(&lock);

	const char* kind = shared ? "process-shared rwlock" : "rwlock";
	char what[128];
	snprintf(what, sizeof(what), "%s: tryrdlock under a writer", kind);
	check_result(results.tryRead, EBUSY, what);
	snprintf(what, sizeof(what), "%s: trywrlock under a writer", kind);
	check_result(results.tryWrite, EBUSY, what);
	snprintf(what, sizeof(what), "%s: timedrdlock under a writer", kind);
	check_result(results.timedRead, ETIMEDOUT, what);
	snprintf(what, sizeof(what), "%s: timedwrlock under a writer", kind);
	check_result(results.timedWrite, ETIMEDOUT, what);
	snprintf(what, sizeof(what), "%s: timedrdlock, deadline in the past",
		kind);
	check_result(results.expiredRead, ETIMEDOUT, what);
	snprintf(what, sizeof(what), "%s: timedwrlock, deadline in the past",
		kind);
	check_result(results.expiredWrite, ETIMEDOUT, what);
}


int
main()
{
	setvbuf(stdout, NULL, _IONBF, 0);

	test_mutex_across_fork(PTHREAD_MUTEX_DEFAULT);
	test_mutex_across_fork(PTHREAD_MUTEX_NORMAL);
	test_mutex_across_fork(PTHREAD_MUTEX_ERRORCHECK);
	test_mutex_across_fork(PTHREAD_MUTEX_RECURSIVE);
	test_shared_mutex_across_fork();
	test_condition_across_fork();
	test_rwlock_across_fork();
	test_stream_across_fork();
	test_timed_rwlock(false);
	test_timed_rwlock(true);

	printf("%s\n", sFailures == 0 ? "PASSED" : "FAILED");
	return sFailures == 0 ? 0 : 1;
}
