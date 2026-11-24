/*
 * airos_mcp - stdio and streamable HTTP transports.
 * Copyright 2026 air/OS contributors. MIT license.
 */
#include "Transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>

#include <Autolock.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

#include <algorithm>
#include <map>

#include "Mcp.h"
#include "Util.h"


void
RunStdioTransport(McpServer& server)
{
	std::string buffer;
	char chunk[65536];
	while (true) {
		ssize_t got = read(0, chunk, sizeof(chunk));
		if (got <= 0)
			break;
		buffer.append(chunk, got);
		size_t start = 0;
		while (true) {
			size_t newline = buffer.find('\n', start);
			if (newline == std::string::npos)
				break;
			std::string line = TrimString(buffer.substr(start, newline - start));
			start = newline + 1;
			if (line.empty())
				continue;
			std::string response = server.HandleText(line);
			if (!response.empty()) {
				response += '\n';
				size_t done = 0;
				while (done < response.size()) {
					ssize_t written = write(1, response.data() + done,
						response.size() - done);
					if (written <= 0)
						return;
					done += written;
				}
			}
		}
		buffer.erase(0, start);
	}
}


// #pragma mark - HTTP


namespace {

struct ConnectionArgs {
	McpServer* server;
	HttpOptions options;
	HttpTransport* transport;
	uint64 generation;
	int socket;
	std::string peer;
};


// The limits of a request (airos-works/haiku#198). The body is read only
// for a request that has the token: before that, the sender is anybody who
// can reach the port, and gets a header of 64 KiB within the header timeout.
const size_t kMaxHeaderSize = 64 * 1024;
const size_t kMaxBodySize = 64 * 1024 * 1024;

// A body must arrive within the body timeout plus what its size takes at
// this rate: enough for 64 MiB over a bad link, but a sender cannot keep a
// connection (and what the server has buffered for it) for as long as it
// likes by sending a byte every now and then.
const size_t kMinBodyRate = 32 * 1024;	// bytes per second

// After an answer to a request whose body was not read, the rest of what the
// client sends is discarded for a moment, so that it gets the answer rather
// than a reset connection.
const bigtime_t kLingerTimeout = 2 * 1000000LL;
const size_t kLingerSize = 1024 * 1024;


struct HttpRequest {
	std::string method;
	std::string path;
	std::string version;
	std::map<std::string, std::string> headers;	// lower-case names
	std::string body;
	bool keepAlive;
	bool hasBody;
	bool chunked;
	size_t contentLength;
};


// Reads what has arrived, waiting for it until the deadline at most.
ssize_t
ReadSome(int fd, char* chunk, size_t size, bigtime_t deadline)
{
	while (true) {
		bigtime_t now = system_time();
		if (now >= deadline) {
			errno = ETIMEDOUT;
			return -1;
		}
		pollfd descriptor = { fd, POLLIN, 0 };
		int ready = poll(&descriptor, 1,
			(int)std::min((deadline - now + 999) / 1000, (bigtime_t)60000));
		if (ready < 0 && errno != EINTR)
			return -1;
		if (ready > 0)
			return recv(fd, chunk, size, 0);
	}
}


// The time a body may take: see kMinBodyRate. No single wait for more of it
// is longer than the idle timeout.
struct BodyClock {
	bigtime_t end;
	bigtime_t idleTimeout;

	BodyClock(const HttpOptions& options)
		:
		end(system_time() + options.bodyTimeout),
		idleTimeout(options.idleTimeout)
	{
	}

