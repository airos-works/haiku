/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Modified pages of a file that is being removed, against a write that
	waits for the modified page quota (airos-works/haiku#146).

	A device's page writer cannot write the pages of a vnode that is being
	freed, and leaves them in its queue. The vnode of a removed file is freed
	by its file system, which needs its journal for that. A write that grows
	a file holds the journal while it waits for the device's modified pages
	to get under the quota. With enough pages of the removed file left, that
	was for ever: the write waited for pages only the vnode's removal takes
	away, the removal for the write's journal, and the page writer went
	through its queue at full speed (100 % of a CPU, no I/O) without finding
	a page it could write. Every later write to the volume that needs the
	journal hung behind them.

	The test builds that state on a slow device the page writer has no
	estimate of yet (one just attached: without an estimate there is no
	quota). It writes the first part of a large file (V), a small file (X)
	and the rest of V, which puts their pages on the page writer's queue in
	that order, and removes V. A thread appends to a third file (W) in small
	writes; as soon as the page writer has timed its first run, one of them
	has to wait for the quota. Then V's descriptor is closed. While the page
	writer writes X only its runs hold no page of V: V's vnode loses its last
	reference and is freed, with the rest of V still on the queue.

	On a device that is too fast, or one the page writer limits writes to
	already, the state does not come about and the test skips itself or
	passes without having tested anything: it is a test for a rig (QEMU with
	a USB stick throttled to 4 MiB/s), not for every boot.

	page_writer_stall_test [directory]   (default: /tmp)
*/


#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <OS.h>


static const size_t kMiB = 1024 * 1024;
static const size_t kFirstPartSize = 16 * kMiB;
	// of V; the page writer is busy with it while the write gets to wait
static const size_t kRestSize = 32 * kMiB;
	// of V; what has to keep the device over its quota (3 s of writing)
static const size_t kOtherFileSize = 4 * kMiB;
static const size_t kAppendSize = 64 * 1024;
static const bigtime_t kAppendInterval = 100000;
static const bigtime_t kBlockedAfter = 400000;
static const bigtime_t kBlockDeadline = 8 * 1000 * 1000;
static const bigtime_t kWriteDeadline = 30 * 1000 * 1000;

static char sDirectory[B_PATH_NAME_LENGTH - 16];
	// (room for the names of the files in it)
static int32 sAppends;
	// writes that have returned
static int32 sStop;
static int sAppendError;


static void
path_for(char* path, const char* name)
{
	snprintf(path, B_PATH_NAME_LENGTH, "%s/%s", sDirectory, name);
}


/*!	The CPU time of all page writer threads. */
static bigtime_t
page_writer_time()
{
	bigtime_t time = 0;
	int32 cookie = 0;
	thread_info info;
	while (get_next_thread_info(B_SYSTEM_TEAM, &cookie, &info) == B_OK) {
		if (strncmp(info.name, "page writer", 11) == 0)
			time += info.kernel_time + info.user_time;
	}
	return time;
}


static void*
appender(void*)
{
	char path[B_PATH_NAME_LENGTH];
	path_for(path, "W");
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		sAppendError = errno;
		return NULL;
	}

	static char buffer[kAppendSize];
	memset(buffer, 'w', sizeof(buffer));

	// each write grows the file: the file system keeps its journal for it
	while (atomic_get(&sStop) == 0) {
		if (write(fd, buffer, sizeof(buffer)) != (ssize_t)sizeof(buffer)) {
			sAppendError = errno;
			break;
		}
		atomic_add(&sAppends, 1);
		snooze(kAppendInterval);
	}

	close(fd);
	return NULL;
}


static bool
write_all(int fd, const char* buffer, size_t size)
{
	while (size > 0) {
		ssize_t written = write(fd, buffer, size);
		if (written <= 0)
			return false;
		buffer += written;
		size -= written;
	}
	return true;
}


static int
skip(const char* why)
{
	printf("SKIP: %s\n", why);

	char path[B_PATH_NAME_LENGTH];
	static const char* const kNames[] = { "V", "X", "W", "after" };
	for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
		path_for(path, kNames[i]);
		unlink(path);
	}
	rmdir(sDirectory);
	return 0;
}


