/* lsend: the Linux side of nettest's "send" and "sendn" for comparisons
   through relay.py. send ADDR S: one stream for S seconds; sendn ADDR MB N:
   N uploads of MB each, timed until the receiver closes. Byte i is i % 251,
   as nettest's sink checks. Prints "NT ..." lines on the console. */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static unsigned char buf[251 * 1044];

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static int dial(const char* address)
{
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(5900);
	a.sin_addr.s_addr = inet_addr(address);
	for (int i = 0; i < 120; i++) {
		int c = socket(AF_INET, SOCK_STREAM, 0);
		if (connect(c, (struct sockaddr*)&a, sizeof(a)) == 0)
			return c;
		close(c);
		usleep(500000);
	}
	return -1;
}

static void report(int c)
{
	struct tcp_info ti;
	socklen_t l = sizeof(ti);
	if (getsockopt(c, IPPROTO_TCP, TCP_INFO, &ti, &l) == 0)
		printf("NT linux total_retrans %u rtt %u us cwnd %u\n",
			ti.tcpi_total_retrans, ti.tcpi_rtt, ti.tcpi_snd_cwnd);
}

int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (size_t j = 0; j < sizeof(buf); j++)
		buf[j] = (unsigned char)(j % 251);
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "send") && i + 2 < argc) {
			int c = dial(argv[i + 1]);
			int seconds = atoi(argv[i + 2]);
			i += 2;
			if (c < 0) { printf("NT FAIL no connection\n"); continue; }
			double start = now();
			size_t total = 0;
			while (now() - start < seconds) {
				size_t off = total % sizeof(buf);
				ssize_t n = send(c, buf + off, sizeof(buf) - off, 0);
				if (n <= 0)
					break;
				total += n;
			}
			printf("NT RATE send %.1f MB/s\n", total / (now() - start) / 1e6);
			report(c);
			close(c);
		} else if (!strcmp(argv[i], "sendn") && i + 3 < argc) {
			const char* address = argv[i + 1];
			int mb = atoi(argv[i + 2]), count = atoi(argv[i + 3]);
			i += 3;
			for (int k = 0; k < count; k++) {
				int c = dial(address);
				if (c < 0) { printf("NT FAIL no connection\n"); break; }
				size_t want = (size_t)mb << 20, total = 0;
				double start = now();
				while (total < want) {
					size_t off = total % sizeof(buf);
					size_t len = sizeof(buf) - off;
					if (len > want - total)
						len = want - total;
					ssize_t n = send(c, buf + off, len, 0);
					if (n <= 0)
						break;
					total += n;
				}
				shutdown(c, SHUT_WR);
				char tmp[64];
				while (recv(c, tmp, sizeof(tmp), 0) > 0)
					;
				double secs = now() - start;
				printf("NT SENDN %d %.3f s %.1f MB/s\n", k, secs, total / secs / 1e6);
				report(c);
				close(c);
				sleep(1);
			}
		}
	}
	return 0;
}
