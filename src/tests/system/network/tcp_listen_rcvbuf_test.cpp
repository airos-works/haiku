/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	A listening socket with a large SO_RCVBUF, and bulk data on a connection
	accepted from it (airos-works/haiku#173).

	The accepted connection took its window shift from the listener's buffer
	size but sized its receive buffer by itself, and rounded the new size
	with a segment size shifted to 0: a divide error in the kernel (x86_64)
	at the first megabytes of an upload. That needs a segment size below
	1 << shift, so the default is to connect to the address of the first
	interface that is not the loopback (its MTU of 1500 counts, although the
	data never leaves the machine); the loopback's own 64 KB segments never
	shift to 0.

	Usage: tcp_listen_rcvbuf_test [address [megabytes [seconds]]]

	Checks that the transfer runs for the given time (default 3 s), that the
	accepted socket reports the listener's buffer size, and that the size
	the listener got is within the limit sockets have now. Before the fix
	the machine is in the kernel debugger instead.
*/


#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <OS.h>


static sockaddr_in sAddress;
static int sSeconds = 3;
static volatile bool sClientFailed = false;


static void
set_timeouts(int fd, int seconds)
{
	// no wait in this test is without a deadline
	struct timeval timeout = { seconds, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}


static bool
first_interface_address(in_addr& address)
{
	struct ifaddrs* list;
	if (getifaddrs(&list) != 0)
		return false;

	bool found = false;
	for (struct ifaddrs* i = list; i != NULL && !found; i = i->ifa_next) {
		if (i->ifa_addr == NULL || i->ifa_addr->sa_family != AF_INET
			|| (i->ifa_flags & IFF_LOOPBACK) != 0
			|| (i->ifa_flags & IFF_UP) == 0)
			continue;
		address = ((sockaddr_in*)i->ifa_addr)->sin_addr;
		found = address.s_addr != INADDR_ANY;
	}
	freeifaddrs(list);
	return found;
}


static void*
client(void*)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	set_timeouts(fd, 5);
	if (connect(fd, (sockaddr*)&sAddress, sizeof(sAddress)) != 0) {
		printf("connect: %s\n", strerror(errno));
		sClientFailed = true;
		close(fd);
		return NULL;
	}

	static char block[65536];
	memset(block, 'r', sizeof(block));
	const bigtime_t end = system_time() + sSeconds * 1000000LL;
	while (system_time() < end) {
		if (send(fd, block, sizeof(block), 0) < 0) {
			printf("send: %s\n", strerror(errno));
			sClientFailed = true;
			break;
		}
	}
	close(fd);
	return NULL;
}


int
main(int argc, char** argv)
{
	int megabytes = 128;
	memset(&sAddress, 0, sizeof(sAddress));
	sAddress.sin_family = AF_INET;
	sAddress.sin_len = sizeof(sAddress);
	if (argc > 1) {
		if (inet_pton(AF_INET, argv[1], &sAddress.sin_addr) != 1) {
			fprintf(stderr, "usage: %s [address [megabytes [seconds]]]\n",
				argv[0]);
			return 1;
		}
	} else if (!first_interface_address(sAddress.sin_addr)) {
		printf("no interface but the loopback: the test cannot fail there\n");
		sAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	}
	if (argc > 2)
		megabytes = atoi(argv[2]);
	if (argc > 3)
		sSeconds = atoi(argv[3]);

	signal(SIGPIPE, SIG_IGN);

	int listener = socket(AF_INET, SOCK_STREAM, 0);
	int size = megabytes << 20;
	if (setsockopt(listener, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) != 0) {
		printf("FAIL: SO_RCVBUF %d MiB: %s\n", megabytes, strerror(errno));
		return 1;
	}
	int listenerSize = 0;
	socklen_t length = sizeof(listenerSize);
	getsockopt(listener, SOL_SOCKET, SO_RCVBUF, &listenerSize, &length);

	if (bind(listener, (sockaddr*)&sAddress, sizeof(sAddress)) != 0
		|| listen(listener, 1) != 0) {
		printf("FAIL: bind/listen: %s\n", strerror(errno));
		return 1;
	}
	length = sizeof(sAddress);
	getsockname(listener, (sockaddr*)&sAddress, &length);
	char name[32];
	inet_ntop(AF_INET, &sAddress.sin_addr, name, sizeof(name));
	printf("listening on %s:%d, SO_RCVBUF asked %d MiB, got %d bytes\n", name,
		ntohs(sAddress.sin_port), megabytes, listenerSize);
	fflush(stdout);

	pthread_t thread;
	pthread_create(&thread, NULL, client, NULL);

	set_timeouts(listener, 5);
	int fd = accept(listener, NULL, NULL);
	if (fd < 0) {
		printf("FAIL: accept: %s\n", strerror(errno));
		return 1;
	}
	set_timeouts(fd, 5);

	int acceptedSize = 0, segmentSize = 0;
	length = sizeof(acceptedSize);
	getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &acceptedSize, &length);
	length = sizeof(segmentSize);
	getsockopt(fd, IPPROTO_TCP, TCP_MAXSEG, &segmentSize, &length);
	printf("accepted: SO_RCVBUF %d bytes, segment size %d\n", acceptedSize,
		segmentSize);
	fflush(stdout);

	static char buffer[1 << 20];
	uint64 total = 0;
	const bigtime_t start = system_time();
	const bigtime_t deadline = start + (sSeconds + 10) * 1000000LL;
	bool failed = false;
	while (system_time() < deadline) {
		ssize_t bytes = recv(fd, buffer, sizeof(buffer), 0);
		if (bytes < 0) {
			printf("FAIL: recv: %s\n", strerror(errno));
			failed = true;
			break;
		}
		if (bytes == 0)
			break;
		total += bytes;
	}
	const bigtime_t took = system_time() - start;
	pthread_join(thread, NULL);
	close(fd);
	close(listener);

	printf("received %" B_PRIu64 " KiB in %.2f s (%.1f MiB/s)\n", total / 1024,
		took / 1e6, total / 1048576.0 / (took / 1e6));

	if (failed || sClientFailed)
		return 1;
	if (total < 1024 * 1024) {
		printf("FAIL: less than 1 MiB went through\n");
		return 1;
	}
	if (acceptedSize != listenerSize) {
		printf("FAIL: the accepted socket does not report the listener's "
			"buffer size\n");
		return 1;
	}
	if ((uint32)listenerSize > (64u << 20)) {
		printf("FAIL: no limit on SO_RCVBUF\n");
		return 1;
	}

	printf("PASS\n");
	return 0;
}
