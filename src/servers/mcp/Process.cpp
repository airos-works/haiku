/*
 * airos_mcp - running programs and watching what becomes of them.
 * Copyright 2026 air/OS contributors. MIT license.
 */
#include "Process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "Util.h"


extern char** environ;


static std::string
_MakeTempPath(const char* name)
{
	const char* dir = getenv("TMPDIR");
	if (dir == NULL || dir[0] == 0)
		dir = "/tmp";
	return Format("%s/airos_mcp-%s-%d-%lld", dir, name, (int)getpid(),
		(long long)system_time());
}


status_t
StartProcess(const RunOptions& options, RunResult& result)
{
	result = RunResult();
	std::string outputPath = options.outputPath;
	if (outputPath.empty())
		outputPath = _MakeTempPath("out");
	result.outputPath = outputPath;

	int outFd = open(outputPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
		0644);
	if (outFd < 0) {
		result.error = errno;
		result.errorText = Format("cannot create %s: %s", outputPath.c_str(),
			strerror(errno));
		return result.error;
	}

	std::string stdinPath = "/dev/null";
	std::string stdinTemp;
	if (!options.stdinData.empty()) {
		stdinTemp = _MakeTempPath("in");
		if (!WriteStringToFile(stdinTemp, options.stdinData, 0600)) {
			close(outFd);
			result.error = errno;
			result.errorText = "cannot write the stdin data";
			return result.error;
		}
		stdinPath = stdinTemp;
	}
	int inFd = open(stdinPath.c_str(), O_RDONLY | O_CLOEXEC);
	if (inFd < 0) {
		close(outFd);
		result.error = errno;
		result.errorText = "cannot open stdin source";
		return result.error;
	}

	std::vector<std::string> argv = options.argv;
	if (argv.empty()) {
		argv.push_back("/bin/sh");
		argv.push_back("-c");
		argv.push_back(options.shellCommand);
	}
	std::vector<char*> cArgv;
	for (size_t i = 0; i < argv.size(); i++)
		cArgv.push_back(const_cast<char*>(argv[i].c_str()));
	cArgv.push_back(NULL);

	// Environment: ours plus the overrides. Changing the working directory
	// happens via a shell wrapper when needed, as posix_spawn has no chdir.
	std::vector<std::string> envStrings;
	for (char** e = environ; e != NULL && *e != NULL; e++) {
		std::string entry(*e);
		size_t eq = entry.find('=');
		std::string key = eq == std::string::npos ? entry : entry.substr(0, eq);
		if (options.environment.count(key) == 0)
			envStrings.push_back(entry);
	}
	for (std::map<std::string, std::string>::const_iterator it
			= options.environment.begin(); it != options.environment.end(); ++it)
		envStrings.push_back(it->first + "=" + it->second);
	std::vector<char*> cEnv;
	for (size_t i = 0; i < envStrings.size(); i++)
		cEnv.push_back(const_cast<char*>(envStrings[i].c_str()));
	cEnv.push_back(NULL);

	if (!options.workingDirectory.empty()) {
		// Wrap in a shell that changes directory first.
		std::string wrapped = "cd " + options.workingDirectory + " && exec";
		for (size_t i = 0; i < argv.size(); i++) {
			wrapped += " '";
			for (size_t j = 0; j < argv[i].size(); j++) {
				if (argv[i][j] == '\'')
					wrapped += "'\\''";
				else
					wrapped += argv[i][j];
			}
			wrapped += "'";
		}
		// Keep the wrapped string alive for the spawn call below.
		static thread_local std::string sWrapped;
		sWrapped = wrapped;
		cArgv.clear();
		cArgv.push_back(const_cast<char*>("/bin/sh"));
		cArgv.push_back(const_cast<char*>("-c"));
		cArgv.push_back(const_cast<char*>(sWrapped.c_str()));
		cArgv.push_back(NULL);
	}

	posix_spawn_file_actions_t actions;
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, inFd, 0);
	posix_spawn_file_actions_adddup2(&actions, outFd, 1);
	posix_spawn_file_actions_adddup2(&actions, outFd, 2);

	posix_spawnattr_t attr;
	posix_spawnattr_init(&attr);
	posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
	posix_spawnattr_setpgroup(&attr, 0);

	pid_t pid = -1;
	bigtime_t start = system_time();
	int status = posix_spawn(&pid, cArgv[0], &actions, &attr, cArgv.data(),
		cEnv.data());
	posix_spawn_file_actions_destroy(&actions);
	posix_spawnattr_destroy(&attr);
	close(inFd);
	close(outFd);
	if (!stdinTemp.empty())
		unlink(stdinTemp.c_str());

	if (status != 0) {
		result.error = status;
		result.errorText = Format("cannot start %s: %s", cArgv[0],
			strerror(status));
		return result.error;
	}
	result.pid = pid;
	result.elapsed = system_time() - start;
	return B_OK;
}


