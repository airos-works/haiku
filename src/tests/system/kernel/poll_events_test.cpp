/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	poll_events_test: the events poll() and select() report for pipes,
	FIFOs, sockets, files and devices, ready at once and when woken
	(airos-works/haiku#227: pipes and sockets now take all of a descriptor's
	events in one select hook call instead of one call per event; nothing
	that poll() or select() reports may change).

	- pipe ends: empty, with data, full, the other end closed, an event the
	  end cannot have; a FIFO opened for reading and writing; the same
	  descriptor in many entries of one poll();
	- UNIX stream sockets: empty, with data, the peer closed or shut down;
	  UDP and TCP listening sockets;
	- a regular file and /dev/null;
	- the same through select() for a few of them;
	- a poll() that blocks and is woken by a write, a read, a close of the
	  other end, a datagram and a connection;
	- many zero-timeout poll()s of many descriptors, then a write to all of
	  them: every select entry must be gone by then.

	Every wait has a deadline, and a watchdog ends the test after 60 s.
*/


#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
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


static status_t
watchdog(void*)
{
	snooze(60000000);
	printf("FAIL  the test did not finish within 60 s\n");
	_exit(1);
	return 0;
}


static const char*
events_string(short events)
{
	static char buffer[8][128];
	static int next = 0;
	char* string = buffer[next++ % 8];
	string[0] = '\0';
	static const struct {
		short	flag;
		const char*	name;
	} kFlags[] = {
		{ POLLIN, "IN" }, { POLLOUT, "OUT" }, { POLLRDBAND, "RDBAND" },
		{ POLLWRBAND, "WRBAND" }, { POLLPRI, "PRI" }, { POLLERR, "ERR" },
		{ POLLHUP, "HUP" }, { POLLNVAL, "NVAL" }
	};
	for (size_t i = 0; i < sizeof(kFlags) / sizeof(kFlags[0]); i++) {
		if ((events & kFlags[i].flag) != 0) {
			if (string[0] != '\0')
				strcat(string, "|");
			strcat(string, kFlags[i].name);
		}
	}
	if (string[0] == '\0')
		strcpy(string, "0");
	return string;
}


static void
check_poll(const char* name, int fd, short events, int timeout, short expected)
{
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = events;
	pfd.revents = 0;
	int result = poll(&pfd, 1, timeout);
	CHECK(result == (expected != 0 ? 1 : 0) && pfd.revents == expected,
		"%-44s poll(%s): %d, %s (expected %s)", name, events_string(events),
		result, events_string(pfd.revents), events_string(expected));
}


static void
check_select(const char* name, int fd, bool read, bool write, bool error,
	bool expectRead, bool expectWrite, bool expectError)
{
	fd_set readSet, writeSet, errorSet;
	FD_ZERO(&readSet);
	FD_ZERO(&writeSet);
	FD_ZERO(&errorSet);
	if (read)
		FD_SET(fd, &readSet);
	if (write)
		FD_SET(fd, &writeSet);
	if (error)
		FD_SET(fd, &errorSet);
	// Only the sets asked for are passed: select() sets a descriptor in any
	// set it is given when that set's events happen, asked for or not.
	struct timeval tv = { 0, 0 };
	int result = select(fd + 1, read ? &readSet : NULL,
		write ? &writeSet : NULL, error ? &errorSet : NULL, &tv);
	bool gotRead = FD_ISSET(fd, &readSet);
	bool gotWrite = FD_ISSET(fd, &writeSet);
	bool gotError = FD_ISSET(fd, &errorSet);
	int expected = expectRead + expectWrite + expectError;
	CHECK(result == expected && gotRead == expectRead
			&& gotWrite == expectWrite && gotError == expectError,
		"%-44s select(%s%s%s): %d, %s%s%s (expected %d, %s%s%s)", name,
		read ? "r" : "", write ? "w" : "", error ? "e" : "", result,
		gotRead ? "r" : "", gotWrite ? "w" : "", gotError ? "e" : "",
		expected, expectRead ? "r" : "", expectWrite ? "w" : "",
		expectError ? "e" : "");
}


