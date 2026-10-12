// tcpbench sink <port>: accept one TCP connection, read until it closes,
// print the rate. tcpbench send <host> <port> <MiB> [interface]: send that
// much, optionally bound to one interface (SO_BINDTODEVICE, e.g.
// /dev/net/usb_ecm/0) so that a test of a USB network adapter cannot leave
// through the onboard one. With the peer on the other machine this measures
// what a USB network adapter moves - over USB 3 a gigabit adapter runs at the
// wire's ~112 MiB/s, over USB 2 it stops near 35.
#include <OS.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void
report(const char* what, off_t bytes, bigtime_t took)
{
	printf("%s %.1f MiB in %.2f s: %.1f MiB/s (%.0f Mbit/s)\n", what,
		bytes / 1048576.0, took / 1e6, bytes / 1048576.0 / (took / 1e6),
		bytes * 8.0 / took);
}

int
main(int argc, char** argv)
{
	if (argc >= 3 && strcmp(argv[1], "sink") == 0) {
		int listener = socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sockaddr_in address = {};
		address.sin_family = AF_INET;
		address.sin_port = htons(atoi(argv[2]));
		address.sin_addr.s_addr = INADDR_ANY;
		if (bind(listener, (sockaddr*)&address, sizeof(address)) != 0
			|| listen(listener, 1) != 0) {
			perror("listen");
			return 1;
		}
		int connection = accept(listener, NULL, NULL);
		if (connection < 0) {
			perror("accept");
			return 1;
		}
		static char buffer[256 * 1024];
		off_t total = 0;
		bigtime_t start = system_time();
		ssize_t got;
		while ((got = read(connection, buffer, sizeof(buffer))) > 0)
			total += got;
		report("received", total, system_time() - start);
		close(connection);
		close(listener);
		return 0;
	}
	if (argc >= 5 && strcmp(argv[1], "send") == 0) {
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (argc > 5) {
			// Haiku takes the interface index here, not its name
			uint32 index = if_nametoindex(argv[5]);
			if (index == 0 || setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
					&index, sizeof(index)) != 0) {
				perror("SO_BINDTODEVICE");
				return 1;
			}
		}
		sockaddr_in address = {};
		address.sin_family = AF_INET;
		address.sin_port = htons(atoi(argv[3]));
		inet_aton(argv[2], &address.sin_addr);
		if (connect(fd, (sockaddr*)&address, sizeof(address)) != 0) {
			perror("connect");
			return 1;
		}
		static char buffer[256 * 1024];
		memset(buffer, 0x5a, sizeof(buffer));
		off_t total = (off_t)atoi(argv[4]) * 1048576;
		off_t sent = 0;
		bigtime_t start = system_time();
		while (sent < total) {
			ssize_t written = write(fd, buffer, sizeof(buffer));
			if (written <= 0) {
				perror("write");
				break;
			}
			sent += written;
		}
		close(fd);
		report("sent", sent, system_time() - start);
		return 0;
	}
	if (argc >= 4 && strcmp(argv[1], "udpsink") == 0) {
		// count datagrams for a number of seconds after the first one
		int fd = socket(AF_INET, SOCK_DGRAM, 0);
		int size = 4 * 1024 * 1024;
		setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
		sockaddr_in address = {};
		address.sin_family = AF_INET;
		address.sin_port = htons(atoi(argv[2]));
		address.sin_addr.s_addr = INADDR_ANY;
		if (bind(fd, (sockaddr*)&address, sizeof(address)) != 0) {
			perror("bind");
			return 1;
		}
		static char buffer[65536];
		ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
		bigtime_t start = system_time();
		bigtime_t duration = atoi(argv[3]) * 1000000LL;
		off_t total = got > 0 ? got : 0;
		int64 datagrams = 1;
		struct timeval timeout = { 1, 0 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		while (system_time() - start < duration) {
			got = recv(fd, buffer, sizeof(buffer), 0);
			if (got <= 0)
				break;
			total += got;
			datagrams++;
		}
		bigtime_t took = system_time() - start;
		printf("%lld datagrams, %.0f/s\n", (long long)datagrams,
			datagrams / (took / 1e6));
		report("received", total, took);
		return 0;
	}
	fprintf(stderr, "usage: %s sink <port> | send <host> <port> <MiB>"
		" [interface] | udpsink <port> <seconds>\n", argv[0]);
	return 1;
}
