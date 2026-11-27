/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	select_wakeup_test: how poll(), select() and wait_for_objects() wake up
	(airos-works/haiku#227: their sync object no longer has a semaphore; the
	waiting thread blocks itself, and the sync object lives on its stack).

	- a write, a released semaphore, a port message, a deleted semaphore and
	  an exiting thread wake a waiting call, and only the ready one is
	  reported;
	- a descriptor closed by another thread while it is polled or selected
	  wakes poll() (POLLNVAL), also with 64 descriptors (more than are kept
	  inline), and many times with the close at any point of the call;
	  select() is not woken (it does not select B_EVENT_INVALID) and times
	  out as usual;
	- a wait_for_objects() with a short timeout on a port or a semaphore that
	  another thread deletes, or on a thread that exits, many rounds: the
	  call returns (B_EVENT_INVALID or a timeout) and leaves nothing behind
	  that the deletion could notify later;
	- a signal wakes a blocked poll() with EINTR; relative timeouts of 0 give
	  B_WOULD_BLOCK, passed absolute ones B_TIMED_OUT, a short one waits;
	- a stress run: threads poll pipes with short timeouts while others
	  write, read, close and dup2() over the polled descriptors. Nothing may
	  crash or hang, and every round of the closers must end.

	Every wait has a deadline; a watchdog ends the test after 120 s.
*/


#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
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
		fflush(stdout); \
	} while (false)


static void
signal_handler(int)
{
}


static void
crash_handler(int signal)
{
	printf("FAIL  signal %d\n", signal);
	fflush(stdout);
	_exit(1);
}


static status_t
watchdog(void*)
{
	snooze(120000000);
	printf("FAIL  the test did not finish within 120 s\n");
	fflush(stdout);
	_exit(1);
	return 0;
}


struct delayed_action {
	bigtime_t	delay;
	int			what;
	int			fd;
	int32		object;
	thread_id	target;
};

enum {
	ACTION_WRITE,
	ACTION_CLOSE,
	ACTION_RELEASE_SEM,
	ACTION_DELETE_SEM,
	ACTION_WRITE_PORT,
	ACTION_SIGNAL,
	ACTION_EXIT
};


static status_t
action_thread(void* data)
{
	delayed_action* action = (delayed_action*)data;
	snooze(action->delay);
	switch (action->what) {
		case ACTION_WRITE:
			write(action->fd, "x", 1);
			break;
		case ACTION_CLOSE:
			close(action->fd);
			break;
		case ACTION_RELEASE_SEM:
			release_sem(action->object);
			break;
		case ACTION_DELETE_SEM:
			delete_sem(action->object);
			break;
		case ACTION_WRITE_PORT:
			write_port(action->object, 1, "x", 1);
			break;
		case ACTION_SIGNAL:
			send_signal(action->target, SIGUSR1);
			break;
		case ACTION_EXIT:
			break;
	}
	return B_OK;
}


static thread_id
start_action(delayed_action& action)
{
	thread_id thread = spawn_thread(action_thread, "action",
		B_NORMAL_PRIORITY, &action);
	resume_thread(thread);
	return thread;
}


static void
join(thread_id thread)
{
	status_t status;
	wait_for_thread(thread, &status);
}


