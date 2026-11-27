/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	getaddrinfo()'s AAAA and A questions sent together (res_nsend_pair() in
	res_send.c, airos-works/haiku#232), against a name server stub on the
	loopback interface; resolv.conf is not used.
	- Both answers in one round trip, each in the buffer of its question even
	  when they come in the other order.
	- A server that drops one of the two questions costs its first wait and
	  a short one, and the other answer is kept.
	- One lost answer is asked for again, and does not end the sending of
	  questions together; two lookups in a row that lose one do.
	- A truncated answer is asked for again over TCP, the other one kept.
	- The "single-request" option sends them one after the other.
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


// private to libnetwork (netresolv's resolv.h)
extern "C" int __res_nsend_pair(res_state, const unsigned char*, int,
	unsigned char*, int, int*, const unsigned char*, int, unsigned char*, int,
	int*);

// netresolv's private resolv.h
#define RES_SNGLKUP	(1 << 19)


enum {
	kAnswerReversed,	// both, the second question's answer first
	kDropAAAA,			// never answers AAAA
	kLoseFirstAAAA,		// loses the first AAAA question it gets
	kSamePort,			// drops a question from the port of the previous one
	kTruncateA			// A answers truncated, the whole one over TCP
};

static int sFailures = 0;
static volatile int sMode = kAnswerReversed;
static volatile int sQuestions = 0;
static volatile bool sLostOne = false;


static double
now()
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec + time.tv_nsec / 1e9;
}


