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

	// What a peer gets before it has shown the token; anybody who reaches
	// the port is such a peer. (Tests shorten the times.)
	bigtime_t idleTimeout;		// waiting for a request, or for its next byte
	bigtime_t headerTimeout;	// from a request's first byte to its last
								// header line
	bigtime_t bodyTimeout;		// for a body, plus what its size takes at
								// the minimum rate
	size_t maxConnections;
	size_t maxUnauthenticatedPerPeer;
		// connections of one address that have not shown the token yet

	HttpOptions()
		:
		bindAddress("0.0.0.0"),
		port(7780),
		idleTimeout(30 * 1000000LL),
		headerTimeout(30 * 1000000LL),
		bodyTimeout(60 * 1000000LL),
		maxConnections(32),
		maxUnauthenticatedPerPeer(16)
	{
	}
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
	void ConnectionAuthenticated(int socket);
	void ConnectionClosed(int socket);

private:
	struct Connection {
		thread_id thread;
		uint32 address;			// the peer's, network byte order
		bigtime_t accepted;
		bool authenticated;		// a request of it had the token
		bool evicted;			// shut down to make room, about to end
	};

	static int32 _AcceptThread(void* data);
	bool _MakeRoom(uint32 address);
	McpServer& fServer;
	BLocker fLock;
	int fListener;
	thread_id fThread;
	uint64 fGeneration;
	bool fStopping;
	HttpOptions fOptions;
	std::map<int, Connection> fConnections;
};

#endif // AIROS_MCP_TRANSPORT_H
