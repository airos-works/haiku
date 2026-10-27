/*
 * mcp_server - attach to app_server on demand for GUI-facing tools.
 * Copyright 2026 air/OS contributors. MIT license.
 */
#include <stdio.h>

#include <Application.h>
#include <OS.h>
#include <Server.h>

#include "Tools.h"


namespace {

static sem_id sLock = create_sem(1, "mcp app lock");
static sem_id sReady = create_sem(0, "mcp app ready");
static BApplication* sApp = NULL;
static bool sStarted = false;
static bool sFailed = false;


int32
AppThread(void*)
{
	// The native service registers without a GUI connection so it can still
	// inspect a wedged desktop. Connect to app_server only for GUI tools.
	BServer* server = static_cast<BServer*>(be_app);
	status_t error = server->InitGUIContext();
	acquire_sem(sLock);
	if (error != B_OK)
		sFailed = true;
	else
		sApp = server;
	release_sem(sLock);
	release_sem(sReady);
	return 0;
}

} // namespace


bool
EnsureApplication(const char** reason)
{
	acquire_sem(sLock);
	if (sApp != NULL) {
		release_sem(sLock);
		return true;
	}
	if (!sStarted) {
		sStarted = true;
		thread_id thread = spawn_thread(AppThread, "mcp application",
			B_NORMAL_PRIORITY, NULL);
		if (thread < 0) {
			sFailed = true;
			release_sem(sReady);
		} else
			resume_thread(thread);
	}
	release_sem(sLock);

	status_t result = acquire_sem_etc(sReady, 1, B_RELATIVE_TIMEOUT, 8000000);
	if (result == B_OK) {
		release_sem(sReady);	// keep it available for other waiters
		acquire_sem(sLock);
		bool ready = sApp != NULL;
		release_sem(sLock);
		if (ready)
			return true;
		if (reason != NULL)
			*reason = "the BApplication could not be created (registrar or "
				"app_server refused)";
		return false;
	}
	acquire_sem(sLock);
	bool failed = sFailed;
	release_sem(sLock);
	if (reason != NULL)
		*reason = failed ? "the BApplication could not be created"
			: "the registrar or app_server did not answer within 8 s; the desktop "
			"may be hung (check teams_list for app_server and registrar)";
	return false;
}
