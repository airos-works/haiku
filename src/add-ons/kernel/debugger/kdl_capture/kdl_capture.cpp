/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Kernel debugger capture, for a machine whose kernel debugger nobody sees
	(the lab X399: its only monitor hangs off the GPU under test, and its
	KVM's keyboard does not work). Records what the kernel debugger prints,
	types a few commands into it (a stack crawl) and leaves it again, so that
	the machine runs on long enough for a thread to append the record to a
	file: a panic then reads like a crash log after the next boot, instead
	of a machine that stopped without a trace.

	Lab use only: it leaves every kernel debugger session, panics included,
	without anybody at the keyboard. Kernel driver settings file
	"kdl_capture": "file <path>" (default /boot/home/kdl_capture.log), and
	"commands <commands, ';' between them>" (default "sc"), typed before
	"co". After eight sessions it only leaves them (a panic that repeats).
	The kernel debugger hands its output to modules with serial debug output
	on only (the default).
*/


#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include <debug.h>
#include <driver_settings.h>
#include <KernelExport.h>
#include <OS.h>
#include <syscalls.h>


static const size_t kCaptureSize = 256 * 1024;
static char sCapture[kCaptureSize];
static volatile size_t sCaptured;
static int32 sSessions;

static volatile bool sInDebugger;

static char sFile[B_PATH_NAME_LENGTH] = "/boot/home/kdl_capture.log";
static char sCommands[160] = "sc";
static char sScript[256];
static size_t sScriptPosition;
static size_t sScriptLength;

static thread_id sWriter = -1;
static volatile bool sQuit;


static void
capture(const char* text, size_t length)
{
	size_t at = sCaptured;
	if (at + length > kCaptureSize)
		length = kCaptureSize - at;
	memcpy(sCapture + at, text, length);
	sCaptured = at + length;
}


static void
enter_debugger()
{
	sInDebugger = true;
	char line[96];
	int32 session = ++sSessions;
	snprintf(line, sizeof(line), "\n==== kernel debugger session %" B_PRId32
		", %" B_PRId64 " ms after boot\n", session, system_time() / 1000);
	capture(line, strlen(line));

	// typed in: no paging (it would wait for a key), the commands, and out
	sScript[0] = '\0';
	strlcat(sScript, "paging 0\n", sizeof(sScript));
	if (session <= 8) {
		strlcat(sScript, sCommands, sizeof(sScript));
		strlcat(sScript, "\n", sizeof(sScript));
	}
	strlcat(sScript, "co\n", sizeof(sScript));
	sScriptPosition = 0;
	sScriptLength = strlen(sScript);
}


static void
exit_debugger()
{
	sInDebugger = false;
}


static int
debugger_puts(const char* text, int32 length)
{
	// with serial debug output on, every dprintf() comes here as well
	if (sInDebugger)
		capture(text, length);
	return length;
}


static int
debugger_getchar()
{
	if (sScriptPosition >= sScriptLength)
		return -1;
	return sScript[sScriptPosition++];
}


/*!	Appends what was captured to the file, every half second; runs outside
	the kernel debugger, once the machine goes on. */
static status_t
writer(void*)
{
	size_t written = 0;
	while (!sQuit) {
		snooze(500000);
		size_t captured = sCaptured;
		if (captured == written)
			continue;
		int fd = _kern_open(-1, sFile, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd < 0)
			continue;
		ssize_t bytes = _kern_write(fd, -1, sCapture + written,
			captured - written);
		_kern_fsync(fd, false);
		_kern_close(fd);
		if (bytes > 0)
			written += bytes;
	}
	return B_OK;
}


static status_t
std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		{
			void* settings = load_driver_settings("kdl_capture");
			const char* file = settings != NULL
				? get_driver_parameter(settings, "file", NULL, NULL) : NULL;
			if (file != NULL)
				strlcpy(sFile, file, sizeof(sFile));
			const char* commands = settings != NULL
				? get_driver_parameter(settings, "commands", NULL, NULL) : NULL;
			if (commands != NULL)
				strlcpy(sCommands, commands, sizeof(sCommands));
			for (char* c = sCommands; *c != '\0'; c++) {
				if (*c == ';')
					*c = '\n';
			}
			unload_driver_settings(settings);
			sQuit = false;
			sWriter = spawn_kernel_thread(writer, "kdl capture writer",
				B_URGENT_DISPLAY_PRIORITY, NULL);
			if (sWriter < 0)
				return sWriter;
			resume_thread(sWriter);
			dprintf("kdl_capture: kernel debugger sessions go to %s\n", sFile);
			return B_OK;
		}
		case B_MODULE_UNINIT:
		{
			sQuit = true;
			status_t result;
			wait_for_thread(sWriter, &result);
			return B_OK;
		}
	}
	return B_BAD_VALUE;
}


static struct debugger_module_info sModuleInfo = {
	{
		"debugger/kdl_capture/v1",
		0,
		&std_ops
	},
	enter_debugger,
	exit_debugger,
	debugger_puts,
	debugger_getchar,
	NULL
};

module_info* modules[] = {
	(module_info*)&sModuleInfo,
	NULL
};