static void
make_pipe(int fds[2])
{
	if (pipe(fds) != 0) {
		printf("FAIL  pipe(): %s\n", strerror(errno));
		exit(1);
	}
}


static void
make_socketpair(int fds[2])
{
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		printf("FAIL  socketpair(): %s\n", strerror(errno));
		exit(1);
	}
}


static int
make_udp_socket(struct sockaddr_in& address)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof(address);
	if (fd < 0 || bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0
		|| getsockname(fd, (struct sockaddr*)&address, &length) != 0) {
		printf("FAIL  UDP socket: %s\n", strerror(errno));
		exit(1);
	}
	return fd;
}


static int
make_tcp_listener(struct sockaddr_in& address)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t length = sizeof(address);
	if (fd < 0 || bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0
		|| listen(fd, 4) != 0
		|| getsockname(fd, (struct sockaddr*)&address, &length) != 0) {
		printf("FAIL  TCP listener: %s\n", strerror(errno));
		exit(1);
	}
	return fd;
}


static void
fill_nonblocking(int fd)
{
	int flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	char buffer[4096];
	memset(buffer, 'x', sizeof(buffer));
	for (int i = 0; i < 100000; i++) {
		if (write(fd, buffer, sizeof(buffer)) < 0)
			break;
	}
	// a pipe may take a few bytes more
	for (int i = 0; i < 100000; i++) {
		if (write(fd, buffer, 1) < 0)
			break;
	}
	fcntl(fd, F_SETFL, flags);
}


static void
test_pipes()
{
	int fds[2];
	make_pipe(fds);
	check_poll("pipe read end, empty", fds[0], POLLIN, 0, 0);
	check_poll("pipe read end, POLLOUT asked", fds[0], POLLOUT, 0, 0);
	check_poll("pipe write end, empty pipe", fds[1], POLLOUT, 0, POLLOUT);
	check_poll("pipe write end, POLLIN asked", fds[1], POLLIN, 0, 0);
	write(fds[1], "a", 1);
	check_poll("pipe read end, with data", fds[0], POLLIN, 0, POLLIN);
	check_poll("pipe read end, with data", fds[0], POLLIN | POLLOUT, 0,
		POLLIN);
	check_select("pipe read end, with data", fds[0], true, true, true,
		true, false, false);
	check_select("pipe write end", fds[1], true, true, true,
		false, true, false);

	// the same descriptor in many entries
	struct pollfd many[64];
	for (int i = 0; i < 64; i++) {
		many[i].fd = i % 2 == 0 ? fds[0] : fds[1];
		many[i].events = i % 2 == 0 ? POLLIN : POLLOUT;
		many[i].revents = 0;
	}
	int result = poll(many, 64, 0);
	int good = 0;
	for (int i = 0; i < 64; i++) {
		if (many[i].revents == (i % 2 == 0 ? POLLIN : POLLOUT))
			good++;
	}
	CHECK(result == 64 && good == 64,
		"64 entries of the same two pipe ends: %d, %d of 64 right", result,
		good);

	close(fds[1]);
	check_poll("pipe read end, data, writer closed", fds[0], POLLIN, 0,
		POLLIN | POLLHUP);
	char c;
	read(fds[0], &c, 1);
	check_poll("pipe read end, empty, writer closed", fds[0], POLLIN, 0,
		POLLHUP);
	check_poll("pipe read end, empty, writer closed", fds[0], 0, 0, POLLHUP);
	check_select("pipe read end, writer closed", fds[0], true, false, true,
		true, false, false);
	close(fds[0]);

	make_pipe(fds);
	fill_nonblocking(fds[1]);
	check_poll("pipe write end, full", fds[1], POLLOUT, 0, 0);
	check_select("pipe write end, full", fds[1], false, true, false,
		false, false, false);
	close(fds[0]);
	check_poll("pipe write end, full, reader closed", fds[1], POLLOUT, 0,
		POLLERR);
	close(fds[1]);

	make_pipe(fds);
	close(fds[0]);
	check_poll("pipe write end, reader closed", fds[1], POLLOUT, 0,
		POLLOUT | POLLERR);
	check_poll("pipe write end, reader closed", fds[1], 0, 0, POLLERR);
	check_select("pipe write end, reader closed", fds[1], false, true, true,
		false, true, true);
	close(fds[1]);
}


