/*
 * airos_mcp - running commands and tests, foreground and background.
 * Copyright 2026 air/OS contributors. MIT license.
 */
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "Mcp.h"
#include "Process.h"
#include "Tools.h"
#include "Util.h"


namespace {

struct Job {
	int id;
	std::string command;
	RunOptions options;
	RunResult result;
	bool done;
	thread_id waiter;
	bigtime_t started;
};

static std::map<int, Job*> sJobs;
static int sNextJob = 1;
static sem_id sJobsLock = create_sem(1, "mcp jobs");


void
FillRunOptions(const JsonValue& args, RunOptions& options, std::string& error)
{
	const JsonValue* argv = args.Get("argv");
	if (argv != NULL && argv->IsArray() && argv->Size() > 0) {
		for (size_t i = 0; i < argv->Size(); i++)
			options.argv.push_back(argv->At(i).AsString());
	} else {
		options.shellCommand = args.GetString("command");
		if (options.shellCommand.empty())
			error = "command (a shell command line) or argv is required";
	}
	options.workingDirectory = ExpandPath(args.GetString("cwd"));
	const JsonValue* env = args.Get("env");
	if (env != NULL && env->IsObject()) {
		const std::vector<std::pair<std::string, JsonValue> >& members
			= env->Members();
		for (size_t i = 0; i < members.size(); i++)
			options.environment[members[i].first] = members[i].second.AsString();
	}
	options.stdinData = args.GetString("stdin");
	options.maxOutputBytes = (size_t)args.GetInt("max_output_bytes", 64 * 1024);
	options.killOnTimeout = args.GetBool("kill_on_timeout", true);
	options.returnWhenDebugged = args.GetBool("return_when_debugged", true);
}


JsonValue
DescendantsToJson(const std::vector<StuckTeam>& teams)
{
	JsonValue out = JsonValue::Array();
	for (size_t i = 0; i < teams.size(); i++) {
		JsonValue team = JsonValue::Object();
		team.Set("team", (int64_t)teams[i].team);
		team.Set("args", teams[i].args);
		team.Set("threads", teams[i].threadCount);
		team.Set("debugged", teams[i].debugged);
		out.Push(team);
	}
	return out;
}


JsonValue
ResultToJson(const RunResult& result, bool includeOutput)
{
	JsonValue out = JsonValue::Object();
	out.Set("started", result.error == B_OK);
	if (result.error != B_OK) {
		out.Set("error", result.errorText);
		return out;
	}
	out.Set("pid", (int64_t)result.pid);
	out.Set("exited", result.exited);
	if (result.exited) {
		if (result.signal != 0)
			out.Set("signal", result.signal);
		else
			out.Set("exit_code", result.exitCode);
	}
	out.Set("timed_out", result.timedOut);
	out.Set("killed", result.killed);
	out.Set("debugged", result.debugged);
	out.Set("elapsed_s", result.elapsed / 1000000.0);
	if (includeOutput) {
		out.Set("output", result.output);
		out.Set("output_truncated", result.outputTruncated);
	}
	if (!result.descendants.empty())
		out.Set("teams_left", DescendantsToJson(result.descendants));
	std::string state;
	if (result.debugged)
		state = "a team of this command crashed or hit a breakpoint and is held by "
			"the debug server (listed in teams_left with debugged=true). It is not "
			"reaped and `timeout`/`tail` wrappers never return. Inspect it (team_threads, "
			"Debugger --save-report) or clear it with team_kill.";
	else if (result.timedOut)
		state = result.killed ? "timed out; the process group was killed"
			: "timed out; the process group is still running";
	else if (result.exited && result.signal != 0)
		state = Format("terminated by signal %d", result.signal);
	else if (result.exited)
		state = result.exitCode == 0 ? "exited normally" : Format("exited with status %d",
			result.exitCode);
	else
		state = "running";
	out.Set("state", state);
	return out;
}


ToolResult
RunCommand(const JsonValue& args)
{
	RunOptions options;
	std::string error;
	FillRunOptions(args, options, error);
	if (!error.empty())
		return ToolResult::Error(error);
	double timeout = args.GetDouble("timeout_s", 60);
	options.timeout = SecondsToMicro(timeout);
	RunResult result = RunProcess(options);
	ToolResult out = ToolResult::Json(ResultToJson(result, true));
	if (result.error != B_OK)
		out.isError = true;
	return out;
}


int32
JobWaiter(void* data)
{
	Job* job = (Job*)data;
	WaitForProcess(job->options, job->result);
	// Keep waiting for the real end when we stopped because of a debugged
	// descendant; the status tool shows the debugged flag meanwhile.
	while (!job->result.exited && !job->result.timedOut) {
		snooze(250000);
		PollProcess(job->result);
		if (job->options.timeout > 0
				&& system_time() - job->started > job->options.timeout) {
			job->result.timedOut = true;
			if (job->options.killOnTimeout) {
				KillProcessGroup(job->result.pid);
				job->result.killed = true;
			}
		}
	}
	job->result.elapsed = system_time() - job->started;
	AutoLocker locker(sJobsLock);
	job->done = true;
	return 0;
}


ToolResult
RunStart(const JsonValue& args)
{
	Job* job = new Job;
	std::string error;
	FillRunOptions(args, job->options, error);
	if (!error.empty()) {
		delete job;
		return ToolResult::Error(error);
	}
	job->options.timeout = SecondsToMicro(args.GetDouble("timeout_s", 0));
	job->options.returnWhenDebugged = false;
	job->command = job->options.argv.empty() ? job->options.shellCommand
		: job->options.argv[0];
	job->done = false;
	job->started = system_time();
	std::string outputPath = ExpandPath(args.GetString("output_path"));
	if (outputPath.empty()) {
		const char* dir = getenv("TMPDIR");
		if (dir == NULL || dir[0] == 0)
			dir = "/tmp";
		outputPath = Format("%s/airos_mcp-job-%lld.log", dir, (long long)system_time());
	}
	job->options.outputPath = outputPath;
	if (StartProcess(job->options, job->result) != B_OK) {
		std::string text = job->result.errorText;
		delete job;
		return ToolResult::Error(text);
	}
	{
		AutoLocker locker(sJobsLock);
		job->id = sNextJob++;
		sJobs[job->id] = job;
	}
	job->waiter = spawn_thread(JobWaiter, "mcp job waiter", B_NORMAL_PRIORITY, job);
	resume_thread(job->waiter);

	JsonValue out = JsonValue::Object();
	out.Set("job", job->id);
	out.Set("pid", (int64_t)job->result.pid);
	out.Set("output_path", outputPath);
	out.Set("hint", "poll with run_status {job}; stop with run_stop {job}");
	return ToolResult::Json(out);
}


Job*
FindJob(int id)
{
	std::map<int, Job*>::iterator it = sJobs.find(id);
	return it == sJobs.end() ? NULL : it->second;
}


ToolResult
RunStatus(const JsonValue& args)
{
	int id = (int)args.GetInt("job", -1);
	double wait = args.GetDouble("wait_s", 0);
	size_t tailBytes = (size_t)args.GetInt("tail_bytes", 16 * 1024);
	int64_t offset = args.GetInt("offset", -1);

	Job* job;
	{
		AutoLocker locker(sJobsLock);
		job = FindJob(id);
	}
	if (job == NULL)
		return ToolResult::Error(Format("no job %d (see run_list)", id));

	bigtime_t deadline = system_time() + SecondsToMicro(wait);
	while (!job->done && system_time() < deadline)
		snooze(100000);

	if (!job->done)
		PollProcess(job->result);
	JsonValue out = ResultToJson(job->result, false);
	out.Set("job", id);
	out.Set("command", job->command);
	out.Set("done", job->done);
	if (!job->done)
		out.Set("elapsed_s", (system_time() - job->started) / 1000000.0);
	std::string all;
	ReadFileToString(job->options.outputPath, all);
	out.Set("output_bytes", (int64_t)all.size());
	if (offset >= 0) {
		std::string part = (size_t)offset < all.size() ? all.substr(offset) : "";
		if (tailBytes > 0 && part.size() > tailBytes)
			part.resize(tailBytes);
		out.Set("output", part);
		out.Set("next_offset", (int64_t)(offset + part.size()));
	} else
		out.Set("output", TailBytes(all, tailBytes));
	out.Set("output_path", job->options.outputPath);
	return ToolResult::Json(out);
}


ToolResult
RunStop(const JsonValue& args)
{
	int id = (int)args.GetInt("job", -1);
	bool forget = args.GetBool("forget", false);
	Job* job;
	{
		AutoLocker locker(sJobsLock);
		job = FindJob(id);
	}
	if (job == NULL)
		return ToolResult::Error(Format("no job %d", id));
	int killed = 0;
	if (!job->done) {
		killed = KillProcessGroup(job->result.pid);
		for (int i = 0; i < 40 && !job->done; i++)
			snooze(50000);
	}
	JsonValue out = ResultToJson(job->result, false);
	out.Set("job", id);
	out.Set("killed_teams", killed);
	out.Set("done", job->done);
	if (forget && job->done) {
		AutoLocker locker(sJobsLock);
		sJobs.erase(id);
		unlink(job->options.outputPath.c_str());
		delete job;
		out.Set("forgotten", true);
	}
	return ToolResult::Json(out);
}


ToolResult
RunList(const JsonValue& args)
{
	AutoLocker locker(sJobsLock);
	JsonValue out = JsonValue::Array();
	for (std::map<int, Job*>::iterator it = sJobs.begin(); it != sJobs.end(); ++it) {
		Job* job = it->second;
		JsonValue entry = JsonValue::Object();
		entry.Set("job", job->id);
		entry.Set("command", job->command);
		entry.Set("pid", (int64_t)job->result.pid);
		entry.Set("done", job->done);
		entry.Set("debugged", job->result.debugged);
		entry.Set("elapsed_s", (job->done ? job->result.elapsed
			: system_time() - job->started) / 1000000.0);
		entry.Set("output_path", job->options.outputPath);
		out.Push(entry);
	}
	JsonValue result = JsonValue::Object();
	result.Set("jobs", out);
	return ToolResult::Json(result);
}

} // namespace


