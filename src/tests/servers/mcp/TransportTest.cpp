/*
 * Copyright 2026 air/OS contributors. MIT license.
 */


/*!	mcp_server's HTTP transport against clients without the token
	(airos-works/haiku#198): the body of a request was read (up to 64 MiB,
	into memory) before the token was checked, a connection could be kept
	for ever by sending a byte now and then, and 32 connections from anybody
	shut everybody else out.

		MCPTransportTest
			starts a transport of its own on 127.0.0.1 (short time limits)
		MCPTransportTest --connect <host> <port> <token> [--team <id>]
			tests a running mcp_server with its real limits (minutes); with
			the server's team it also measures what the server keeps in
			memory

	First what a client with the token gets, which must stay as it is, then
	the abuse, then the valid requests again.
*/


#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <OS.h>

#include "Mcp.h"
#include "Transport.h"
#include "Util.h"


static std::string sHost = "127.0.0.1";
static int sPort = 0;
static std::string sToken;
static team_id sServerTeam = -1;
static bigtime_t sIdleTimeout = 30 * 1000000LL;
static bigtime_t sHeaderTimeout = 30 * 1000000LL;
static bigtime_t sBodyTimeout = 60 * 1000000LL;
static int sFailures = 0;

static const char* const kPing
	= "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}";


static void
report(bool ok, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	printf("%s ", ok ? "ok  " : "FAIL");
	vprintf(format, args);
	printf("\n");
	fflush(stdout);
	va_end(args);
	if (!ok)
		sFailures++;
}


static int
connect_to_server()
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons(sPort);
	inet_aton(sHost.c_str(), &address.sin_addr);
	timeval timeout = { 5, 0 };
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
	if (connect(fd, (sockaddr*)&address, sizeof(address)) != 0) {
		close(fd);
		return -1;
	}
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return fd;
}


static bool
send_all(int fd, const char* data, size_t size)
{
	size_t done = 0;
	while (done < size) {
		ssize_t sent = send(fd, data + done, size - done, 0);
		if (sent <= 0)
			return false;
		done += sent;
	}
	return true;
}


static bool
send_all(int fd, const std::string& data)
{
	return send_all(fd, data.data(), data.size());
}


/*!	Waits for the server's side to do something: 1 when there is data, 0 when
	it closed (or reset) the connection, -1 when nothing happened in time.
*/
static int
wait_for_server(int fd, bigtime_t timeout)
{
	pollfd descriptor = { fd, POLLIN, 0 };
	int ready = poll(&descriptor, 1, timeout / 1000);
	if (ready <= 0)
		return -1;
	char byte;
	ssize_t got = recv(fd, &byte, 1, MSG_PEEK);
	return got > 0 ? 1 : 0;
}


/*!	Reads one response; the status, or 0 when the server closed the
	connection, -1 when nothing came in time.
*/
static int
read_response(int fd, std::string& body, bigtime_t timeout,
	std::string* _head = NULL)
{
	bigtime_t deadline = system_time() + timeout;
	std::string buffer;
	size_t headerEnd = std::string::npos;
	size_t contentLength = 0;
	while (true) {
		if (headerEnd == std::string::npos) {
			headerEnd = buffer.find("\r\n\r\n");
			if (headerEnd != std::string::npos) {
				std::string head = buffer.substr(0, headerEnd);
				for (size_t i = 0; i < head.size(); i++)
					head[i] = tolower(head[i]);
				size_t at = head.find("content-length:");
				if (at != std::string::npos)
					contentLength = strtoul(head.c_str() + at + 15, NULL, 10);
				if (_head != NULL)
					*_head = head;
			}
		}
		if (headerEnd != std::string::npos
			&& buffer.size() >= headerEnd + 4 + contentLength) {
			break;
		}

		bigtime_t now = system_time();
		if (now >= deadline)
			return -1;
		pollfd descriptor = { fd, POLLIN, 0 };
		if (poll(&descriptor, 1, (deadline - now + 999) / 1000) <= 0)
			continue;
		char chunk[16384];
		ssize_t got = recv(fd, chunk, sizeof(chunk), 0);
		if (got <= 0)
			return 0;
		buffer.append(chunk, got);
	}
	body = buffer.substr(headerEnd + 4, contentLength);
	return atoi(buffer.c_str() + 9);
}


static std::string
request(const char* method, const char* path, const std::string& extraHeaders,
	const std::string& body, bool contentLength = true)
{
	std::string text = Format("%s %s HTTP/1.1\r\nHost: %s:%d\r\n", method,
		path, sHost.c_str(), sPort);
	text += extraHeaders;
	if (contentLength && (!body.empty() || strcmp(method, "POST") == 0)) {
		text += "Content-Type: application/json\r\n";
		text += Format("Content-Length: %zu\r\n", body.size());
	}
	text += "\r\n";
	text += body;
	return text;
}