static void
check(bool condition, const char* what)
{
	if (!condition) {
		printf("FAIL: %s\n", what);
		sFailures++;
	}
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


struct Reply {
	unsigned char	data[512];
	ssize_t			length;
	sockaddr_in		to;
};


/*!	Turns the question in reply into its answer: 192.0.2.10 for A, no data
	for AAAA. Returns the question type, or -1.
*/
static int
make_answer(Reply& reply)
{
	ssize_t position = 12;
	while (position < reply.length && reply.data[position] != 0)
		position += reply.data[position] + 1;
	position += 5;
	if (position > reply.length)
		return -1;
	int type = (reply.data[position - 4] << 8) | reply.data[position - 3];

	reply.data[2] = 0x80 | (reply.data[2] & 0x01);
	reply.data[3] = 0x80;
	reply.data[6] = 0;
	reply.data[7] = type == ns_t_a ? 1 : 0;
	reply.data[8] = reply.data[9] = reply.data[10] = reply.data[11] = 0;
	reply.length = position;
	if (type == ns_t_a) {
		static const unsigned char kRecord[] = { 0xc0, 0x0c, 0, 1, 0, 1,
			0, 0, 0, 60, 0, 4, 192, 0, 2, 10 };
		memcpy(reply.data + position, kRecord, sizeof(kRecord));
		reply.length += sizeof(kRecord);
	}
	return type;
}


static void*
stub_thread(void* data)
{
	int fd = (int)(intptr_t)data;
	Reply held;
	bool holding = false;
	sockaddr_in last;
	double lastTime = -10;
	memset(&last, 0, sizeof(last));

	while (true) {
		Reply reply;
		socklen_t fromLength = sizeof(reply.to);
		reply.length = recvfrom(fd, reply.data, sizeof(reply.data) - 32, 0,
			(sockaddr*)&reply.to, &fromLength);
		if (reply.length < 12)
			continue;
		sQuestions++;
		bool samePort = reply.to.sin_port == last.sin_port
			&& now() - lastTime < 0.5;
		last = reply.to;
		lastTime = now();
		int type = make_answer(reply);
		if (type < 0)
			continue;

		switch (sMode) {
			case kAnswerReversed:
				// the first question waits for the second
				if (!holding) {
					held = reply;
					holding = true;
					continue;
				}
				usleep(300000);
				sendto(fd, reply.data, reply.length, 0, (sockaddr*)&reply.to,
					sizeof(reply.to));
				sendto(fd, held.data, held.length, 0, (sockaddr*)&held.to,
					sizeof(held.to));
				holding = false;
				continue;
			case kDropAAAA:
				if (type == ns_t_aaaa)
					continue;
				break;
			case kLoseFirstAAAA:
				if (type == ns_t_aaaa && !sLostOne) {
					sLostOne = true;
					continue;
				}
				break;
			case kSamePort:
				if (samePort)
					continue;
				break;
			case kTruncateA:
				if (type == ns_t_a) {
					reply.data[2] |= 0x02;
					reply.data[7] = 0;
					reply.length -= 16;
				}
				break;
		}
		sendto(fd, reply.data, reply.length, 0, (sockaddr*)&reply.to,
			sizeof(reply.to));
	}
	return NULL;
}


/*!	Answers A questions over TCP with 192.0.2.11.
*/
static void*
tcp_stub_thread(void* data)
{
	int listener = (int)(intptr_t)data;
	while (true) {
		int fd = accept(listener, NULL, NULL);
		if (fd < 0)
			continue;
		Reply reply;
		unsigned char length[2];
		if (recv(fd, length, 2, MSG_WAITALL) == 2) {
			reply.length = (length[0] << 8) | length[1];
			if (reply.length > 12 && reply.length < 400
				&& recv(fd, reply.data, reply.length, MSG_WAITALL)
					== reply.length
				&& make_answer(reply) == ns_t_a) {
				reply.data[reply.length - 1] = 11;
				length[0] = reply.length >> 8;
				length[1] = reply.length & 0xff;
				send(fd, length, 2, 0);
				send(fd, reply.data, reply.length, 0);
			}
		}
		close(fd);
	}
	return NULL;
}


struct Pair {
	unsigned char	question[2][512];
	int				questionLength[2];
	unsigned char	answer[2][512];
	int				answerLength[2];
	int				result;
	double			seconds;
};


static int
answer_type(const unsigned char* answer, int length)
{
	ssize_t position = 12;
	while (position < length && answer[position] != 0)
		position += answer[position] + 1;
	if (position + 3 > length)
		return -1;
	return (answer[position + 1] << 8) | answer[position + 2];
}


static void
ask(res_state state, const char* name, Pair& pair)
{
	static const int kTypes[2] = { ns_t_aaaa, ns_t_a };
	for (int i = 0; i < 2; i++) {
		pair.questionLength[i] = res_nmkquery(state, ns_o_query, name, ns_c_in,
			kTypes[i], NULL, 0, NULL, pair.question[i],
			sizeof(pair.question[i]));
	}
	// as getaddrinfo() does
	if (pair.question[0][0] == pair.question[1][0]
		&& pair.question[0][1] == pair.question[1][1])
		pair.question[1][1]++;

	sQuestions = 0;
	double start = now();
	pair.result = __res_nsend_pair(state, pair.question[0],
		pair.questionLength[0], pair.answer[0], sizeof(pair.answer[0]),
		&pair.answerLength[0], pair.question[1], pair.questionLength[1],
		pair.answer[1], sizeof(pair.answer[1]), &pair.answerLength[1]);
	pair.seconds = now() - start;
	printf("%s: result %d, answers %d/%d bytes, %d questions, %.2f s\n", name,
		pair.result, pair.answerLength[0], pair.answerLength[1], sQuestions,
		pair.seconds);
}


/*!	Whether answer i of pair is the answer to question i.
*/
static bool
answered(const Pair& pair, int i)
{
	return pair.result == 0 && pair.answerLength[i] >= 12
		&& pair.answer[i][0] == pair.question[i][0]
		&& pair.answer[i][1] == pair.question[i][1]
		&& answer_type(pair.answer[i], pair.answerLength[i])
			== (i == 0 ? ns_t_aaaa : ns_t_a);
}


static void
init_state(res_state state, int port)
{
	memset(state, 0, sizeof(*state));
	if (res_ninit(state) != 0) {
		printf("FAIL: res_ninit()\n");
		exit(1);
	}
	sockaddr_in& address = state->nsaddr_list[0];
	memset(&address, 0, sizeof(address));
	address.sin_len = sizeof(address);
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	state->nscount = 1;
	state->retrans = 2;
	state->retry = 2;
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

	int port;
	int fd = bound_socket(port);
	if (fd < 0) {
		printf("FAIL: no socket: %s\n", strerror(errno));
		return 1;
	}
	pthread_t thread;
	pthread_create(&thread, NULL, stub_thread, (void*)(intptr_t)fd);

	// TCP on the same port
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	int reuse = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	if (listener < 0 || bind(listener, (sockaddr*)&address, sizeof(address))
			!= 0 || listen(listener, 4) != 0) {
		printf("FAIL: no TCP socket: %s\n", strerror(errno));
		return 1;
	}
	pthread_t tcpThread;
	pthread_create(&tcpThread, NULL, tcp_stub_thread,
		(void*)(intptr_t)listener);

	struct __res_state state;
	init_state(&state, port);
	Pair pair;

	// Both questions are out before the first answer comes.
	sMode = kAnswerReversed;
	ask(&state, "reversed.test", pair);
	check(pair.result == 0, "the pair was not sent together");
	check(answered(pair, 0) && answered(pair, 1),
		"an answer is missing or in the wrong buffer");
	check(pair.seconds < 0.5, "the questions were not out at the same time");

	// The server drops AAAA questions.
	sMode = kDropAAAA;
	ask(&state, "noaaaa.test", pair);
	check(answered(pair, 1), "the A answer was not kept");
	check(pair.answerLength[0] < 0, "an answer to a dropped question");
	check(pair.seconds > 2.5 && pair.seconds < 3.6,
		"not the first wait (2 s) and a short one (1 s)");

	// One answer lost now and then: asked again, still sent together.
	init_state(&state, port);
	sMode = kLoseFirstAAAA;
	sLostOne = false;
	ask(&state, "lost.test", pair);
	check(answered(pair, 0) && answered(pair, 1),
		"the lost question was not asked again");
	ask(&state, "after-lost.test", pair);
	check(answered(pair, 0) && answered(pair, 1) && pair.seconds < 0.5,
		"the lookup after a lost answer failed");
	check((state.options & RES_SNGLKUP) == 0,
		"one lost answer ended sending questions together");

	// A middlebox that drops the second of two questions from one port.
	init_state(&state, port);
	sMode = kSamePort;
	ask(&state, "port1.test", pair);
	check(answered(pair, 0) && answered(pair, 1),
		"the question was not asked again from another port");
	ask(&state, "port2.test", pair);
	check(answered(pair, 0) && answered(pair, 1),
		"the question was not asked again from another port");
	check((state.options & RES_SNGLKUP) != 0,
		"two lookups in a row lost an answer, still sending together");
	ask(&state, "port3.test", pair);
	check(pair.result == -1, "not one question after the other");

	// A truncated answer: the whole one over TCP.
	init_state(&state, port);
	sMode = kTruncateA;
	ask(&state, "truncated.test", pair);
	check(answered(pair, 0) && answered(pair, 1),
		"an answer is missing after a truncated one");
	check(pair.answerLength[1] > 4 && (pair.answer[1][2] & 0x02) == 0
		&& pair.answer[1][pair.answerLength[1] - 1] == 11,
		"the A answer is not the one over TCP");

	// The "single-request" option.
	setenv("RES_OPTIONS", "single-request", 1);
	init_state(&state, port);
	unsetenv("RES_OPTIONS");
	sMode = kAnswerReversed;
	ask(&state, "single.test", pair);
	check(pair.result == -1, "\"single-request\" sent them together");

	printf("%s\n", sFailures == 0 ? "PASS" : "FAILED");
	return sFailures == 0 ? 0 : 1;
}