std::vector<StuckTeam>
TeamsInProcessGroup(pid_t pgid)
{
	std::vector<StuckTeam> teams;
	int32 cookie = 0;
	team_info info;
	while (get_next_team_info(&cookie, &info) == B_OK) {
		if (info.team <= 1)
			continue;
		if (getpgid(info.team) != pgid)
			continue;
		StuckTeam team;
		team.team = info.team;
		team.args = info.args;
		team.debugged = info.debugger_nub_thread >= 0;
		team.threadCount = info.thread_count;
		teams.push_back(team);
	}
	return teams;
}


bool
TeamExists(team_id team)
{
	team_info info;
	return get_team_info(team, &info) == B_OK;
}


bool
TeamIsDebugged(team_id team)
{
	team_info info;
	return get_team_info(team, &info) == B_OK && info.debugger_nub_thread >= 0;
}


void
PollProcess(RunResult& result)
{
	if (result.pid <= 0)
		return;
	if (!result.exited) {
		int status = 0;
		pid_t done = waitpid(result.pid, &status, WNOHANG);
		if (done == result.pid) {
			result.exited = true;
			if (WIFEXITED(status))
				result.exitCode = WEXITSTATUS(status);
			else if (WIFSIGNALED(status))
				result.signal = WTERMSIG(status);
		}
	}
	result.descendants = TeamsInProcessGroup(result.pid);
	result.debugged = false;
	for (size_t i = 0; i < result.descendants.size(); i++) {
		if (result.descendants[i].debugged)
			result.debugged = true;
	}
}


int
KillProcessGroup(pid_t pgid)
{
	int killed = 0;
	std::vector<StuckTeam> teams = TeamsInProcessGroup(pgid);
	for (size_t i = 0; i < teams.size(); i++) {
		if (kill_team(teams[i].team) == B_OK)
			killed++;
	}
	killpg(pgid, SIGKILL);
	return killed;
}


bool
KillTeamAndWait(team_id team, bigtime_t timeout)
{
	kill_team(team);
	bigtime_t deadline = system_time() + timeout;
	while (system_time() < deadline) {
		if (!TeamExists(team))
			return true;
		snooze(20000);
	}
	return !TeamExists(team);
}


void
WaitForProcess(const RunOptions& options, RunResult& result)
{
	if (result.pid <= 0)
		return;
	bigtime_t start = system_time() - result.elapsed;
	bigtime_t deadline = options.timeout > 0 ? start + options.timeout : 0;
	bigtime_t lastDebugCheck = 0;
	while (true) {
		int status = 0;
		pid_t done = waitpid(result.pid, &status, WNOHANG);
		if (done == result.pid) {
			result.exited = true;
			if (WIFEXITED(status))
				result.exitCode = WEXITSTATUS(status);
			else if (WIFSIGNALED(status))
				result.signal = WTERMSIG(status);
			break;
		}
		bigtime_t now = system_time();
		if (options.returnWhenDebugged && now - lastDebugCheck > 300000) {
			lastDebugCheck = now;
			std::vector<StuckTeam> teams = TeamsInProcessGroup(result.pid);
			for (size_t i = 0; i < teams.size(); i++) {
				if (teams[i].debugged) {
					result.debugged = true;
					break;
				}
			}
			if (result.debugged)
				break;
		}
		if (deadline != 0 && now >= deadline) {
			result.timedOut = true;
			break;
		}
		snooze(20000);
	}
	result.elapsed = system_time() - start;
	if (result.timedOut && options.killOnTimeout) {
		KillProcessGroup(result.pid);
		result.killed = true;
		int status;
		for (int i = 0; i < 50; i++) {
			if (waitpid(result.pid, &status, WNOHANG) == result.pid) {
				result.exited = true;
				if (WIFSIGNALED(status))
					result.signal = WTERMSIG(status);
				break;
			}
			snooze(20000);
		}
	}
	// What is left in the group, debugged or not.
	result.descendants = TeamsInProcessGroup(result.pid);
	for (size_t i = 0; i < result.descendants.size(); i++) {
		if (result.descendants[i].debugged)
			result.debugged = true;
	}
}


void
CollectOutput(const RunOptions& options, RunResult& result)
{
	std::string all;
	ReadFileToString(result.outputPath, all);
	if (options.maxOutputBytes != 0 && all.size() > options.maxOutputBytes) {
		result.outputTruncated = true;
		result.output = TailBytes(all, options.maxOutputBytes);
	} else
		result.output = all;
}


RunResult
RunProcess(const RunOptions& options)
{
	RunResult result;
	if (StartProcess(options, result) != B_OK)
		return result;
	WaitForProcess(options, result);
	CollectOutput(options, result);
	if (options.outputPath.empty())
		unlink(result.outputPath.c_str());
	return result;
}


RunResult
RunShell(const std::string& command, bigtime_t timeout)
{
	RunOptions options;
	options.shellCommand = command;
	options.timeout = timeout;
	options.maxOutputBytes = 1024 * 1024;
	return RunProcess(options);
}