static std::string
bearer()
{
	return "Authorization: Bearer " + sToken + "\r\n";
}


/*!	One request on a connection of its own; the status as read_response().
*/
static int
exchange(const std::string& text, std::string& body, std::string* _head = NULL)
{
	int fd = connect_to_server();
	if (fd < 0)
		return -2;
	int status = send_all(fd, text) ? read_response(fd, body, 5000000, _head)
		: 0;
	close(fd);
	return status;
}


static size_t
team_memory(team_id team)
{
	size_t total = 0;
	ssize_t cookie = 0;
	area_info info;
	while (get_next_area_info(team, &cookie, &info) == B_OK)
		total += info.ram_size;
	return total;
}


// #pragma mark - what a valid client gets


static void
test_valid(const char* when)
{
	printf("-- valid requests (%s)\n", when);
	std::string body;
	std::string head;

	int status = exchange(request("POST", "/mcp", bearer(), kPing), body);
	report(status == 200 && body.find("\"result\"") != std::string::npos,
		"POST /mcp with \"Authorization: Bearer\": %d %s", status,
		body.substr(0, 60).c_str());

	status = exchange(request("POST", "/", "X-AirOS-Token: " + sToken + "\r\n",
		kPing), body);
	report(status == 200 && body.find("\"result\"") != std::string::npos,
		"POST / with \"X-AirOS-Token\": %d", status);

	int fd = connect_to_server();
	bool ok = fd >= 0;
	for (int i = 0; ok && i < 3; i++) {
		ok = send_all(fd, request("POST", "/mcp", bearer(), kPing))
			&& read_response(fd, body, 5000000, &head) == 200
			&& head.find("connection: keep-alive") != std::string::npos;
	}
	if (fd >= 0)
		close(fd);
	report(ok, "three requests on one connection (keep-alive)");

	// a large body, in one piece and chunked
	std::string large = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\","
		"\"params\":{\"padding\":\"";
	large.append(2 * 1024 * 1024, 'x');
	large += "\"}}";
	status = exchange(request("POST", "/mcp", bearer(), large), body);
	report(status == 200 && body.find("\"id\":7") != std::string::npos,
		"POST with a body of %zu bytes: %d", large.size(), status);

	std::string chunked;
	for (size_t offset = 0; offset < large.size(); offset += 100000) {
		std::string piece = large.substr(offset, 100000);
		chunked += Format("%zx\r\n", piece.size()) + piece + "\r\n";
	}
	chunked += "0\r\n\r\n";
	status = exchange(request("POST", "/mcp",
		bearer() + "Transfer-Encoding: chunked\r\n", chunked, false), body);
	report(status == 200 && body.find("\"id\":7") != std::string::npos,
		"POST with the same body in chunks: %d", status);

	status = exchange(request("GET", "/health", "", ""), body, &head);
	report(status == 200 && body.find("\"ok\":true") != std::string::npos
			&& head.find("connection: keep-alive") != std::string::npos,
		"GET /health without the token: %d %s", status,
		TrimString(body).c_str());

	status = exchange(request("GET", "/mcp", bearer(), ""), body);
	report(status == 405, "GET /mcp with the token: %d", status);
	status = exchange(request("DELETE", "/mcp", bearer(), ""), body);
	report(status == 200, "DELETE /mcp with the token: %d", status);
	status = exchange(request("OPTIONS", "/mcp", "", ""), body);
	report(status == 204, "OPTIONS /mcp: %d", status);
	status = exchange(request("GET", "/other", "", ""), body);
	report(status == 404, "GET /other: %d", status);
	status = exchange(request("POST", "/mcp",
		bearer() + "Origin: http://example.org\r\n", kPing), body);
	report(status == 403, "POST with an Origin header: %d", status);

	// without the token, or with a wrong one: 401 as before
	status = exchange(request("POST", "/mcp", "", kPing), body, &head);
	report(status == 401
			&& body.find("bearer token required") != std::string::npos
			&& head.find("www-authenticate: bearer") != std::string::npos,
		"POST without the token: %d %s", status, TrimString(body).c_str());
	status = exchange(request("POST", "/mcp",
		"Authorization: Bearer " + std::string(sToken.size(), '0') + "\r\n",
		kPing), body);
	report(status == 401, "POST with a wrong token: %d", status);
	status = exchange(request("POST", "/mcp", "Authorization: Bearer "
		+ sToken.substr(0, sToken.size() - 1) + "\r\n", kPing), body);
	report(status == 401, "POST with the token cut short: %d", status);
	status = exchange(request("GET", "/mcp", "", ""), body, &head);
	report(status == 401
			&& head.find("connection: keep-alive") != std::string::npos,
		"GET /mcp without the token: %d", status);
}


