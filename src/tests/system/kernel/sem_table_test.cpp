/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	The semaphore table is backed in steps as semaphores are created
	(airos-works/haiku#215). This test checks what must not change with that:

	- the whole table can still be used: several threads create semaphores
	  until the kernel says B_NO_MORE_SEMS, and that happens only when all
	  max_sems slots are in use;
	- every semaphore created works and has its own ID;
	- deleted IDs stay invalid, also after their slots are used again;
	- an ID whose slot was never backed is refused, not faulted on;
	- creating, using and deleting from several threads at once, while
	  another thread looks up random IDs and a waiter sees its semaphore
	  deleted.

	It prints the "sem_table" areas of the kernel before and after. While it
	runs, the system has no semaphores to spare for a moment: run it on a
	test machine. Pass "--no-fill" to leave out filling the table.
*/


#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>


static const int32 kFillThreads = 8;
static const int32 kChurnThreads = 6;
static const bigtime_t kChurnTime = 10 * 1000000LL;

static int32 sFailures = 0;


#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
			atomic_add(&sFailures, 1); \
		} \
	} while (false)


static void
deadline_handler(int)
{
	static const char kMessage[] = "FAILED: deadline passed\n";
	write(STDOUT_FILENO, kMessage, sizeof(kMessage) - 1);
	_exit(1);
}


static size_t
sem_table_bytes(int32* _areas)
{
	size_t bytes = 0;
	int32 areas = 0;
	ssize_t cookie = 0;
	area_info info;
	while (get_next_area_info(B_SYSTEM_TEAM, &cookie, &info) == B_OK) {
		if (strcmp(info.name, "sem_table") == 0) {
			bytes += info.ram_size;
			areas++;
		}
	}
	if (_areas != NULL)
		*_areas = areas;
	return bytes;
}


static void
print_state(const char* when)
{
	system_info info;
	get_system_info(&info);
	int32 areas;
	size_t bytes = sem_table_bytes(&areas);
	printf("%s: %" B_PRIu32 " of %" B_PRIu32 " semaphores used, sem_table "
		"%zu KiB in %" B_PRId32 " area(s)\n", when, info.used_sems,
		info.max_sems, bytes / 1024, areas);
}


static bool
sem_is_invalid(sem_id id)
{
	int32 count;
	sem_info info;
	bool invalid = true;
	if (get_sem_count(id, &count) != B_BAD_SEM_ID)
		invalid = false;
	if (get_sem_info(id, &info) != B_BAD_SEM_ID)
		invalid = false;
	if (release_sem_etc(id, 1, B_DO_NOT_RESCHEDULE) != B_BAD_SEM_ID)
		invalid = false;
	if (acquire_sem_etc(id, 1, B_RELATIVE_TIMEOUT, 0) != B_BAD_SEM_ID)
		invalid = false;
	if (delete_sem(id) != B_BAD_SEM_ID)
		invalid = false;
	return invalid;
}


// #pragma mark - filling the table


struct fill_data {
	sem_id*	ids;
	int32	capacity;
	int32	count;
	status_t error;
	int32	usedAtFailure;
};


static status_t
fill_thread(void* _data)
{
	fill_data* data = (fill_data*)_data;
	data->count = 0;
	data->error = B_OK;
	while (data->count < data->capacity) {
		sem_id id = create_sem(1, "sem table test");
		if (id < 0) {
			system_info info;
			get_system_info(&info);
			data->usedAtFailure = info.used_sems;
			data->error = id;
			break;
		}
		data->ids[data->count++] = id;
	}
	return B_OK;
}


static int
compare_ids(const void* a, const void* b)
{
	sem_id first = *(const sem_id*)a;
	sem_id second = *(const sem_id*)b;
	return first < second ? -1 : (first > second ? 1 : 0);
}