	bigtime_t Deadline(size_t size) const
	{
		bigtime_t deadline = end + (bigtime_t)size * 1000000LL / kMinBodyRate;
		return std::min(deadline, system_time() + idleTimeout);
	}
};


// Reads until the buffer holds "needed" bytes; "size" is the size of the body
// as far as it is known, for the deadline.
bool
ReadExact(int fd, std::string& buffer, size_t needed, const BodyClock& clock,
	size_t size)
{
	char chunk[65536];
	while (buffer.size() < needed) {
		ssize_t got = ReadSome(fd, chunk, sizeof(chunk),
			clock.Deadline(std::max(size, buffer.size())));
		if (got <= 0)
			return false;
		buffer.append(chunk, got);
	}
	return true;
}


// Reads the request line and the headers of one request; false when the
// connection ended, the request is bad or it did not come in time. The body,
// if there is one, has not been read then: see ReadBody().
bool
ReadHeader(int fd, std::string& buffer, HttpRequest& request,
	const HttpOptions& options)
{
	// Waiting for a request may take the idle timeout; once its first byte is
	// here, all of the header has to follow within the header timeout,
	// however slowly its sender lets the bytes trickle.
	bool started = !buffer.empty();
	bigtime_t deadline = system_time()
		+ (started ? options.headerTimeout : options.idleTimeout);
	size_t headerEnd;
	while ((headerEnd = buffer.find("\r\n\r\n")) == std::string::npos) {
		if (buffer.size() > kMaxHeaderSize)
			return false;
		char chunk[16384];
		ssize_t got = ReadSome(fd, chunk, sizeof(chunk), deadline);
		if (got <= 0)
			return false;
		buffer.append(chunk, got);
		if (!started) {
			started = true;
			deadline = system_time() + options.headerTimeout;
		}
	}
	std::string head = buffer.substr(0, headerEnd);
	buffer.erase(0, headerEnd + 4);

	std::vector<std::string> lines = SplitLines(head);
	if (lines.empty())
		return false;
	std::string requestLine = TrimString(lines[0]);
	size_t space1 = requestLine.find(' ');
	size_t space2 = requestLine.rfind(' ');
	if (space1 == std::string::npos || space2 == space1)
		return false;
	request.method = requestLine.substr(0, space1);
	request.path = requestLine.substr(space1 + 1, space2 - space1 - 1);
	request.version = requestLine.substr(space2 + 1);
	request.headers.clear();
	for (size_t i = 1; i < lines.size(); i++) {
		std::string line = TrimString(lines[i]);
		size_t colon = line.find(':');
		if (colon == std::string::npos)
			continue;
		std::string name = line.substr(0, colon);
		for (size_t j = 0; j < name.size(); j++)
			name[j] = tolower(name[j]);
		request.headers[name] = TrimString(line.substr(colon + 1));
	}
	size_t query = request.path.find('?');
	if (query != std::string::npos)
		request.path.erase(query);

	request.keepAlive = request.version == "HTTP/1.1";
	std::map<std::string, std::string>::iterator it
		= request.headers.find("connection");
	if (it != request.headers.end()) {
		if (strcasecmp(it->second.c_str(), "close") == 0)
			request.keepAlive = false;
		else if (strcasecmp(it->second.c_str(), "keep-alive") == 0)
			request.keepAlive = true;
	}

	request.contentLength = 0;
	it = request.headers.find("content-length");
	if (it != request.headers.end())
		request.contentLength = strtoul(it->second.c_str(), NULL, 10);
	request.chunked = request.headers.count("transfer-encoding") != 0;
	request.hasBody = request.chunked || request.contentLength > 0;
	request.body.clear();
	return true;
}


// Reads the body of a request whose header ReadHeader() returned; false when
// the connection ended, the body is too large or it did not come in time.
bool
ReadBody(int fd, std::string& buffer, HttpRequest& request,
	const HttpOptions& options)
{
	size_t contentLength = request.contentLength;
	if (contentLength > kMaxBodySize)
		return false;
	BodyClock clock(options);
	if (request.chunked) {
		// Chunked bodies: decode.
		request.body.clear();
		while (true) {
			size_t received = request.body.size();
			size_t lineEnd;
			while ((lineEnd = buffer.find("\r\n")) == std::string::npos) {
				if (buffer.size() > 65536)
					return false;
				if (!ReadExact(fd, buffer, buffer.size() + 1, clock, received))
					return false;
			}
			size_t chunkSize = strtoul(buffer.substr(0, lineEnd).c_str(), NULL, 16);
			buffer.erase(0, lineEnd + 2);
			if (chunkSize > kMaxBodySize - request.body.size())
				return false;
			if (chunkSize == 0) {
				// trailer
				while ((lineEnd = buffer.find("\r\n")) == std::string::npos) {
					if (buffer.size() > 65536)
						return false;
					if (!ReadExact(fd, buffer, buffer.size() + 1, clock,
							received)) {
						return false;
					}
				}
				buffer.erase(0, lineEnd + 2);
				break;
			}
			if (!ReadExact(fd, buffer, chunkSize + 2, clock,
					received + chunkSize)) {
				return false;
			}
			request.body.append(buffer, 0, chunkSize);
			buffer.erase(0, chunkSize + 2);
		}
		return true;
	}
	if (!ReadExact(fd, buffer, contentLength, clock, contentLength))
		return false;
	request.body = buffer.substr(0, contentLength);
	buffer.erase(0, contentLength);
	return true;
}


// Ends a connection whose client may still be sending (a body nobody reads):
// closing with unread data resets the connection, and the client could lose
// the answer it was just sent.
void
LingerBeforeClose(int fd)
{
	shutdown(fd, SHUT_WR);
	bigtime_t deadline = system_time() + kLingerTimeout;
	size_t discarded = 0;
	char chunk[16384];
	while (discarded < kLingerSize) {
		ssize_t got = ReadSome(fd, chunk, sizeof(chunk), deadline);
		if (got <= 0)
			break;
		discarded += got;
	}
}


bool
SendAll(int fd, const std::string& data)
{
	size_t done = 0;
	while (done < data.size()) {
		ssize_t written = send(fd, data.data() + done, data.size() - done, 0);
		if (written <= 0)
			return false;
		done += written;
	}
	return true;
}


bool
SendResponse(int fd, int status, const char* statusText,
	const std::string& contentType, const std::string& body, bool keepAlive,
	const std::string& extraHeaders = std::string())
{
	std::string response = Format("HTTP/1.1 %d %s\r\n", status, statusText);
	if (!contentType.empty())
		response += "Content-Type: " + contentType + "\r\n";
	response += Format("Content-Length: %zu\r\n", body.size());
	response += "Cache-Control: no-store\r\n";

	response += "Access-Control-Allow-Headers: Content-Type, Authorization, "
		"Mcp-Session-Id, MCP-Protocol-Version, Accept\r\n";
	response += "Access-Control-Allow-Methods: POST, GET, DELETE, OPTIONS\r\n";
	response += extraHeaders;
	response += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
	response += "\r\n";
	response += body;
	return SendAll(fd, response);
}


// Whether the client gave the token, in a time that does not tell how much of
// it was right.
bool
SameToken(const std::string& given, const std::string& token)
{
	unsigned char difference = given.size() != token.size() ? 1 : 0;
	for (size_t i = 0; i < token.size(); i++)
		difference |= token[i] ^ (i < given.size() ? given[i] : 0);
	return difference == 0;
}


bool
Authorized(const HttpRequest& request, const HttpOptions& options)
{
	if (options.token.empty())
		return true;
	std::map<std::string, std::string>::const_iterator it
		= request.headers.find("authorization");
	if (it != request.headers.end()) {
		std::string value = it->second;
		if (strncasecmp(value.c_str(), "Bearer ", 7) == 0
				&& SameToken(TrimString(value.substr(7)), options.token))
			return true;
	}
	it = request.headers.find("x-airos-token");
	if (it != request.headers.end() && SameToken(it->second, options.token))
		return true;
	return false;
}


int32
ConnectionThread(void* data)
{
	ConnectionArgs* args = (ConnectionArgs*)data;
	McpServer& server = *args->server;
	HttpOptions options = args->options;
	HttpTransport* transport = args->transport;
	uint64 generation = args->generation;
	int fd = args->socket;
	std::string peer = args->peer;
	delete args;

	std::string buffer;
	bool authenticated = false;
	bool linger = false;
	while (true) {
		HttpRequest request;
		if (!ReadHeader(fd, buffer, request, options))
			break;

		// Only who has the token gets its body read (up to 64 MiB, kept in
		// memory). Anybody else gets the answer to the header alone, and the
		// connection ends, as what follows is the body nobody read.
		bool authorized = Authorized(request, options);
		bool keepAlive = request.keepAlive;
		if (authorized) {
			if (!authenticated) {
				authenticated = true;
				transport->ConnectionAuthenticated(fd);
			}
			if (!ReadBody(fd, buffer, request, options))
				break;
		} else if (request.hasBody) {
			keepAlive = false;
			linger = true;
		}
		if (!transport->IsCurrent(generation))
			break;

		// Native MCP clients do not send Origin. Reject browser origins, including
		// "null", instead of exposing system control to arbitrary web pages.
		if (request.headers.count("origin") != 0) {
			SendResponse(fd, 403, "Forbidden", "text/plain",
				"Browser origins are not allowed\n", false);
			break;
		} else if (request.method == "OPTIONS") {
			if (!SendResponse(fd, 204, "No Content", "", "", keepAlive))
				break;
		} else if (request.path == "/health" && request.method == "GET") {
			JsonValue health = JsonValue::Object();
			health.Set("ok", true);
			health.Set("server", "mcp_server");
			health.Set("tools", (int64_t)server.Tools().size());
			health.Set("auth", !options.token.empty());
			if (!SendResponse(fd, 200, "OK", "application/json",
					health.Dump() + "\n", keepAlive))
				break;
		} else if (request.path != "/mcp" && request.path != "/") {
			if (!SendResponse(fd, 404, "Not Found", "text/plain",
					"airos_mcp: POST JSON-RPC to /mcp\n", keepAlive))
				break;
		} else if (!authorized) {
			syslog(LOG_WARNING, "airos_mcp: unauthorized request from %s",
				peer.c_str());
			if (!SendResponse(fd, 401, "Unauthorized", "application/json",
					"{\"error\":\"bearer token required\"}\n", keepAlive,
					"WWW-Authenticate: Bearer\r\n"))
				break;
		} else if (request.method == "GET") {
			// No server-initiated stream: tell the client so.
			if (!SendResponse(fd, 405, "Method Not Allowed", "text/plain",
					"no SSE stream; use POST\n", keepAlive, "Allow: POST, DELETE\r\n"))
				break;
		} else if (request.method == "DELETE") {
			if (!SendResponse(fd, 200, "OK", "", "", keepAlive))
				break;
		} else if (request.method == "POST") {
			std::string response = server.HandleText(request.body);
			// Stateless HTTP: do not advertise a shared session identifier.
			std::string extra;
			if (response.empty()) {
				if (!SendResponse(fd, 202, "Accepted", "", "", keepAlive, extra))
					break;
			} else if (!SendResponse(fd, 200, "OK", "application/json", response,
					keepAlive, extra))
				break;
		} else {
			if (!SendResponse(fd, 405, "Method Not Allowed", "text/plain",
					"use POST\n", keepAlive))
				break;
		}
		if (!keepAlive)
			break;
	}
	if (linger)
		LingerBeforeClose(fd);
	transport->ConnectionClosed(fd);
	return 0;
}

} // namespace


