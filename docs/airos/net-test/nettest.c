/*
 * nettest: guest regression tests for airos-works/haiku #121-#130.
 *
 * Most tests play a TCP peer at 10.9.0.2 behind the tunnel interface tun/0
 * (10.9.0.1/24): the peer reads the guest's packets from /dev/tun/0 and
 * writes hand-made answers, so it can send what no well-behaved stack would.
 *
 * Every test prints "PASS <name>" or "FAIL <name>: why" and never waits
 * without a deadline.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netinet6/in6.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <OS.h>

extern status_t _kern_generic_syscall(const char* subsystem, uint32 function,
	void* buffer, size_t bufferSize);

#define PEER_ADDR	"10.9.0.2"
#define LOCAL_ADDR	"10.9.0.1"
#define PEER_PORT	80

static int sTun = -1;
static int sFailures = 0;

static void
fail(const char* test, const char* fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	printf("FAIL %s: %s\n", test, buf);
	fflush(stdout);
	sFailures++;
}

static void
pass(const char* test, const char* fmt, ...)
{
	char buf[512] = "";
	if (fmt != NULL) {
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
	}
	printf("PASS %s%s%s\n", test, buf[0] ? ": " : "", buf);
	fflush(stdout);
}

static bigtime_t
now_ms(void)
{
	return system_time() / 1000;
}

/* ---- checksums ---- */

static uint32_t
sum16(const void* data, size_t length, uint32_t sum)
{
	const uint8_t* p = (const uint8_t*)data;
	while (length > 1) {
		sum += (p[0] << 8) | p[1];
		p += 2;
		length -= 2;
	}
	if (length)
		sum += p[0] << 8;
	return sum;
}

static uint16_t
fold(uint32_t sum)
{
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return htons(~sum & 0xffff);
}

/* ---- the peer ---- */

struct tcp_hdr {
	uint16_t sport, dport;
	uint32_t seq, ack;
	uint8_t off, flags;
	uint16_t win, sum, urg;
} __attribute__((packed));

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

struct seg {
	uint32_t src, dst;
	uint8_t proto;
	uint16_t ipid;
	uint16_t sport, dport;
	uint32_t seq, ack;
	uint8_t flags;
	uint16_t win;
	int mss;
	int has_ts;
	uint32_t tsval, tsecr;
	int len;			/* payload */
	uint8_t data[2048];
};

struct peer {
	uint16_t guest_port;
	uint32_t guest_next;	/* next sequence expected from the guest */
	uint32_t seq;			/* our next sequence */
	uint32_t ts_recent;
	uint32_t tsval;
	int with_ts;
};

static int
tun_read(uint8_t* buf, size_t size, int timeout_ms)
{
	struct pollfd p = { sTun, POLLIN, 0 };
	int r = poll(&p, 1, timeout_ms);
	if (r <= 0)
		return 0;
	ssize_t n = read(sTun, buf, size);
	return n > 0 ? (int)n : 0;
}

/* reads the next IPv4 packet from the guest; returns 0 on timeout */
static int
read_seg(struct seg* s, int timeout_ms)
{
	uint8_t buf[4096];
	bigtime_t end = now_ms() + timeout_ms;
	for (;;) {
		int left = (int)(end - now_ms());
		if (left <= 0)
			return 0;
		int n = tun_read(buf, sizeof(buf), left);
		if (n <= 0)
			return 0;
		if ((buf[0] >> 4) != 4)
			continue;
		int ihl = (buf[0] & 0xf) * 4;
		memset(s, 0, sizeof(*s));
		memcpy(&s->src, buf + 12, 4);
		memcpy(&s->dst, buf + 16, 4);
		s->proto = buf[9];
		s->ipid = (buf[4] << 8) | buf[5];
		int total = (buf[2] << 8) | buf[3];
		if (total > n)
			total = n;
		if (s->proto != IPPROTO_TCP) {
			s->len = total - ihl;
			continue;
		}
		struct tcp_hdr* t = (struct tcp_hdr*)(buf + ihl);
		int thl = (t->off >> 4) * 4;
		s->sport = ntohs(t->sport);
		s->dport = ntohs(t->dport);
		s->seq = ntohl(t->seq);
		s->ack = ntohl(t->ack);
		s->flags = t->flags;
		s->win = ntohs(t->win);
		s->mss = -1;
		uint8_t* o = buf + ihl + sizeof(struct tcp_hdr);
		uint8_t* oend = buf + ihl + thl;
		while (o < oend) {
			if (o[0] == 0)
				break;
			if (o[0] == 1) {
				o++;
				continue;
			}
			if (o + 1 >= oend || o[1] < 2)
				break;
			if (o[0] == 2 && o[1] == 4)
				s->mss = (o[2] << 8) | o[3];
			if (o[0] == 8 && o[1] == 10) {
				s->has_ts = 1;
				s->tsval = ntohl(*(uint32_t*)(o + 2));
				s->tsecr = ntohl(*(uint32_t*)(o + 6));
			}
			o += o[1];
		}
		s->len = total - ihl - thl;
		if (s->len > (int)sizeof(s->data))
			s->len = sizeof(s->data);
		if (s->len > 0)
			memcpy(s->data, buf + ihl + thl, s->len);
		return 1;
	}
}

static uint16_t sPeerIPID = 1;
static int sPeerSackPermitted = 0;
static uint32_t sSackBlocks[3][2];
static int sSackCount = 0;
static uint16_t sPeerWindow = 65535;

static void
write_ip(uint32_t src, uint32_t dst, uint8_t proto, const uint8_t* payload,
	int length)
{
	uint8_t buf[4096];
	memset(buf, 0, 20);
	buf[0] = 0x45;
	int total = 20 + length;
	buf[2] = total >> 8;
	buf[3] = total & 0xff;
	buf[4] = sPeerIPID >> 8;
	buf[5] = sPeerIPID & 0xff;
	sPeerIPID++;
	buf[8] = 64;
	buf[9] = proto;
	memcpy(buf + 12, &src, 4);
	memcpy(buf + 16, &dst, 4);
	uint16_t csum = fold(sum16(buf, 20, 0));
	memcpy(buf + 10, &csum, 2);
	memcpy(buf + 20, payload, length);
	if (write(sTun, buf, total) != total)
		printf("tun write failed: %s\n", strerror(errno));
}

static void
send_tcp(struct peer* p, uint8_t flags, uint32_t seq, uint32_t ack,
	int mss, const void* data, int length)
{
	uint8_t buf[2048];
	struct tcp_hdr* t = (struct tcp_hdr*)buf;
	int olen = 0;
	uint8_t* o = buf + sizeof(*t);
	if (mss > 0) {
		o[olen++] = 2; o[olen++] = 4;
		o[olen++] = mss >> 8; o[olen++] = mss & 0xff;
	}
	if (mss > 0 && sPeerSackPermitted) {
		o[olen++] = 1; o[olen++] = 1;
		o[olen++] = 4; o[olen++] = 2;
	}
	if (p->with_ts) {
		o[olen++] = 1; o[olen++] = 1;
		o[olen++] = 8; o[olen++] = 10;
		uint32_t v = htonl(p->tsval++);
		memcpy(o + olen, &v, 4); olen += 4;
		v = htonl(p->ts_recent);
		memcpy(o + olen, &v, 4); olen += 4;
	}
	if (sSackCount > 0 && (flags & F_SYN) == 0) {
		o[olen++] = 1; o[olen++] = 1;
		o[olen++] = 5; o[olen++] = 2 + 8 * sSackCount;
		for (int i = 0; i < sSackCount; i++) {
			uint32_t v = htonl(sSackBlocks[i][0]);
			memcpy(o + olen, &v, 4); olen += 4;
			v = htonl(sSackBlocks[i][1]);
			memcpy(o + olen, &v, 4); olen += 4;
		}
	}
	memset(t, 0, sizeof(*t));
	t->sport = htons(PEER_PORT);
	t->dport = htons(p->guest_port);
	t->seq = htonl(seq);
	t->ack = htonl(ack);
	t->off = ((sizeof(*t) + olen) / 4) << 4;
	t->flags = flags;
	t->win = htons(sPeerWindow);
	int hl = sizeof(*t) + olen;
	if (length > 0)
		memcpy(buf + hl, data, length);

	uint32_t src = inet_addr(PEER_ADDR), dst = inet_addr(LOCAL_ADDR);
	uint8_t pseudo[12];
	memcpy(pseudo, &src, 4);
	memcpy(pseudo + 4, &dst, 4);
	pseudo[8] = 0;
	pseudo[9] = IPPROTO_TCP;
	pseudo[10] = (hl + length) >> 8;
	pseudo[11] = (hl + length) & 0xff;
	uint32_t sum = sum16(pseudo, 12, 0);
	sum = sum16(buf, hl + length, sum);
	t->sum = fold(sum);
	write_ip(src, dst, IPPROTO_TCP, buf, hl + length);
}

/* ---- the guest side ---- */

static int
guest_connect_start(uint16_t* _port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	int flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = AF_INET;
	a.sin_port = htons(PEER_PORT);
	a.sin_addr.s_addr = inet_addr(PEER_ADDR);
	connect(fd, (struct sockaddr*)&a, sizeof(a));
	return fd;
}