static void
test_fifo()
{
	// The live system's boot volume refuses FIFOs; the current directory
	// will do then.
	char path[64];
	snprintf(path, sizeof(path), "/tmp/poll_events_test.%d", (int)getpid());
	unlink(path);
	if (mkfifo(path, 0600) != 0) {
		snprintf(path, sizeof(path), "poll_events_test.%d", (int)getpid());
		unlink(path);
		if (mkfifo(path, 0600) != 0) {
			CHECK(false, "mkfifo(): %s", strerror(errno));
			return;
		}
	}
	int fd = open(path, O_RDWR);
	CHECK(fd >= 0, "FIFO opened O_RDWR");
	if (fd >= 0) {
		check_poll("FIFO O_RDWR, empty", fd, POLLIN | POLLOUT, 0, POLLOUT);
		write(fd, "a", 1);
		check_poll("FIFO O_RDWR, with data", fd, POLLIN | POLLOUT, 0,
			POLLIN | POLLOUT);
		check_select("FIFO O_RDWR, with data", fd, true, true, true,
			true, true, false);
		int reader = open(path, O_RDONLY | O_NONBLOCK);
		check_poll("FIFO O_RDONLY, data, a writer open", reader, POLLIN, 0,
			POLLIN);
		close(fd);
		check_poll("FIFO O_RDONLY, data, no writer", reader, POLLIN, 0,
			POLLIN | POLLHUP);
		close(reader);
	}
	unlink(path);
}


static void
test_sockets()
{
	int fds[2];
	make_socketpair(fds);
	check_poll("unix stream, empty", fds[0], POLLIN, 0, 0);
	check_poll("unix stream, empty", fds[0], POLLIN | POLLOUT, 0, POLLOUT);
	write(fds[1], "a", 1);
	check_poll("unix stream, with data", fds[0], POLLIN, 0, POLLIN);
	check_poll("unix stream, with data", fds[0], POLLIN | POLLPRI, 0,
		POLLIN);
	check_select("unix stream, with data", fds[0], true, true, true,
		true, true, false);
	shutdown(fds[1], SHUT_WR);
	check_poll("unix stream, data, peer shut down writing", fds[0], POLLIN,
		0, POLLIN);
	char c;
	read(fds[0], &c, 1);
	check_poll("unix stream, empty, peer shut down writing", fds[0], POLLIN,
		0, POLLIN | POLLHUP);
	close(fds[0]);
	close(fds[1]);

	make_socketpair(fds);
	close(fds[1]);
	check_poll("unix stream, peer closed", fds[0], POLLIN, 0,
		POLLIN | POLLHUP);
	check_poll("unix stream, peer closed", fds[0], POLLOUT, 0,
		POLLOUT | POLLHUP);
	check_select("unix stream, peer closed", fds[0], true, true, true,
		true, true, false);
	close(fds[0]);

	struct sockaddr_in address;
	int udp = make_udp_socket(address);
	check_poll("UDP socket, empty", udp, POLLIN, 0, 0);
	check_poll("UDP socket, empty", udp, POLLIN | POLLOUT, 0, POLLOUT);
	sendto(udp, "a", 1, 0, (struct sockaddr*)&address, sizeof(address));
	check_poll("UDP socket, a datagram", udp, POLLIN, 1000, POLLIN);
	check_select("UDP socket, a datagram", udp, true, true, true,
		true, true, false);
	close(udp);

	int listener = make_tcp_listener(address);
	check_poll("TCP listener, nothing", listener, POLLIN | POLLOUT, 0, 0);
	int client = socket(AF_INET, SOCK_STREAM, 0);
	connect(client, (struct sockaddr*)&address, sizeof(address));
	check_poll("TCP listener, a connection", listener, POLLIN, 1000, POLLIN);
	check_poll("TCP client, connected", client, POLLOUT, 1000, POLLOUT);
	close(client);
	close(listener);
}