HttpTransport::HttpTransport(McpServer& server)
	:
	fServer(server), fListener(-1), fThread(-1), fGeneration(0), fStopping(true)
{
}


HttpTransport::~HttpTransport()
{
	Stop();
	std::map<int, Connection> connections;
	{
		BAutolock lock(&fLock);
		connections = fConnections;
	}
	for (const auto& connection : connections) {
		status_t result;
		wait_for_thread(connection.second.thread, &result);
	}
}


bool
HttpTransport::IsRunning()
{
	BAutolock lock(&fLock);
	return fListener >= 0 && !fStopping;
}


bool
HttpTransport::IsCurrent(uint64 generation)
{
	BAutolock lock(&fLock);
	return !fStopping && generation == fGeneration;
}


void
HttpTransport::ConnectionAuthenticated(int socket)
{
	BAutolock lock(&fLock);
	std::map<int, Connection>::iterator found = fConnections.find(socket);
	if (found != fConnections.end())
		found->second.authenticated = true;
}


/*!	Makes room for a new connection from \a address; the lock is held.
	Returns whether it may be taken.

	A peer that has not shown the token must not be able to keep those who
	have it out (airos-works/haiku#198). So a connection that has not
	authenticated gives way to a new one: the oldest of its address, when that
	address has as many of them as one address may have, or the oldest of all,
	when all connections are taken. Whoever fills the table with idle
	connections only loses them again, and a client with the token gets in
	even from the same address.
	Connections that have shown the token are never given up; when they take
	all there is, the new one is refused as before.
*/
bool
HttpTransport::_MakeRoom(uint32 address)
{
	size_t count = 0;
	size_t unauthenticatedOfPeer = 0;
	std::map<int, Connection>::iterator oldest = fConnections.end();
	std::map<int, Connection>::iterator oldestOfPeer = fConnections.end();
	for (std::map<int, Connection>::iterator it = fConnections.begin();
			it != fConnections.end(); ++it) {
		Connection& connection = it->second;
		if (connection.evicted)
			continue;
		count++;
		if (connection.authenticated)
			continue;
		if (oldest == fConnections.end()
			|| connection.accepted < oldest->second.accepted) {
			oldest = it;
		}
		if (connection.address != address)
			continue;
		unauthenticatedOfPeer++;
		if (oldestOfPeer == fConnections.end()
			|| connection.accepted < oldestOfPeer->second.accepted) {
			oldestOfPeer = it;
		}
	}

	if (unauthenticatedOfPeer >= fOptions.maxUnauthenticatedPerPeer
		&& oldestOfPeer != fConnections.end()) {
		oldest = oldestOfPeer;
	} else if (count < fOptions.maxConnections)
		return true;
	else if (oldest == fConnections.end())
		return false;

	// Its thread ends when it sees the socket shut down, and removes it.
	oldest->second.evicted = true;
	shutdown(oldest->first, SHUT_RDWR);
	return true;
}