/* the guest connects, the peer answers with \a mss; returns the socket */
static int
handshake(const char* test, struct peer* p, int mss, int with_ts)
{
	memset(p, 0, sizeof(*p));
	p->with_ts = with_ts;
	p->tsval = 1000;
	p->seq = 0x10000000;
	int fd = guest_connect_start(NULL);
	struct seg s;
	bigtime_t end = now_ms() + 3000;
	while (now_ms() < end) {
		if (!read_seg(&s, 500))
			continue;
		if (s.proto == IPPROTO_TCP && (s.flags & F_SYN) && !(s.flags & F_ACK)
			&& s.dport == PEER_PORT) {
			p->guest_port = s.sport;
			p->guest_next = s.seq + 1;
			p->ts_recent = s.tsval;
			send_tcp(p, F_SYN | F_ACK, p->seq, p->guest_next, mss, NULL, 0);
			p->seq++;
			/* wait for the ACK */
			while (now_ms() < end) {
				if (!read_seg(&s, 500))
					continue;
				if (s.proto == IPPROTO_TCP && s.sport == p->guest_port
					&& (s.flags & F_ACK) && !(s.flags & F_SYN)) {
					if (s.has_ts)
						p->ts_recent = s.tsval;
					/* connect() completes */
					int flags = fcntl(fd, F_GETFL);
					fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
					return fd;
				}
			}
		}
	}
	fail(test, "no handshake");
	close(fd);
	return -1;
}

static void
set_timeout(int fd, int ms)
{
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* ---- #128: peer MSS 12 ---- */

static void
test_mss(int mss)
{
	char name[32];
	snprintf(name, sizeof(name), "mss%d", mss);
	struct peer p;
	int fd = handshake(name, &p, mss, 1);
	if (fd < 0)
		return;

	char data[2000];
	memset(data, 'x', sizeof(data));
	set_timeout(fd, 3000);
	if (send(fd, data, sizeof(data), 0) != (ssize_t)sizeof(data)) {
		fail(name, "send: %s", strerror(errno));
		close(fd);
		return;
	}

	int segments = 0, empty = 0, maxLength = 0;
	uint32_t received = 0;
	struct seg s;
	bigtime_t end = now_ms() + 4000;
	while (now_ms() < end && received < sizeof(data)) {
		if (!read_seg(&s, 200))
			continue;
		if (s.proto != IPPROTO_TCP || s.sport != p.guest_port)
			continue;
		segments++;
		if (s.len == 0)
			empty++;
		if (s.len > maxLength)
			maxLength = s.len;
		if (s.has_ts)
			p.ts_recent = s.tsval;
		if (s.len > 0 && s.seq == p.guest_next) {
			p.guest_next += s.len;
			received += s.len;
			send_tcp(&p, F_ACK, p.seq, p.guest_next, 0, NULL, 0);
		}
		if (empty > 5000)
			break;
	}
	if (empty > 1000)
		fail(name, "%d empty segments (endless loop), %u of %zu bytes",
			empty, received, sizeof(data));
	else if (received < sizeof(data))
		fail(name, "%u of %zu bytes arrived (%d segments)", received,
			sizeof(data), segments);
	else
		pass(name, "%u bytes in %d segments of up to %d bytes", received,
			segments, maxLength);

	send_tcp(&p, F_RST | F_ACK, p.seq, p.guest_next, 0, NULL, 0);
	close(fd);
}

/* ---- #127: RFC 5961 ---- */

static int
wait_ack(struct peer* p, int timeout_ms, uint32_t* _ack)
{
	struct seg s;
	bigtime_t end = now_ms() + timeout_ms;
	while (now_ms() < end) {
		if (!read_seg(&s, (int)(end - now_ms())))
			return 0;
		if (s.proto == IPPROTO_TCP && s.sport == p->guest_port
			&& (s.flags & F_ACK) && !(s.flags & F_RST)) {
			if (s.has_ts)
				p->ts_recent = s.tsval;
			if (_ack)
				*_ack = s.ack;
			return 1;
		}
	}
	return 0;
}

static void
drain(int ms)
{
	struct seg s;
	bigtime_t end = now_ms() + ms;
	while (now_ms() < end)
		read_seg(&s, (int)(end - now_ms()));
}

static int
recv_some(int fd, char* buf, int size, int timeout_ms)
{
	set_timeout(fd, timeout_ms);
	ssize_t n = recv(fd, buf, size, 0);
	if (n < 0) {
		/* Haiku's errno values are negative */
		return errno == ECONNRESET ? -2 : -1;
	}
	return (int)n;
}

static void
test_rfc5961(void)
{
	const char* name = "rfc5961";
	struct peer p;
	int fd = handshake(name, &p, 1460, 1);
	if (fd < 0)
		return;

	char buf[64];
	send_tcp(&p, F_ACK | F_PSH, p.seq, p.guest_next, 0, "hello", 5);
	p.seq += 5;
	int n = recv_some(fd, buf, sizeof(buf), 1000);
	if (n != 5) {
		fail(name, "normal data not received (%d)", n);
		close(fd);
		return;
	}
	drain(100);

	/* 1. an in-window reset that is not exact */
	send_tcp(&p, F_RST | F_ACK, p.seq + 1000, p.guest_next, 0, NULL, 0);
	int challenged = wait_ack(&p, 500, NULL);
	n = send(fd, "ping", 4, 0);
	uint32_t ack;
	struct seg s;
	int alive = 0;
	bigtime_t end = now_ms() + 1000;
	while (now_ms() < end) {
		if (!read_seg(&s, 200))
			continue;
		if (s.proto == IPPROTO_TCP && s.sport == p.guest_port && s.len == 4) {
			alive = 1;
			p.guest_next += 4;
			if (s.has_ts)
				p.ts_recent = s.tsval;
			send_tcp(&p, F_ACK, p.seq, p.guest_next, 0, NULL, 0);
			break;
		}
		if (s.proto == IPPROTO_TCP && (s.flags & F_RST))
			break;
	}
	if (!alive || n != 4)
		fail(name, "in-window reset killed the connection");
	else if (!challenged)
		fail(name, "in-window reset survived but got no challenge ACK");
	else
		pass("rfc5961-rst", "in-window reset answered with a challenge ACK");
	if (!alive) {
		close(fd);
		return;
	}
	drain(150);

	/* 2. a SYN on the established connection */
	send_tcp(&p, F_SYN, 0x55555555, 0, 1460, NULL, 0);
	challenged = wait_ack(&p, 500, &ack);
	n = send(fd, "pong", 4, 0);
	alive = 0;
	end = now_ms() + 1000;
	while (now_ms() < end) {
		if (!read_seg(&s, 200))
			continue;
		if (s.proto == IPPROTO_TCP && s.sport == p.guest_port && s.len == 4) {
			alive = 1;
			p.guest_next += 4;
			if (s.has_ts)
				p.ts_recent = s.tsval;
			send_tcp(&p, F_ACK, p.seq, p.guest_next, 0, NULL, 0);
			break;
		}
		if (s.proto == IPPROTO_TCP && (s.flags & F_RST))
			break;
	}
	if (!alive)
		fail(name, "SYN on an established connection killed it");
	else if (!challenged)
		fail(name, "SYN survived but got no challenge ACK");
	else
		pass("rfc5961-syn", "SYN answered with a challenge ACK");
	if (!alive) {
		close(fd);
		return;
	}
	drain(150);

	/* 3. data without the ACK flag, and data acknowledging unsent data */
	send_tcp(&p, F_PSH, p.seq, 0, 0, "evil", 4);
	send_tcp(&p, F_ACK | F_PSH, p.seq, p.guest_next + 100000, 0, "bad2", 4);
	n = recv_some(fd, buf, sizeof(buf), 500);
	if (n > 0)
		fail(name, "segment without a valid ACK delivered %d bytes (%.4s)", n,
			buf);
	else
		pass("rfc5961-ack", "data without a valid ACK dropped");
	drain(150);

	/* 4. an exact reset still resets */
	send_tcp(&p, F_RST | F_ACK, p.seq, p.guest_next, 0, NULL, 0);
	n = recv_some(fd, buf, sizeof(buf), 1000);
	if (n == -2 || n == 0)
		pass("rfc5961-exact", "exact reset resets (%s)", n == 0 ? "EOF"
			: "ECONNRESET");
	else
		fail(name, "exact reset did not reset (recv %d)", n);
	close(fd);
}

/* ---- #127: forged ICMP "fragmentation needed" ---- */

static void
test_icmp_case(int inFlight)
{
	const char* name = inFlight ? "icmp-mtu-valid" : "icmp-mtu";
	struct peer p;
	int fd = handshake(name, &p, 1460, 1);
	if (fd < 0)
		return;

	/* forge an error for a sequence number that is not in flight */
	uint8_t icmp[8 + 20 + 8];
	memset(icmp, 0, sizeof(icmp));
	icmp[0] = 3;
	icmp[1] = 4;
	icmp[6] = 0;
	icmp[7] = 100;	/* next-hop MTU 100 */
	uint8_t* ip = icmp + 8;
	ip[0] = 0x45;
	ip[3] = 60;
	ip[8] = 64;
	ip[9] = IPPROTO_TCP;
	uint32_t src = inet_addr(LOCAL_ADDR), dst = inet_addr(PEER_ADDR);
	memcpy(ip + 12, &src, 4);
	memcpy(ip + 16, &dst, 4);
	uint16_t csum = fold(sum16(ip, 20, 0));
	memcpy(ip + 10, &csum, 2);
	uint8_t* th = ip + 20;
	uint16_t sp = htons(p.guest_port), dp = htons(PEER_PORT);
	memcpy(th, &sp, 2);
	memcpy(th + 2, &dp, 2);
	if (inFlight) {
		/* put data in flight that the peer does not acknowledge */
		char pending[100];
		memset(pending, 'z', sizeof(pending));
		send(fd, pending, sizeof(pending), 0);
		struct seg q;
		bigtime_t qend = now_ms() + 1000;
		while (now_ms() < qend) {
			if (read_seg(&q, 200) && q.proto == IPPROTO_TCP
				&& q.sport == p.guest_port && q.len > 0)
				break;
		}
	}
	uint32_t badseq = htonl(inFlight ? p.guest_next : p.guest_next + 50000);
	memcpy(th + 4, &badseq, 4);
	csum = fold(sum16(icmp, sizeof(icmp), 0));
	memcpy(icmp + 2, &csum, 2);
	write_ip(dst, src, IPPROTO_ICMP, icmp, sizeof(icmp));
	usleep(100000);

	char data[4000];
	memset(data, 'y', sizeof(data));
	set_timeout(fd, 3000);
	send(fd, data, sizeof(data), 0);
	int maxLength = 0, minLength = 100000;
	uint32_t received = 0;
	struct seg s;
	bigtime_t end = now_ms() + 3000;
	while (now_ms() < end && received < sizeof(data)) {
		if (!read_seg(&s, 200))
			continue;
		if (s.proto != IPPROTO_TCP || s.sport != p.guest_port || s.len == 0)
			continue;
		if (s.has_ts)
			p.ts_recent = s.tsval;
		if (s.len > maxLength)
			maxLength = s.len;
		if (s.len < minLength)
			minLength = s.len;
		if (s.seq == p.guest_next) {
			p.guest_next += s.len;
			received += s.len;
		}
		send_tcp(&p, F_ACK, p.seq, p.guest_next, 0, NULL, 0);
	}
	if (received < sizeof(data))
		fail(name, "only %u bytes arrived", received);
	else if (inFlight)
		printf("  %s: segments of up to %d bytes after a valid error\n", name,
			maxLength);
	else if (maxLength < 1000)
		fail(name, "forged ICMP error shrank segments to %d bytes", maxLength);
	else
		pass(name, "segments stay at %d bytes", maxLength);
	send_tcp(&p, F_RST | F_ACK, p.seq, p.guest_next, 0, NULL, 0);
	close(fd);
}


static void
test_icmp(void)
{
	test_icmp_case(0);
	test_icmp_case(1);
}

/* ---- #127: sequence numbers, ports, IP IDs ---- */

static void
test_predictable(void)
{
	const char* name = "predictable";
	uint32_t isn[8], ipid[8];
	uint16_t port[8];
	bigtime_t when[8];
	int count = 0;
	for (int i = 0; i < 8; i++) {
		int fd = guest_connect_start(NULL);
		struct seg s;
		bigtime_t end = now_ms() + 1000;
		while (now_ms() < end) {
			if (!read_seg(&s, 300))
				continue;
			if (s.proto == IPPROTO_TCP && (s.flags & F_SYN)) {
				isn[count] = s.seq;
				port[count] = s.sport;
				ipid[count] = s.ipid;
				when[count] = system_time();
				count++;
				struct peer p;
				memset(&p, 0, sizeof(p));
				p.guest_port = s.sport;
				send_tcp(&p, F_RST | F_ACK, 0, s.seq + 1, 0, NULL, 0);
				break;
			}
		}
		close(fd);
		usleep(2000);
	}
	if (count < 4) {
		fail(name, "only %d SYNs seen", count);
		return;
	}
	int clockLike = 0, smallSteps = 0, idSteps = 0;
	for (int i = 1; i < count; i++) {
		double dt = (when[i] - when[i - 1]) / 1e6;
		int32_t d = (int32_t)(isn[i] - isn[i - 1]);
		double rate = d / dt;
		printf("  SYN port %u isn %u ipid %u (+%d in %.1f ms, %.0f/s)\n",
			port[i], isn[i], ipid[i], d, dt * 1000, rate);
		if (d > 0 && rate < 1e6)
			clockLike++;
		int dp = (int)port[i] - (int)port[i - 1];
		if (dp > 0 && dp <= 32)
			smallSteps++;
		int di = (int)((ipid[i] - ipid[i - 1]) & 0xffff);
		if (di > 0 && di < 64)
			idSteps++;
	}
	if (clockLike >= count / 2)
		fail("predictable-isn", "%d of %d ISN steps follow a clock", clockLike,
			count - 1);
	else
		pass("predictable-isn", NULL);
	if (smallSteps >= count / 2)
		fail("predictable-port", "%d of %d ports are a small step up",
			smallSteps, count - 1);
	else
		pass("predictable-port", NULL);
	/* IP IDs to the same peer may count up; this is informational */
	printf("  ip id small steps to one peer: %d of %d\n", idSteps, count - 1);
}

/* ---- #128: IPv6 extension headers ---- */

static int
udp6_echo_works(void)
{
	int s = socket(AF_INET6, SOCK_DGRAM, 0);
	struct sockaddr_in6 a;
	memset(&a, 0, sizeof(a));
	a.sin6_len = sizeof(a);
	a.sin6_family = AF_INET6;
	a.sin6_addr = in6addr_loopback;
	a.sin6_port = htons(5556);
	if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
		printf("  udp6 bind: %s\n", strerror(errno));
		close(s);
		return 0;
	}
	if (sendto(s, "ok", 2, 0, (struct sockaddr*)&a, sizeof(a)) != 2)
		printf("  udp6 sendto: %s\n", strerror(errno));
	char buf[8];
	set_timeout(s, 1000);
	ssize_t n = recv(s, buf, sizeof(buf), 0);
	if (n != 2)
		printf("  udp6 recv: %s\n", strerror(errno));
	close(s);
	return n == 2;
}