static void
test_fill(int32 maxSems)
{
	system_info info;
	get_system_info(&info);
	int32 usedBefore = info.used_sems;

	// no thread can be spawned once the table is full: start them all, then
	// let them go
	fill_data data[kFillThreads];
	thread_id threads[kFillThreads];
	sem_id* ids = (sem_id*)malloc(sizeof(sem_id) * maxSems);
	if (ids == NULL) {
		CHECK(false, "no memory");
		return;
	}
	for (int32 i = 0; i < kFillThreads; i++) {
		data[i].ids = (sem_id*)malloc(sizeof(sem_id) * maxSems);
		data[i].capacity = maxSems;
		data[i].usedAtFailure = -1;
		threads[i] = spawn_thread(fill_thread, "fill", B_NORMAL_PRIORITY,
			&data[i]);
	}
	bigtime_t start = system_time();
	for (int32 i = 0; i < kFillThreads; i++)
		resume_thread(threads[i]);

	int32 total = 0;
	int32 usedAtFull = -1;
	for (int32 i = 0; i < kFillThreads; i++) {
		status_t result;
		wait_for_thread(threads[i], &result);
		CHECK(data[i].error == B_NO_MORE_SEMS,
			"fill thread %" B_PRId32 " stopped with %s, not B_NO_MORE_SEMS",
			i, strerror(data[i].error));
		if (data[i].usedAtFailure > usedAtFull)
			usedAtFull = data[i].usedAtFailure;
		memcpy(ids + total, data[i].ids, sizeof(sem_id) * data[i].count);
		total += data[i].count;
	}
	bigtime_t fillTime = system_time() - start;

	print_state("table full");
	printf("filled: %" B_PRId32 " created by %" B_PRId32 " threads in %"
		B_PRId64 " ms (%" B_PRId32 " were in use before)\n", total,
		kFillThreads, fillTime / 1000, usedBefore);
	// other teams may delete one in the meantime
	CHECK(usedAtFull >= maxSems - 16, "B_NO_MORE_SEMS with %" B_PRId32
		" of %" B_PRId32 " in use", usedAtFull, maxSems);
	CHECK(total >= maxSems - usedBefore - 512, "only %" B_PRId32
		" semaphores could be created", total);

	// all distinct, all in distinct slots, all working
	qsort(ids, total, sizeof(sem_id), compare_ids);
	int32 duplicates = 0;
	for (int32 i = 1; i < total; i++) {
		if (ids[i] == ids[i - 1])
			duplicates++;
	}
	CHECK(duplicates == 0, "%" B_PRId32 " duplicate IDs", duplicates);

	uint8* slots = (uint8*)calloc(maxSems, 1);
	int32 sharedSlots = 0;
	int32 broken = 0;
	for (int32 i = 0; i < total; i++) {
		int32 slot = ids[i] % maxSems;
		if (slots[slot]++ != 0)
			sharedSlots++;
		int32 count = -1;
		if (acquire_sem_etc(ids[i], 1, B_RELATIVE_TIMEOUT, 0) != B_OK
			|| get_sem_count(ids[i], &count) != B_OK || count != 0
			|| release_sem_etc(ids[i], 1, B_DO_NOT_RESCHEDULE) != B_OK) {
			broken++;
		}
	}
	free(slots);
	CHECK(sharedSlots == 0, "%" B_PRId32 " semaphores share a slot",
		sharedSlots);
	CHECK(broken == 0, "%" B_PRId32 " semaphores do not work", broken);

	// delete them all, then check they are gone
	int32 deleteErrors = 0;
	for (int32 i = 0; i < total; i++) {
		if (delete_sem(ids[i]) != B_OK)
			deleteErrors++;
	}
	CHECK(deleteErrors == 0, "%" B_PRId32 " delete_sem() calls failed",
		deleteErrors);
	print_state("deleted");

	int32 stillValid = 0;
	for (int32 i = 0; i < total; i++) {
		if (!sem_is_invalid(ids[i]))
			stillValid++;
	}
	CHECK(stillValid == 0, "%" B_PRId32 " deleted IDs still work",
		stillValid);

	// recycle: create as many again; no new ID may be an old one, and the
	// old ones must stay invalid
	int32 recycled = 0;
	sem_id* newIDs = (sem_id*)malloc(sizeof(sem_id) * total);
	for (int32 i = 0; i < total; i++) {
		newIDs[i] = create_sem(0, "sem table test recycled");
		if (newIDs[i] < 0)
			break;
		recycled++;
		if (bsearch(&newIDs[i], ids, total, sizeof(sem_id), compare_ids)
				!= NULL) {
			CHECK(false, "ID %" B_PRId32 " handed out again", newIDs[i]);
		}
	}
	CHECK(recycled >= total - 512, "only %" B_PRId32 " of %" B_PRId32
		" recreated", recycled, total);
	stillValid = 0;
	for (int32 i = 0; i < total; i++) {
		if (!sem_is_invalid(ids[i]))
			stillValid++;
	}
	CHECK(stillValid == 0, "%" B_PRId32 " stale IDs work after their slots "
		"were used again", stillValid);
	for (int32 i = 0; i < recycled; i++)
		delete_sem(newIDs[i]);
	printf("recycled: %" B_PRId32 " created in the freed slots, stale IDs "
		"refused\n", recycled);

	free(newIDs);
	for (int32 i = 0; i < kFillThreads; i++)
		free(data[i].ids);
	free(ids);
}


// #pragma mark - IDs that were never handed out


static void
test_unused_ids(int32 maxSems)
{
	// Every slot, in the first two generations, plus some far away. A slot
	// that is not backed must be refused like any other bad ID; the two
	// lookups must agree for one that is.
	int32 valid = 0;
	int32 disagree = 0;
	for (int64 id = 0; id < 2 * (int64)maxSems; id++) {
		int32 count;
		sem_info info;
		status_t countError = get_sem_count((sem_id)id, &count);
		status_t infoError = get_sem_info((sem_id)id, &info);
		if ((countError == B_OK) != (infoError == B_OK))
			disagree++;
		if (countError == B_OK)
			valid++;
		else if (countError != B_BAD_SEM_ID)
			disagree++;
	}
	for (int32 i = 0; i < 100000; i++) {
		sem_id id = (sem_id)((((uint32)rand() << 16) ^ (uint32)rand())
			& 0x7fffffff);
		int32 count;
		status_t error = get_sem_count(id, &count);
		if (error != B_OK && error != B_BAD_SEM_ID)
			disagree++;
		release_sem_etc(-1 - i, 1, B_DO_NOT_RESCHEDULE);
	}
	CHECK(disagree == 0, "%" B_PRId32 " lookups of unused IDs went wrong",
		disagree);
	printf("unused IDs: %" B_PRId32 " IDs looked up, %" B_PRId32
		" of them live\n", 2 * maxSems + 100000, valid);
}