static void
test_wakeups()
{
	int pipes[2][2];
	pipe(pipes[0]);
	pipe(pipes[1]);

	// a write to the second of two pipes wakes poll(), infinite timeout
	delayed_action action = { 20000, ACTION_WRITE, pipes[1][1] };
	thread_id thread = start_action(action);
	struct pollfd fds[2] = {
		{ pipes[0][0], POLLIN, 0 },
		{ pipes[1][0], POLLIN, 0 }
	};
	bigtime_t start = system_time();
	int result = poll(fds, 2, 10000);
	bigtime_t waited = system_time() - start;
	CHECK(result == 1 && fds[0].revents == 0 && fds[1].revents == POLLIN
			&& waited >= 15000,
		"poll(2 pipes) woken by a write: %d, revents %#x %#x, %lld us",
		result, fds[0].revents, fds[1].revents, (long long)waited);
	join(thread);
	char buffer[16];
	read(pipes[1][0], buffer, sizeof(buffer));

	// the same for select()
	thread = start_action(action);
	fd_set readSet;
	FD_ZERO(&readSet);
	FD_SET(pipes[0][0], &readSet);
	FD_SET(pipes[1][0], &readSet);
	struct timeval timeout = { 10, 0 };
	int nfds = max_c(pipes[0][0], pipes[1][0]) + 1;
	result = select(nfds, &readSet, NULL, NULL, &timeout);
	CHECK(result == 1 && !FD_ISSET(pipes[0][0], &readSet)
			&& FD_ISSET(pipes[1][0], &readSet),
		"select(2 pipes) woken by a write: %d", result);
	join(thread);
	read(pipes[1][0], buffer, sizeof(buffer));

	// many wakeups in a row, each from another thread
	const int kRounds = 2000;
	int woken = 0;
	start = system_time();
	for (int i = 0; i < kRounds; i++) {
		delayed_action quick = { 0, ACTION_WRITE, pipes[0][1] };
		thread = start_action(quick);
		struct pollfd fd = { pipes[0][0], POLLIN, 0 };
		if (poll(&fd, 1, 10000) == 1 && fd.revents == POLLIN)
			woken++;
		join(thread);
		read(pipes[0][0], buffer, sizeof(buffer));
	}
	CHECK(woken == kRounds, "%d of %d polls woken by another thread's write"
		" (%.1f us per round)", woken, kRounds,
		(system_time() - start) / (double)kRounds);

	// wait_for_objects(): a semaphore, a port and a pipe; the port wakes it
	sem_id sem = create_sem(0, "wakeup test");
	port_id port = create_port(4, "wakeup test");
	object_wait_info infos[3] = {
		{ sem, B_OBJECT_TYPE_SEMAPHORE, B_EVENT_ACQUIRE_SEMAPHORE },
		{ port, B_OBJECT_TYPE_PORT, B_EVENT_READ },
		{ pipes[0][0], B_OBJECT_TYPE_FD, B_EVENT_READ }
	};
	delayed_action portAction = { 20000, ACTION_WRITE_PORT, -1, port };
	thread = start_action(portAction);
	ssize_t count = wait_for_objects_etc(infos, 3, B_RELATIVE_TIMEOUT,
		10000000);
	CHECK(count == 1 && infos[0].events == 0
			&& infos[1].events == B_EVENT_READ && infos[2].events == 0,
		"wait_for_objects(sem, port, pipe) woken by the port: %zd, "
		"events %#x %#x %#x", count, infos[0].events, infos[1].events,
		infos[2].events);
	join(thread);
	int32 code;
	read_port(port, &code, buffer, sizeof(buffer));

	// ... a released semaphore
	infos[0].events = B_EVENT_ACQUIRE_SEMAPHORE;
	infos[1].events = B_EVENT_READ;
	infos[2].events = B_EVENT_READ;
	delayed_action semAction = { 20000, ACTION_RELEASE_SEM, -1, sem };
	thread = start_action(semAction);
	count = wait_for_objects_etc(infos, 3, B_RELATIVE_TIMEOUT, 10000000);
	CHECK(count == 1 && infos[0].events == B_EVENT_ACQUIRE_SEMAPHORE
			&& infos[1].events == 0 && infos[2].events == 0,
		"wait_for_objects(sem, port, pipe) woken by the semaphore: %zd, "
		"events %#x %#x %#x", count, infos[0].events, infos[1].events,
		infos[2].events);
	join(thread);
	acquire_sem(sem);

	// ... a deleted semaphore
	infos[0].events = B_EVENT_ACQUIRE_SEMAPHORE;
	semAction.what = ACTION_DELETE_SEM;
	thread = start_action(semAction);
	count = wait_for_objects_etc(infos, 1, B_RELATIVE_TIMEOUT, 10000000);
	CHECK(count == 1 && infos[0].events == B_EVENT_INVALID,
		"wait_for_objects(sem) woken by delete_sem(): %zd, events %#x",
		count, infos[0].events);
	join(thread);

	// ... an exiting thread
	delayed_action exitAction = { 20000, ACTION_EXIT };
	thread = start_action(exitAction);
	object_wait_info threadInfo = { thread, B_OBJECT_TYPE_THREAD,
		B_EVENT_INVALID };
	count = wait_for_objects_etc(&threadInfo, 1, B_RELATIVE_TIMEOUT,
		10000000);
	CHECK(count == 1 && threadInfo.events == B_EVENT_INVALID,
		"wait_for_objects(thread) woken by its exit: %zd, events %#x",
		count, threadInfo.events);
	join(thread);

	// timeouts of wait_for_objects()
	infos[1].events = B_EVENT_READ;
	count = wait_for_objects_etc(infos + 1, 1, B_RELATIVE_TIMEOUT, 0);
	CHECK(count == B_WOULD_BLOCK, "wait_for_objects(port, relative 0): %s",
		strerror(count));
	infos[1].events = B_EVENT_READ;
	count = wait_for_objects_etc(infos + 1, 1, B_ABSOLUTE_TIMEOUT,
		system_time() - 1000);
	CHECK(count == B_TIMED_OUT, "wait_for_objects(port, passed absolute): %s",
		strerror(count));
	infos[1].events = B_EVENT_READ;
	start = system_time();
	count = wait_for_objects_etc(infos + 1, 1, B_RELATIVE_TIMEOUT, 20000);
	waited = system_time() - start;
	CHECK(count == B_TIMED_OUT && waited >= 19000,
		"wait_for_objects(port, relative 20 ms): %s after %lld us",
		strerror(count), (long long)waited);
	infos[1].events = B_EVENT_READ;
	write_port(port, 1, "x", 1);
	count = wait_for_objects_etc(infos + 1, 1, B_RELATIVE_TIMEOUT, 0);
	CHECK(count == 1 && infos[1].events == B_EVENT_READ,
		"wait_for_objects(port with a message, relative 0): %zd", count);
	read_port(port, &code, buffer, sizeof(buffer));
	delete_port(port);

	// a signal wakes a blocked poll()
	signal(SIGUSR1, signal_handler);
	delayed_action signalAction = { 20000, ACTION_SIGNAL };
	signalAction.target = find_thread(NULL);
	thread = start_action(signalAction);
	struct pollfd fd = { pipes[0][0], POLLIN, 0 };
	errno = 0;
	result = poll(&fd, 1, 10000);
	int error = errno;
	CHECK(result == -1 && error == EINTR,
		"poll(empty pipe) and a signal: %d, %s", result, strerror(error));
	join(thread);

	for (int i = 0; i < 2; i++) {
		close(pipes[i][0]);
		close(pipes[i][1]);
	}
}


