/*
 * Copyright 2009, Olivier Coursière. All rights reserved.
 * Copyright 2004, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <string.h>

#include <kernel.h>
#include <lock.h>
#include <shutdown_hooks.h>
#include <syscalls.h>
#include <util/AutoLock.h>


static const int32 kMaxShutdownHooks = 16;

struct registered_shutdown_hook {
	shutdown_hook	hook;
	void*			cookie;
};

static registered_shutdown_hook sShutdownHooks[kMaxShutdownHooks];
static int32 sShutdownHookCount;
static mutex sShutdownHooksLock = MUTEX_INITIALIZER("shutdown hooks");


static void
call_shutdown_hooks(bool reboot)
{
	MutexLocker locker(sShutdownHooksLock);
	for (int32 i = sShutdownHookCount - 1; i >= 0; i--)
		sShutdownHooks[i].hook(reboot, sShutdownHooks[i].cookie);
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
	
	// Now shutdown all system services!
	// TODO: Once we are sure we can shutdown the system on all hardware
	// checking reboot may not be necessary anymore.
	if (reboot) {
		while (get_next_team_info(&cookie, &info) == B_OK) {
			if (info.team == B_SYSTEM_TEAM)
				continue;
			kill_team(info.team);
		}
	}
	
	sync();

	// Hardware a reboot leaves as it is (a GPU integrated into the
	// processor, say) has to be brought to rest by its driver.
	call_shutdown_hooks(reboot);

	return arch_cpu_shutdown(reboot);
}


//	#pragma mark -


status_t
_user_shutdown(bool reboot)
{
	if (geteuid() != 0)
		return B_NOT_ALLOWED;
	return system_shutdown(reboot);
}

