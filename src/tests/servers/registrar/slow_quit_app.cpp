/*
 * Copyright 2026 air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	A user application that is slow to quit, for the shutdown process of the
	registrar (airos-works/haiku#159):

		slow_quit_app <mode> <seconds> <log file>

	quit	QuitRequested() takes <seconds> ("saving"), then agrees
	busy	the looper is busy for <seconds> from the start, so the quit
			request waits in its queue; then it quits at once
	hang	QuitRequested() never returns
	modal	QuitRequested() shows an alert and agrees when it is closed
	refuse	QuitRequested() takes <seconds> and refuses

	Every step is appended to the log file with the wall time and synced, so
	that it is known how far the application came when it was killed. An
	application that was allowed to finish writes "saved".
*/


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Alert.h>
#include <Application.h>
#include <OS.h>
#include <String.h>


static const uint32 kMsgBusy = 'busy';

static const char* sMode = "quit";
static bigtime_t sSeconds = 10;
static const char* sLogPath = NULL;


static void
log_step(const char* step)
{
	bigtime_t now = real_time_clock_usecs();
	char line[256];
	int length = snprintf(line, sizeof(line), "%s %" B_PRId32 " %s %"
		B_PRIdBIGTIME ".%03" B_PRIdBIGTIME "\n", sMode, (int32)getpid(), step,
		now / 1000000, (now / 1000) % 1000);
	fputs(line, stdout);
	fflush(stdout);

	int fd = open(sLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0)
		return;
	write(fd, line, length);
	fsync(fd);
	close(fd);
}


class SlowQuitApp : public BApplication {
public:
	SlowQuitApp(const char* signature)
		:
		BApplication(signature)
	{
	}

	virtual void ReadyToRun()
	{
		log_step("ready");
		if (strcmp(sMode, "busy") == 0)
			PostMessage(kMsgBusy);
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == kMsgBusy) {
			log_step("busy-begin");
			snooze(sSeconds * 1000000);
			log_step("busy-end");
			return;
		}
		BApplication::MessageReceived(message);
	}

	virtual bool QuitRequested()
	{
		log_step("quit-requested");
		if (strcmp(sMode, "hang") == 0) {
			while (true)
				snooze(1000000);
		} else if (strcmp(sMode, "modal") == 0) {
			BAlert* alert = new BAlert("slow_quit_app", "Save the changes?",
				"Save");
			alert->Go();
		} else if (strcmp(sMode, "busy") != 0)
			snooze(sSeconds * 1000000);

		if (strcmp(sMode, "refuse") == 0) {
			log_step("refused");
			return false;
		}
		log_step("saved");
		return true;
	}
};


int
main(int argc, char** argv)
{
	if (argc != 4) {
		fprintf(stderr, "usage: %s quit|busy|hang|modal|refuse <seconds> "
			"<log file>\n", argv[0]);
		return 1;
	}
	sMode = argv[1];
	sSeconds = atoi(argv[2]);
	sLogPath = argv[3];

	BString signature("application/x-vnd.airos-test-slow-quit-");
	signature << sMode;
	SlowQuitApp app(signature);
	app.Run();
	log_step("exit");
	return 0;
}
