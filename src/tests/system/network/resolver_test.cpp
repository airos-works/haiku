/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	The resolver's sending of queries (res_send.c), against a name server
	stub on the loopback interface; resolv.conf is not used.
	- A name server that does not answer costs its timeout once, not on
	  every query (airos-works/haiku#232).
*/


#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <resolv.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>


static int sFailures = 0;


static double
now()
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec + time.tv_nsec / 1e9;
}


static int
bound_socket(int& _port)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd < 0 || bind(fd, (sockaddr*)&address, sizeof(address)) != 0)
		return -1;
	socklen_t length = sizeof(address);
	getsockname(fd, (sockaddr*)&address, &length);
	_port = ntohs(address.sin_port);
	return fd;
}


/*!	Answers every question with the address 192.0.2.10.
*/
static void*
stub_thread(void* data)
{
	int fd = (int)(intptr_t)data;
	while (true) {
		unsigned char buffer[512];
		sockaddr_in from;
		socklen_t fromLength = sizeof(from);
		ssize_t length = recvfrom(fd, buffer, sizeof(buffer) - 16, 0,
			(sockaddr*)&from, &fromLength);
		if (length < 12)
			continue;

		// the end of the question (name, type, class)
		ssize_t position = 12;
		while (position < length && buffer[position] != 0)
			position += buffer[position] + 1;
		position += 5;
		if (position > length)
			continue;

		buffer[2] = 0x80 | (buffer[2] & 0x01);
		buffer[3] = 0x80;
		buffer[6] = 0;
		buffer[7] = 1;
		buffer[8] = buffer[9] = buffer[10] = buffer[11] = 0;
		static const unsigned char kRecord[] = { 0xc0, 0x0c, 0, 1, 0, 1,
			0, 0, 0, 60, 0, 4, 192, 0, 2, 10 };
		memcpy(buffer + position, kRecord, sizeof(kRecord));
		sendto(fd, buffer, position + sizeof(kRecord), 0, (sockaddr*)&from,
			fromLength);
	}
	return NULL;
}


static void
set_servers(res_state state, int count, const int* ports)
{
	for (int i = 0; i < count; i++) {
		sockaddr_in& address = state->nsaddr_list[i];
		memset(&address, 0, sizeof(address));
		address.sin_len = sizeof(address);
		address.sin_family = AF_INET;
		address.sin_port = htons(ports[i]);
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	}
	state->nscount = count;
	state->retrans = 2;
	state->retry = 2;
}


static double
query(res_state state, const char* name, int& _result)
{
	unsigned char answer[512];
	double start = now();
	_result = res_nquery(state, name, ns_c_in, ns_t_a, answer,
		sizeof(answer));
	return now() - start;
}


static void
crash_handler(int signal)
{
	printf("FAIL: signal %d\n", signal);
	_exit(1);
}


int
main()
{
	signal(SIGSEGV, crash_handler);
	alarm(60);
		// a deadline for the whole test

	int livePort, deadPort;
	int live = bound_socket(livePort);
	int dead = bound_socket(deadPort);
		// bound but never read: no answer, and no ICMP error either
	if (live < 0 || dead < 0) {
		printf("FAIL: no sockets: %s\n", strerror(errno));
		return 1;
	}
	pthread_t thread;
	pthread_create(&thread, NULL, stub_thread, (void*)(intptr_t)live);

	struct __res_state state;
	memset(&state, 0, sizeof(state));
	if (res_ninit(&state) != 0) {
		printf("FAIL: res_ninit()\n");
		return 1;
	}

	// The first of two servers does not answer.
	int ports[2] = { deadPort, livePort };
	set_servers(&state, 2, ports);
	int result;
	double first = query(&state, "first.test", result);
	printf("dead first server, first query: %d, %.2f s\n", result, first);
	if (result < 0) {
		printf("FAIL: no answer from the second server\n");
		sFailures++;
	}
	double second = query(&state, "second.test", result);
	printf("dead first server, second query: %d, %.2f s\n", result, second);
	if (result < 0 || second > 1.0) {
		printf("FAIL: the dead server cost its timeout again\n");
		sFailures++;
	}

	printf("%s\n", sFailures == 0 ? "PASS" : "FAILED");
	return sFailures == 0 ? 0 : 1;
}
