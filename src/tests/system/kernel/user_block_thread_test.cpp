/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	The userland wait behind libroot's rw_lock, pthread_rwlock and
	RWLockManager: _kern_block_thread() and _kern_unblock_thread() with
	user_thread::wait_status.

	A signal that interrupts the wait must leave it armed: the kernel restarts
	the syscall after an ignored signal or one with SA_RESTART, and callers
	retry on B_INTERRUPTED. When the kernel stored B_INTERRUPTED in
	wait_status, the next call returned at once, forever, and the wake-up from
	_kern_unblock_thread() was dropped - a waiter spun at full CPU and never
	woke (a hung Deskbar menu on the X399, 2026-11-01).

	Every case runs in a thread of its own with a deadline, so a regression
	fails the test instead of hanging it. Prints one PASS or FAIL line per
	case; the exit status is 0 when all of them passed.
*/


#include <OS.h>
#include <pthread.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include <syscalls.h>
#include <user_thread.h>


// CPU time a waiting thread may use in kSettleTime: anything near the time
// itself is a spin.
static const bigtime_t kSettleTime = 500000;
static const bigtime_t kMaxWaitCPU = 50000;
static const bigtime_t kDeadline = 3000000;


static int sFailures = 0;


static void
report(bool passed, const char* name, const char* format, ...)
{
	char details[256] = "";
	va_list args;
	va_start(args, format);
	vsnprintf(details, sizeof(details), format, args);
	va_end(args);

	printf("%s: %s%s%s\n", passed ? "PASS" : "FAIL", name,
		details[0] != '\0' ? ": " : "", details);
	fflush(stdout);
	if (!passed)
		sFailures++;
}


static bigtime_t
cpu_time(thread_id thread)
{
	thread_info info;
	if (get_thread_info(thread, &info) != B_OK)
		return -1;
	return info.user_time + info.kernel_time;
}


static void
noop_handler(int)
{
}


static void
set_handler(int signalNumber, void (*handler)(int), int flags)
{
	struct sigaction action = {};
	action.sa_handler = handler;
	action.sa_flags = flags;
	sigaction(signalNumber, &action, NULL);
}


// waits for the thread to end, kills it after the deadline
static bool
join(thread_id thread, status_t* _result)
{
	bigtime_t end = system_time() + kDeadline;
	while (system_time() < end) {
		thread_info info;
		if (get_thread_info(thread, &info) != B_OK)
			break;
		snooze(10000);
	}
	thread_info info;
	if (get_thread_info(thread, &info) == B_OK) {
		kill_thread(thread);
		return false;
	}
	wait_for_thread(thread, _result);
		// B_BAD_THREAD_ID by now: the status was saved in the waiter struct
	return true;
}


// #pragma mark - _kern_block_thread()


struct Waiter {
	thread_id	thread;
	bool		retryOnInterrupt;
	int32		state;
		// 0 starting, 1 armed and about to block, 2 done
	status_t	firstResult;
	int32		statusAfterInterrupt;
	status_t	result;
};


static status_t
block_thread_waiter(void* data)
{
	Waiter* waiter = (Waiter*)data;
	get_user_thread()->wait_status = 1;
	atomic_set(&waiter->state, 1);

	status_t result = _kern_block_thread(0, 0);
	waiter->firstResult = result;
	waiter->statusAfterInterrupt = get_user_thread()->wait_status;
	if (waiter->retryOnInterrupt) {
		while (result == B_INTERRUPTED)
			result = _kern_block_thread(0, 0);
	}
	waiter->result = result;
	atomic_set(&waiter->state, 2);
	return B_OK;
}


/*!	Blocks a thread, sends it \a signalNumber, makes sure it neither spins
	nor returns (unless the signal is expected to interrupt), then unblocks it
	with a status of its own and checks what the wait returned.
*/
static void
test_block_thread_signal(const char* name, int signalNumber,
	void (*handler)(int), int flags, bool interrupts)
{
	set_handler(signalNumber, handler, flags);

	Waiter waiter = {};
	waiter.retryOnInterrupt = true;
	waiter.thread = spawn_thread(block_thread_waiter, name, B_NORMAL_PRIORITY,
		&waiter);
	resume_thread(waiter.thread);
	while (atomic_get(&waiter.state) == 0)
		snooze(1000);
	snooze(100000);

	bigtime_t before = cpu_time(waiter.thread);
	send_signal(waiter.thread, signalNumber);
	snooze(kSettleTime);
	bigtime_t spent = cpu_time(waiter.thread) - before;
	bool returnedEarly = atomic_get(&waiter.state) == 2;

	// a status nothing else returns (an unblock status must be <= 0: a
	// positive wait_status means "still waiting")
	const status_t kWakeStatus = B_NAME_NOT_FOUND;
	_kern_unblock_thread(waiter.thread, kWakeStatus);
	status_t threadResult;
	bool ended = join(waiter.thread, &threadResult);

	set_handler(signalNumber, SIG_DFL, 0);

	if (!ended) {
		report(false, name, "the waiter never woke after the unblock "
			"(%lld us of CPU in %lld us)", (long long)spent,
			(long long)kSettleTime);
		return;
	}
	if (returnedEarly) {
		report(false, name, "the wait returned before the unblock (%s)",
			strerror(waiter.result));
		return;
	}
	if (spent > kMaxWaitCPU) {
		report(false, name, "the waiter spun: %lld us of CPU in %lld us",
			(long long)spent, (long long)kSettleTime);
		return;
	}
	if (interrupts && waiter.firstResult != B_INTERRUPTED) {
		report(false, name, "the caught signal did not interrupt the wait "
			"(%s)", strerror(waiter.firstResult));
		return;
	}
	if (interrupts && waiter.statusAfterInterrupt <= 0) {
		report(false, name, "the interruption disarmed the wait "
			"(wait_status %" B_PRId32 ")", waiter.statusAfterInterrupt);
		return;
	}
	if (waiter.result != kWakeStatus) {
		report(false, name, "the wait returned %s, not the unblock's status",
			strerror(waiter.result));
		return;
	}
	report(true, name, "%lld us of CPU while waiting", (long long)spent);
}