static void
write_ip6(const uint8_t* src, const uint8_t* dst, uint8_t next,
	const uint8_t* payload, int length)
{
	uint8_t buf[2048];
	memset(buf, 0, 40);
	buf[0] = 0x60;
	buf[4] = length >> 8;
	buf[5] = length & 0xff;
	buf[6] = next;
	buf[7] = 64;
	memcpy(buf + 8, src, 16);
	memcpy(buf + 24, dst, 16);
	memcpy(buf + 40, payload, length);
	if (write(sTun, buf, 40 + length) != 40 + length)
		printf("tun write (IPv6) failed: %s\n", strerror(errno));
}

static const uint8_t kGuest6[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 1 };
static const uint8_t kPeer6[16] = { 0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 2 };

/* a destination options header followed by UDP to port 5555 */
static void
send_udp6_behind_options(const char* text)
{
	uint8_t pkt[8 + 8 + 16];
	int textLength = strlen(text);
	memset(pkt, 0, sizeof(pkt));
	pkt[0] = IPPROTO_UDP;
	pkt[1] = 0;
	pkt[2] = 1;		/* PadN */
	pkt[3] = 4;
	uint8_t* udp = pkt + 8;
	int udpLength = 8 + textLength;
	udp[0] = 0x15; udp[1] = 0xb4;	/* source 5556 */
	udp[2] = 0x15; udp[3] = 0xb3;	/* destination 5555 */
	udp[4] = 0; udp[5] = udpLength;
	memcpy(udp + 8, text, textLength);
	uint8_t pseudo[40];
	memset(pseudo, 0, sizeof(pseudo));
	memcpy(pseudo, kPeer6, 16);
	memcpy(pseudo + 16, kGuest6, 16);
	pseudo[35] = udpLength;
	pseudo[39] = IPPROTO_UDP;
	uint32_t sum = sum16(pseudo, 40, 0);
	sum = sum16(udp, udpLength, sum);
	uint16_t c = fold(sum);
	memcpy(udp + 6, &c, 2);
	write_ip6(kPeer6, kGuest6, IPPROTO_DSTOPTS, pkt, 8 + udpLength);
}

static void
test_ipv6_ext(void)
{
	const char* name = "ipv6-ext";
	system("ifconfig tun/0 inet6 fd00::1 prefixlen 64");
	usleep(300000);

	int u = socket(AF_INET6, SOCK_DGRAM, 0);
	struct sockaddr_in6 ua;
	memset(&ua, 0, sizeof(ua));
	ua.sin6_len = sizeof(ua);
	ua.sin6_family = AF_INET6;
	memcpy(&ua.sin6_addr, kGuest6, 16);
	ua.sin6_port = htons(5555);
	if (bind(u, (struct sockaddr*)&ua, sizeof(ua)) != 0) {
		fail(name, "bind [fd00::1]:5555: %s", strerror(errno));
		close(u);
		return;
	}
	char buf[16];
	set_timeout(u, 1000);

	send_udp6_behind_options("hi");
	ssize_t n = recv(u, buf, sizeof(buf), 0);
	if (n == 2 && memcmp(buf, "hi", 2) == 0)
		pass("ipv6-ext-valid", "UDP behind a destination options header arrives");
	else
		fail("ipv6-ext-valid", "UDP behind a destination options header lost");

	/* two zero-length destination options headers in a row */
	uint8_t evil[16];
	memset(evil, 0, sizeof(evil));
	evil[0] = IPPROTO_DSTOPTS;
	evil[2] = 1; evil[3] = 4;
	evil[8] = IPPROTO_DSTOPTS;
	evil[10] = 1; evil[11] = 4;
	write_ip6(kPeer6, kGuest6, IPPROTO_DSTOPTS, evil, sizeof(evil));
	usleep(200000);

	send_udp6_behind_options("again");
	n = recv(u, buf, sizeof(buf), 0);
	if (n == 5)
		pass(name, "the interface still receives after a zero-length header chain");
	else
		fail(name, "the interface receives nothing after a zero-length header chain");
	close(u);
}

