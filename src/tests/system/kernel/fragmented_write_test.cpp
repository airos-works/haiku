/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	The page writer's writes of badly fragmented files against a stream of
	metadata writes.

	A page writer transfer of a file in more than eight extents is sent in
	parts: the disk driver's report of the first part starts the next one
	(do_iterative_fd_io_iterate()), on the thread that reports. nvme_disk sent
	that part from there, which waits for the lock that keeps bounced writes
	(the journal's and the block cache's) apart from the others. A bounced
	write that was waiting for the lock waited in turn for the transfers the
	reporting thread had yet to report or complete: from then on every write
	to the disk blocked, and reads went on (airos-works/haiku#141; a desktop
	whose Bluetooth mouse stopped, as its add-on wrote a status file).

	The test interleaves the blocks of a few files (a page appended to each in
	turn, closed every time), then has one thread per file write it over and
	over without syncing, which leaves the writing to the page writer, while
	other threads create, sync and remove small files. A disk driver with the
	bug stops within seconds; then no metadata operation finishes any more.
	The threads run in a child team, as they cannot be interrupted once they
	wait for the disk: the test ends without them. Where the volume cannot be
	written, or is too slow to set the files up in time (an emulated machine
	from a USB stick), there is nothing to test, and the test passes. Prints
	PASS or FAIL, exits 0 on PASS.

	fragmented_write_test [directory [seconds]]
*/


#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>


static const int kFiles = 8;
static const size_t kFileSize = 2 * 1024 * 1024;
static const size_t kMinFileSize = 256 * 1024;
	// (64 extents at least: transfers of more than eight, in several parts)
static const int kMetaThreads = 4;
static const bigtime_t kDefaultDuration = 10000000;
static const bigtime_t kStallTimeout = 8000000;
static const bigtime_t kSetUpTime = 8000000;
static const bigtime_t kSetUpTimeout = 20000000;


struct shared_state {
	int32	ready;
		// 1: the files are there, -1: the volume is too slow
	int32	fileSize;
	int32	chunks;
	int32	metaOperations;
};

static shared_state* sState;
static char sDirectory[512];


static void
file_path(char* path, size_t size, int index)
{
	snprintf(path, size, "%s/fragmented%d", sDirectory, index);
}


/*!	Appends a page to each file in turn and closes the file every time, which
	trims what the file system allocated ahead: the files' blocks interleave.
	Stops after kSetUpTime, with smaller files.
	\return The size of each file, 0 on error.
*/
static size_t
create_fragmented_files()
{
	char page[B_PAGE_SIZE];
	char path[B_PATH_NAME_LENGTH];
	const bigtime_t start = system_time();
	size_t offset = 0;
	for (; offset < kFileSize; offset += sizeof(page)) {
		if (system_time() - start > kSetUpTime)
			break;
		for (int i = 0; i < kFiles; i++) {
			file_path(path, sizeof(path), i);
			int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (fd < 0) {
				printf("  cannot create %s: %s\n", path, strerror(errno));
				return 0;
			}
			memset(page, 'a' + i, sizeof(page));
			ssize_t written = write(fd, page, sizeof(page));
			close(fd);
			if (written != (ssize_t)sizeof(page)) {
				printf("  cannot write %s: %s\n", path, strerror(errno));
				return 0;
			}
		}
	}
	return offset;
}


static void*
file_writer(void* data)
{
	const int index = (int)(addr_t)data;
	char path[B_PATH_NAME_LENGTH];
	file_path(path, sizeof(path), index);
	int fd = open(path, O_RDWR);
	if (fd < 0)
		return NULL;

	static const size_t kChunk = 65536;
	char* buffer = (char*)malloc(kChunk);
	if (buffer == NULL)
		return NULL;
	const size_t fileSize = (size_t)atomic_get(&sState->fileSize);
	for (uint32 pass = 0;; pass++) {
		memset(buffer, 'A' + pass % 26, kChunk);
		for (size_t offset = 0; offset + kChunk <= fileSize; offset += kChunk) {
			if (pwrite(fd, buffer, kChunk, offset) < 0)
				return NULL;
			atomic_add(&sState->chunks, 1);
		}
	}
	return NULL;
}


static void*
metadata_writer(void* data)
{
	const int index = (int)(addr_t)data;
	char path[B_PATH_NAME_LENGTH];
	for (uint32 count = 0;; count++) {
		snprintf(path, sizeof(path), "%s/fragmented-meta%d-%" B_PRIu32,
			sDirectory, index, count % 64);
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0)
			continue;
		if (write(fd, path, strlen(path)) < 0) {
			// (still counts as stuck when this goes on)
		}
		// writes the log: a run of writes that the disk driver may bounce
		fsync(fd);
		close(fd);
		unlink(path);
		atomic_add(&sState->metaOperations, 1);
	}
	return NULL;
}


