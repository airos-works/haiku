/*
 * Copyright 2009, Olivier Coursière. All rights reserved.
 * Copyright 2004, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <string.h>

#include <OS.h>

#include <kernel.h>
#include <lock.h>
#include <shutdown_hooks.h>
#include <syscalls.h>
#include <team.h>
#include <thread.h>
#include <util/AutoLock.h>


static const int32 kMaxShutdownHooks = 16;

struct registered_shutdown_hook {
	shutdown_hook	hook;
	void*			cookie;
};

static registered_shutdown_hook sShutdownHooks[kMaxShutdownHooks];
static int32 sShutdownHookCount;
static mutex sShutdownHooksLock = MUTEX_INITIALIZER("shutdown hooks");

// What system_shutdown() does after the teams were ended runs in a kernel
// thread of its own and is watched: a step that does not finish in its time
// (a driver's hook waiting for hardware that no longer answers, say) must not
// keep the machine from going off. With the teams gone already, such a
// machine answers pings and nothing else (airos-works/haiku #136).
static const char* volatile sShutdownStep;
static int64 sShutdownStepStart;
static int64 sShutdownStepLimit;
static int32 sShutdownStepsDone;

static const bigtime_t kShutdownTeamsLimit = 10000000;
static const bigtime_t kShutdownSyncLimit = 60000000;
static const bigtime_t kShutdownHookLimit = 20000000;


static void
shutdown_step(const char* step, bigtime_t limit)
{
	dprintf("shutdown: %s\n", step);
	sShutdownStep = step;
	atomic_set64(&sShutdownStepLimit, limit);
	atomic_set64(&sShutdownStepStart, system_time());
}


static void
call_shutdown_hooks(bool reboot)
{
	MutexLocker locker(sShutdownHooksLock);
	for (int32 i = sShutdownHookCount - 1; i >= 0; i--) {
		dprintf("shutdown: hook %" B_PRId32 " of %" B_PRId32 " (%p)\n",
			sShutdownHookCount - i, sShutdownHookCount,
			sShutdownHooks[i].hook);
		shutdown_step("a driver's shutdown hook", kShutdownHookLimit);
		sShutdownHooks[i].hook(reboot, sShutdownHooks[i].cookie);
	}
}


/*!	Gives the ended teams time to go: their drivers' cleanup (a GPU driver
	releasing a desktop's buffers, say) should be over before the drivers'
	shutdown hooks bring the hardware to rest. The caller's own team cannot
	end before the caller returns, and is not waited for. A team stuck in the
	kernel is waited for until \a limit, then logged.
*/
static void
wait_for_teams(team_id callerTeam, bigtime_t limit)
{
	bigtime_t start = system_time();
	while (true) {
		int32 cookie = 0;
		int32 left = 0;
		team_info info;
		while (get_next_team_info(&cookie, &info) == B_OK) {
			if (info.team != B_SYSTEM_TEAM && info.team != callerTeam) {
				// also one started while the teams were being ended
				kill_team(info.team);
				left++;
			}
		}

		bigtime_t waited = system_time() - start;
		if (left == 0) {
			dprintf("shutdown: the teams ended in %" B_PRId64 " ms\n",
				waited / 1000);
			return;
		}
		if (waited > limit)
			break;

		snooze(20000);
	}

	int32 cookie = 0;
	team_info info;
	while (get_next_team_info(&cookie, &info) == B_OK) {
		if (info.team == B_SYSTEM_TEAM || info.team == callerTeam)
			continue;
		dprintf("shutdown: team %" B_PRId32 " (%s) did not end in %" B_PRId64
			" s\n", info.team, info.args, limit / 1000000);
	}
}


static status_t
finish_shutdown(void* _reboot)
{
	bool reboot = _reboot != NULL;

	sync();

	// Hardware a reboot leaves as it is (a GPU integrated into the
	// processor, say) has to be brought to rest by its driver.
	call_shutdown_hooks(reboot);

	atomic_set(&sShutdownStepsDone, 1);
	return B_OK;
}


