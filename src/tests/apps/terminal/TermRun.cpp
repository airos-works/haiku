/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Runs a program in a pseudo terminal on the build host with Terminal's
	emulator answering its queries, the way it would run in Terminal, and
	records what it printed. A script types into it:

		termrun [-c columns] [-r rows] [-o raw-output] [-s script] -- command
		termrun [-c columns] [-r rows] [-s script] -p raw-output

	The second form plays a recorded output back instead (the script's
	"dump" and "links" then show the result).

	Script lines: "wait <ms>", "waitfor <text>" (until the screen shows it,
	30 s at most), "send <text>" (with \r, \n, \t, \e, \xNN escapes),
	"resize <columns> <rows>", "dump" (screen and history as text),
	"links" (the addresses found on the screen, see TerminalLinks.h) and
	"quit". Used to capture what programs like Claude Code or tmux really
	send, for the tests.
*/


#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "Terminal.h"
#include "TerminalLinks.h"


static int sMaster = -1;
static FILE* sRaw = NULL;


static int64_t
now_ms()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}


static std::string
unescape(const char* text)
{
	std::string result;
	for (const char* p = text; *p != '\0'; p++) {
		if (*p != '\\' || p[1] == '\0') {
			result += *p;
			continue;
		}
		p++;
		switch (*p) {
			case 'r': result += '\r'; break;
			case 'n': result += '\n'; break;
			case 't': result += '\t'; break;
			case 'e': result += '\x1b'; break;
			case 'x':
			{
				char hex[3] = { p[1], p[1] ? p[2] : 0, 0 };
				result += (char)strtol(hex, NULL, 16);
				p += 2;
				break;
			}
			default: result += *p; break;
		}
	}
	return result;
}


static void
pump(Terminal& terminal, int timeoutMs)
{
	int64_t end = now_ms() + timeoutMs;
	while (true) {
		int left = (int)(end - now_ms());
		if (left < 0)
			left = 0;
		struct pollfd pfd = { sMaster, POLLIN, 0 };
		int result = poll(&pfd, 1, left);
		if (result > 0 && (pfd.revents & (POLLIN | POLLHUP)) != 0) {
			uint8 buffer[65536];
			ssize_t bytes = read(sMaster, buffer, sizeof(buffer));
			if (bytes > 0) {
				if (sRaw != NULL)
					fwrite(buffer, 1, bytes, sRaw);
				terminal.Lock();
				terminal.Feed(buffer, bytes);
				std::string answer;
				terminal.TakeOutput(answer);
				terminal.Unlock();
				if (!answer.empty())
					write(sMaster, answer.data(), answer.size());
				continue;
			}
			if (bytes <= 0 && (pfd.revents & POLLHUP) != 0)
				return;
		}
		if (left == 0)
			return;
	}
}


static std::string
screen_text(Terminal& terminal, bool history)
{
	std::string text;
	terminal.Lock();
	int32 first = history ? -terminal.HistorySize() : 0;
	for (int32 row = first; row < terminal.Rows(); row++) {
		std::string line;
		terminal.GetText(TermPoint(0, row),
			TermPoint(terminal.LineLength(row), row), line);
		while (!line.empty() && line.back() == '\n')
			line.pop_back();
		char prefix[32];
		snprintf(prefix, sizeof(prefix), "%4d%c|", (int)row,
			(terminal.LineFlags(row) & LINE_WRAPPED) != 0 ? '+' : ' ');
		text += prefix;
		text += line;
		text += "\n";
	}
	terminal.Unlock();
	return text;
}


static void
print_links(Terminal& terminal)
{
	terminal.Lock();
	std::vector<std::string> seen;
	int32 first = -terminal.HistorySize();
	for (int32 row = first; row < terminal.Rows(); row++) {
		for (int32 column = 0; column < terminal.Columns(); column++) {
			TermLinkRange link;
			if (!FindTerminalLink(terminal, TermPoint(column, row), link))
				continue;
			char where[64];
			snprintf(where, sizeof(where), "%d,%d-%d,%d %s", (int)link.start.y,
				(int)link.start.x, (int)link.end.y, (int)link.end.x,
				link.hyperlink != 0 ? "osc8" : "text");
			std::string entry = std::string(where) + " " + link.uri;
			bool known = false;
			for (size_t i = 0; i < seen.size(); i++)
				known |= seen[i] == entry;
			if (!known) {
				seen.push_back(entry);
				printf("LINK %s\n", entry.c_str());
			}
		}
	}
	terminal.Unlock();
}


