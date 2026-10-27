/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab tool: the other direction of cubie_fetch. Sends a number of
	mebibytes of pseudo-random bytes to a TCP port of the lab host (a sink
	that counts them), and says how long that took.

	cubie_send <IPv4 address> <port> <MiB> */


#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <OS.h>


int
main(int argc, char** argv)
{
	if (argc != 4) {
		fprintf(stderr, "usage: %s <IPv4 address> <port> <MiB>\n", argv[0]);
		return 1;
	}

	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons(atoi(argv[2]));
	if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1) {
		fprintf(stderr, "bad address %s\n", argv[1]);
		return 1;
	}
	long long wanted = atoll(argv[3]) * 1024 * 1024;

	int connection = socket(AF_INET, SOCK_STREAM, 0);
	if (connection < 0
		|| connect(connection, (sockaddr*)&address, sizeof(address)) != 0) {
		fprintf(stderr, "connect: %s\n", strerror(errno));
		return 1;
	}

	// random enough that nothing on the way compresses it
	static uint32 buffer[64 * 1024];
	uint32 state = 0x2545f491;
	for (size_t i = 0; i < sizeof(buffer) / sizeof(buffer[0]); i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		buffer[i] = state;
	}

	bigtime_t start = system_time();
	long long total = 0;
	while (total < wanted) {
		size_t chunk = sizeof(buffer);
		if ((long long)chunk > wanted - total)
			chunk = wanted - total;
		ssize_t count = write(connection, buffer, chunk);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			fprintf(stderr, "write: %s after %lld bytes\n", strerror(errno),
				total);
			return 1;
		}
		total += count;
	}
	shutdown(connection, SHUT_WR);
	char end;
	while (read(connection, &end, 1) > 0)
		;
	close(connection);

	bigtime_t elapsed = system_time() - start;
	printf("%lld bytes in %.1f s, %.1f Mbit/s\n", total, elapsed / 1e6,
		total * 8.0 / elapsed);
	return 0;
}