/* ---- #130: ioctls ---- */

static void
test_ioctl_pointers(void)
{
	const char* name = "ioctl-pointer";
	int s = socket(AF_INET, SOCK_DGRAM, 0);
	char ok[4096];
	struct ifconf conf;
	conf.ifc_len = sizeof(ok);
	conf.ifc_buf = ok;
	if (ioctl(s, SIOCGIFCONF, &conf, sizeof(conf)) != 0 || conf.ifc_len <= 0) {
		fail(name, "SIOCGIFCONF with a user buffer failed: %s",
			strerror(errno));
		close(s);
		return;
	}
	/* an address in the kernel's half: never writable from user space */
	conf.ifc_len = 4096;
	conf.ifc_buf = (char*)(uintptr_t)0xffffffff80000000ULL;
	int r1 = ioctl(s, SIOCGIFCONF, &conf, sizeof(conf));
	int e1 = errno;
	conf.ifc_len = 4096;
	conf.ifc_buf = (char*)(uintptr_t)0xffffffff80000000ULL;
	int r2 = ioctl(s, SIOCGRTTABLE, &conf, sizeof(conf));
	int e2 = errno;
	if (r1 == 0 || r2 == 0)
		fail(name, "kernel address accepted (SIOCGIFCONF %d, SIOCGRTTABLE %d)",
			r1, r2);
	else
		pass(name, "kernel addresses refused (%s, %s)", strerror(e1),
			strerror(e2));
	close(s);
}

struct arp_control {
	in_addr_t	address;
	uint8		ethernet_address[6];
	uint32		flags;
	uint32		cookie;
};

static void
test_privileges(void)
{
	pid_t child = fork();
	if (child == 0) {
		if (setgid(100) != 0 || setuid(1000) != 0) {
			printf("FAIL privileges: setuid: %s\n", strerror(errno));
			_exit(1);
		}
		int allowed = 0;
		int s = socket(AF_INET, SOCK_DGRAM, 0);

		struct ifreq request;
		memset(&request, 0, sizeof(request));
		strcpy(request.ifr_name, "loop");
		if (ioctl(s, SIOCGIFFLAGS, &request, sizeof(request)) != 0)
			printf("  reading flags failed: %s\n", strerror(errno));
		request.ifr_flags &= ~IFF_UP;
		if (ioctl(s, SIOCSIFFLAGS, &request, sizeof(request)) == 0) {
			printf("  user could take loop down\n");
			request.ifr_flags |= IFF_UP;
			ioctl(s, SIOCSIFFLAGS, &request, sizeof(request));
			allowed++;
		}

		struct ifreq mtu;
		memset(&mtu, 0, sizeof(mtu));
		strcpy(mtu.ifr_name, "tun/0");
		mtu.ifr_mtu = 1000;
		if (ioctl(s, SIOCSIFMTU, &mtu, sizeof(mtu)) == 0) {
			printf("  user could set the MTU\n");
			allowed++;
		}

		int raw6 = socket(AF_INET6, SOCK_RAW, IPPROTO_DSTOPTS);
		if (raw6 >= 0) {
			printf("  user could open a raw IPv6 socket\n");
			allowed++;
			close(raw6);
		}

		int u = socket(AF_INET, SOCK_DGRAM, 0);
		struct sockaddr_in a;
		memset(&a, 0, sizeof(a));
		a.sin_len = sizeof(a);
		a.sin_family = AF_INET;
		a.sin_port = htons(53);
		if (bind(u, (struct sockaddr*)&a, sizeof(a)) == 0) {
			printf("  user could bind UDP port 53\n");
			allowed++;
		}
		close(u);

		struct arp_control control;
		memset(&control, 0, sizeof(control));
		control.address = inet_addr("10.9.0.77");
		control.ethernet_address[0] = 2;
		if (_kern_generic_syscall("network/arp", 1, &control,
				sizeof(control)) == B_OK) {
			printf("  user could set an ARP entry\n");
			allowed++;
		}

		/* reading must still work */
		char buf[4096];
		struct ifconf conf;
		conf.ifc_len = sizeof(buf);
		conf.ifc_buf = buf;
		if (ioctl(s, SIOCGIFCONF, &conf, sizeof(conf)) != 0) {
			printf("  user could not list interfaces: %s\n", strerror(errno));
			allowed += 100;
		}
		close(s);
		_exit(allowed);
	}
	int status = 0;
	waitpid(child, &status, 0);
	int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	if (code == 0)
		pass("privileges", "a user other than root cannot change the network");
	else
		fail("privileges", "%d operation(s) allowed or broken (%d)", code % 100,
			code);
}

/* ---- #121: TIME_WAIT sockets and the timer thread ---- */

static int
loopback_listener(uint16_t port)
{
	int l = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(l, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(l, 256) != 0) {
		printf("  listen %u: %s\n", port, strerror(errno));
		close(l);
		return -1;
	}
	return l;
}

static int
loopback_connect(uint16_t port)
{
	int c = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(c, (struct sockaddr*)&a, sizeof(a)) != 0) {
		close(c);
		return -1;
	}
	return c;
}

struct sink_args {
	int fd;
	size_t total;
};

static void*
sink_thread(void* _args)
{
	struct sink_args* args = (struct sink_args*)_args;
	static char buf[65536];
	ssize_t n;
	while ((n = recv(args->fd, buf, sizeof(buf), 0)) > 0)
		args->total += n;
	return NULL;
}

/* MB/s of a loopback transfer lasting about \a ms */
static double
loopback_rate(int ms)
{
	int l = loopback_listener(5600);
	if (l < 0)
		return 0;
	int c = loopback_connect(5600);
	int a = accept(l, NULL, NULL);
	close(l);
	if (c < 0 || a < 0)
		return 0;
	struct sink_args args = { a, 0 };
	pthread_t thread;
	pthread_create(&thread, NULL, sink_thread, &args);
	static char buf[65536];
	memset(buf, 'r', sizeof(buf));
	bigtime_t start = system_time();
	size_t sent = 0;
	set_timeout(c, 5000);
	while (system_time() - start < ms * 1000LL) {
		ssize_t n = send(c, buf, sizeof(buf), 0);
		if (n <= 0)
			break;
		sent += n;
	}
	close(c);
	pthread_join(thread, NULL);
	bigtime_t elapsed = system_time() - start;
	close(a);
	return args.total / (elapsed / 1e6) / 1e6;
}

static void
test_timewait(int count)
{
	const char* name = "timewait";
	double before = loopback_rate(2000);

	int l = loopback_listener(5601);
	if (l < 0) {
		fail(name, "no listener");
		return;
	}
	bigtime_t start = system_time();
	int made = 0;
	for (int i = 0; i < count; i++) {
		int c = loopback_connect(5601);
		if (c < 0)
			break;
		int a = accept(l, NULL, NULL);
		/* the client closes first, so its side goes to TIME_WAIT */
		close(c);
		if (a >= 0)
			close(a);
		made++;
	}
	close(l);
	double connectTime = (system_time() - start) / 1e6;
	usleep(500000);

	double after = loopback_rate(2000);
	printf("  %d connections in %.1f s; loopback %.0f MB/s before, %.0f MB/s "
		"after\n", made, connectTime, before, after);
	if (made < count)
		fail(name, "only %d of %d connections", made, count);
	else if (after < before * 0.5)
		fail(name, "%d sockets in TIME_WAIT cut loopback from %.0f to %.0f "
			"MB/s", made, before, after);
	else
		pass(name, "%.0f -> %.0f MB/s with %d sockets in TIME_WAIT", before,
			after, made);
}

/* ---- #126 / #129: UDP receive queue ---- */

static void
send_udp4(uint16_t dport, const void* data, int length)
{
	uint8_t buf[2048];
	uint8_t* u = buf;
	int ulen = 8 + length;
	u[0] = 0x15; u[1] = 0xb4;	/* 5556 */
	u[2] = dport >> 8; u[3] = dport & 0xff;
	u[4] = ulen >> 8; u[5] = ulen & 0xff;
	u[6] = 0; u[7] = 0;		/* no checksum */
	if (length > 0)
		memcpy(u + 8, data, length);
	uint32_t src = inet_addr(PEER_ADDR), dst = inet_addr(LOCAL_ADDR);
	/* the tunnel's queue holds 32 packets: retry while it is full */
	for (int tries = 0; tries < 1000; tries++) {
		uint8_t ip[2048];
		memset(ip, 0, 20);
		ip[0] = 0x45;
		int total = 20 + ulen;
		ip[2] = total >> 8; ip[3] = total & 0xff;
		ip[8] = 64; ip[9] = IPPROTO_UDP;
		memcpy(ip + 12, &src, 4);
		memcpy(ip + 16, &dst, 4);
		uint16_t csum = fold(sum16(ip, 20, 0));
		memcpy(ip + 10, &csum, 2);
		memcpy(ip + 20, u, ulen);
		if (write(sTun, ip, total) == total)
			return;
		usleep(100);
	}
}

static int
udp_socket(uint16_t port)
{
	int u = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = inet_addr(LOCAL_ADDR);
	bind(u, (struct sockaddr*)&a, sizeof(a));
	return u;
}

static int
udp_drain(int u)
{
	char buf[2048];
	int count = 0;
	while (recv(u, buf, sizeof(buf), MSG_DONTWAIT) >= 0)
		count++;
	return count;
}

