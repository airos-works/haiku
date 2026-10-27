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


struct HttpRequest {
	std::string method;
	std::string path;
	std::string version;
	std::map<std::string, std::string> headers;	// lower-case names
	std::string body;
	bool keepAlive;
};


bool
ReadExact(int fd, std::string& buffer, size_t needed)
{
	char chunk[65536];
	while (buffer.size() < needed) {
		ssize_t got = read(fd, chunk, sizeof(chunk));
		if (got <= 0)
			return false;
		buffer.append(chunk, got);
	}
	return true;
}


// Reads one request; false when the connection ended or the request is bad.
bool
ReadRequest(int fd, std::string& buffer, HttpRequest& request)
{
	size_t headerEnd;
	while ((headerEnd = buffer.find("\r\n\r\n")) == std::string::npos) {
		if (buffer.size() > 64 * 1024)
			return false;
		char chunk[16384];
		ssize_t got = read(fd, chunk, sizeof(chunk));
		if (got <= 0)
			return false;
		buffer.append(chunk, got);
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

	size_t contentLength = 0;
	it = request.headers.find("content-length");
	if (it != request.headers.end())
		contentLength = strtoul(it->second.c_str(), NULL, 10);
	if (contentLength > 64 * 1024 * 1024)
		return false;
	if (request.headers.count("transfer-encoding") != 0) {
		// Chunked bodies: decode.
		request.body.clear();
		while (true) {
			size_t lineEnd;
			while ((lineEnd = buffer.find("\r\n")) == std::string::npos) {
				if (buffer.size() > 65536)
					return false;
				if (!ReadExact(fd, buffer, buffer.size() + 1))
					return false;
			}
			size_t chunkSize = strtoul(buffer.substr(0, lineEnd).c_str(), NULL, 16);
			buffer.erase(0, lineEnd + 2);
			if (chunkSize > 64 * 1024 * 1024 - request.body.size())
				return false;
			if (chunkSize == 0) {
				// trailer
				while ((lineEnd = buffer.find("\r\n")) == std::string::npos) {
					if (!ReadExact(fd, buffer, buffer.size() + 1))
						return false;
				}
				buffer.erase(0, lineEnd + 2);
				break;
			}
			if (!ReadExact(fd, buffer, chunkSize + 2))
				return false;
			request.body.append(buffer, 0, chunkSize);
			buffer.erase(0, chunkSize + 2);
		}
		return true;
	}
	if (!ReadExact(fd, buffer, contentLength))
		return false;
	request.body = buffer.substr(0, contentLength);
	buffer.erase(0, contentLength);
	return true;
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
				&& TrimString(value.substr(7)) == options.token)
			return true;
	}
	it = request.headers.find("x-airos-token");
	if (it != request.headers.end() && it->second == options.token)
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
	while (true) {
		HttpRequest request;
		if (!ReadRequest(fd, buffer, request) || !transport->IsCurrent(generation))
			break;

		bool keepAlive = request.keepAlive;
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
		} else if (!Authorized(request, options)) {
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
	std::map<int, thread_id> connections;
	{
		BAutolock lock(&fLock);
		connections = fConnections;
	}
	for (const auto& connection : connections) {
		status_t result;
		wait_for_thread(connection.second, &result);
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
		if (self->fConnections.size() >= 32) {
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
		self->fConnections[fd] = thread;
		resume_thread(thread);
	}
}
