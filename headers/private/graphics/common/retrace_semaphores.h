/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RETRACE_SEMAPHORES_H
#define _RETRACE_SEMAPHORES_H


/*!	For a display driver: the semaphores it releases at every vertical blank.
	They are what its accelerant's B_ACCELERANT_RETRACE_SEMAPHORE hands out,
	so what app_server waits on before it copies drawing to the screen
	(B_WAIT_FOR_RETRACE), and what BScreen::WaitForRetrace() waits on.

	Userland may not acquire a semaphore the kernel owns, so every team that
	asks gets a semaphore of its own, owned by that team: app_server's lives
	as long as app_server, which gives it to its clients. A semaphore whose
	team is gone is dropped at the next blank.

	Get() is not for interrupt handlers, and its callers serialize it (the
	driver's lock). Release() is for interrupt handlers as well: it lets
	every thread that waits go, and leaves nothing for threads that come
	later, so a wait always ends at a blank still to come.
*/


#include <KernelExport.h>
#include <OS.h>


class RetraceSemaphores {
public:
	RetraceSemaphores()
	{
		for (int32 i = 0; i < kCount; i++)
			fSemaphores[i] = -1;
	}

	~RetraceSemaphores()
	{
		for (int32 i = 0; i < kCount; i++) {
			if (fSemaphores[i] >= 0)
				delete_sem(fSemaphores[i]);
		}
	}

	sem_id Get()
	{
		thread_info thread;
		status_t status = get_thread_info(find_thread(NULL), &thread);
		if (status != B_OK)
			return status;

		int32 free = -1;
		for (int32 i = 0; i < kCount; i++) {
			sem_id semaphore = atomic_get(&fSemaphores[i]);
			sem_info info;
			if (semaphore >= 0 && get_sem_info(semaphore, &info) != B_OK) {
				// its team is gone
				atomic_test_and_set(&fSemaphores[i], -1, semaphore);
				semaphore = -1;
			}
			if (semaphore < 0) {
				if (free < 0)
					free = i;
				continue;
			}
			if (info.team == thread.team)
				return semaphore;
		}
		if (free < 0)
			return B_NO_MORE_SEMS;

		sem_id semaphore = create_sem(0, "retrace");
		if (semaphore < 0)
			return semaphore;
		status = set_sem_owner(semaphore, thread.team);
		if (status != B_OK) {
			delete_sem(semaphore);
			return status;
		}
		atomic_set(&fSemaphores[free], semaphore);
		return semaphore;
	}

	// Whether a thread waits for the next blank.
	bool HasWaiters()
	{
		for (int32 i = 0; i < kCount; i++) {
			int32 count;
			sem_id semaphore = atomic_get(&fSemaphores[i]);
			if (semaphore >= 0 && get_sem_count(semaphore, &count) == B_OK
				&& count < 0) {
				return true;
			}
		}
		return false;
	}

	// At a vertical blank. Returns whether a thread was let go.
	bool Release()
	{
		bool released = false;
		for (int32 i = 0; i < kCount; i++) {
			sem_id semaphore = atomic_get(&fSemaphores[i]);
			if (semaphore < 0)
				continue;
			int32 count;
			if (get_sem_count(semaphore, &count) != B_OK) {
				atomic_test_and_set(&fSemaphores[i], -1, semaphore);
				continue;
			}
			if (count < 0) {
				release_sem_etc(semaphore, 0,
					B_DO_NOT_RESCHEDULE | B_RELEASE_ALL);
				released = true;
			}
		}
		return released;
	}

private:
	static const int32 kCount = 8;

	sem_id fSemaphores[kCount];
};


#endif	/* _RETRACE_SEMAPHORES_H */