static void
test_udp_burst(void)
{
	const char* name = "udp-burst";
	int u = udp_socket(5700);
	char payload[1472];
	memset(payload, 'u', sizeof(payload));
	const int total = 400;
	for (int i = 0; i < total; i++)
		send_udp4(5700, payload, sizeof(payload));
	usleep(300000);
	int got = udp_drain(u);
	close(u);
	printf("  %d of %d full-size datagrams queued for a reader that came late\n",
		got, total);
	if (got < 300)
		fail(name, "only %d of %d datagrams of a burst were kept", got, total);
	else
		pass(name, "%d of %d datagrams of a burst kept", got, total);
}

static void
test_udp_empty(void)
{
	const char* name = "udp-empty";
	int u = udp_socket(5701);
	const int total = 20000;
	for (int i = 0; i < total; i++)
		send_udp4(5701, NULL, 0);
	usleep(300000);
	int got = udp_drain(u);
	close(u);
	printf("  %d of %d empty datagrams queued on a socket nobody reads\n", got,
		total);
	if (got > 2000)
		fail(name, "%d empty datagrams queued without limit", got);
	else
		pass(name, "queue stopped at %d empty datagrams", got);
	system("netstat -s 2>&1 | grep -A 5 '^udp'");
}

/* ---- #129: SYN flood, fragment flood ---- */

/* a TCP segment from any address and port to the guest */
static void
send_tcp_raw(uint32_t src, uint16_t sport, uint16_t dport, uint8_t flags,
	uint32_t seq, uint32_t ack, int mss, const void* data, int length)
{
	uint8_t buf[2048];
	struct tcp_hdr* t = (struct tcp_hdr*)buf;
	int olen = 0;
	uint8_t* o = buf + sizeof(*t);
	if (mss > 0) {
		o[olen++] = 2; o[olen++] = 4;
		o[olen++] = mss >> 8; o[olen++] = mss & 0xff;
	}
	memset(t, 0, sizeof(*t));
	t->sport = htons(sport);
	t->dport = htons(dport);
	t->seq = htonl(seq);
	t->ack = htonl(ack);
	t->off = ((sizeof(*t) + olen) / 4) << 4;
	t->flags = flags;
	t->win = htons(65535);
	int hl = sizeof(*t) + olen;
	if (length > 0)
		memcpy(buf + hl, data, length);
	uint32_t dst = inet_addr(LOCAL_ADDR);
	uint8_t pseudo[12];
	memcpy(pseudo, &src, 4);
	memcpy(pseudo + 4, &dst, 4);
	pseudo[8] = 0;
	pseudo[9] = IPPROTO_TCP;
	pseudo[10] = (hl + length) >> 8;
	pseudo[11] = (hl + length) & 0xff;
	uint32_t sum = sum16(pseudo, 12, 0);
	sum = sum16(buf, hl + length, sum);
	t->sum = fold(sum);
	write_ip(src, dst, IPPROTO_TCP, buf, hl + length);
}

static void
test_syn_flood(void)
{
	const char* name = "syn-flood";
	int l = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = AF_INET;
	a.sin_port = htons(7000);
	a.sin_addr.s_addr = inet_addr(LOCAL_ADDR);
	if (bind(l, (struct sockaddr*)&a, sizeof(a)) != 0 || listen(l, 4) != 0) {
		fail(name, "listen: %s", strerror(errno));
		close(l);
		return;
	}

	/* 100 SYNs from forged addresses that never answer */
	for (int i = 0; i < 100; i++) {
		uint32_t src = htonl(ntohl(inet_addr("10.9.0.100")) + i);
		send_tcp_raw(src, 20000 + i, 7000, F_SYN, 0x1000 * i, 0, 1460, NULL, 0);
	}
	drain(300);

	/* now a real client */
	uint32_t client = inet_addr(PEER_ADDR);
	uint32_t iss = 0x77777777;
	send_tcp_raw(client, 4444, 7000, F_SYN, iss, 0, 1460, NULL, 0);
	struct seg sg;
	int gotSynAck = 0;
	uint32_t serverSeq = 0;
	bigtime_t end = now_ms() + 2000;
	while (now_ms() < end) {
		if (!read_seg(&sg, 200))
			continue;
		if (sg.proto == IPPROTO_TCP && sg.dport == 4444
			&& (sg.flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK)) {
			gotSynAck = 1;
			serverSeq = sg.seq;
			break;
		}
	}
	if (!gotSynAck) {
		fail(name, "a real client got no SYN-ACK while the queue was full of "
			"forged SYNs");
		close(l);
		return;
	}
	send_tcp_raw(client, 4444, 7000, F_ACK | F_PSH, iss + 1, serverSeq + 1, 0,
		"hello", 5);

	int flags = fcntl(l, F_GETFL);
	fcntl(l, F_SETFL, flags | O_NONBLOCK);
	int c = -1;
	end = now_ms() + 2000;
	while (now_ms() < end && c < 0) {
		/* the queue may hold forged half-open connections first */
		c = accept(l, NULL, NULL);
		if (c >= 0) {
			struct sockaddr_in peer;
			socklen_t len = sizeof(peer);
			getpeername(c, (struct sockaddr*)&peer, &len);
			if (peer.sin_addr.s_addr != client) {
				close(c);
				c = -1;
			}
		} else
			usleep(20000);
	}
	if (c < 0) {
		fail(name, "the real client's connection was never accepted");
		close(l);
		return;
	}
	char buf[16];
	int n = recv_some(c, buf, sizeof(buf), 1000);
	if (n == 5 && memcmp(buf, "hello", 5) == 0)
		pass(name, "a real client got through a full queue (SYN cookie)");
	else
		fail(name, "accepted, but the data did not arrive (%d)", n);
	send_tcp_raw(client, 4444, 7000, F_RST | F_ACK, iss + 6, serverSeq + 1, 0,
		NULL, 0);
	close(c);
	close(l);
	system("netstat -s 2>&1 | grep -i cookie");
}

static void
send_fragment(uint32_t src, uint16_t id, int offset, int more,
	const uint8_t* data, int length)
{
	uint8_t ip[2048];
	memset(ip, 0, 20);
	ip[0] = 0x45;
	int total = 20 + length;
	ip[2] = total >> 8; ip[3] = total & 0xff;
	ip[4] = id >> 8; ip[5] = id & 0xff;
	uint16_t frag = (offset / 8) | (more ? 0x2000 : 0);
	ip[6] = frag >> 8; ip[7] = frag & 0xff;
	ip[8] = 64; ip[9] = IPPROTO_UDP;
	uint32_t dst = inet_addr(LOCAL_ADDR);
	memcpy(ip + 12, &src, 4);
	memcpy(ip + 16, &dst, 4);
	uint16_t csum = fold(sum16(ip, 20, 0));
	memcpy(ip + 10, &csum, 2);
	memcpy(ip + 20, data, length);
	for (int tries = 0; tries < 1000; tries++) {
		if (write(sTun, ip, total) == total)
			return;
		usleep(100);
	}
}

struct ip_stats {
	uint64_t v[64];
};

static void
test_fragment_flood(void)
{
	const char* name = "frag-flood";
	/* 2,000 first fragments that are never completed */
	uint8_t junk[1000];
	memset(junk, 'f', sizeof(junk));
	for (int i = 0; i < 2000; i++) {
		uint32_t src = htonl(ntohl(inet_addr("10.9.0.100")) + (i % 100));
		send_fragment(src, 1000 + i, 0, 1, junk, sizeof(junk));
	}
	drain(300);

	/* a real fragmented datagram still arrives */
	int u = udp_socket(5800);
	uint8_t data[8 + 1600];
	memset(data, 'd', sizeof(data));
	data[0] = 0x15; data[1] = 0xb4;
	data[2] = 5800 >> 8; data[3] = 5800 & 0xff;
	data[4] = sizeof(data) >> 8; data[5] = sizeof(data) & 0xff;
	data[6] = 0; data[7] = 0;
	uint32_t client = inet_addr(PEER_ADDR);
	send_fragment(client, 7777, 0, 1, data, 1000);
	send_fragment(client, 7777, 1000, 0, data + 1000, sizeof(data) - 1000);
	char buf[2048];
	set_timeout(u, 1000);
	ssize_t n = recv(u, buf, sizeof(buf), 0);
	close(u);

	struct ip_stats stats;
	memset(&stats, 0, sizeof(stats));
	status_t status = _kern_generic_syscall("network/stats", 1, &stats,
		sizeof(stats));
	/* ip_reassembly_dropped is the 6th counter */
	uint64_t dropped = status == B_OK ? stats.v[5] : 0;
	printf("  reassembly: %llu packets dropped by the limits\n",
		(unsigned long long)dropped);
	if (n != 1600)
		fail(name, "a real fragmented datagram was lost (%zd)", n);
	else if (dropped < 1700)
		fail(name, "2,000 incomplete packets kept (only %llu dropped)",
			(unsigned long long)dropped);
	else
		pass(name, "incomplete packets capped, a real one still reassembled");
}

/* ---- #129: out-of-order one-byte segments ---- */

