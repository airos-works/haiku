/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	poll_timeout_test: what poll(), select() and acquire_sem_etc() return
	for a timeout that is zero or has passed already (airos-works/haiku#227:
	the kernel used to block on a timer that fired at once; it now returns at
	once, and nothing else may change).

	- nothing ready, zero timeout: 0, nothing reported; something ready: it
	  is reported; a closed descriptor: POLLNVAL / EBADF; a negative one:
	  ignored;
	- a signal pending and unblocked by the call's own mask (ppoll(),
	  pselect()): EINTR, even with a zero timeout, as before (and as on
	  Linux);
	- a short timeout still waits that long;
	- select() with a high nfds and one descriptor set, at the top or at
	  the bottom;
	- acquire_sem_etc() with an absolute timeout in the past: B_TIMED_OUT,
	  B_OK when the semaphore is free; B_INTERRUPTED for a signal that comes
	  while it waits for a timeout in the future.

	Prints the time of a zero-timeout poll() of an empty pipe as well (not
	checked: it depends on the machine).
*/


#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <unistd.h>

#include <OS.h>


static int sFailures = 0;


#define CHECK(condition, ...) \
	do { \
		if (condition) { \
			printf("ok    "); \
		} else { \
			printf("FAIL  "); \
			sFailures++; \
		} \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} while (false)


static void
signal_handler(int signal)
{
}


static status_t
watchdog(void*)
{
	snooze(60000000);
	printf("FAIL  the test did not finish within 60 s\n");
	_exit(1);
	return 0;
}


static void
test_poll(int readEnd, int writeEnd)
{
	struct pollfd fds[3];
	fds[0].fd = readEnd;
	fds[0].events = POLLIN;
	fds[0].revents = 0x7fff;
	int result = poll(fds, 1, 0);
	CHECK(result == 0 && fds[0].revents == 0,
		"poll(empty pipe, 0) = %d, revents %#x", result, fds[0].revents);

	fds[0].fd = writeEnd;
	fds[0].events = POLLOUT;
	result = poll(fds, 1, 0);
	CHECK(result == 1 && fds[0].revents == POLLOUT,
		"poll(write end, 0) = %d, revents %#x", result, fds[0].revents);

	write(writeEnd, "x", 1);
	fds[0].fd = readEnd;
	fds[0].events = POLLIN;
	fds[1].fd = -1;
	fds[1].events = POLLIN;
	fds[1].revents = 0x7fff;
	int closed = dup(readEnd);
	close(closed);
	fds[2].fd = closed;
	fds[2].events = POLLIN;
	result = poll(fds, 3, 0);
	CHECK(result == 2 && fds[0].revents == POLLIN && fds[1].revents == 0
			&& fds[2].revents == POLLNVAL,
		"poll(readable, -1, closed; 0) = %d, revents %#x %#x %#x", result,
		fds[0].revents, fds[1].revents, fds[2].revents);
	char buffer;
	read(readEnd, &buffer, 1);

	fds[0].fd = readEnd;
	fds[0].events = POLLIN;
	bigtime_t start = system_time();
	result = poll(fds, 1, 20);
	bigtime_t waited = system_time() - start;
	CHECK(result == 0 && waited >= 19000,
		"poll(empty pipe, 20 ms) = %d after %lld us", result,
		(long long)waited);

	// a pending signal the call's mask unblocks
	sigset_t blocked, empty;
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGUSR1);
	sigemptyset(&empty);
	sigprocmask(SIG_BLOCK, &blocked, NULL);
	raise(SIGUSR1);
	struct timespec zero = { 0, 0 };
	errno = 0;
	result = ppoll(fds, 1, &zero, &empty);
	int error = errno;
	sigprocmask(SIG_UNBLOCK, &blocked, NULL);
	CHECK(result == -1 && error == EINTR,
		"ppoll(empty pipe, 0, signal pending) = %d, %s", result,
		strerror(error));

	// what it costs
	const int kRuns = 100000;
	start = system_time();
	for (int i = 0; i < kRuns; i++)
		poll(fds, 1, 0);
	printf("info  poll(empty pipe, 0): %.2f us per call\n",
		(double)(system_time() - start) / kRuns);
}