void
HttpTransport::ConnectionClosed(int socket)
{
	BAutolock lock(&fLock);
	fConnections.erase(socket);
	close(socket);
}


void
HttpTransport::Stop()
{
	{
		BAutolock lock(&fLock);
		fStopping = true;
		++fGeneration;
		if (fListener >= 0)
			shutdown(fListener, SHUT_RDWR);
		for (const auto& connection : fConnections)
			shutdown(connection.first, SHUT_RDWR);
	}
	if (fThread >= 0) {
		status_t result;
		wait_for_thread(fThread, &result);
		fThread = -1;
	}
	BAutolock lock(&fLock);
	if (fListener >= 0)
		close(fListener);
	fListener = -1;
}


status_t
HttpTransport::Start(const HttpOptions& options)
{
	BAutolock lock(&fLock);
	if (!fStopping)
		return B_OK;
	// The system service must never listen without authentication.
	if (options.token.empty())
		return B_NOT_ALLOWED;
	int listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0)
		return errno;
	fcntl(listener, F_SETFD, FD_CLOEXEC);
	fcntl(listener, F_SETFL, O_NONBLOCK);
	int one = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons(options.port);
	if (inet_aton(options.bindAddress.c_str(), &address.sin_addr) == 0) {
		close(listener);
		return B_BAD_VALUE;
	}
	if (bind(listener, (sockaddr*)&address, sizeof(address)) < 0
		|| listen(listener, 16) < 0) {
		status_t error = errno;
		close(listener);
		return error;
	}
	fOptions = options;
	fListener = listener;
	fStopping = false;
	++fGeneration;
	fThread = spawn_thread(_AcceptThread, "mcp listener", B_NORMAL_PRIORITY, this);
	if (fThread < 0) {
		fStopping = true;
		close(fListener);
		fListener = -1;
		return fThread;
	}
	resume_thread(fThread);
	syslog(LOG_INFO, "mcp_server: listening on %s:%d", options.bindAddress.c_str(),
		options.port);
	return B_OK;
}


