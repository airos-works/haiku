/* Copyright 2026 air/OS contributors. Distributed under the MIT License. */
#ifndef _MCP_SERVER_H
#define _MCP_SERVER_H

#include <LaunchRoster.h>
#include <Messenger.h>
#include <OS.h>
#include <Roster.h>

static const char* const kMCPServerSignature = "application/x-vnd.Haiku-mcp_server";
static const char* const kMCPServerJob = "x-vnd.Haiku-mcp_server";
static const uint32 kMCPGetStatus = 'mcpg';
static const uint32 kMCPSetEnabled = 'mcpe';
static const uint32 kMCPSetUIAccess = 'mcpu';
static const int32 kMCPPort = 7780;

// Local IPC only. SetEnabled takes a bool "enabled", SetUIAccess a bool
// "ui_access" (agents may read and use applications through the ui_* tools).
// All messages reply with int32 "error", bool "enabled", bool "running",
// bool "ui_access" and string "connection_info". Connection information
// (including the credential) is only returned while on.


// The service quits a while after it was last asked anything while MCP is
// off, and turns its launch job off first so that launch_daemon does not
// start it again at once. This starts it again (B_LAUNCH_JOB turns the job
// back on) and waits for it to answer.
static inline BMessenger
MCPServerMessenger()
{
	BMessenger server(kMCPServerSignature);
	if (server.IsValid())
		return server;

	if (BLaunchRoster().Start(kMCPServerJob) == B_OK) {
		for (int i = 0; i < 200 && !server.IsValid(); i++) {
			snooze(50000);
			server = BMessenger(kMCPServerSignature);
		}
	}
	if (!server.IsValid()) {
		// No job to start (safe mode): run it as an application.
		be_roster->Launch(kMCPServerSignature);
		server = BMessenger(kMCPServerSignature);
	}
	return server;
}


// Sends a request to the service, starting it when it is not running.
static inline status_t
MCPServerSendRequest(BMessage& request, BMessage& reply, bigtime_t timeout)
{
	status_t error = B_ERROR;
	for (int attempt = 0; attempt < 2; attempt++) {
		if (attempt > 0)
			snooze(200000);
		reply.MakeEmpty();
		reply.what = 0;
		error = MCPServerMessenger().SendMessage(&request, &reply, timeout,
			timeout);
		if (error == B_OK && reply.what == B_REPLY)
			return B_OK;
		// It may have been quitting as it was asked (no reply): once more.
		if (error == B_OK)
			error = B_ERROR;
	}
	return error;
}

#endif
