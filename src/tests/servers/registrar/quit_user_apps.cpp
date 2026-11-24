/*
 * Copyright 2026 air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Asks the registrar to quit the user's applications the way a shutdown
	does (B_REG_QUIT_USER_APPS, what the System Updater sends before an
	installation), waits for the answer and prints it with the time it took.
	The machine stays up. For slow_quit_app (airos-works/haiku#159).
*/


#include <stdio.h>
#include <string.h>

#include <Application.h>
#include <Message.h>
#include <OS.h>
#include <Roster.h>

#include <RegistrarDefs.h>
#include <RosterPrivate.h>


int
main()
{
	BApplication app("application/x-vnd.airos-test-quit-user-apps");

	bigtime_t start = system_time();
	BMessage request(BPrivate::B_REG_QUIT_USER_APPS);
	BMessage answer;
	status_t error = BRoster::Private().SendTo(&request, &answer, false);
	double seconds = (system_time() - start) / 1e6;

	if (error != B_OK) {
		printf("quit_user_apps: no answer after %.1f s: %s\n", seconds,
			strerror(error));
		return 1;
	}
	if (answer.what != BPrivate::B_REG_SUCCESS) {
		printf("quit_user_apps: refused after %.1f s: %s (application "
			"\"%s\")\n", seconds, strerror(answer.GetInt32("error", B_ERROR)),
			answer.GetString("app", ""));
		return 2;
	}
	printf("quit_user_apps: all user applications gone after %.1f s\n",
		seconds);
	return 0;
}
