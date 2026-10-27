/*
 * airos_mcp - one call that asks the machine the questions that have fooled
 * us before.
 * Copyright 2026 air/OS contributors. MIT license.
 */
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <OS.h>

#include "Mcp.h"
#include "Process.h"
#include "Tools.h"
#include "Util.h"


namespace {

struct Check {
	std::string name;
	bool ok;
	std::string detail;
};


void
Add(std::vector<Check>& checks, const char* name, bool ok, const std::string& detail)
{
	Check check;
	check.name = name;
	check.ok = ok;
	check.detail = detail;
	checks.push_back(check);
}


int
CountTeams(const char* name, std::vector<team_id>* ids = NULL)
{
	int count = 0;
	int32 cookie = 0;
	team_info info;
	while (get_next_team_info(&cookie, &info) == B_OK) {
		std::string args = info.args;
		size_t space = args.find(' ');
		std::string command = space == std::string::npos ? args : args.substr(0, space);
		size_t slash = command.rfind('/');
		std::string base = slash == std::string::npos ? command : command.substr(slash + 1);
		if (base == name) {
			count++;
			if (ids != NULL)
				ids->push_back(info.team);
		}
	}
	return count;
}


int
FreePtys()
{
	std::vector<int> fds;
	for (int i = 0; i < 128; i++) {
		int fd = posix_openpt(O_RDWR | O_NOCTTY);
		if (fd < 0)
			break;
		fds.push_back(fd);
	}
	for (size_t i = 0; i < fds.size(); i++)
		close(fds[i]);
	return fds.size();
}


ToolResult
HealthCheck(const JsonValue& args)
{
	std::vector<Check> checks;
	double clientTime = args.GetDouble("client_time", 0);
	bool quick = args.GetBool("quick", false);

	// Clock
	time_t now = time(NULL);
	if (clientTime > 0) {
		double skew = (double)now - clientTime;
		Add(checks, "clock agrees with the client", fabs(skew) < 5,
			Format("device is %+.0f s from the client; a device clock ahead of the "
				"build host makes synced sources look old (sync with tar -m)", skew));
	} else {
		char buffer[64];
		struct tm tm;
		localtime_r(&now, &tm);
		strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S %Z", &tm);
		Add(checks, "clock", true, Format("%s (pass client_time to check the skew)",
			buffer));
	}

	// Essential teams
	const char* essential[] = { "app_server", "registrar", "input_server",
		"net_server", "launch_daemon", "debug_server", "media_server", NULL };
	for (int i = 0; essential[i] != NULL; i++) {
		int count = CountTeams(essential[i]);
		Add(checks, (std::string(essential[i]) + " running").c_str(), count > 0,
			count > 0 ? Format("%d team%s", count, count == 1 ? "" : "s")
			: (strcmp(essential[i], "media_server") == 0 ? "not running (fine on a "
			"headless or minimal image)" : "NOT RUNNING"));
		if (count == 0 && strcmp(essential[i], "media_server") == 0)
			checks.back().ok = true;
	}

	// Screen saver
	int blankers = CountTeams("screen_blanker");
	Add(checks, "screen saver not hiding the desktop", blankers == 0, blankers == 0
		? "screen_blanker not running" : "screen_blanker is running: a blanked "
		"desktop gives direct windows empty clip lists (GPU present path turns off) "
		"and stops KVM capture; kill it before measuring");

	// Debugged teams
	std::vector<std::string> debugged;
	int32 cookie = 0;
	team_info info;
	while (get_next_team_info(&cookie, &info) == B_OK) {
		if (info.debugger_nub_thread >= 0)
			debugged.push_back(Format("%s (team %lld)", info.args, (long long)info.team));
	}
	std::string debuggedList;
	for (size_t i = 0; i < debugged.size(); i++)
		debuggedList += (i > 0 ? ", " : "") + debugged[i];
	Add(checks, "no teams stopped in the debugger", debugged.empty(), debugged.empty()
		? "none" : debuggedList + " - crashed and held by the debug server; team_kill "
		"clears them");

	// PTYs and lingering shells
	int freePtys = FreePtys();
	Add(checks, "pseudo terminals available", freePtys >= 8, Format("%d free (64 "
		"total); every lab-shell login left behind keeps one", freePtys));
	int shells = CountTeams("bash") + CountTeams("sh");
	Add(checks, "shell count", shells < 40, Format("%d shells running", shells));

	// Memory
	system_info sys;
	if (get_system_info(&sys) == B_OK) {
		double usedPercent = 100.0 * sys.used_pages / sys.max_pages;
		Add(checks, "memory", usedPercent < 90, Format("%.0f%% of %lld MB used, "
			"%lld MB cached", usedPercent,
			(long long)(sys.max_pages * B_PAGE_SIZE / (1024 * 1024)),
			(long long)(sys.cached_pages * B_PAGE_SIZE / (1024 * 1024))));
		Add(checks, "uptime", true, Format("%.1f h, %lld teams, %lld threads",
			system_time() / 3600000000.0, (long long)sys.used_teams,
			(long long)sys.used_threads));
	}

	// Disk
	struct statvfs vfs;
	if (statvfs("/boot", &vfs) == 0) {
		double freeMb = (double)vfs.f_bavail * vfs.f_frsize / (1024 * 1024);
		Add(checks, "boot volume space", freeMb > 500, Format("%.0f MB free", freeMb));
	}

	// Network
	RunResult ifconfig = RunShell("ifconfig 2>/dev/null | grep -o 'inet addr: [0-9.]*'"
		" | awk '{print $3}' | grep -v '^127\\.' | tr '\\n' ' '", 10000000);
	std::string addresses = TrimString(ifconfig.output);
	Add(checks, "network address", !addresses.empty(), addresses.empty()
		? "no address" : addresses);

	// Syslog health: recent trouble words in the current boot
	std::string syslog;
	ReadFileToString("/var/log/syslog", syslog);
	std::vector<std::string> lines = SplitLines(syslog);
	int panics = 0, faults = 0, usbErrors = 0, kdl = 0;
	std::string lastTrouble;
	for (size_t i = lines.size() > 5000 ? lines.size() - 5000 : 0; i < lines.size(); i++) {
		const std::string& l = lines[i];
		if (l.find("PANIC") != std::string::npos) { panics++; lastTrouble = l; }
		else if (l.find("vm_page_fault") != std::string::npos
				|| l.find("segment violation") != std::string::npos) { faults++; lastTrouble = l; }
		else if (l.find("Welcome to Kernel Debugging Land") != std::string::npos) { kdl++; lastTrouble = l; }
		else if (l.find("usb error") != std::string::npos
				|| l.find("usb_error") != std::string::npos) usbErrors++;
	}
	Add(checks, "syslog free of panics and faults", panics == 0 && faults == 0 && kdl == 0,
		Format("%d PANIC, %d page faults/crashes, %d KDL entries, %d USB errors in "
			"the last %zu lines%s", panics, faults, kdl, usbErrors,
			lines.size() > 5000 ? (size_t)5000 : lines.size(),
			lastTrouble.empty() ? "" : ("; last: " + lastTrouble).c_str()));

	// Display and GPU
	if (!quick) {
		RunResult mode = RunShell("screenmode -s 2>/dev/null", 10000000);
		std::string modeText = TrimString(mode.output);
		Add(checks, "a display is being driven", !modeText.empty(), modeText.empty()
			? "screenmode reports no mode (app_server down or no display)" : modeText);
		JsonValue graphics = JsonValue::Array();
		RunResult gfx = RunShell("ls /dev/graphics 2>/dev/null | tr '\\n' ' '", 10000000);
		std::string gfxText = TrimString(gfx.output);
		Add(checks, "graphics driver published a device", !gfxText.empty(), gfxText.empty()
			? "nothing in /dev/graphics (frame buffer only)" : gfxText);
	}

	// Launch daemon services that are stopped but enabled
	RunResult services = RunShell("launch_roster list 2>/dev/null | sed 's/\\x1b\\[[0-9;]*m//g' "
		"| awk '$2 == \"service\" && $3 == \"stopped\" && $4 == \"yes\" {print $1}' | tr '\\n' ' '",
		10000000);
	std::string stopped = TrimString(services.output);
	Add(checks, "enabled services that are stopped", true, stopped.empty()
		? "none" : stopped + " (on-demand services show as stopped until used; "
		"launch_roster start <name> starts one)");

	JsonValue result = JsonValue::Object();
	JsonValue list = JsonValue::Array();
	int okCount = 0;
	for (size_t i = 0; i < checks.size(); i++) {
		JsonValue c = JsonValue::Object();
		c.Set("check", checks[i].name);
		c.Set("ok", checks[i].ok);
		c.Set("detail", checks[i].detail);
		list.Push(c);
		if (checks[i].ok)
			okCount++;
	}
	result.Set("checks", list);
	result.Set("passed", okCount);
	result.Set("failed", (int64_t)(checks.size() - okCount));
	RunResult uname = RunShell("uname -v", 5000000);
	result.Set("kernel", TrimString(uname.output));
	return ToolResult::Json(result);
}

} // namespace


void
RegisterHealthTools(McpServer& server)
{
	server.AddTool("health_check",
		"Ask the machine the questions that have produced wrong conclusions "
		"before, one line each: clock skew against the client, essential servers "
		"running, screen_blanker hiding the desktop, teams stopped in the "
		"debugger, free pseudo terminals and lingering shells, memory, boot "
		"volume space, network address, panics and faults in the syslog, a "
		"display being driven and a graphics driver device, enabled services that "
		"are stopped. Run it before measuring anything and whenever a result "
		"looks wrong.",
		"{\"type\":\"object\",\"properties\":{\"client_time\":{\"type\":\"number\","
		"\"description\":\"the client's Unix time in seconds, to report the clock "
		"skew\"},\"quick\":{\"type\":\"boolean\",\"description\":\"skip the "
		"display checks that run external commands\"}}}", HealthCheck);
}
