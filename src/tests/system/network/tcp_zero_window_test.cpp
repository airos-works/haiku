/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	A receiver that stops reading for a while, then reads again
	(airos-works/haiku#173).

	While the peer's window is closed and everything sent is acknowledged,
	nothing is lost, so nothing may time out: the sender has to probe the
	window from its persist timer only. It used to arm the retransmit timer
	for every (empty) probe, and each expiry counted a retransmit timeout,
	doubled the timeout and cut the congestion window to one segment.

	Usage: tcp_zero_window_test [seconds [address]]

	One connection (over the loopback by default), a sender that writes all
	the time and a receiver that sleeps for the given time (default 5 s)
	after the first megabyte. Prints the stack's counters over the pause and
	how fast the data flows again, and fails if retransmit timeouts were
	counted or the transfer does not resume within 2 s. (The window probes
	are counted with the retransmitted segments, as keepalive probes are.)
	The counters are the whole stack's, so one timeout is let pass: it may
	be another connection's. The pause has to be 4 s or more for the test
	to tell anything.
*/


#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <OS.h>

#include <net_stat.h>


extern "C" status_t _kern_generic_syscall(const char* subsystem,
	uint32 function, void* buffer, size_t bufferSize);


static sockaddr_in sAddress;
static int sPause = 5;
static volatile int64 sSent = 0;
static volatile bool sStop = false;
static volatile bool sSenderFailed = false;


static void
set_timeouts(int fd, int seconds)
{
	// no wait in this test is without a deadline
	struct timeval timeout = { seconds, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}


static bool
get_stats(net_protocol_stats& stats)
{
	memset(&stats, 0, sizeof(stats));
	return _kern_generic_syscall(NET_STAT_SYSCALLS,
		NET_STAT_GET_PROTOCOL_STATS, &stats, sizeof(stats)) == B_OK;
}


static void*
sender(void*)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	set_timeouts(fd, sPause + 10);
	if (connect(fd, (sockaddr*)&sAddress, sizeof(sAddress)) != 0) {
		printf("connect: %s\n", strerror(errno));
		sSenderFailed = true;
		close(fd);
		return NULL;
	}

	static char block[16384];
	memset(block, 'z', sizeof(block));
	while (!sStop) {
		ssize_t bytes = send(fd, block, sizeof(block), 0);
		if (bytes < 0) {
			printf("send: %s\n", strerror(errno));
			sSenderFailed = true;
			break;
		}
		atomic_add64((int64*)&sSent, bytes);
	}
	close(fd);
	return NULL;
}


/*!	Reads for the given time, returns the number of bytes. */
static int64
read_for(int fd, bigtime_t time)
{
	static char buffer[1 << 20];
	int64 total = 0;
	const bigtime_t end = system_time() + time;
	while (system_time() < end) {
		ssize_t bytes = recv(fd, buffer, sizeof(buffer), 0);
		if (bytes <= 0)
			break;
		total += bytes;
	}
	return total;
}


int
main(int argc, char** argv)
{
	if (argc > 1)
		sPause = atoi(argv[1]);
	memset(&sAddress, 0, sizeof(sAddress));
	sAddress.sin_family = AF_INET;
	sAddress.sin_len = sizeof(sAddress);
	sAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (argc > 2 && inet_pton(AF_INET, argv[2], &sAddress.sin_addr) != 1) {
		fprintf(stderr, "usage: %s [seconds [address]]\n", argv[0]);
		return 1;
	}

	signal(SIGPIPE, SIG_IGN);

	int listener = socket(AF_INET, SOCK_STREAM, 0);
	if (bind(listener, (sockaddr*)&sAddress, sizeof(sAddress)) != 0
		|| listen(listener, 1) != 0) {
		printf("FAIL: bind/listen: %s\n", strerror(errno));
		return 1;
	}
	socklen_t length = sizeof(sAddress);
	getsockname(listener, (sockaddr*)&sAddress, &length);

	pthread_t thread;
	pthread_create(&thread, NULL, sender, NULL);

	set_timeouts(listener, 5);
	int fd = accept(listener, NULL, NULL);
	if (fd < 0) {
		printf("FAIL: accept: %s\n", strerror(errno));
		return 1;
	}
	set_timeouts(fd, 2);

	// let the connection get up to speed
	const int64 warmUp = read_for(fd, 500000);
	printf("before the pause: %" B_PRId64 " KiB in 0.5 s\n", warmUp / 1024);
	fflush(stdout);

	net_protocol_stats before, after;
	if (!get_stats(before)) {
		printf("FAIL: no network statistics\n");
		return 1;
	}

	// The receiver stops reading: its buffer fills, its window closes, and
	// the sender has all it sent acknowledged.
	const int64 sentAtPause = sSent;
	snooze(sPause * 1000000LL);
	const int64 sentInPause = sSent - sentAtPause;

	get_stats(after);

	// read again: how long until the sender gets something into its queue,
	// and how much arrives in the first half second
	const int64 sentAtResume = sSent;
	const bigtime_t resume = system_time();
	bigtime_t sendResumed = -1;
	int64 first = 0;
	static char buffer[65536];
	while (system_time() < resume + 2000000) {
		if (sendResumed < 0 && sSent > sentAtResume)
			sendResumed = system_time() - resume;
		if (sendResumed >= 0 && system_time() >= resume + 500000)
			break;
		ssize_t bytes = recv(fd, buffer, sizeof(buffer), 0);
		if (bytes <= 0)
			break;
		if (system_time() < resume + 500000)
			first += bytes;
	}

	sStop = true;
	read_for(fd, 300000);
	close(fd);
	pthread_join(thread, NULL);
	close(listener);

	const uint64 timeouts = after.tcp_retransmit_timeouts
		- before.tcp_retransmit_timeouts;
	const uint64 retransmitted = after.tcp_retransmitted
		- before.tcp_retransmitted;
	printf("%d s pause: sender queued %" B_PRId64 " KiB more, then blocked\n",
		sPause, sentInPause / 1024);
	printf("  retransmit timeouts: %" B_PRIu64 "\n", timeouts);
	printf("  segments retransmitted: %" B_PRIu64 "\n", retransmitted);
	printf("  segments sent / received in the pause: %" B_PRIu64 " / %"
		B_PRIu64 "\n", after.tcp_sent - before.tcp_sent,
		after.tcp_received - before.tcp_received);
	printf("after the pause: %" B_PRId64 " KiB in the first 0.5 s, "
		"sender going again after %.1f ms\n", first / 1024,
		sendResumed / 1000.0);

	if (sSenderFailed)
		return 1;
	if (timeouts >= 2) {
		// (one may be another connection's; the bug counts one for every
		// probe, three in five seconds)
		printf("FAIL: a closed window was taken for a loss\n");
		return 1;
	}
	if (sendResumed < 0) {
		printf("FAIL: the sender did not go on within 2 s\n");
		return 1;
	}

	printf("PASS\n");
	return 0;
}