static int
child_main()
{
	const size_t fileSize = create_fragmented_files();
	if (fileSize == 0)
		return 1;
	atomic_set(&sState->fileSize, (int32)fileSize);
	if (fileSize < kMinFileSize) {
		atomic_set(&sState->ready, -1);
		return 0;
	}
	atomic_set(&sState->ready, 1);

	pthread_t thread;
	for (int i = 0; i < kFiles; i++)
		pthread_create(&thread, NULL, file_writer, (void*)(addr_t)i);
	for (int i = 0; i < kMetaThreads; i++)
		pthread_create(&thread, NULL, metadata_writer, (void*)(addr_t)i);

	// until the parent ends this team
	while (true)
		snooze(1000000);
	return 0;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

	const char* directory = argc > 1 ? argv[1] : "/tmp";
	const bigtime_t duration = argc > 2
		? (bigtime_t)atoi(argv[2]) * 1000000 : kDefaultDuration;
	snprintf(sDirectory, sizeof(sDirectory), "%s/fragmented_write_test-%d",
		directory, (int)getpid());
	if (mkdir(sDirectory, 0755) != 0) {
		printf("  cannot create %s: %s\n", sDirectory, strerror(errno));
		printf("PASS (nothing tested: no volume to write to)\n");
		return 0;
	}

	sState = (shared_state*)mmap(NULL, B_PAGE_SIZE, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sState == MAP_FAILED) {
		printf("FAIL: no shared memory: %s\n", strerror(errno));
		return 1;
	}
	memset(sState, 0, sizeof(shared_state));

	printf("fragmented_write_test: %d files of %zu KiB in %s, %d metadata "
		"threads, %d s\n", kFiles, kFileSize / 1024, sDirectory, kMetaThreads,
		(int)(duration / 1000000));

	pid_t child = fork();
	if (child < 0) {
		printf("FAIL: fork: %s\n", strerror(errno));
		return 1;
	}
	if (child == 0)
		_exit(child_main());

	const char* failure = NULL;
	bool slow = false;
	const bigtime_t start = system_time();
	bigtime_t ready = -1;
	bigtime_t lastProgress = start;
	int32 lastOperations = 0;
	while (true) {
		snooze(250000);
		const bigtime_t now = system_time();

		if (ready < 0 && atomic_get(&sState->ready) < 0) {
			printf("  %d KiB of each file in %d s\n",
				(int)(atomic_get(&sState->fileSize) / 1024),
				(int)(kSetUpTime / 1000000));
			slow = true;
			break;
		}

		int status;
		if (waitpid(child, &status, WNOHANG) == child) {
			failure = "the files could not be set up";
			child = -1;
			break;
		}

		if (ready < 0) {
			if (atomic_get(&sState->ready) != 0) {
				ready = lastProgress = now;
				printf("  %d KiB each, created in %d ms\n",
					(int)(atomic_get(&sState->fileSize) / 1024),
					(int)((now - start) / 1000));
			} else if (now - start > kSetUpTimeout) {
				failure = "creating the files did not finish";
				break;
			}
			continue;
		}

		const int32 operations = atomic_get(&sState->metaOperations);
		if (operations != lastOperations) {
			lastOperations = operations;
			lastProgress = now;
		} else if (now - lastProgress > kStallTimeout) {
			failure = "writes to the disk stopped: no metadata operation "
				"finished for 8 seconds";
			break;
		}

		if (now - ready >= duration)
			break;
	}

	if (!slow) {
		printf("  %d metadata operations, %d chunks of 64 KiB written\n",
			(int)atomic_get(&sState->metaOperations),
			(int)atomic_get(&sState->chunks));
	}

	if (child > 0) {
		// Its threads do not go away while they wait for a disk that
		// stopped: no waiting for them then.
		kill(child, SIGKILL);
		if (failure == NULL)
			waitpid(child, NULL, 0);
	}

	if (failure != NULL) {
		printf("FAIL: %s\n", failure);
		return 1;
	}

	char path[B_PATH_NAME_LENGTH];
	for (int i = 0; i < kFiles; i++) {
		file_path(path, sizeof(path), i);
		unlink(path);
	}
	for (int i = 0; i < kMetaThreads; i++) {
		for (int j = 0; j < 64; j++) {
			snprintf(path, sizeof(path), "%s/fragmented-meta%d-%d", sDirectory,
				i, j);
			unlink(path);
		}
	}
	rmdir(sDirectory);

	if (slow) {
		printf("PASS (nothing tested: the volume is too slow to set the "
			"files up)\n");
		return 0;
	}
	printf("PASS\n");
	return 0;
}
