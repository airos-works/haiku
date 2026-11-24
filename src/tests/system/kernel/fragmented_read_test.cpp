/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Cold reads of badly fragmented files from several threads.

	A read of a file in more than eight extents is sent in parts: the disk
	driver's report of one part maps and starts the next
	(do_iterative_fd_io_iterate()), which takes the lock of the file's map.
	Mapping a part of a fragmented file that was not read before has the file
	system read a block of the file's extent list, synchronously and with
	that lock held. nvme_disk had the thread that waits for such a block
	report whatever else had finished on its queue: the report of a part of
	the same file then waited, on that thread, for the lock the thread held
	(airos-works/haiku#166). Nothing reads that file any more after that, and
	what was left to report on the queue never is.

	The test interleaves the blocks of a few files (a piece appended to each
	in turn, closed every time), large enough for their extent lists to take
	several blocks. Then it mounts the volume again and again, so that those
	blocks have to be read from the disk, and reads every file from three
	threads at once: one from the start to the end, two in pieces from all
	over. The threads run in a child team, as they cannot be interrupted once
	they wait for the disk: the test ends without them. Prints PASS or FAIL,
	exits 0 on PASS.

	fragmented_read_test MOUNT_POINT DEVICE [seconds]
		DEVICE holds a BFS volume that nothing else uses and is not mounted;
		the test mounts it at MOUNT_POINT, and leaves its files on it.
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
#include <fs_volume.h>


// (A file stays in the range of its indirect block runs: with 2 KiB
// blocks, 1024 runs of 8 KiB. Interleaved files of more ran into the
// double indirect range, where the page writer failed every write with
// B_BAD_DATA on master of 2026-11-24, a bug of its own.)
static const int kFiles = 16;
static const off_t kFileSize = 6 * 1024 * 1024;
static const size_t kPiece = 8192;
static const bigtime_t kDefaultDuration = 60000000;
static const bigtime_t kStallTimeout = 10000000;
static const bigtime_t kSetUpTimeout = 300000000;


struct shared_state {
	int32	started;
	int32	finished;
	int32	errors;
	int64	bytes;
};

static shared_state* sState;
static char sMountPoint[256];
static sem_id sStart;


static void
file_path(char* path, size_t size, int index)
{
	snprintf(path, size, "%s/fragmented-read-%d", sMountPoint, index);
}


static bool
files_exist()
{
	for (int i = 0; i < kFiles; i++) {
		char path[512];
		struct stat st;
		file_path(path, sizeof(path), i);
		if (stat(path, &st) != 0 || st.st_size != kFileSize)
			return false;
	}
	return true;
}


/*!	Appends a piece to each file in turn and closes the file every time,
	which gives its preallocated blocks back: the files' blocks alternate.
*/
static bool
create_files()
{
	static char piece[kPiece];
	memset(piece, 0x5a, sizeof(piece));
	const bigtime_t start = system_time();

	for (int i = 0; i < kFiles; i++) {
		char path[512];
		file_path(path, sizeof(path), i);
		unlink(path);
	}
	for (off_t size = 0; size < kFileSize; size += kPiece) {
		for (int i = 0; i < kFiles; i++) {
			char path[512];
			file_path(path, sizeof(path), i);
			int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (fd < 0 || write(fd, piece, kPiece) != (ssize_t)kPiece) {
				fprintf(stderr, "writing %s: %s\n", path, strerror(errno));
				return false;
			}
			close(fd);
		}
		if (system_time() - start > kSetUpTimeout) {
			fprintf(stderr, "setting the files up takes too long\n");
			return false;
		}
	}
	sync();
	return true;
}


struct reader {
	int		file;
	int		kind;
		// 0: from the start to the end, else in pieces from all over
};


static void*
read_file(void* data)
{
	reader* self = (reader*)data;
	static const size_t kBufferSize = 256 * 1024;
	char* buffer = (char*)malloc(kBufferSize);
	char path[512];
	file_path(path, sizeof(path), self->file);

	int fd = open(path, O_RDONLY);
	if (fd < 0 || buffer == NULL) {
		atomic_add(&sState->errors, 1);
		atomic_add(&sState->finished, 1);
		return NULL;
	}

	atomic_add(&sState->started, 1);
	acquire_sem(sStart);

	if (self->kind == 0) {
		ssize_t bytes;
		while ((bytes = read(fd, buffer, kBufferSize)) > 0)
			atomic_add64(&sState->bytes, bytes);
		if (bytes < 0)
			atomic_add(&sState->errors, 1);
	} else {
		// pieces a megabyte apart, from the end or from the middle: each
		// lies in a part of the file that was not mapped yet
		const off_t step = 1024 * 1024;
		const off_t count = kFileSize / step;
		for (off_t i = 0; i < count; i++) {
			off_t index = self->kind == 1 ? count - 1 - i
				: (count / 2 + i * 7) % count;
			ssize_t bytes = pread(fd, buffer, 64 * 1024,
				index * step + 300 * 1024);
			if (bytes < 0)
				atomic_add(&sState->errors, 1);
			else
				atomic_add64(&sState->bytes, bytes);
		}
	}

	close(fd);
	free(buffer);
	atomic_add(&sState->finished, 1);
	return NULL;
}


//! One round of reading, in a team of its own.
static void
run_readers()
{
	static const int kKinds = 3;
	pthread_t threads[kFiles * kKinds];
	reader readers[kFiles * kKinds];
	sStart = create_sem(0, "fragmented read start");

	for (int i = 0; i < kFiles * kKinds; i++) {
		readers[i].file = i / kKinds;
		readers[i].kind = i % kKinds;
		pthread_create(&threads[i], NULL, read_file, &readers[i]);
	}
	while (atomic_get(&sState->started) + atomic_get(&sState->finished)
			< kFiles * kKinds) {
		snooze(1000);
	}
	release_sem_etc(sStart, kFiles * kKinds, 0);
	for (int i = 0; i < kFiles * kKinds; i++)
		pthread_join(threads[i], NULL);
}


static bool
mount_volume(const char* device)
{
	if (fs_mount_volume(sMountPoint, device, "bfs", 0, NULL) < 0) {
		fprintf(stderr, "mounting %s at %s: %s\n", device, sMountPoint,
			strerror(errno));
		return false;
	}
	return true;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 3) {
		fprintf(stderr,
			"usage: fragmented_read_test MOUNT_POINT DEVICE [seconds]\n");
		return 2;
	}
	strlcpy(sMountPoint, argv[1], sizeof(sMountPoint));
	const char* device = argv[2];
	const bigtime_t duration = argc > 3
		? atoll(argv[3]) * 1000000LL : kDefaultDuration;

	sState = (shared_state*)mmap(NULL, sizeof(shared_state),
		PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (sState == MAP_FAILED) {
		perror("mmap");
		return 2;
	}

	mkdir(sMountPoint, 0755);
	if (!mount_volume(device))
		return 2;
	if (!files_exist()) {
		printf("writing %d files of %lld MiB in pieces of %zu bytes\n",
			kFiles, (long long)(kFileSize >> 20), kPiece);
		if (!create_files())
			return 2;
	}

	const bigtime_t start = system_time();
	int rounds = 0;
	while (system_time() - start < duration) {
		// mount again: nothing of the files is in memory (forced: a service
		// that looks at new volumes may keep something of it open)
		status_t status = fs_unmount_volume(sMountPoint, B_FORCE_UNMOUNT);
		if (status != B_OK) {
			fprintf(stderr, "unmounting %s: %s\n", sMountPoint,
				strerror(status));
			return 2;
		}
		if (!mount_volume(device))
			return 2;

		memset(sState, 0, sizeof(shared_state));
		pid_t child = fork();
		if (child == 0) {
			run_readers();
			_exit(0);
		}
		if (child < 0) {
			perror("fork");
			return 2;
		}

		int64 lastBytes = -1;
		bigtime_t lastProgress = system_time();
		while (true) {
			int status;
			if (waitpid(child, &status, WNOHANG) == child)
				break;
			const int64 bytes = atomic_get64(&sState->bytes);
			if (bytes != lastBytes) {
				lastBytes = bytes;
				lastProgress = system_time();
			} else if (system_time() - lastProgress > kStallTimeout) {
				printf("FAIL round %d: no read finished for %lld s, %d of %d "
					"readers are done, %lld KiB read\n", rounds + 1,
					(long long)(kStallTimeout / 1000000),
					(int)atomic_get(&sState->finished), kFiles * 3,
					(long long)(bytes >> 10));
				return 1;
			}
			snooze(20000);
		}

		if (atomic_get(&sState->errors) != 0) {
			printf("FAIL round %d: %d read errors\n", rounds + 1,
				(int)atomic_get(&sState->errors));
			return 1;
		}
		rounds++;
	}

	printf("PASS %d rounds of cold reads of %d fragmented files from %d "
		"threads\n", rounds, kFiles, kFiles * 3);
	return 0;
}