static void
test_close_while_polled()
{
	// One of 64 polled descriptors (more than fit inline) is closed by
	// another thread.
	const int kCount = 64;
	int pipes[kCount][2];
	struct pollfd fds[kCount];
	for (int i = 0; i < kCount; i++) {
		pipe(pipes[i]);
		fds[i].fd = pipes[i][0];
		fds[i].events = POLLIN;
	}

	delayed_action action = { 20000, ACTION_CLOSE, pipes[kCount / 2][0] };
	thread_id thread = start_action(action);
	int result = poll(fds, kCount, 10000);
	int others = 0;
	for (int i = 0; i < kCount; i++) {
		if (i != kCount / 2 && fds[i].revents != 0)
			others++;
	}
	CHECK(result == 1 && fds[kCount / 2].revents == POLLNVAL && others == 0,
		"poll(64 pipes), one closed meanwhile: %d, revents %#x, %d others",
		result, fds[kCount / 2].revents, others);
	join(thread);

	// select() does not select B_EVENT_INVALID: a descriptor closed meanwhile
	// does not wake it (as on Linux). It must still time out normally and
	// report nothing else.
	int target = pipes[kCount / 2 + 1][0];
	fd_set readSet;
	FD_ZERO(&readSet);
	int nfds = 0;
	for (int i = 0; i < kCount; i++) {
		if (i == kCount / 2)
			continue;
		FD_SET(pipes[i][0], &readSet);
		nfds = max_c(nfds, pipes[i][0] + 1);
	}
	action.fd = target;
	thread = start_action(action);
	struct timeval timeout = { 0, 300000 };
	bigtime_t start = system_time();
	result = select(nfds, &readSet, NULL, NULL, &timeout);
	bigtime_t waited = system_time() - start;
	others = 0;
	for (int i = 0; i < nfds; i++) {
		if (i != target && FD_ISSET(i, &readSet))
			others++;
	}
	CHECK((result == 0 || (result == 1 && FD_ISSET(target, &readSet)))
			&& others == 0 && waited < 5000000,
		"select(63 pipes, 300 ms), one closed meanwhile: %d after %lld us",
		result, (long long)waited);
	join(thread);

	for (int i = 0; i < kCount; i++) {
		if (i != kCount / 2 && i != kCount / 2 + 1)
			close(pipes[i][0]);
		close(pipes[i][1]);
	}

	// Many short rounds: the close comes at any point of the poll(), so that
	// it also races with the poll()'s own deselect.
	const int kRounds = 3000;
	int returned = 0;
	for (int i = 0; i < kRounds; i++) {
		int round[2];
		pipe(round);
		delayed_action closeAction = { (bigtime_t)(i % 7) * 10, ACTION_CLOSE,
			round[0] };
		thread = start_action(closeAction);
		struct pollfd fd = { round[0], POLLIN, 0 };
		result = poll(&fd, 1, 5000);
		if (result == 1 && fd.revents == POLLNVAL)
			returned++;
		join(thread);
		close(round[1]);
	}
	CHECK(returned == kRounds, "%d of %d polls returned POLLNVAL when their "
		"descriptor was closed", returned, kRounds);
}


