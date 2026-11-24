/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	dead_threads_test [threads]: what a team keeps of its exited threads
	(airos-works/haiku#190).

	Every thread that ends with nobody waiting for it leaves an entry in its
	team, for a wait_for_thread() or pthread_join() that comes later. The
	list of them was searched from its oldest entry and never trimmed: after
	200,000 detached threads a pthread_join() of a finished thread took
	300 us instead of 1.3, with the team's lock held.

	Checks that
	- threads that have all ended can still be joined later, each with its
	  own result (2000 of them: far more than the limit of 32 the kernel
	  once had, far less than the one it has now);
	- joining a finished thread costs about the same after many detached
	  threads have come and gone (default 100,000) as before: not more than
	  10 times as much, or 20 us (the median of 51 joins).
*/


#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>


static void*
nothing(void* argument)
{
	return argument;
}


static int
compare_times(const void* a, const void* b)
{
	bigtime_t difference = *(const bigtime_t*)a - *(const bigtime_t*)b;
	return difference < 0 ? -1 : (difference > 0 ? 1 : 0);
}


/*!	The median of what joining a finished thread takes, in nanoseconds
	(the median: on a loaded machine single joins take many times as long).
*/
static double
join_cost()
{
	const int kRounds = 51;
	bigtime_t times[kRounds];
	for (int i = 0; i < kRounds; i++) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, nothing, NULL) != 0) {
			i--;
			snooze(1000);
			continue;
		}
		snooze(2000);
			// let it finish first
		bigtime_t start = system_time_nsecs();
		pthread_join(thread, NULL);
		times[i] = system_time_nsecs() - start;
	}
	qsort(times, kRounds, sizeof(times[0]), compare_times);
	return times[kRounds / 2] / 1000.0;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	const int count = argc > 1 ? atoi(argv[1]) : 100000;
	bool ok = true;

	// join later
	const int kJoinLater = 2000;
	static pthread_t threads[kJoinLater];
	int created = 0;
	for (int i = 0; i < kJoinLater; i++) {
		int error;
		while ((error = pthread_create(&threads[i], NULL, nothing,
				(void*)(addr_t)(i + 1))) == EAGAIN) {
			snooze(1000);
		}
		if (error != 0) {
			printf("pthread_create() failed: %s\n", strerror(error));
			return 1;
		}
		created++;
	}
	snooze(500000);
	int wrong = 0;
	for (int i = 0; i < created; i++) {
		void* result = NULL;
		int error = pthread_join(threads[i], &result);
		if (error != 0 || result != (void*)(addr_t)(i + 1)) {
			if (wrong++ == 0) {
				printf("joining thread %d of %d: %s, result %p\n", i, created,
					strerror(error), result);
			}
		}
	}
	printf("%d threads joined after all of them had ended: %d failed\n",
		created, wrong);
	ok &= wrong == 0;

	// the cost of a join with many exited threads behind
	const double before = join_cost();
	printf("pthread_join() of a finished thread, before: %.1f us\n", before);

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	const bigtime_t deadline = system_time() + 240 * 1000000LL;
	int made = 0;
	double after = before;
	for (int step = 0; step < 4 && system_time() < deadline; step++) {
		for (int i = 0; i < count / 4 && system_time() < deadline; i++) {
			pthread_t thread;
			while (pthread_create(&thread, &attr, nothing, NULL) != 0)
				snooze(1000);
			if ((i & 63) == 0)
				snooze(200);
			made++;
		}
		snooze(200000);
		after = join_cost();
		printf("after %d detached threads have exited: %.1f us\n", made,
			after);
	}

	if (after > 20 && after > before * 10) {
		printf("joining got %.0f times as expensive\n", after / before);
		ok = false;
	}

	printf("%s\n", ok ? "PASSED" : "FAILED");
	return ok ? 0 : 1;
}