static void
test_out_of_order(void)
{
	const char* name = "out-of-order";
	struct peer p;
	int fd = handshake(name, &p, 1460, 1);
	if (fd < 0)
		return;

	/* plain reordering: second half first */
	send_tcp(&p, F_ACK, p.seq + 5, p.guest_next, 0, "world", 5);
	send_tcp(&p, F_ACK | F_PSH, p.seq, p.guest_next, 0, "hello", 5);
	char buf[64];
	int n = recv_some(fd, buf, 10, 1000);
	if (n != 10 || memcmp(buf, "helloworld", 10) != 0) {
		fail(name, "reordered data broken (%d)", n);
		close(fd);
		return;
	}
	p.seq += 10;

	/* 5,000 one-byte segments, each after a gap */
	for (int i = 0; i < 5000; i++)
		send_tcp(&p, F_ACK, p.seq + 100 + 2 * i, p.guest_next, 0, "x", 1);
	drain(300);

	/* in-order data still arrives */
	send_tcp(&p, F_ACK | F_PSH, p.seq, p.guest_next, 0, "again", 5);
	n = recv_some(fd, buf, 5, 1000);
	if (n == 5 && memcmp(buf, "again", 5) == 0)
		pass(name, "reordering works; in-order data arrives after a flood of "
			"one-byte segments");
	else
		fail(name, "in-order data lost after the flood (%d)", n);
	send_tcp(&p, F_RST | F_ACK, p.seq + 10, p.guest_next, 0, NULL, 0);
	close(fd);
}

/* ---- #125: loss recovery ---- */

struct send_args {
	int fd;
	int total;
};

static void*
sender_thread(void* _args)
{
	struct send_args* args = (struct send_args*)_args;
	static char buf[16384];
	memset(buf, 's', sizeof(buf));
	int left = args->total;
	while (left > 0) {
		ssize_t n = send(args->fd, buf, left < (int)sizeof(buf) ? left
			: (int)sizeof(buf), 0);
		if (n <= 0)
			break;
		left -= n;
	}
	return NULL;
}

static void
test_recovery(void)
{
	const char* name = "recovery";
	struct peer p;
	int fd = handshake(name, &p, 1460, 1);
	if (fd < 0)
		return;
	int size = 1024 * 1024;
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
	set_timeout(fd, 10000);

	const int total = 600 * 1024;
	struct send_args args = { fd, total };
	pthread_t thread;
	pthread_create(&thread, NULL, sender_thread, &args);

	uint32_t base = p.guest_next;
	uint32_t highest = base;
	int segments = 0, retransmits = 0, lostOnce = 0;
	uint32_t lostSeq = 0;
	struct seg sg;
	bigtime_t end = now_ms() + 15000;
	while (now_ms() < end && p.guest_next - base < (uint32_t)total) {
		if (!read_seg(&sg, 300))
			continue;
		if (sg.proto != IPPROTO_TCP || sg.sport != p.guest_port || sg.len == 0)
			continue;
		if (sg.has_ts)
			p.ts_recent = sg.tsval;
		segments++;
		if ((int32_t)(sg.seq - highest) < 0) {
			retransmits++;
			printf("  resent seq +%u len %d (highest +%u, acked +%u, "
				"segment %d)\n", sg.seq - base, sg.len, highest - base,
				p.guest_next - base, segments);
		} else
			highest = sg.seq + sg.len;

		if (!lostOnce && segments == 40) {
			/* lose this segment once */
			lostOnce = 1;
			lostSeq = sg.seq;
			continue;
		}
		if (sg.seq == p.guest_next) {
			p.guest_next += sg.len;
			/* a hole filled: everything after it that arrived counts too */
			if (lostOnce && sg.seq == lostSeq)
				p.guest_next = highest;
		}
		/* ACK what we have (duplicate ACKs while the hole is open) */
		send_tcp(&p, F_ACK, p.seq, p.guest_next, 0, NULL, 0);
	}
	uint32_t delivered = p.guest_next - base;
	send_tcp(&p, F_RST | F_ACK, p.seq, p.guest_next, 0, NULL, 0);
	close(fd);
	pthread_join(thread, NULL);
	printf("  %u of %d bytes, %d segments, %d retransmitted\n", delivered, total,
		segments, retransmits);
	if (delivered < (uint32_t)total)
		fail(name, "only %u of %d bytes after one loss (%d retransmissions)",
			delivered, total, retransmits);
	else if (retransmits > 5)
		fail(name, "one loss caused %d retransmissions", retransmits);
	else
		pass(name, "one loss repaired with %d retransmission(s)", retransmits);
}

/* After a retransmit timeout, slow start resends from the first hole. One
   duplicate ACK in between (limited transmit) used to move the send point
   to the end of what was sent and leave it there: nothing more went out
   until the next timeout (airos-works/haiku#125). */
static void
test_rto_resend(void)
{
	const char* name = "rto-resend";
	struct peer p;
	int fd = handshake(name, &p, 1460, 1);
	if (fd < 0)
		return;
	char data[60000];
	memset(data, 'r', sizeof(data));
	send(fd, data, sizeof(data), 0);

	/* the first flight: lose all of it */
	uint32_t base = p.guest_next, highest = base;
	struct seg sg;
	bigtime_t end = now_ms() + 50;
	while (now_ms() < end) {
		if (read_seg(&sg, 10) && sg.proto == IPPROTO_TCP && sg.len > 0) {
			if (sg.has_ts)
				p.ts_recent = sg.tsval;
			if ((int32_t)(sg.seq + sg.len - highest) > 0)
				highest = sg.seq + sg.len;
		}
	}
	/* the timeout resends the first segment */
	int seen = 0;
	end = now_ms() + 3000;
	while (now_ms() < end && !seen) {
		if (read_seg(&sg, 50) && sg.proto == IPPROTO_TCP && sg.len > 0
			&& sg.seq == base) {
			if (sg.has_ts)
				p.ts_recent = sg.tsval;
			seen = 1;
		}
	}
	if (!seen) {
		fail(name, "no retransmission after a timeout");
		close(fd);
		return;
	}
	/* acknowledge it, then repeat that ACK once (a duplicate) */
	p.guest_next = base + sg.len;
	send_tcp(&p, F_ACK, p.seq, p.guest_next, 0, NULL, 0);
	usleep(2000);
	send_tcp(&p, F_ACK, p.seq, p.guest_next, 0, NULL, 0);

	/* acknowledge every resent segment in order: slow start should go on
	   resending well before the next timeout (200 ms) */
	int resent = 0;
	end = now_ms() + 150;
	while (now_ms() < end) {
		if (!read_seg(&sg, 10) || sg.proto != IPPROTO_TCP || sg.len == 0)
			continue;
		if (sg.has_ts)
			p.ts_recent = sg.tsval;
		if (sg.seq == p.guest_next && (int32_t)(sg.seq - highest) < 0) {
			resent++;
			p.guest_next += sg.len;
			send_tcp(&p, F_ACK, p.seq, p.guest_next, 0, NULL, 0);
		}
	}
	send_tcp(&p, F_RST | F_ACK, p.seq, p.guest_next, 0, NULL, 0);
	close(fd);
	if (resent < 8)
		fail(name, "only %d segment(s) resent in 150 ms after a duplicate ACK",
			resent);
	else
		pass(name, "%d segments resent in 150 ms after a duplicate ACK", resent);
}

/* The last, short segment of a stream carries the FIN and is lost. After
   the retransmit timeout, slow start (which resends full segments only)
   took the FIN's sequence number for more data and never resent it: the
   connection hung (#125). */
static void
test_fin_loss(void)
{
	const char* name = "fin-loss";
	struct peer p;
	int fd = handshake(name, &p, 1460, 1);
	if (fd < 0)
		return;
	static char data[10000];
	memset(data, 'f', sizeof(data));
	send(fd, data, sizeof(data), 0);
	close(fd);

	uint32_t base = p.guest_next;
	int finDropped = 0, finAcked = 0;
	struct seg sg;
	bigtime_t end = now_ms() + 5000;
	while (now_ms() < end && !finAcked) {
		if (!read_seg(&sg, 20) || sg.proto != IPPROTO_TCP
			|| sg.sport != p.guest_port)
			continue;
		if (sg.has_ts)
			p.ts_recent = sg.tsval;
		if ((sg.flags & F_FIN) != 0 && !finDropped) {
			finDropped = 1;
			continue;
		}
		if (sg.seq == p.guest_next) {
			p.guest_next += sg.len;
			if ((sg.flags & F_FIN) != 0) {
				p.guest_next++;
				finAcked = 1;
			}
		}
		send_tcp(&p, F_ACK, p.seq, p.guest_next, 0, NULL, 0);
	}
	send_tcp(&p, F_RST | F_ACK, p.seq, p.guest_next, 0, NULL, 0);
	if (!finDropped)
		fail(name, "no FIN seen");
	else if (!finAcked)
		fail(name, "the lost last segment and FIN were not resent in 5 s "
			"(%u of 10000 bytes)", p.guest_next - base);
	else
		pass(name, "the lost last segment and FIN were resent");
}

static void
test_window_update(void)
{
	const char* name = "window-update";
	struct peer p;
	int fd = handshake(name, &p, 1460, 1);
	if (fd < 0)
		return;
	char data[8000];
	memset(data, 'w', sizeof(data));
	send(fd, data, sizeof(data), 0);
	/* let the data go out, acknowledge none of it */
	struct seg sg;
	uint32_t highest = p.guest_next;
	/* everything here stays well inside the 200 ms retransmit timeout */
	bigtime_t end = now_ms() + 40;
	while (now_ms() < end) {
		if (read_seg(&sg, 10) && sg.proto == IPPROTO_TCP && sg.len > 0) {
			if (sg.has_ts)
				p.ts_recent = sg.tsval;
			if ((int32_t)(sg.seq + sg.len - highest) > 0)
				highest = sg.seq + sg.len;
		}
	}
	/* three window updates: same ACK, a different window each time */
	for (int i = 0; i < 4; i++) {
		sPeerWindow = 60000 - i * 1000;
		send_tcp(&p, F_ACK, p.seq, p.guest_next, 0, NULL, 0);
		usleep(3000);
	}
	sPeerWindow = 65535;
	int retransmits = 0;
	end = now_ms() + 60;
		/* well below the retransmit timeout */
	while (now_ms() < end) {
		if (read_seg(&sg, 10) && sg.proto == IPPROTO_TCP && sg.len > 0
			&& (int32_t)(sg.seq - highest) < 0)
			retransmits++;
	}
	send_tcp(&p, F_RST | F_ACK, p.seq, p.guest_next, 0, NULL, 0);
	close(fd);
	if (retransmits > 0)
		fail(name, "window updates triggered a fast retransmit");
	else
		pass(name, "window updates are not duplicate ACKs");
}