static void
test_files()
{
	char path[64];
	snprintf(path, sizeof(path), "/tmp/poll_events_test.%d.f", (int)getpid());
	int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600);
	check_poll("regular file", fd, POLLIN | POLLOUT, 0, POLLIN | POLLOUT);
	check_poll("regular file, POLLPRI too", fd, POLLIN | POLLPRI, 0,
		POLLIN | POLLPRI);
	check_select("regular file", fd, true, true, true, true, true, false);
	close(fd);
	unlink(path);

	fd = open("/dev/null", O_RDWR);
	check_poll("/dev/null", fd, POLLIN | POLLOUT, 0, POLLIN | POLLOUT);
	close(fd);
}


struct Action {
	int		what;
	int		fd;
	struct sockaddr_in address;
};

enum {
	WRITE_ONE, READ_ALL, CLOSE, SEND_DATAGRAM, CONNECT
};


static void*
act_later(void* data)
{
	Action* action = (Action*)data;
	usleep(50000);
	char buffer[65536];
	switch (action->what) {
		case WRITE_ONE:
			write(action->fd, "a", 1);
			break;
		case READ_ALL:
		{
			int flags = fcntl(action->fd, F_GETFL);
			fcntl(action->fd, F_SETFL, flags | O_NONBLOCK);
			while (read(action->fd, buffer, sizeof(buffer)) > 0)
				;
			fcntl(action->fd, F_SETFL, flags);
			break;
		}
		case CLOSE:
			close(action->fd);
			break;
		case SEND_DATAGRAM:
		{
			int fd = socket(AF_INET, SOCK_DGRAM, 0);
			sendto(fd, "a", 1, 0, (struct sockaddr*)&action->address,
				sizeof(action->address));
			close(fd);
			break;
		}
		case CONNECT:
			action->fd = socket(AF_INET, SOCK_STREAM, 0);
			connect(action->fd, (struct sockaddr*)&action->address,
				sizeof(action->address));
			break;
	}
	return NULL;
}


static void
check_woken(const char* name, int fd, short events, Action& action,
	short expected)
{
	pthread_t thread;
	pthread_create(&thread, NULL, act_later, &action);
	bigtime_t start = system_time();
	struct pollfd pfd;
	pfd.fd = fd;
	pfd.events = events;
	pfd.revents = 0;
	int result = poll(&pfd, 1, 5000);
	bigtime_t waited = system_time() - start;
	pthread_join(thread, NULL);
	CHECK(result == 1 && pfd.revents == expected && waited >= 30000
			&& waited < 4000000,
		"%-44s woken: %d, %s after %lld ms (expected %s)", name, result,
		events_string(pfd.revents), (long long)waited / 1000,
		events_string(expected));
}


static void
test_wakeups()
{
	int fds[2];
	Action action;

	make_pipe(fds);
	action.what = WRITE_ONE;
	action.fd = fds[1];
	check_woken("pipe read end, by a write", fds[0], POLLIN, action, POLLIN);
	close(fds[0]);
	close(fds[1]);

	make_pipe(fds);
	action.what = CLOSE;
	action.fd = fds[1];
	check_woken("pipe read end, by the writer's close", fds[0], POLLIN,
		action, POLLHUP);
	close(fds[0]);

	make_pipe(fds);
	fill_nonblocking(fds[1]);
	action.what = READ_ALL;
	action.fd = fds[0];
	check_woken("full pipe write end, by a read", fds[1], POLLOUT, action,
		POLLOUT);
	close(fds[0]);
	close(fds[1]);

	make_pipe(fds);
	fill_nonblocking(fds[1]);
	action.what = CLOSE;
	action.fd = fds[0];
	check_woken("full pipe write end, by the reader's close", fds[1],
		POLLOUT, action, POLLERR);
	close(fds[1]);

	make_socketpair(fds);
	action.what = WRITE_ONE;
	action.fd = fds[1];
	check_woken("unix stream, by a write", fds[0], POLLIN, action, POLLIN);
	close(fds[0]);
	close(fds[1]);

	make_socketpair(fds);
	action.what = CLOSE;
	action.fd = fds[1];
	check_woken("unix stream, by the peer's close", fds[0], POLLIN, action,
		POLLIN | POLLHUP);
	close(fds[0]);

	struct sockaddr_in address;
	int udp = make_udp_socket(address);
	action.what = SEND_DATAGRAM;
	action.address = address;
	check_woken("UDP socket, by a datagram", udp, POLLIN, action, POLLIN);
	close(udp);

	int listener = make_tcp_listener(address);
	action.what = CONNECT;
	action.address = address;
	check_woken("TCP listener, by a connection", listener, POLLIN, action,
		POLLIN);
	close(action.fd);
	close(listener);
}