int
main(int argc, char** argv)
{
	snprintf(sDirectory, sizeof(sDirectory), "%s/page_writer_stall_test-%d",
		argc > 1 ? argv[1] : "/tmp", (int)getpid());
	if (mkdir(sDirectory, 0755) != 0) {
		printf("SKIP: cannot create %s: %s\n", sDirectory, strerror(errno));
		return 0;
	}

	char pathV[B_PATH_NAME_LENGTH], pathX[B_PATH_NAME_LENGTH],
		pathW[B_PATH_NAME_LENGTH];
	path_for(pathV, "V");
	path_for(pathX, "X");
	path_for(pathW, "W");

	printf("page_writer_stall_test: %zu MiB of a file that is removed and "
		"%zu MiB of another in %s\n", (kFirstPartSize + kRestSize) / kMiB,
		kOtherFileSize / kMiB, sDirectory);

	char* data = (char*)malloc(kRestSize);
	if (data == NULL)
		return skip("no memory");
	memset(data, 'v', kRestSize);

	int fdV = open(pathV, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	int fdX = open(pathX, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fdV < 0 || fdX < 0)
		return skip("cannot create the files");

	// onto the page writer's queue: V's first part, X, the rest of V
	bigtime_t start = system_time();
	if (!write_all(fdV, data, kFirstPartSize)
		|| !write_all(fdX, data, kOtherFileSize)
		|| !write_all(fdV, data, kRestSize)) {
		close(fdV);
		close(fdX);
		return skip("no space for the files");
	}
	const bigtime_t fillTime = system_time() - start;
	free(data);
	close(fdX);

	if (fillTime > 2000000) {
		// what was written first is on the disk by now
		close(fdV);
		return skip("the page writer limits writes to this device already: "
			"the test needs one it has no estimate of (just attached)");
	}

	if (unlink(pathV) != 0) {
		printf("FAIL: removing V: %s\n", strerror(errno));
		return 1;
	}

	// the writes of which one has to wait for the quota, with its file
	// system's journal: once the page writer has timed its first run
	pthread_t thread;
	if (pthread_create(&thread, NULL, &appender, NULL) != 0) {
		printf("FAIL: starting the appending thread\n");
		return 1;
	}

	start = system_time();
	int32 appends = 0;
	bigtime_t lastAppend = start;
	bool blocked = false;
	while (system_time() - start < kBlockDeadline) {
		snooze(20000);
		const int32 now = atomic_get(&sAppends);
		if (now != appends) {
			appends = now;
			lastAppend = system_time();
		} else if (system_time() - lastAppend > kBlockedAfter) {
			blocked = true;
			break;
		}
	}

	if (!blocked) {
		atomic_set(&sStop, 1);
		pthread_join(thread, NULL);
		close(fdV);
		return skip("no write had to wait for the modified page quota: the "
			"device is too fast for this test");
	}

	// V's last reference but those of the page writer's runs
	const bigtime_t blockedSince = lastAppend;
	const bigtime_t writerTimeBefore = page_writer_time();
	const bigtime_t closed = system_time();
	close(fdV);

	while (atomic_get(&sAppends) == appends
		&& system_time() - closed < kWriteDeadline) {
		snooze(50000);
	}

	const bigtime_t now = system_time();
	const bigtime_t writerTime = page_writer_time() - writerTimeBefore;
	const bool returned = atomic_get(&sAppends) != appends;

	printf("  after %d appended blocks one waited for the quota; it %s "
		"%.1f s after it began, %.1f s after the removed file was closed; "
		"the page writers used %.2f s of CPU time meanwhile\n", (int)appends,
		returned ? "returned" : "had not returned",
		(now - blockedSince) / 1e6, (now - closed) / 1e6, writerTime / 1e6);

	int result = 0;
	if (!returned) {
		printf("FAIL: a write waited more than %d s for the modified page "
			"quota behind the pages of a file that is being removed\n",
			(int)(kWriteDeadline / 1000000));
		result = 1;
	}
	if (now - closed > 3000000 && writerTime > (now - closed) / 2) {
		printf("FAIL: a page writer was running for %.0f %% of the time "
			"while there was nothing it could write\n",
			100.0 * writerTime / (now - closed));
		result = 1;
	}

	// Leaving ends the write, if it still waits, and with it the state.
	if (!returned)
		_exit(result);

	atomic_set(&sStop, 1);
	pthread_join(thread, NULL);
	if (sAppendError != 0) {
		printf("FAIL: appending failed: %s\n", strerror(sAppendError));
		result = 1;
	}

	// the volume goes on taking files
	start = system_time();
	char pathA[B_PATH_NAME_LENGTH];
	path_for(pathA, "after");
	int fd = open(pathA, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, "after", 5) != 5 || fsync(fd) != 0) {
		printf("FAIL: a file created afterwards: %s\n", strerror(errno));
		result = 1;
	}
	if (fd >= 0)
		close(fd);
	printf("  a file created and synced afterwards in %.2f s\n",
		(system_time() - start) / 1e6);

	unlink(pathA);
	unlink(pathW);
	unlink(pathX);
	rmdir(sDirectory);

	if (result == 0)
		printf("PASS\n");
	return result;
}