static void
test_select(int readEnd, int writeEnd)
{
	fd_set set;
	FD_ZERO(&set);
	FD_SET(readEnd, &set);
	struct timeval zero = { 0, 0 };
	int result = select(readEnd + 1, &set, NULL, NULL, &zero);
	CHECK(result == 0 && !FD_ISSET(readEnd, &set),
		"select(empty pipe, 0) = %d, set %d", result,
		FD_ISSET(readEnd, &set) ? 1 : 0);

	write(writeEnd, "x", 1);
	int high = dup2(readEnd, 500);
	if (high != 500) {
		CHECK(false, "dup2(fd, 500): %s", strerror(errno));
		return;
	}
	FD_ZERO(&set);
	FD_SET(high, &set);
	zero.tv_sec = 0;
	zero.tv_usec = 0;
	result = select(high + 1, &set, NULL, NULL, &zero);
	CHECK(result == 1 && FD_ISSET(high, &set),
		"select(readable fd %d, nfds %d, 0) = %d", high, high + 1, result);

	FD_ZERO(&set);
	FD_SET(readEnd, &set);
	fd_set writeSet;
	FD_ZERO(&writeSet);
	FD_SET(writeEnd, &writeSet);
	zero.tv_sec = 0;
	zero.tv_usec = 0;
	result = select(FD_SETSIZE, &set, &writeSet, NULL, &zero);
	CHECK(result == 2 && FD_ISSET(readEnd, &set)
			&& FD_ISSET(writeEnd, &writeSet),
		"select(readable + writable, nfds FD_SETSIZE, 0) = %d", result);
	char buffer;
	read(readEnd, &buffer, 1);
	close(high);

	int closed = dup(readEnd);
	close(closed);
	FD_ZERO(&set);
	FD_SET(closed, &set);
	zero.tv_sec = 0;
	zero.tv_usec = 0;
	errno = 0;
	result = select(closed + 1, &set, NULL, NULL, &zero);
	CHECK(result == -1 && errno == EBADF, "select(closed fd, 0) = %d, %s",
		result, strerror(errno));

	FD_ZERO(&set);
	FD_SET(readEnd, &set);
	struct timeval timeout = { 0, 20000 };
	bigtime_t start = system_time();
	result = select(readEnd + 1, &set, NULL, NULL, &timeout);
	bigtime_t waited = system_time() - start;
	CHECK(result == 0 && waited >= 19000,
		"select(empty pipe, 20 ms) = %d after %lld us", result,
		(long long)waited);

	sigset_t blocked, empty;
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGUSR1);
	sigemptyset(&empty);
	sigprocmask(SIG_BLOCK, &blocked, NULL);
	raise(SIGUSR1);
	FD_ZERO(&set);
	FD_SET(readEnd, &set);
	struct timespec zeroSpec = { 0, 0 };
	errno = 0;
	result = pselect(readEnd + 1, &set, NULL, NULL, &zeroSpec, &empty);
	int error = errno;
	sigprocmask(SIG_UNBLOCK, &blocked, NULL);
	CHECK(result == -1 && error == EINTR,
		"pselect(empty pipe, 0, signal pending) = %d, %s", result,
		strerror(error));
}


static void
test_semaphore()
{
	sem_id sem = create_sem(0, "poll_timeout_test");

	status_t status = acquire_sem_etc(sem, 1, B_ABSOLUTE_TIMEOUT,
		system_time() - 1000);
	CHECK(status == B_TIMED_OUT, "acquire_sem_etc(absolute, past): %s",
		strerror(status));

	status = acquire_sem_etc(sem, 1, B_ABSOLUTE_TIMEOUT, system_time());
	CHECK(status == B_TIMED_OUT, "acquire_sem_etc(absolute, now): %s",
		strerror(status));

	bigtime_t start = system_time();
	status = acquire_sem_etc(sem, 1, B_ABSOLUTE_TIMEOUT, start + 20000);
	bigtime_t waited = system_time() - start;
	CHECK(status == B_TIMED_OUT && waited >= 19000,
		"acquire_sem_etc(absolute, in 20 ms): %s after %lld us",
		strerror(status), (long long)waited);

	status = acquire_sem_etc(sem, 1,
		B_ABSOLUTE_TIMEOUT | B_TIMEOUT_REAL_TIME_BASE,
		real_time_clock_usecs() - 1000);
	CHECK(status == B_TIMED_OUT,
		"acquire_sem_etc(absolute real time, past): %s", strerror(status));

	release_sem(sem);
	status = acquire_sem_etc(sem, 1, B_ABSOLUTE_TIMEOUT, 0);
	CHECK(status == B_OK, "acquire_sem_etc(free, absolute, past): %s",
		strerror(status));

	delete_sem(sem);
}


static sem_id sInterruptSem;


static status_t
interrupted_waiter(void*)
{
	return acquire_sem_etc(sInterruptSem, 1,
		B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT, system_time() + 2000000);
}


static void
test_semaphore_interrupt()
{
	sInterruptSem = create_sem(0, "poll_timeout_test interrupt");
	thread_id thread = spawn_thread(interrupted_waiter, "waiter",
		B_NORMAL_PRIORITY, NULL);
	resume_thread(thread);
	snooze(100000);
	send_signal(thread, SIGUSR1);
	status_t result = B_ERROR;
	wait_for_thread(thread, &result);
	CHECK(result == B_INTERRUPTED,
		"acquire_sem_etc(absolute, B_CAN_INTERRUPT) and a signal: %s",
		strerror(result));
	delete_sem(sInterruptSem);
}


int
main()
{
	setvbuf(stdout, NULL, _IONBF, 0);

	resume_thread(spawn_thread(watchdog, "watchdog", B_NORMAL_PRIORITY,
		NULL));

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = signal_handler;
	sigaction(SIGUSR1, &action, NULL);

	// room for descriptor 500 and for select(FD_SETSIZE, ...)
	struct rlimit limit;
	getrlimit(RLIMIT_NOFILE, &limit);
	if (limit.rlim_cur < FD_SETSIZE) {
		limit.rlim_cur = FD_SETSIZE;
		setrlimit(RLIMIT_NOFILE, &limit);
	}

	int fds[2];
	if (pipe(fds) != 0) {
		printf("FAIL  pipe(): %s\n", strerror(errno));
		return 1;
	}

	test_poll(fds[0], fds[1]);
	test_select(fds[0], fds[1]);
	test_semaphore();
	test_semaphore_interrupt();

	printf("%s: %d failure(s)\n", sFailures == 0 ? "PASSED" : "FAILED",
		sFailures);
	return sFailures == 0 ? 0 : 1;
}
