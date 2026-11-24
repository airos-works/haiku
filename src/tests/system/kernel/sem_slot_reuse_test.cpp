/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	A new semaphore must not report the "latest holder" of the semaphore
	that had its slot before (airos-works/haiku#153): get_sem_info() named a
	thread that never touched it.

	Semaphores are created, acquired and deleted one after the other until
	the kernel has been through its list of free slots once and hands out
	the ones this test left; each new one must say that nobody holds it.
*/


#include <stdio.h>
#include <string.h>

#include <OS.h>


int
main()
{
	system_info systemInfo;
	get_system_info(&systemInfo);
	const int32 rounds = systemInfo.max_sems + 64;

	const thread_id self = find_thread(NULL);
	int32 stale = 0;
	int32 staleSelf = 0;
	thread_id example = 0;

	const bigtime_t end = system_time() + 60 * 1000000LL;
	int32 i;
	for (i = 0; i < rounds && system_time() < end; i++) {
		sem_id sem = create_sem(1, "slot reuse test");
		if (sem < 0) {
			printf("create_sem() failed: %s\n", strerror(sem));
			return 1;
		}

		sem_info info;
		if (get_sem_info(sem, &info) != B_OK) {
			printf("get_sem_info() failed\n");
			return 1;
		}
		if (info.latest_holder != 0) {
			stale++;
			if (info.latest_holder == self)
				staleSelf++;
			else if (example == 0)
				example = info.latest_holder;
		}

		acquire_sem(sem);
		delete_sem(sem);
	}

	printf("%" B_PRId32 " semaphores created (the system has %" B_PRId32
		" slots), %" B_PRId32 " had a latest holder when new", i,
		systemInfo.max_sems, stale);
	if (stale > 0) {
		printf(": %" B_PRId32 " this thread (%" B_PRId32 ") from the "
			"semaphore before, %" B_PRId32 " others (thread %" B_PRId32
			" for one)", staleSelf, self, stale - staleSelf, example);
	}
	printf("\n%s\n", stale == 0 ? "PASSED" : "FAILED");
	return stale == 0 ? 0 : 1;
}