// #pragma mark - objects deleted while waited for


static sem_id sDeleterGo;
static sem_id sDeleterDone;
static volatile int32 sDeleterObject;
static volatile int sDeleterWhat;
static volatile bool sDeleterQuit = false;


static void
busy_wait(bigtime_t duration)
{
	bigtime_t end = system_time() + duration;
	while (system_time() < end)
		;
}


static status_t
deleter_thread(void*)
{
	int round = 0;
	while (acquire_sem(sDeleterGo) == B_OK && !sDeleterQuit) {
		busy_wait((round++ * 7) % 60);
		if (sDeleterWhat == 0)
			delete_port(sDeleterObject);
		else
			delete_sem(sDeleterObject);
		release_sem(sDeleterDone);
	}
	return B_OK;
}


static status_t
exit_at_once(void*)
{
	return B_OK;
}


/*!	A wait_for_objects() that times out while its port or semaphore is being
	deleted, or while its thread exits, must not leave its select_info with
	the object: the info lives on the waiter's stack (#227). Ports used to
	notify their select infos only after the port had been marked deleted,
	when deselect_port() could no longer find it, and a thread that had
	already handed its infos on could still be selected.
*/
static void
test_deleted_while_waiting()
{
	sDeleterGo = create_sem(0, "deleter go");
	sDeleterDone = create_sem(0, "deleter done");
	thread_id deleter = spawn_thread(deleter_thread, "deleter",
		B_NORMAL_PRIORITY, NULL);
	resume_thread(deleter);

	const int kRounds = 5000;
	for (int what = 0; what < 2; what++) {
		int invalid = 0;
		int timedOut = 0;
		int other = 0;
		for (int i = 0; i < kRounds; i++) {
			object_wait_info info;
			if (what == 0) {
				info.object = create_port(1, "deleted port");
				info.type = B_OBJECT_TYPE_PORT;
				info.events = B_EVENT_READ;
			} else {
				info.object = create_sem(0, "deleted sem");
				info.type = B_OBJECT_TYPE_SEMAPHORE;
				info.events = B_EVENT_ACQUIRE_SEMAPHORE;
			}
			sDeleterObject = info.object;
			sDeleterWhat = what;
			release_sem(sDeleterGo);

			ssize_t result = wait_for_objects_etc(&info, 1,
				B_RELATIVE_TIMEOUT, (i * 13) % 50);
			if (result == 1 && info.events == B_EVENT_INVALID)
				invalid++;
			else if (result == B_TIMED_OUT || result == B_WOULD_BLOCK)
				timedOut++;
			else
				other++;

			// the stack where the info was is used again at once
			char scribble[1024];
			memset(scribble, 0x5a, sizeof(scribble));
			__asm__ __volatile__("" : : "r"(scribble) : "memory");

			acquire_sem_etc(sDeleterDone, 1, B_RELATIVE_TIMEOUT, 10000000);
		}
		CHECK(other == 0, "%d wait_for_objects() on a %s deleted meanwhile: "
			"%d invalid, %d timed out, %d other", kRounds,
			what == 0 ? "port" : "semaphore", invalid, timedOut, other);
	}

	sDeleterQuit = true;
	release_sem(sDeleterGo);
	join(deleter);
	delete_sem(sDeleterGo);
	delete_sem(sDeleterDone);

	// a thread that exits while it is waited for with a short timeout
	int invalid = 0;
	int other = 0;
	for (int i = 0; i < kRounds; i++) {
		thread_id thread = spawn_thread(exit_at_once, "exit at once",
			B_NORMAL_PRIORITY, NULL);
		resume_thread(thread);
		busy_wait((i * 7) % 30);
		object_wait_info info = { thread, B_OBJECT_TYPE_THREAD,
			B_EVENT_INVALID };
		ssize_t result = wait_for_objects_etc(&info, 1, B_RELATIVE_TIMEOUT,
			(i * 13) % 50);
		if (result == 1 && info.events == B_EVENT_INVALID)
			invalid++;
		else if (result != B_TIMED_OUT && result != B_WOULD_BLOCK)
			other++;
		char scribble[1024];
		memset(scribble, 0x5a, sizeof(scribble));
		__asm__ __volatile__("" : : "r"(scribble) : "memory");
		join(thread);
	}
	CHECK(other == 0, "%d wait_for_objects() on a thread exiting meanwhile: "
		"%d invalid, %d other", kRounds, invalid, other);

	// The same with the main thread of a child team: nobody of ours keeps
	// that thread, it is gone soon after it has exited.
	const int kChildRounds = 500;
	invalid = 0;
	other = 0;
	for (int i = 0; i < kChildRounds; i++) {
		pid_t child = fork();
		if (child == 0)
			_exit(0);
		if (child < 0) {
			other++;
			continue;
		}
		// the child needs a while to exit: spread the waits over it
		busy_wait((i * 37) % 2000);
		object_wait_info info = { child, B_OBJECT_TYPE_THREAD,
			B_EVENT_INVALID };
		ssize_t result = wait_for_objects_etc(&info, 1, B_RELATIVE_TIMEOUT,
			(i * 13) % 50);
		if (result == 1 && info.events == B_EVENT_INVALID)
			invalid++;
		else if (result != B_TIMED_OUT && result != B_WOULD_BLOCK)
			other++;
		char scribble[1024];
		memset(scribble, 0x5a, sizeof(scribble));
		__asm__ __volatile__("" : : "r"(scribble) : "memory");
		int status;
		waitpid(child, &status, 0);
	}
	CHECK(other == 0, "%d wait_for_objects() on a child's main thread "
		"exiting meanwhile: %d invalid, %d other", kChildRounds, invalid,
		other);
}


