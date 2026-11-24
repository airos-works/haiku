/*
 * Copyright 2026 air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	BMessenger(signature) must come back when the launch_daemon has a job of
	that name which it did not launch (airos-works/haiku#188).

	Every BMessenger(signature) first asks the launch_daemon for the job's
	ports. The daemon kept the request of a plain job that is not launched
	until it would launch that job itself, which may be never: a job whose
	condition is not met or whose target does not run, a job that has ended,
	an application started by hand. The caller waited forever.

		launch_data_test [signature ...]

	Without arguments it asks for jobs every image has and that are not
	launched on a normal start. A messenger must come back within the
	deadline; when the application runs (says the roster), it must be valid.
*/


#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <Messenger.h>
#include <OS.h>
#include <Roster.h>


static const bigtime_t kDeadline = 10 * 1000000LL;

static const char* const kDefaultSignatures[] = {
	"application/x-vnd.Haiku-LaunchBox",
		// a job with a condition (user launch file)
	"application/x-vnd.Haiku-Installer",
		// the job of a target that does not run
	"application/x-vnd.Haiku-cddb_lookup",
		// a job that runs for a moment at each mount
	"application/x-vnd.airos-test-no-such-job"
};


struct Lookup {
	const char*	signature;
	status_t	error;
	bool		valid;
	team_id		team;
};


static status_t
lookup_thread(void* data)
{
	Lookup* lookup = (Lookup*)data;
	BMessenger messenger(lookup->signature, -1, &lookup->error);
	lookup->valid = messenger.IsValid();
	lookup->team = messenger.Team();
	return B_OK;
}


static bool
check(const char* signature)
{
	bool running = be_roster->IsRunning(signature);

	Lookup lookup = { signature, B_ERROR, false, -1 };
	thread_id thread = spawn_thread(lookup_thread, "lookup", B_NORMAL_PRIORITY,
		&lookup);
	if (thread < 0) {
		printf("FAIL %s: no thread: %s\n", signature, strerror(thread));
		return false;
	}
	bigtime_t start = system_time();
	resume_thread(thread);

	status_t result;
	status_t error = wait_for_thread_etc(thread, B_RELATIVE_TIMEOUT, kDeadline,
		&result);
	double seconds = (system_time() - start) / 1e6;
	if (error != B_OK) {
		printf("FAIL %s (%s): BMessenger(signature) did not come back "
			"within %.0f s\n", signature, running ? "running" : "not running",
			seconds);
		return false;
	}

	if (running && !lookup.valid) {
		printf("FAIL %s: runs, but the messenger is not valid: %s (%.3f s)\n",
			signature, strerror(lookup.error), seconds);
		return false;
	}

	if (lookup.valid) {
		printf("ok   %s: messenger to team %" B_PRId32 " after %.3f s\n",
			signature, lookup.team, seconds);
	} else {
		printf("ok   %s: not running, \"%s\" after %.3f s\n", signature,
			strerror(lookup.error), seconds);
	}
	return true;
}


int
main(int argc, char** argv)
{
	bool ok = true;
	if (argc > 1) {
		for (int i = 1; i < argc; i++)
			ok &= check(argv[i]);
	} else {
		for (size_t i = 0; i < B_COUNT_OF(kDefaultSignatures); i++)
			ok &= check(kDefaultSignatures[i]);
	}

	printf("launch_data_test: %s\n", ok ? "PASSED" : "FAILED");
	fflush(stdout);

	// a lookup that is stuck must not keep the test from ending
	_exit(ok ? 0 : 1);
}