static void
test_persist(void)
{
	const char* name = "persist";
	struct peer p;
	int fd = handshake(name, &p, 1460, 1);
	if (fd < 0)
		return;
	/* close the window */
	sPeerWindow = 0;
	send_tcp(&p, F_ACK, p.seq, p.guest_next, 0, NULL, 0);
	usleep(100000);
	char data[2000];
	memset(data, 'p', sizeof(data));
	send(fd, data, sizeof(data), 0);
	/* never open it: count the probes */
	int probes = 0;
	struct seg sg;
	bigtime_t end = now_ms() + 8500;
	while (now_ms() < end) {
		if (read_seg(&sg, 200) && sg.proto == IPPROTO_TCP
			&& sg.sport == p.guest_port)
			probes++;
	}
	sPeerWindow = 65535;
	send_tcp(&p, F_RST | F_ACK, p.seq, p.guest_next, 0, NULL, 0);
	close(fd);
	if (probes >= 3)
		pass(name, "%d window probes in 8.5 s", probes);
	else
		fail(name, "only %d window probe(s) in 8.5 s", probes);
}

/* ---- throughput between two guests ---- */

static void
tcp_sink(int seconds)
{
	int l = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = AF_INET;
	a.sin_port = htons(5900);
	bind(l, (struct sockaddr*)&a, sizeof(a));
	listen(l, 4);
	printf("sink ready\n");
	for (int round = 0; round < 3; round++) {
		int c = accept(l, NULL, NULL);
		if (c < 0)
			break;
		static unsigned char buf[262144];
		size_t total = 0, bad = 0;
		bigtime_t start = 0;
		ssize_t n;
		while ((n = recv(c, buf, sizeof(buf), 0)) > 0) {
			if (start == 0)
				start = system_time();
			/* the sender writes byte i of the stream as i % 251 */
			for (ssize_t j = 0; j < n; j++) {
				if (buf[j] != (unsigned char)((total + j) % 251))
					bad++;
			}
			total += n;
		}
		double secs = (system_time() - start) / 1e6;
		printf("RATE receive %.1f MB/s (%zu bytes in %.2f s)\n",
			total / secs / 1e6, total, secs);
		if (bad != 0)
			printf("FAIL stream: %zu of %zu bytes wrong\n", bad, total);
		else
			printf("PASS stream: all %zu bytes in order\n", total);
		close(c);
	}
	close(l);
	(void)seconds;
}

static int sSendBufferSize = 0;

static void
tcp_send_to(const char* address, int seconds)
{
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_len = sizeof(a);
	a.sin_family = AF_INET;
	a.sin_port = htons(5900);
	a.sin_addr.s_addr = inet_addr(address);
	int c = -1;
	for (int i = 0; i < 60; i++) {
		c = socket(AF_INET, SOCK_STREAM, 0);
		if (connect(c, (struct sockaddr*)&a, sizeof(a)) == 0)
			break;
		close(c);
		c = -1;
		usleep(500000);
	}
	if (c < 0) {
		printf("FAIL send: no connection to %s\n", address);
		return;
	}
	if (sSendBufferSize > 0) {
		setsockopt(c, SOL_SOCKET, SO_SNDBUF, &sSendBufferSize,
			sizeof(sSendBufferSize));
		printf("send buffer %d\n", sSendBufferSize);
	}
	/* byte i of the stream is i % 251; a buffer of 251 * 1044 bytes
	   keeps the pattern continuous from one send to the next */
	static unsigned char buf[251 * 1044];
	for (size_t j = 0; j < sizeof(buf); j++)
		buf[j] = (unsigned char)(j % 251);
	bigtime_t start = system_time();
	size_t total = 0;
	while (system_time() - start < seconds * 1000000LL) {
		size_t offset = total % sizeof(buf);
		ssize_t n = send(c, buf + offset, sizeof(buf) - offset, 0);
		if (n <= 0)
			break;
		total += n;
	}
	double secs = (system_time() - start) / 1e6;
	printf("RATE send %.1f MB/s\n", total / secs / 1e6);
	close(c);
}

/* ---- #125: SACK-based recovery ---- */

struct delayed_ack {
	bigtime_t due;
	uint32_t ack;
	uint32_t blocks[3][2];
	int count;
};

/* The last two segments of a write are lost: no duplicate ACKs follow, so
   only the retransmit timeout (200 ms or more) repaired them, unless a tail
   loss probe (RFC 8985) asks after two round trips (#125). 20 ms path. */
static void
test_tail_loss(void)
{
	const char* name = "tail-loss";
	struct peer p;
	sPeerSackPermitted = 1;
	int fd = handshake(name, &p, 1460, 1);
	if (fd < 0) {
		sPeerSackPermitted = 0;
		return;
	}
	set_timeout(fd, 10000);

	static struct delayed_ack queue[1024];
	int head = 0, tail = 0;
	const bigtime_t kDelay = 10000;
		/* each way: a 20 ms round trip */
	uint32_t base = p.guest_next;
	const int kSegments = 20;
	const uint32_t total = kSegments * 1448;
	uint32_t rangeStart = 0, rangeEnd = 0;
	int dropped = 0, retransmits = 0, round = 0;
	bigtime_t lossTime = 0, repaired = 0;
	uint32_t highest = base;
	struct seg sg;
	static char data[20 * 1448];
	memset(data, 't', sizeof(data));

	/* a few rounds first, so that the sender has a round trip estimate */
	for (round = 0; round < 4 && repaired == 0; round++) {
		int last = round == 3;
		uint32_t roundBase = p.guest_next;
		send(fd, data, total, 0);
		bigtime_t end = now_ms() + 3000;
		while (now_ms() < end && p.guest_next - roundBase < total) {
			while (head != tail && queue[head].due <= system_time()) {
				sSackCount = queue[head].count;
				memcpy(sSackBlocks, queue[head].blocks, sizeof(sSackBlocks));
				send_tcp(&p, F_ACK, p.seq, queue[head].ack, 0, NULL, 0);
				head = (head + 1) % 1024;
			}
			sSackCount = 0;
			if (!read_seg(&sg, 1) || sg.proto != IPPROTO_TCP
				|| sg.sport != p.guest_port || sg.len == 0)
				continue;
			if (sg.has_ts)
				p.ts_recent = sg.tsval;
			int isRetransmit = (int32_t)(sg.seq - highest) < 0;
			if (isRetransmit)
				retransmits++;
			else
				highest = sg.seq + sg.len;
			if (last && !isRetransmit
				&& (int32_t)(sg.seq - (roundBase + total - 2 * 1448)) >= 0) {
				dropped++;
				lossTime = system_time();
				continue;
			}
			uint32_t s0 = sg.seq, s1 = sg.seq + sg.len;
			if (s0 == p.guest_next) {
				p.guest_next = s1;
				if (rangeEnd != rangeStart
					&& (int32_t)(rangeStart - p.guest_next) <= 0) {
					if ((int32_t)(rangeEnd - p.guest_next) > 0)
						p.guest_next = rangeEnd;
					rangeStart = rangeEnd = 0;
				}
			} else if ((int32_t)(s0 - p.guest_next) > 0) {
				if (rangeStart == rangeEnd) {
					rangeStart = s0;
					rangeEnd = s1;
				} else {
					if ((int32_t)(s0 - rangeStart) < 0)
						rangeStart = s0;
					if ((int32_t)(s1 - rangeEnd) > 0)
						rangeEnd = s1;
				}
			}
			struct delayed_ack* a = &queue[tail];
			a->due = system_time() + kDelay;
			a->ack = p.guest_next;
			a->count = rangeStart != rangeEnd ? 1 : 0;
			a->blocks[0][0] = rangeStart;
			a->blocks[0][1] = rangeEnd;
			tail = (tail + 1) % 1024;
		}
		if (last && p.guest_next - roundBase >= total)
			repaired = system_time();
	}
	send_tcp(&p, F_RST | F_ACK, p.seq, p.guest_next, 0, NULL, 0);
	close(fd);
	sPeerSackPermitted = 0;
	(void)base;
	if (dropped != 2 || repaired == 0) {
		fail(name, "the tail was not repaired (%d dropped, %d resent)",
			dropped, retransmits);
		return;
	}
	bigtime_t took = (repaired - lossTime) / 1000;
	if (took > 150)
		fail(name, "two lost tail segments took %lld ms to repair",
			(long long)took);
	else
		pass(name, "two lost tail segments repaired in %lld ms", (long long)took);
}


static int sSackMode = 0;
	/* 0: three holes in one window; 1: one hole whose first resend is lost */

