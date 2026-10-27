/*
 * airos_mcp - teams and threads: list, inspect, kill.
 * Copyright 2026 air/OS contributors. MIT license.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <OS.h>
#include <image.h>

#include "Mcp.h"
#include "Process.h"
#include "Tools.h"
#include "Util.h"


namespace {

// Teams the machine cannot do without (or that are us). Killing one needs
// force=true.
const char* kProtectedTeams[] = {
	"kernel_team", "launch_daemon", "registrar", "app_server", "debug_server",
	"input_server", "net_server", "syslog_daemon", "package_daemon", "sshd",
	"airos_mcp", "mcp_server", NULL
};


bool
IsProtected(const std::string& args)
{
	std::string name = args;
	size_t space = name.find(' ');
	if (space != std::string::npos)
		name.erase(space);
	size_t slash = name.rfind('/');
	if (slash != std::string::npos)
		name.erase(0, slash + 1);
	for (int i = 0; kProtectedTeams[i] != NULL; i++) {
		if (name == kProtectedTeams[i])
			return true;
	}
	return false;
}


JsonValue
ThreadToJson(const thread_info& info)
{
	JsonValue thread = JsonValue::Object();
	thread.Set("thread", (int64_t)info.thread);
	thread.Set("name", info.name);
	thread.Set("state", ThreadStateName(info.state));
	thread.Set("priority", info.priority);
	if (info.state == B_THREAD_WAITING || info.state == B_THREAD_RECEIVING) {
		thread.Set("waiting_on", (int64_t)info.sem);
		sem_info semInfo;
		if (info.sem > 0 && get_sem_info(info.sem, &semInfo) == B_OK) {
			thread.Set("sem_name", semInfo.name);
			thread.Set("sem_owner_team", (int64_t)semInfo.team);
		}
	}
	thread.Set("user_time_s", info.user_time / 1000000.0);
	thread.Set("kernel_time_s", info.kernel_time / 1000000.0);
	return thread;
}


JsonValue
TeamToJson(const team_info& info, bool includeThreads)
{
	JsonValue team = JsonValue::Object();
	team.Set("team", (int64_t)info.team);
	team.Set("args", info.args);
	team.Set("threads", info.thread_count);
	team.Set("images", info.image_count);
	team.Set("uid", (int64_t)info.uid);
	bool debugged = info.debugger_nub_thread >= 0;
	team.Set("debugged", debugged);
	team.Set("pgid", (int64_t)getpgid(info.team));
	team_usage_info usage;
	if (get_team_usage_info(info.team, B_TEAM_USAGE_SELF, &usage) == B_OK) {
		team.Set("user_time_s", usage.user_time / 1000000.0);
		team.Set("kernel_time_s", usage.kernel_time / 1000000.0);
	}
	// A summary of thread states tells a hung team from a busy one.
	int32 cookie = 0;
	thread_info thread;
	int running = 0, waiting = 0, suspended = 0, other = 0;
	JsonValue threads = JsonValue::Array();
	while (get_next_thread_info(info.team, &cookie, &thread) == B_OK) {
		switch (thread.state) {
			case B_THREAD_RUNNING: case B_THREAD_READY: running++; break;
			case B_THREAD_WAITING: case B_THREAD_RECEIVING: case B_THREAD_ASLEEP:
				waiting++; break;
			case B_THREAD_SUSPENDED: suspended++; break;
			default: other++;
		}
		if (includeThreads)
			threads.Push(ThreadToJson(thread));
	}
	JsonValue states = JsonValue::Object();
	states.Set("running", running);
	states.Set("waiting", waiting);
	states.Set("suspended", suspended);
	if (other > 0)
		states.Set("other", other);
	team.Set("thread_states", states);
	if (includeThreads)
		team.Set("thread_list", threads);
	return team;
}


ToolResult
TeamsList(const JsonValue& args)
{
	std::string filter = args.GetString("filter");
	bool debuggedOnly = args.GetBool("debugged_only", false);
	bool includeThreads = args.GetBool("include_threads", false);

	JsonValue teams = JsonValue::Array();
	int debuggedCount = 0;
	int32 cookie = 0;
	team_info info;
	while (get_next_team_info(&cookie, &info) == B_OK) {
		bool debugged = info.debugger_nub_thread >= 0;
		if (debugged)
			debuggedCount++;
		if (debuggedOnly && !debugged)
			continue;
		if (!filter.empty() && strcasestr(info.args, filter.c_str()) == NULL
				&& atol(filter.c_str()) != info.team)
			continue;
		teams.Push(TeamToJson(info, includeThreads));
	}
	JsonValue result = JsonValue::Object();
	result.Set("teams", teams);
	result.Set("count", (int64_t)teams.Size());
	result.Set("debugged_teams", debuggedCount);
	if (debuggedCount > 0)
		result.Set("note", "debugged teams are stopped by the debug server after a "
			"crash; they hold their files and ports and make `hey quit` and "
			"`timeout` hang. team_kill clears them.");
	return ToolResult::Json(result);
}


ToolResult
TeamThreads(const JsonValue& args)
{
	team_id team = (team_id)args.GetInt("team", -1);
	std::string name = args.GetString("name");
	if (team < 0 && !name.empty()) {
		int32 cookie = 0;
		team_info info;
		while (get_next_team_info(&cookie, &info) == B_OK) {
			if (strcasestr(info.args, name.c_str()) != NULL) {
				team = info.team;
				break;
			}
		}
	}
	team_info info;
	if (team < 0 || get_team_info(team, &info) != B_OK)
		return ToolResult::Error(Format("no such team: %lld", (long long)team));
	JsonValue result = TeamToJson(info, true);
	if (info.debugger_nub_thread >= 0) {
		result.Set("debugger_nub_thread", (int64_t)info.debugger_nub_thread);
		result.Set("hint", "stopped in the debugger; `Debugger --save-report --team "
			+ Format("%lld", (long long)team) + "` writes a report (and ends the "
			"team); team_kill clears it without a report");
	}
	// Images give the loaded libraries and add-ons of the team.
	JsonValue images = JsonValue::Array();
	int32 cookie = 0;
	image_info image;
	while (get_next_image_info(team, &cookie, &image) == B_OK) {
		JsonValue entry = JsonValue::Object();
		entry.Set("id", (int64_t)image.id);
		entry.Set("name", image.name);
		entry.Set("text", Format("%p", image.text));
		entry.Set("text_size", (int64_t)image.text_size);
		images.Push(entry);
	}
	result.Set("image_list", images);
	return ToolResult::Json(result);
}


ToolResult
TeamKill(const JsonValue& args)
{
	team_id team = (team_id)args.GetInt("team", -1);
	std::string pattern = args.GetString("name");
	bool force = args.GetBool("force", false);
	bool all = args.GetBool("all_matching", false);
	int signalNumber = (int)args.GetInt("signal", 0);

	std::vector<team_info> targets;
	if (team >= 0) {
		team_info info;
		if (get_team_info(team, &info) != B_OK)
			return ToolResult::Error(Format("no such team: %lld", (long long)team));
		targets.push_back(info);
	} else if (!pattern.empty()) {
		int32 cookie = 0;
		team_info info;
		while (get_next_team_info(&cookie, &info) == B_OK) {
			if (info.team == getpid())
				continue;
			if (strcasestr(info.args, pattern.c_str()) != NULL) {
				targets.push_back(info);
				if (!all)
					break;
			}
		}
		if (targets.empty())
			return ToolResult::Error("no team matches " + pattern);
	} else
		return ToolResult::Error("team or name is required");

	JsonValue results = JsonValue::Array();
	for (size_t i = 0; i < targets.size(); i++) {
		JsonValue entry = JsonValue::Object();
		entry.Set("team", (int64_t)targets[i].team);
		entry.Set("args", targets[i].args);
		entry.Set("was_debugged", targets[i].debugger_nub_thread >= 0);
		if (targets[i].team == getpid() || targets[i].team == B_SYSTEM_TEAM) {
			entry.Set("skipped", "refusing to kill the kernel or this server");
			results.Push(entry);
			continue;
		}
		if (IsProtected(targets[i].args) && !force) {
			entry.Set("skipped", "protected system team; pass force=true to kill it "
				"anyway");
			results.Push(entry);
			continue;
		}
		bool gone;
		if (signalNumber > 0) {
			send_signal(targets[i].team, signalNumber);
			bigtime_t deadline = system_time() + 2000000;
			while (system_time() < deadline && TeamExists(targets[i].team))
				snooze(20000);
			gone = !TeamExists(targets[i].team);
			entry.Set("signal", signalNumber);
		} else
			gone = KillTeamAndWait(targets[i].team);
		entry.Set("gone", gone);
		if (!gone)
			entry.Set("note", "still present 2 s after the kill; a team holding a "
				"kernel resource (a device in a driver call, pages pinned by a GPU "
				"import) only ends when that returns");
		results.Push(entry);
	}
	JsonValue out = JsonValue::Object();
	out.Set("results", results);
	return ToolResult::Json(out);
}

} // namespace


void
RegisterTeamTools(McpServer& server)
{
	server.AddTool("teams_list",
		"List the teams (processes) on the device with thread counts, thread state "
		"summary, CPU time and whether the team is stopped in the debugger "
		"(crashed). Filter by a substring of the command line or by team id.",
		"{\"type\":\"object\",\"properties\":{"
		"\"filter\":{\"type\":\"string\",\"description\":\"substring of the command "
		"line, or a team id\"},"
		"\"debugged_only\":{\"type\":\"boolean\",\"description\":\"only teams held "
		"by the debugger\"},"
		"\"include_threads\":{\"type\":\"boolean\",\"description\":\"include every "
		"thread of each team (verbose)\"}}}", TeamsList);
	server.AddTool("team_threads",
		"Threads and loaded images of one team: thread names, states, what they "
		"wait on (semaphore name and owner), CPU times. Use it to tell a deadlock "
		"from a busy loop, or to see which add-ons a team loaded.",
		"{\"type\":\"object\",\"properties\":{\"team\":{\"type\":\"integer\"},"
		"\"name\":{\"type\":\"string\",\"description\":\"substring of the command "
		"line, used when team is not given\"}}}", TeamThreads);
	server.AddTool("team_kill",
		"Kill a team by id or by a substring of its command line, and verify it is "
		"gone. Clears teams stopped in the debugger after a crash. System teams "
		"(app_server, registrar, launch_daemon, ...) need force=true.",
		"{\"type\":\"object\",\"properties\":{\"team\":{\"type\":\"integer\"},"
		"\"name\":{\"type\":\"string\"},"
		"\"all_matching\":{\"type\":\"boolean\",\"description\":\"kill every team "
		"matching name, not just the first\"},"
		"\"signal\":{\"type\":\"integer\",\"description\":\"send this signal instead "
		"of killing outright (e.g. 15 for SIGTERM)\"},"
		"\"force\":{\"type\":\"boolean\"}}}", TeamKill);
}