/*!	Many zero-timeout poll()s and select()s of many pipes and sockets, then a
	write to every one of them, then the same again. A select entry left
	behind by a poll() would now be notified into a stack frame that is gone,
	or keep its pool from being freed; neither may happen.
*/
static void
test_many()
{
	static const int kCount = 64;
	int pipes[kCount][2];
	int sockets[kCount][2];
	for (int i = 0; i < kCount; i++) {
		make_pipe(pipes[i]);
		make_socketpair(sockets[i]);
	}

	struct pollfd fds[4 * kCount];
	for (int i = 0; i < kCount; i++) {
		fds[4 * i].fd = pipes[i][0];
		fds[4 * i].events = POLLIN;
		fds[4 * i + 1].fd = pipes[i][1];
		fds[4 * i + 1].events = POLLOUT;
		fds[4 * i + 2].fd = sockets[i][0];
		fds[4 * i + 2].events = POLLIN;
		fds[4 * i + 3].fd = sockets[i][1];
		fds[4 * i + 3].events = POLLIN | POLLOUT;
	}

	int bad = 0;
	for (int round = 0; round < 2; round++) {
		for (int i = 0; i < 2000; i++) {
			int result = poll(fds, 4 * kCount, 0);
			if (result != (round == 0 ? 2 * kCount : 4 * kCount))
				bad++;
			fd_set set;
			FD_ZERO(&set);
			int maxFD = 0;
			for (int j = 0; j < kCount; j++) {
				FD_SET(pipes[j][0], &set);
				FD_SET(sockets[j][0], &set);
				if (pipes[j][0] > maxFD)
					maxFD = pipes[j][0];
				if (sockets[j][0] > maxFD)
					maxFD = sockets[j][0];
			}
			if (maxFD < FD_SETSIZE) {
				struct timeval tv = { 0, 0 };
				result = select(maxFD + 1, &set, NULL, NULL, &tv);
				if (result != (round == 0 ? 0 : 2 * kCount))
					bad++;
			}
		}
		CHECK(bad == 0, "round %d: 2000 polls of %d descriptors and selects"
			" of %d, %d wrong results", round, 4 * kCount, 2 * kCount, bad);
		bad = 0;
		for (int i = 0; i < kCount; i++) {
			write(pipes[i][1], "a", 1);
			write(sockets[i][1], "a", 1);
		}
	}

	for (int i = 0; i < kCount; i++) {
		close(pipes[i][0]);
		close(pipes[i][1]);
		close(sockets[i][0]);
		close(sockets[i][1]);
	}
}


int
main()
{
	thread_id thread = spawn_thread(watchdog, "watchdog", B_NORMAL_PRIORITY,
		NULL);
	resume_thread(thread);

	struct rlimit limit;
	getrlimit(RLIMIT_NOFILE, &limit);
	if (limit.rlim_cur < 512 && limit.rlim_max >= 512) {
		limit.rlim_cur = 512;
		setrlimit(RLIMIT_NOFILE, &limit);
	}

	test_pipes();
	test_fifo();
	test_sockets();
	test_files();
	test_wakeups();
	test_many();

	if (sFailures != 0) {
		printf("%d check(s) failed\n", sFailures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