// #pragma mark - concurrent create/delete


static int32 sStop = 0;
static int32 sChurnCreated = 0;


static status_t
churn_thread(void*)
{
	sem_id batch[300];
	int32 rounds = 0;
	while (atomic_get(&sStop) == 0) {
		// a batch now and then, so the table has to grow and the free list
		// gets shuffled
		int32 batchSize = (rounds % 16 == 0) ? 300 : 1;
		int32 created = 0;
		for (; created < batchSize; created++) {
			batch[created] = create_sem(1, "churn");
			if (batch[created] < 0) {
				CHECK(false, "create_sem: %s", strerror(batch[created]));
				break;
			}
		}
		for (int32 i = 0; i < created; i++) {
			CHECK(acquire_sem_etc(batch[i], 1, B_RELATIVE_TIMEOUT, 1000000)
				== B_OK, "acquire of a new semaphore failed");
			CHECK(release_sem(batch[i]) == B_OK, "release failed");
		}
		for (int32 i = 0; i < created; i++) {
			CHECK(delete_sem(batch[i]) == B_OK, "delete failed");
			CHECK(sem_is_invalid(batch[i]), "deleted ID %" B_PRId32
				" still works", batch[i]);
		}
		atomic_add(&sChurnCreated, created);
		rounds++;
	}
	return B_OK;
}


static status_t
lookup_thread(void* _maxSems)
{
	int32 maxSems = (int32)(addr_t)_maxSems;
	uint32 seed = 1;
	while (atomic_get(&sStop) == 0) {
		seed = seed * 1103515245 + 12345;
		sem_id id = (sem_id)(seed % (uint32)(4 * maxSems));
		int32 count;
		sem_info info;
		get_sem_count(id, &count);
		get_sem_info(id, &info);
	}
	return B_OK;
}


static status_t
waiter_thread(void* _sem)
{
	sem_id sem = (sem_id)(addr_t)_sem;
	return acquire_sem_etc(sem, 1, B_RELATIVE_TIMEOUT, 10000000);
}


static void
test_concurrent(int32 maxSems)
{
	thread_id churn[kChurnThreads];
	for (int32 i = 0; i < kChurnThreads; i++) {
		churn[i] = spawn_thread(churn_thread, "churn", B_NORMAL_PRIORITY,
			NULL);
		resume_thread(churn[i]);
	}
	thread_id lookup = spawn_thread(lookup_thread, "lookup",
		B_NORMAL_PRIORITY, (void*)(addr_t)maxSems);
	resume_thread(lookup);

	// waiters on semaphores that get deleted under them
	int32 waitersWoken = 0;
	bigtime_t end = system_time() + kChurnTime;
	while (system_time() < end) {
		sem_id sem = create_sem(0, "deleted under waiter");
		thread_id waiter = spawn_thread(waiter_thread, "waiter",
			B_NORMAL_PRIORITY, (void*)(addr_t)sem);
		resume_thread(waiter);
		snooze(2000);
		delete_sem(sem);
		status_t result = B_OK;
		wait_for_thread(waiter, &result);
		CHECK(result == B_BAD_SEM_ID, "waiter woke with %s, not "
			"B_BAD_SEM_ID", strerror(result));
		waitersWoken++;
	}

	atomic_set(&sStop, 1);
	status_t result;
	for (int32 i = 0; i < kChurnThreads; i++)
		wait_for_thread(churn[i], &result);
	wait_for_thread(lookup, &result);

	printf("concurrent: %" B_PRId32 " created and deleted by %" B_PRId32
		" threads, %" B_PRId32 " waiters woken by delete_sem\n",
		sChurnCreated, kChurnThreads, waitersWoken);
}


int
main(int argc, char** argv)
{
	bool fill = argc < 2 || strcmp(argv[1], "--no-fill") != 0;

	signal(SIGALRM, deadline_handler);
	alarm(600);

	system_info info;
	get_system_info(&info);
	int32 maxSems = info.max_sems;

	print_state("start");
	test_unused_ids(maxSems);
	if (fill)
		test_fill(maxSems);
	test_concurrent(maxSems);
	test_unused_ids(maxSems);
	print_state("end");

	printf("%s\n", sFailures == 0 ? "PASSED" : "FAILED");
	return sFailures == 0 ? 0 : 1;
}
