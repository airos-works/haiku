/*
 * airos_mcp - stdio and streamable HTTP transports.
 * Copyright 2026 air/OS contributors. MIT license.
 */
#ifndef AIROS_MCP_TRANSPORT_H
#define AIROS_MCP_TRANSPORT_H

#include <map>
#include <string>

#include <Locker.h>

#include <OS.h>

class McpServer;


// Newline-delimited JSON-RPC on stdin/stdout. Returns when stdin closes.
void RunStdioTransport(McpServer& server);


struct HttpOptions {
	std::string bindAddress;	// "0.0.0.0" by default
	int port;
	std::string token;			// required by the system service
	HttpOptions() : bindAddress("0.0.0.0"), port(7780) {}
};

// The service owns this for its entire lifetime. Stop closes network access;
// tools already executing may finish, but their connection cannot be reused.
class HttpTransport {
public:
	HttpTransport(McpServer& server);
	~HttpTransport();
	status_t Start(const HttpOptions& options);
	void Stop();
	bool IsRunning();
	bool IsCurrent(uint64 generation);
	void ConnectionClosed(int socket);

private:
	static int32 _AcceptThread(void* data);
	McpServer& fServer;
	BLocker fLock;
	int fListener;
	thread_id fThread;
	uint64 fGeneration;
	bool fStopping;
	HttpOptions fOptions;
	std::map<int, thread_id> fConnections;
};

#endif // AIROS_MCP_TRANSPORT_H