int
main(int argc, char** argv)
{
	int columns = 100;
	int rows = 30;
	const char* rawPath = NULL;
	const char* scriptPath = NULL;
	const char* replayPath = NULL;
	int i = 1;
	for (; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		if (strcmp(argv[i], "-c") == 0 && i + 1 < argc)
			columns = atoi(argv[++i]);
		else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc)
			rows = atoi(argv[++i]);
		else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
			rawPath = argv[++i];
		else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
			scriptPath = argv[++i];
		else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
			replayPath = argv[++i];
	}
	if (replayPath != NULL) {
		Terminal terminal;
		terminal.Init(columns, rows, 2000);
		terminal.SetCellSize(10, 20);
		FILE* file = fopen(replayPath, "rb");
		if (file == NULL) {
			perror(replayPath);
			return 1;
		}
		std::string data;
		char buffer[65536];
		size_t bytes;
		while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0)
			data.append(buffer, bytes);
		fclose(file);
		// what a full screen program showed before it quit
		size_t quit = data.rfind("\x1b[?1049l");
		if (quit != std::string::npos) {
			data.resize(quit);
			// (tmux clears the screen first)
			size_t clear = data.rfind("\x1b[2J");
			if (clear != std::string::npos && data.size() - clear < 200)
				data.resize(clear);
		}
		terminal.Lock();
		terminal.Feed((const uint8*)data.data(), data.size());
		terminal.Unlock();
		bool links = true;
		FILE* script = scriptPath != NULL ? fopen(scriptPath, "r") : NULL;
		char line[256];
		while (script != NULL && fgets(line, sizeof(line), script) != NULL) {
			if (strncmp(line, "dump", 4) == 0)
				printf("%s", screen_text(terminal, true).c_str());
		}
		if (links)
			print_links(terminal);
		return 0;
	}
	if (i >= argc) {
		fprintf(stderr, "usage: termrun [-c cols] [-r rows] [-o raw] "
			"[-s script] -- command...\n");
		return 1;
	}

	Terminal terminal;
	terminal.Init(columns, rows, 2000);
	terminal.SetCellSize(10, 20);

	struct winsize size = {};
	size.ws_col = columns;
	size.ws_row = rows;
	size.ws_xpixel = columns * 10;
	size.ws_ypixel = rows * 20;
	pid_t child = forkpty(&sMaster, NULL, NULL, &size);
	if (child < 0) {
		perror("forkpty");
		return 1;
	}
	if (child == 0) {
		execvp(argv[i], argv + i);
		perror("execvp");
		_exit(127);
	}

	if (rawPath != NULL)
		sRaw = fopen(rawPath, "wb");

	FILE* script = scriptPath != NULL ? fopen(scriptPath, "r") : NULL;
	char line[4096];
	while (script != NULL && fgets(line, sizeof(line), script) != NULL) {
		size_t length = strlen(line);
		while (length > 0 && (line[length - 1] == '\n'
				|| line[length - 1] == '\r'))
			line[--length] = '\0';
		if (length == 0 || line[0] == '#')
			continue;
		if (strncmp(line, "wait ", 5) == 0)
			pump(terminal, atoi(line + 5));
		else if (strncmp(line, "waitfor ", 8) == 0) {
			int64_t end = now_ms() + 30000;
			bool found = false;
			while (now_ms() < end) {
				pump(terminal, 200);
				if (screen_text(terminal, false).find(line + 8)
						!= std::string::npos) {
					found = true;
					break;
				}
			}
			if (!found)
				fprintf(stderr, "termrun: \"%s\" did not appear\n", line + 8);
		} else if (strncmp(line, "send ", 5) == 0) {
			std::string data = unescape(line + 5);
			write(sMaster, data.data(), data.size());
			pump(terminal, 50);
		} else if (strncmp(line, "resize ", 7) == 0) {
			int newColumns = 0, newRows = 0;
			sscanf(line + 7, "%d %d", &newColumns, &newRows);
			terminal.Lock();
			terminal.Resize(newColumns, newRows);
			terminal.Unlock();
			size.ws_col = newColumns;
			size.ws_row = newRows;
			ioctl(sMaster, TIOCSWINSZ, &size);
			pump(terminal, 50);
		} else if (strcmp(line, "dump") == 0) {
			pump(terminal, 0);
			printf("%s", screen_text(terminal, true).c_str());
		} else if (strcmp(line, "links") == 0) {
			pump(terminal, 0);
			print_links(terminal);
		} else if (strcmp(line, "quit") == 0)
			break;
	}

	kill(child, SIGHUP);
	pump(terminal, 300);
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
	if (sRaw != NULL)
		fclose(sRaw);
	return 0;
}