status_t
register_shutdown_hook(shutdown_hook hook, void* cookie)
{
	MutexLocker locker(sShutdownHooksLock);
	if (sShutdownHookCount == kMaxShutdownHooks)
		return B_NO_MEMORY;

	sShutdownHooks[sShutdownHookCount].hook = hook;
	sShutdownHooks[sShutdownHookCount].cookie = cookie;
	sShutdownHookCount++;
	return B_OK;
}


status_t
unregister_shutdown_hook(shutdown_hook hook, void* cookie)
{
	MutexLocker locker(sShutdownHooksLock);
	for (int32 i = 0; i < sShutdownHookCount; i++) {
		if (sShutdownHooks[i].hook == hook
			&& sShutdownHooks[i].cookie == cookie) {
			memmove(&sShutdownHooks[i], &sShutdownHooks[i + 1],
				(sShutdownHookCount - i - 1) * sizeof(sShutdownHooks[0]));
			sShutdownHookCount--;
			return B_OK;
		}
	}
	return B_ENTRY_NOT_FOUND;
}


status_t
system_shutdown(bool reboot)
{
	int32 cookie = 0;
	team_info info;

	gKernelShutdown = true;

	// Each step is logged: the EFI loader keeps the syslog of a session that
	// ended in a hang, and it shows where the shutdown stopped.
	dprintf("shutdown: %s requested by thread %" B_PRId32 "\n",
		reboot ? "reboot" : "power off", thread_get_current_thread_id());

	// Write back the caches before anything is torn down: ending the teams
	// runs their drivers' cleanup (GPU contexts, for one), and a machine that
	// stops there loses what was only cached - files come back with their
	// new size but the old contents of their blocks.
	dprintf("shutdown: syncing\n");
	sync();

	// Now shutdown all system services!
	// TODO: Once we are sure we can shutdown the system on all hardware
	// checking reboot may not be necessary anymore.
	if (reboot) {
		dprintf("shutdown: ending the teams\n");
		while (get_next_team_info(&cookie, &info) == B_OK) {
			if (info.team == B_SYSTEM_TEAM)
				continue;
			kill_team(info.team);
		}
		wait_for_teams(team_get_current_team_id(), kShutdownTeamsLimit);
	}

	// The rest runs in a kernel thread: for a reboot, the calling thread's
	// team was ended above, so it has a kill signal pending, which ends each of its
	// interruptible waits at once - a driver's hook would see all of them
	// fail. And a step that hangs must not keep the machine from going off.
	shutdown_step("syncing after the teams were ended", kShutdownSyncLimit);
	void* argument = reboot ? (void*)1 : NULL;
	atomic_set(&sShutdownStepsDone, 0);
	thread_id thread = spawn_kernel_thread(&finish_shutdown, "shutdown",
		B_URGENT_DISPLAY_PRIORITY, argument);
	status_t status = thread >= 0 ? resume_thread(thread) : thread;
	if (status != B_OK) {
		dprintf("shutdown: no thread for the last steps: %s\n",
			strerror(status));
		finish_shutdown(argument);
	}

	// snooze() is not ended by a pending signal
	while (atomic_get(&sShutdownStepsDone) == 0) {
		snooze(100000);
		bigtime_t running = system_time() - atomic_get64(&sShutdownStepStart);
		if (atomic_get(&sShutdownStepsDone) == 0
			&& running > atomic_get64(&sShutdownStepLimit)) {
			dprintf("shutdown: %s did not finish in %" B_PRId64 " s; going "
				"on without it\n", sShutdownStep, running / 1000000);
			break;
		}
	}

	dprintf("shutdown: %s\n", reboot ? "resetting" : "powering off");
	status = arch_cpu_shutdown(reboot);
	dprintf("shutdown: %s failed: %s\n", reboot ? "reset" : "power off",
		strerror(status));
	return status;
}


//	#pragma mark -


status_t
_user_shutdown(bool reboot)
{
	if (geteuid() != 0)
		return B_NOT_ALLOWED;
	return system_shutdown(reboot);
}