// #pragma mark - stress


static const int kStressPipes = 8;
static int sStressPipes[kStressPipes][2];
static volatile bool sStop = false;
static int32 sPolls = 0;
static int32 sPollErrors = 0;
static int32 sCloses = 0;
static int32 sCloserErrors = 0;
static pthread_mutex_t sCloserLock = PTHREAD_MUTEX_INITIALIZER;


static status_t
stress_poller(void* data)
{
	int index = (int)(addr_t)data;
	while (!sStop) {
		struct pollfd fds[kStressPipes + 12];
		int count = (index % 2) == 0 ? kStressPipes : kStressPipes + 12;
		for (int i = 0; i < count; i++) {
			fds[i].fd = sStressPipes[(i + index) % kStressPipes][0];
			fds[i].events = POLLIN;
		}
		int result = poll(fds, count, (index % 3) == 0 ? 0 : 2);
		if (result < 0 && errno != EINTR)
			atomic_add(&sPollErrors, 1);
		atomic_add(&sPolls, 1);

		if ((index % 4) == 3) {
			fd_set readSet;
			FD_ZERO(&readSet);
			int nfds = 0;
			for (int i = 0; i < kStressPipes; i++) {
				int fd = sStressPipes[i][0];
				if (fd >= 0 && fd < FD_SETSIZE) {
					FD_SET(fd, &readSet);
					nfds = max_c(nfds, fd + 1);
				}
			}
			struct timeval timeout = { 0, 1000 };
			select(nfds, &readSet, NULL, NULL, &timeout);
				// EBADF is fine here, the closers are fast
		}
	}
	return B_OK;
}


