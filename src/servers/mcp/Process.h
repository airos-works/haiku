/*
 * airos_mcp - running programs and watching what becomes of them.
 * Copyright 2026 air/OS contributors. MIT license.
 */
#ifndef AIROS_MCP_PROCESS_H
#define AIROS_MCP_PROCESS_H

#include <map>
#include <string>
#include <vector>

#include <OS.h>


struct StuckTeam {
	team_id team;
	std::string args;
	bool debugged;
	int threadCount;
};

struct RunOptions {
	std::vector<std::string> argv;	// empty: use shellCommand with /bin/sh -c
	std::string shellCommand;
	std::string workingDirectory;
	std::map<std::string, std::string> environment;
	std::string stdinData;
	bigtime_t timeout;				// 0: no limit
	size_t maxOutputBytes;			// tail kept; 0: all
	bool killOnTimeout;
	bool returnWhenDebugged;		// stop waiting as soon as a descendant
									// team is held by the debugger
	std::string outputPath;			// where output goes; empty: temporary

	RunOptions()
		: timeout(60000000), maxOutputBytes(64 * 1024), killOnTimeout(true),
		returnWhenDebugged(true) {}
};

struct RunResult {
	status_t error;				// B_OK when the program was started
	std::string errorText;
	pid_t pid;
	int exitCode;				// -1 when not exited
	int signal;					// terminating signal, 0 if none
	bool exited;
	bool timedOut;
	bool debugged;				// a descendant is stopped in the debugger
	bool killed;
	bigtime_t elapsed;
	std::string output;			// stdout+stderr, interleaved
	bool outputTruncated;
	std::vector<StuckTeam> descendants;	// teams of the process group still alive
	std::string outputPath;

	RunResult()
		: error(B_OK), pid(-1), exitCode(-1), signal(0), exited(false),
		timedOut(false), debugged(false), killed(false), elapsed(0),
		outputTruncated(false) {}
};

// Starts the program; the caller then waits with WaitForProcess or polls
// with PollProcess.
status_t StartProcess(const RunOptions& options, RunResult& result);
// Waits according to options.timeout, filling in result.
void WaitForProcess(const RunOptions& options, RunResult& result);
// Non-blocking state refresh: exited/descendants/debugged.
void PollProcess(RunResult& result);
// Collects the output file into result.output (tail of maxOutputBytes).
void CollectOutput(const RunOptions& options, RunResult& result);

// Runs to completion (or timeout) and returns everything.
RunResult RunProcess(const RunOptions& options);
// Convenience: a shell command with the default options.
RunResult RunShell(const std::string& command, bigtime_t timeout = 30000000);

// Every team whose process group is pgid.
std::vector<StuckTeam> TeamsInProcessGroup(pid_t pgid);
// Kills the process group and every team in it; returns how many were killed.
int KillProcessGroup(pid_t pgid);
// kill_team() followed by a wait of up to timeout for the team to go away.
bool KillTeamAndWait(team_id team, bigtime_t timeout = 2000000);
bool TeamExists(team_id team);
bool TeamIsDebugged(team_id team);

#endif // AIROS_MCP_PROCESS_H