static const char* kRunSchemaProperties =
	"\"command\":{\"type\":\"string\",\"description\":\"shell command line, run "
	"with /bin/sh -c\"},"
	"\"argv\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":"
	"\"program and arguments, used instead of command\"},"
	"\"cwd\":{\"type\":\"string\",\"description\":\"working directory\"},"
	"\"env\":{\"type\":\"object\",\"description\":\"extra environment variables\"},"
	"\"stdin\":{\"type\":\"string\",\"description\":\"data for standard input\"},"
	"\"max_output_bytes\":{\"type\":\"integer\",\"description\":\"keep only the "
	"last N bytes of output (default 65536)\"},"
	"\"kill_on_timeout\":{\"type\":\"boolean\",\"description\":\"kill the whole "
	"process group when the timeout passes (default true)\"},";


void
RegisterRunTools(McpServer& server)
{
	std::string runSchema = std::string("{\"type\":\"object\",\"properties\":{")
		+ kRunSchemaProperties
		+ "\"timeout_s\":{\"type\":\"number\",\"description\":\"seconds to wait "
		"(default 60)\"},"
		"\"return_when_debugged\":{\"type\":\"boolean\",\"description\":\"return as "
		"soon as a team of the command is stopped in the debugger instead of "
		"waiting for the timeout (default true)\"}}}";
	server.AddTool("run_command",
		"Run a shell command on the device and return its combined output, exit "
		"status and what became of it. Output goes to a file, never a pipe, so a "
		"crashed child does not make the call hang: when a team of the command is "
		"stopped by the debug server (a crash shows up this way on Haiku and is "
		"not reaped by kill -9 of the shell), the call returns at once with "
		"debugged=true and the team listed. Use this instead of ssh with timeout "
		"or tail for tests.",
		runSchema.c_str(), RunCommand);

	std::string startSchema = std::string("{\"type\":\"object\",\"properties\":{")
		+ kRunSchemaProperties
		+ "\"timeout_s\":{\"type\":\"number\",\"description\":\"kill after this "
		"many seconds (default 0 = never)\"},"
		"\"output_path\":{\"type\":\"string\",\"description\":\"file for the output "
		"(default a file under /tmp)\"}}}";
	server.AddTool("run_start",
		"Start a command in the background (a server, a soak test, a long "
		"build) and return a job id. Output is written to a file on the device; "
		"read it with run_status.",
		startSchema.c_str(), RunStart);
	server.AddTool("run_status",
		"State and output of a background job from run_start: running, exited, "
		"timed out, or stopped in the debugger, plus the tail of its output (or a "
		"slice from offset). wait_s blocks up to that long for the job to finish.",
		"{\"type\":\"object\",\"properties\":{\"job\":{\"type\":\"integer\"},"
		"\"wait_s\":{\"type\":\"number\"},\"tail_bytes\":{\"type\":\"integer\","
		"\"description\":\"default 16384\"},\"offset\":{\"type\":\"integer\","
		"\"description\":\"read output from this byte offset instead of the tail\"}},"
		"\"required\":[\"job\"]}", RunStatus);
	server.AddTool("run_stop",
		"Kill a background job's process group. With forget=true also drop its "
		"record and output file once it is done.",
		"{\"type\":\"object\",\"properties\":{\"job\":{\"type\":\"integer\"},"
		"\"forget\":{\"type\":\"boolean\"}},\"required\":[\"job\"]}", RunStop);
	server.AddTool("run_list", "List background jobs started through this server.",
		"{\"type\":\"object\",\"properties\":{}}", RunList);
}