static void
test_sack(void)
{
	const char* name = sSackMode == 1 ? "lost-resend" : "sack";
	uint32_t lostSeq = 0, lostLength = 0;
	int resendDropped = 0;
	bigtime_t holeFilled = 0;
	struct peer p;
	sPeerSackPermitted = 1;
	int fd = handshake(name, &p, 1460, 1);
	if (fd < 0) {
		sPeerSackPermitted = 0;
		return;
	}
	int size = 1024 * 1024;
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
	set_timeout(fd, 10000);

	const int total = 600 * 1024;
	struct send_args args = { fd, total };
	pthread_t thread;
	pthread_create(&thread, NULL, sender_thread, &args);

	/* ACKs go out 30 ms after the segment that caused them: a 30 ms path */
	static struct delayed_ack queue[4096];
	int head = 0, tail = 0;
	const bigtime_t kDelay = 30000;

	uint32_t ranges[64][2];
	int rangeCount = 0;
	uint32_t base = p.guest_next;
	uint32_t highest = base;
	int segments = 0, retransmits = 0, dropped = 0;
	bigtime_t firstRetransmit = 0, lastRetransmit = 0, firstLoss = 0;
	struct seg sg;
	bigtime_t end = now_ms() + 20000;
	while (now_ms() < end && p.guest_next - base < (uint32_t)total) {
		/* send the ACKs that are due */
		while (head != tail && queue[head].due <= system_time()) {
			sSackCount = queue[head].count;
			memcpy(sSackBlocks, queue[head].blocks, sizeof(sSackBlocks));
			send_tcp(&p, F_ACK, p.seq, queue[head].ack, 0, NULL, 0);
			head = (head + 1) % 4096;
		}
		sSackCount = 0;

		if (!read_seg(&sg, 2))
			continue;
		if (sg.proto != IPPROTO_TCP || sg.sport != p.guest_port || sg.len == 0)
			continue;
		if (sg.has_ts)
			p.ts_recent = sg.tsval;
		segments++;
		int isRetransmit = (int32_t)(sg.seq - highest) < 0;
		if (isRetransmit && firstLoss != 0) {
			/* (a tail loss probe before the losses is not a repair) */
			retransmits++;
			if (firstRetransmit == 0)
				firstRetransmit = system_time();
			lastRetransmit = system_time();
		} else if (!isRetransmit)
			highest = sg.seq + sg.len;

		if (sSackMode == 1) {
			/* lose one segment, and its first resend too */
			if (!isRetransmit && segments == 15) {
				firstLoss = system_time();
				lostSeq = sg.seq;
				lostLength = sg.len;
				dropped++;
				continue;
			}
			if (isRetransmit && sg.seq == lostSeq && !resendDropped) {
				resendDropped = 1;
				continue;
			}
		} else if (!isRetransmit && (segments == 40 || segments == 43
				|| segments == 46)) {
			if (dropped == 0)
				firstLoss = system_time();
			dropped++;
			continue;
		}

		uint32_t s0 = sg.seq, s1 = sg.seq + sg.len;
		if (lostLength != 0 && holeFilled == 0 && s0 == lostSeq
			&& s0 == p.guest_next)
			holeFilled = system_time();
		if (s0 == p.guest_next) {
			p.guest_next = s1;
			int merged = 1;
			while (merged) {
				merged = 0;
				for (int i = 0; i < rangeCount; i++) {
					if ((int32_t)(ranges[i][0] - p.guest_next) <= 0
						&& (int32_t)(ranges[i][1] - p.guest_next) > 0) {
						p.guest_next = ranges[i][1];
						merged = 1;
					}
				}
			}
			for (int i = 0; i < rangeCount; i++) {
				if ((int32_t)(ranges[i][1] - p.guest_next) <= 0) {
					ranges[i][0] = ranges[rangeCount - 1][0];
					ranges[i][1] = ranges[rangeCount - 1][1];
					rangeCount--;
					i--;
				}
			}
		} else if ((int32_t)(s0 - p.guest_next) > 0) {
			int found = 0;
			for (int i = 0; i < rangeCount; i++) {
				if (ranges[i][1] == s0) {
					ranges[i][1] = s1;
					found = 1;
					break;
				}
				if (ranges[i][0] == s1) {
					ranges[i][0] = s0;
					found = 1;
					break;
				}
			}
			if (!found && rangeCount < 64) {
				ranges[rangeCount][0] = s0;
				ranges[rangeCount][1] = s1;
				rangeCount++;
			}
		}

		/* queue the ACK with up to three ranges, the highest first */
		struct delayed_ack* a = &queue[tail];
		a->due = system_time() + kDelay;
		a->ack = p.guest_next;
		a->count = 0;
		for (int n = 0; n < 3 && n < rangeCount; n++) {
			int best = -1;
			for (int j = 0; j < rangeCount; j++) {
				int used = 0;
				for (int k = 0; k < a->count; k++)
					used |= a->blocks[k][0] == ranges[j][0];
				if (!used && (best < 0
						|| (int32_t)(ranges[j][0] - ranges[best][0]) > 0))
					best = j;
			}
			a->blocks[a->count][0] = ranges[best][0];
			a->blocks[a->count][1] = ranges[best][1];
			a->count++;
		}
		tail = (tail + 1) % 4096;
	}
	sSackCount = 0;
	sPeerSackPermitted = 0;
	uint32_t delivered = p.guest_next - base;
	send_tcp(&p, F_RST | F_ACK, p.seq, p.guest_next, 0, NULL, 0);
	close(fd);
	pthread_join(thread, NULL);
	double spread = (lastRetransmit - firstRetransmit) / 1000.0;
	printf("  %u of %d bytes, %d dropped, %d retransmitted within %.0f ms "
		"(30 ms round trip), first %.0f ms after the first loss\n", delivered,
		total, dropped, retransmits, spread,
		(firstRetransmit - firstLoss) / 1000.0);
	if (sSackMode == 1) {
		bigtime_t took = holeFilled != 0 ? (holeFilled - firstLoss) / 1000 : -1;
		if (delivered < (uint32_t)total)
			fail(name, "only %u of %d bytes arrived", delivered, total);
		else if (!resendDropped || took < 0)
			fail(name, "the resend was not seen");
		else if (took > 150)
			fail(name, "a lost resend took %lld ms to repair (the timeout)",
				(long long)took);
		else
			pass(name, "a lost resend repaired %lld ms after the loss",
				(long long)took);
		return;
	}
	if (delivered < (uint32_t)total)
		fail(name, "only %u of %d bytes arrived", delivered, total);
	else if (retransmits > dropped + 2)
		fail(name, "%d losses caused %d retransmissions", dropped, retransmits);
	else if (spread > 25)
		fail(name, "the %d holes took %.0f ms (a round trip each)", dropped,
			spread);
	else
		pass(name, "%d holes repaired with %d retransmissions in %.0f ms",
			dropped, retransmits, spread);
}

/* ---- main ---- */

static int
setup_tun(void)
{
	if (sTun >= 0)
		return 0;
	if (system("ifconfig tun/0 inet " LOCAL_ADDR " netmask 255.255.255.0 up")
			!= 0) {
		printf("ifconfig tun/0 failed\n");
	}
	sTun = open("/dev/tun/0", O_RDWR);
	if (sTun < 0) {
		printf("FAIL setup: /dev/tun/0: %s\n", strerror(errno));
		return -1;
	}
	drain(300);
	return 0;
}

int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 1; i < argc; i++) {
		const char* t = argv[i];
		if (!strcmp(t, "sink"))
			tcp_sink(atoi(argv[++i]));
		else if (!strcmp(t, "sndbuf"))
			sSendBufferSize = atoi(argv[++i]);
		else if (!strcmp(t, "send")) {
			tcp_send_to(argv[i + 1], atoi(argv[i + 2]));
			i += 2;
		} else if (!strcmp(t, "timewait"))
			test_timewait(20000);
		else if (!strcmp(t, "ioctl"))
			test_ioctl_pointers();
		else if (!strcmp(t, "priv")) {
			if (setup_tun() == 0)
				test_privileges();
		} else if (setup_tun() != 0)
			return 1;
		else if (!strcmp(t, "mss12"))
			test_mss(12);
		else if (!strcmp(t, "mss1460"))
			test_mss(1460);
		else if (!strcmp(t, "rfc5961"))
			test_rfc5961();
		else if (!strcmp(t, "icmp"))
			test_icmp();
		else if (!strcmp(t, "predictable"))
			test_predictable();
		else if (!strcmp(t, "sack"))
			test_sack();
		else if (!strcmp(t, "recovery"))
			test_recovery();
		else if (!strcmp(t, "lostresend")) {
			sSackMode = 1;
			test_sack();
			sSackMode = 0;
		} else if (!strcmp(t, "finloss"))
			test_fin_loss();
		else if (!strcmp(t, "tailloss"))
			test_tail_loss();
		else if (!strcmp(t, "rtoresend"))
			test_rto_resend();
		else if (!strcmp(t, "winupdate"))
			test_window_update();
		else if (!strcmp(t, "persist"))
			test_persist();
		else if (!strcmp(t, "ooo"))
			test_out_of_order();
		else if (!strcmp(t, "synflood"))
			test_syn_flood();
		else if (!strcmp(t, "fragflood"))
			test_fragment_flood();
		else if (!strcmp(t, "udpburst"))
			test_udp_burst();
		else if (!strcmp(t, "udpempty"))
			test_udp_empty();
		else if (!strcmp(t, "ipv6ext"))
			test_ipv6_ext();
		else
			printf("unknown test %s\n", t);
	}
	printf("nettest done, %d failure(s)\n", sFailures);
	return sFailures != 0;
}