static status_t
stress_writer(void*)
{
	char buffer[64];
	int i = 0;
	while (!sStop) {
		int index = i++ % kStressPipes;
		write(sStressPipes[index][1], "x", 1);
		if ((i % 3) == 0)
			read(sStressPipes[index][0], buffer, sizeof(buffer));
		snooze(50);
	}
	return B_OK;
}


static bool
nonblocking_pipe(int fds[2])
{
	// O_NONBLOCK belongs to the open file, dup2() copies share it: no read or
	// write of the stress threads can block
	if (pipe(fds) != 0)
		return false;
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	fcntl(fds[1], F_SETFL, O_NONBLOCK);
	return true;
}


static status_t
stress_closer(void* data)
{
	// each closer has half of the pipes
	int i = (int)(addr_t)data;
	while (!sStop) {
		int index = i % kStressPipes;
		i += 2;
		// The closers take turns: one's close() and dup2() to the same
		// number must not race with the other's pipe().
		pthread_mutex_lock(&sCloserLock);
		int fresh[2];
		if (!nonblocking_pipe(fresh)) {
			pthread_mutex_unlock(&sCloserLock);
			snooze(100);
			continue;
		}
		if ((i % 2) == 0) {
			// dup2() over the polled descriptor: the old one is closed
			dup2(fresh[0], sStressPipes[index][0]);
			close(fresh[0]);
			close(fresh[1]);
		} else {
			// close() the read end and put a new one in its place
			int old = sStressPipes[index][0];
			close(old);
			if (dup2(fresh[0], old) != old)
				atomic_add(&sCloserErrors, 1);
			close(fresh[0]);
			close(sStressPipes[index][1]);
			sStressPipes[index][1] = fresh[1];
		}
		pthread_mutex_unlock(&sCloserLock);
		atomic_add(&sCloses, 1);
		snooze(200);
	}
	return B_OK;
}


static void
test_stress()
{
	for (int i = 0; i < kStressPipes; i++)
		nonblocking_pipe(sStressPipes[i]);

	const int kPollers = 8;
	thread_id threads[kPollers + 3];
	int threadCount = 0;
	for (int i = 0; i < kPollers; i++) {
		threads[threadCount++] = spawn_thread(stress_poller, "poller",
			B_NORMAL_PRIORITY, (void*)(addr_t)i);
	}
	threads[threadCount++] = spawn_thread(stress_writer, "writer",
		B_NORMAL_PRIORITY, NULL);
	threads[threadCount++] = spawn_thread(stress_closer, "closer",
		B_NORMAL_PRIORITY, (void*)(addr_t)0);
	threads[threadCount++] = spawn_thread(stress_closer, "closer",
		B_NORMAL_PRIORITY, (void*)(addr_t)1);
	for (int i = 0; i < threadCount; i++)
		resume_thread(threads[i]);

	snooze(5000000);
	sStop = true;

	// the pollers use short timeouts: they must all end soon
	bigtime_t start = system_time();
	for (int i = 0; i < threadCount; i++)
		join(threads[i]);
	bigtime_t joined = system_time() - start;

	CHECK(sPollErrors == 0 && sCloserErrors == 0 && joined < 2000000,
		"stress: %" B_PRId32 " polls, %" B_PRId32 " closes/dup2s, %" B_PRId32
		" + %" B_PRId32 " unexpected errors, threads ended in %lld us",
		sPolls, sCloses, sPollErrors, sCloserErrors, (long long)joined);

	for (int i = 0; i < kStressPipes; i++) {
		close(sStressPipes[i][0]);
		close(sStressPipes[i][1]);
	}
}


int
main()
{
	signal(SIGSEGV, crash_handler);
	signal(SIGBUS, crash_handler);
	signal(SIGILL, crash_handler);
	signal(SIGPIPE, SIG_IGN);
		// the stress writers write to pipes the closers have just closed
	resume_thread(spawn_thread(watchdog, "watchdog", B_NORMAL_PRIORITY,
		NULL));

	test_wakeups();
	test_close_while_polled();
	test_deleted_while_waiting();
	test_stress();

	if (sFailures != 0) {
		printf("%d check(s) FAILED\n", sFailures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