static void
test_block_thread_unblocked_first()
{
	const char* name = "unblock before block";
	get_user_thread()->wait_status = 1;
	_kern_unblock_thread(find_thread(NULL), B_BUSY);
	status_t result = _kern_block_thread(0, 0);
	report(result == B_BUSY, name, "returned %s", strerror(result));
}


static void
test_block_thread_timeout()
{
	const char* name = "timeout";
	get_user_thread()->wait_status = 1;
	bigtime_t start = system_time();
	status_t result = _kern_block_thread(B_RELATIVE_TIMEOUT, 100000);
	bigtime_t waited = system_time() - start;
	report(result == B_TIMED_OUT && waited >= 90000
			&& get_user_thread()->wait_status == B_TIMED_OUT, name,
		"%s after %lld us, wait_status %" B_PRId32, strerror(result),
		(long long)waited, get_user_thread()->wait_status);
}


static void
test_block_thread_interrupt_returns()
{
	// Without SA_RESTART the caller sees B_INTERRUPTED, and the wait stays
	// armed for its next call.
	const char* name = "caught signal: B_INTERRUPTED, still armed";
	set_handler(SIGUSR2, noop_handler, 0);

	Waiter waiter = {};
	waiter.retryOnInterrupt = false;
	waiter.thread = spawn_thread(block_thread_waiter, name, B_NORMAL_PRIORITY,
		&waiter);
	resume_thread(waiter.thread);
	while (atomic_get(&waiter.state) == 0)
		snooze(1000);
	snooze(100000);
	send_signal(waiter.thread, SIGUSR2);

	status_t threadResult;
	bool ended = join(waiter.thread, &threadResult);
	set_handler(SIGUSR2, SIG_DFL, 0);

	report(ended && waiter.firstResult == B_INTERRUPTED
			&& waiter.statusAfterInterrupt > 0, name,
		"%s, wait_status %" B_PRId32 " afterwards",
		ended ? strerror(waiter.firstResult) : "did not return",
		waiter.statusAfterInterrupt);
}


// #pragma mark - pthread_rwlock


static pthread_rwlock_t sRWLock = PTHREAD_RWLOCK_INITIALIZER;
static int32 sReaderState;


static status_t
pthread_reader(void*)
{
	atomic_set(&sReaderState, 1);
	pthread_rwlock_rdlock(&sRWLock);
	atomic_set(&sReaderState, 2);
	pthread_rwlock_unlock(&sRWLock);
	return B_OK;
}


static void
test_pthread_rwlock_signal()
{
	const char* name = "pthread_rwlock read wait + SIGCHLD";
	pthread_rwlock_wrlock(&sRWLock);
	atomic_set(&sReaderState, 0);

	thread_id thread = spawn_thread(pthread_reader, name, B_NORMAL_PRIORITY,
		NULL);
	resume_thread(thread);
	while (atomic_get(&sReaderState) == 0)
		snooze(1000);
	snooze(100000);

	bigtime_t before = cpu_time(thread);
	send_signal(thread, SIGCHLD);
	snooze(kSettleTime);
	bigtime_t spent = cpu_time(thread) - before;

	pthread_rwlock_unlock(&sRWLock);
	status_t result;
	bool ended = join(thread, &result);

	report(ended && atomic_get(&sReaderState) == 2 && spent <= kMaxWaitCPU,
		name, "%lld us of CPU while waiting, %s", (long long)spent,
		ended ? "woke after the unlock" : "never woke");
}


int
main()
{
	test_block_thread_unblocked_first();
	test_block_thread_timeout();
	test_block_thread_signal("ignored signal (SIGCHLD)", SIGCHLD, SIG_DFL, 0,
		false);
	test_block_thread_signal("caught signal, SA_RESTART", SIGUSR1,
		noop_handler, SA_RESTART, false);
	test_block_thread_signal("caught signal, retried by the caller", SIGUSR1,
		noop_handler, 0, true);
	test_block_thread_interrupt_returns();
	test_pthread_rwlock_signal();

	printf("%s: %d failure(s)\n", sFailures == 0 ? "PASS" : "FAIL", sFailures);
	return sFailures == 0 ? 0 : 1;
}