// #pragma mark - the abuse


/*!	Without the token, the announced body must not be waited for.
*/
static void
test_token_before_body()
{
	int fd = connect_to_server();
	if (fd < 0) {
		report(false, "no connection");
		return;
	}
	std::string head = Format("POST /mcp HTTP/1.1\r\nHost: x\r\n"
		"Content-Length: %d\r\n\r\n", 32 * 1024 * 1024);
	bigtime_t start = system_time();
	send_all(fd, head);
	std::string body;
	int status = read_response(fd, body, 5000000);
	report(status == 401, "no token, \"Content-Length: 32 MiB\", no body "
		"sent: %s after %.1f s", status == 401 ? "401"
			: status < 0 ? "no answer (the server waits for the body)"
			: "connection closed", (system_time() - start) / 1e6);
	close(fd);
}


/*!	What the server keeps of bodies sent without the token.
*/
static void
test_body_memory()
{
	const int kConnections = 6;
	const size_t kBodySize = 48 * 1024 * 1024;

	size_t before = sServerTeam >= 0 ? team_memory(sServerTeam) : 0;
	std::vector<char> zeros(256 * 1024, '0');
	std::vector<int> sockets;
	size_t sentTotal = 0;
	bigtime_t start = system_time();
	for (int i = 0; i < kConnections; i++) {
		int fd = connect_to_server();
		if (fd < 0)
			continue;
		sockets.push_back(fd);
		if (!send_all(fd, Format("POST /mcp HTTP/1.1\r\nHost: x\r\n"
				"Content-Length: %zu\r\n\r\n", kBodySize))) {
			continue;
		}
		// all but the last byte, so that the server goes on waiting
		size_t sent = 0;
		while (sent < kBodySize - 1) {
			size_t size = std::min(zeros.size(), kBodySize - 1 - sent);
			ssize_t result = send(fd, &zeros[0], size, 0);
			if (result <= 0)
				break;
			sent += result;
		}
		sentTotal += sent;
	}
	snooze(500000);
	double seconds = (system_time() - start) / 1e6;

	if (sServerTeam >= 0) {
		size_t after = team_memory(sServerTeam);
		ssize_t grown = (ssize_t)after - (ssize_t)before;
		report(grown < 16 * 1024 * 1024, "%d connections without the token, "
			"%d MiB announced each: the server took %.1f of %.1f MiB offered "
			"in %.1f s, its memory grew by %.1f MiB", kConnections,
			(int)(kBodySize >> 20), sentTotal / 1048576.0,
			kConnections * (kBodySize / 1048576.0), seconds,
			grown / 1048576.0);
	} else {
		report(sentTotal < 16 * 1024 * 1024, "%d connections without the "
			"token, %d MiB announced each: the server took %.1f of %.1f MiB "
			"offered in %.1f s (its memory was not measured: no --team)",
			kConnections, (int)(kBodySize >> 20), sentTotal / 1048576.0,
			kConnections * (kBodySize / 1048576.0), seconds);
	}
	for (size_t i = 0; i < sockets.size(); i++)
		close(sockets[i]);
	snooze(300000);
}


/*!	Connections that send nothing must not keep a client with the token out.
*/
static void
test_connection_table(const char* what)
{
	const int kIdle = 40;
	std::vector<int> sockets;
	for (int i = 0; i < kIdle; i++) {
		int fd = connect_to_server();
		if (fd >= 0)
			sockets.push_back(fd);
	}
	snooze(300000);

	std::string body;
	int status = exchange(request("POST", "/mcp", bearer(), kPing), body);

	int closed = 0;
	for (size_t i = 0; i < sockets.size(); i++) {
		if (wait_for_server(sockets[i], 0) == 0)
			closed++;
		close(sockets[i]);
	}
	report(status == 200, "%d silent connections%s, then a request with the "
		"token: %s (the server had closed %d of them)", (int)sockets.size(),
		what, status == 200 ? "200" : status == 0 ? "connection closed"
			: "no answer", closed);
	snooze(300000);
}