int32
HttpTransport::_AcceptThread(void* data)
{
	HttpTransport* self = (HttpTransport*)data;
	while (true) {
		pollfd descriptor = { self->fListener, POLLIN, 0 };
		int ready = poll(&descriptor, 1, 250);
		BAutolock lock(&self->fLock);
		if (self->fStopping)
			return B_OK;
		if (ready <= 0)
			continue;
		sockaddr_in peer;
		socklen_t peerLength = sizeof(peer);
		int fd = accept(self->fListener, (sockaddr*)&peer, &peerLength);
		if (fd < 0)
			continue;
		if (!self->_MakeRoom(peer.sin_addr.s_addr)) {
			close(fd);
			continue;
		}
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		fcntl(fd, F_SETFL, 0);
		int one = 1;
		setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		timeval timeout = { 30, 0 };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
		ConnectionArgs* args = new ConnectionArgs;
		args->server = &self->fServer;
		args->options = self->fOptions;
		args->transport = self;
		args->generation = self->fGeneration;
		args->socket = fd;
		args->peer = Format("%s:%d", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
		thread_id thread = spawn_thread(ConnectionThread, "mcp connection",
			B_NORMAL_PRIORITY, args);
		if (thread < 0) {
			close(fd);
			delete args;
			continue;
		}
		Connection connection = { thread, peer.sin_addr.s_addr, system_time(),
			false, false };
		self->fConnections[fd] = connection;
		resume_thread(thread);
	}
}