/*!	A header that trickles in must end at the header timeout.
*/
static void
test_slow_header()
{
	int fd = connect_to_server();
	if (fd < 0) {
		report(false, "no connection");
		return;
	}
	send_all(fd, "POST /mcp HTTP/1.1\r\nX-Slow: ");
	bigtime_t start = system_time();
	bigtime_t giveUp = start + sHeaderTimeout + sIdleTimeout + 5000000;
	bool closed = false;
	while (system_time() < giveUp) {
		if (wait_for_server(fd, 500000) == 0 || send(fd, "a", 1, 0) != 1) {
			closed = true;
			break;
		}
	}
	double seconds = (system_time() - start) / 1e6;
	report(closed && seconds < sHeaderTimeout / 1e6 + 3, "a header sent one "
		"byte every 0.5 s: %s after %.1f s (limit %.0f s)",
		closed ? "closed" : "still open", seconds, sHeaderTimeout / 1e6);
	close(fd);
}


/*!	A body that trickles in, with the token: the body timeout ends it.
*/
static void
test_slow_body()
{
	int fd = connect_to_server();
	if (fd < 0) {
		report(false, "no connection");
		return;
	}
	send_all(fd, "POST /mcp HTTP/1.1\r\n" + bearer()
		+ "Content-Length: 100000\r\n\r\n");
	bigtime_t start = system_time();
	bigtime_t giveUp = start + sBodyTimeout + sIdleTimeout + 5000000;
	bool closed = false;
	while (system_time() < giveUp) {
		if (wait_for_server(fd, 500000) == 0 || send(fd, "a", 1, 0) != 1) {
			closed = true;
			break;
		}
	}
	double seconds = (system_time() - start) / 1e6;
	report(closed && seconds < sBodyTimeout / 1e6 + 6, "a body sent one byte "
		"every 0.5 s (with the token): %s after %.1f s (limit %.0f s + "
		"3 s for its size)", closed ? "closed" : "still open", seconds,
		sBodyTimeout / 1e6);
	close(fd);
}


/*!	A connection that never sends anything ends at the idle timeout.
*/
static void
test_idle()
{
	int fd = connect_to_server();
	if (fd < 0) {
		report(false, "no connection");
		return;
	}
	bigtime_t start = system_time();
	int result = wait_for_server(fd, sIdleTimeout + 5000000);
	double seconds = (system_time() - start) / 1e6;
	report(result == 0 && seconds < sIdleTimeout / 1e6 + 3, "a connection "
		"that sends nothing: %s after %.1f s (limit %.0f s)",
		result == 0 ? "closed" : "still open", seconds, sIdleTimeout / 1e6);
	close(fd);
}


static void
run_tests(bool evictionOnly)
{
	if (evictionOnly) {
		test_connection_table(" (no limit per address: the oldest makes room)");
		return;
	}
	test_valid("before the abuse");
	printf("-- requests without the token\n");
	test_token_before_body();
	test_body_memory();
	test_connection_table("");
	test_slow_header();
	test_idle();
	test_slow_body();
	test_valid("after the abuse");
}


int
main(int argc, char** argv)
{
	signal(SIGPIPE, SIG_IGN);

	if (argc >= 5 && strcmp(argv[1], "--connect") == 0) {
		sHost = argv[2];
		sPort = atoi(argv[3]);
		sToken = argv[4];
		if (argc >= 7 && strcmp(argv[5], "--team") == 0)
			sServerTeam = atoi(argv[6]);
		printf("testing the server at %s:%d (team %" B_PRId32 ")\n",
			sHost.c_str(), sPort, sServerTeam);
		run_tests(false);
	} else {
		McpServer server("transport-test", "1");
		HttpTransport transport(server);
		HttpOptions options;
		options.bindAddress = "127.0.0.1";
		options.token = "0123456789abcdef0123456789abcdef"
			"0123456789abcdef0123456789abcdef";
		options.idleTimeout = sIdleTimeout = 3 * 1000000LL;
		options.headerTimeout = sHeaderTimeout = 3 * 1000000LL;
		options.bodyTimeout = sBodyTimeout = 4 * 1000000LL;
		sToken = options.token;
		sServerTeam = getpid();

		status_t error = B_ERROR;
		for (int i = 0; i < 20 && error != B_OK; i++) {
			options.port = sPort = 20000 + (getpid() + i * 7) % 20000;
			error = transport.Start(options);
		}
		if (error != B_OK) {
			printf("FAIL the transport does not start: %s\n", strerror(error));
			return 1;
		}
		printf("testing a transport of its own at 127.0.0.1:%d\n", sPort);
		run_tests(false);

		// the same table without the limit per address
		transport.Stop();
		options.maxUnauthenticatedPerPeer = 1000;
		if (transport.Start(options) == B_OK)
			run_tests(true);
		else
			report(false, "the transport does not start again");
		transport.Stop();
	}

	printf("MCPTransportTest: %s\n", sFailures == 0 ? "PASSED" : "FAILED");
	return sFailures == 0 ? 0 : 1;
}
