/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Files that grow into BFS' double indirect range in small appends
	(airos-works/haiku#220).

	Appending to a few files in turn, each append with its own open and
	close, gives every file one block run per append, so that the files soon
	reach the double indirect range (12 + 4 x block size / 8 runs). There,
	closing a file whose size was a multiple of the range's run size left the
	stream claiming the trimmed runs as preallocated: the next append
	allocated nothing, write() succeeded, and the page writer then failed
	those pages with B_BAD_DATA for ever (fsync() of such a file does not
	return), and the data is lost. Trimming and truncating also leaked the
	blocks of the second level arrays (and deleting such a file all of them).

	The test appends 8 KiB to each file in turn, checks as it goes that the
	volume lost at least as many free blocks as the files hold, fsyncs and compares the
	files (fsync in a child, with a deadline), then truncates every file to a
	run boundary inside the range and appends to it again, checks again,
	removes the files, and compares the volume's free blocks with what it had
	before. The free block counts are only exact on a volume nothing else
	writes to: give --exact for that (a scratch volume); otherwise only a
	volume that lost too few blocks fails the test.

	bfs_double_indirect_test [--exact] DIRECTORY [FILES [MIB]]
	Prints PASS or FAIL, exits 0 on PASS.
*/

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fs_info.h>
#include <OS.h>


#define CHUNK	8192
#define MAX_FILES 32

static const char* sDirectory;
static char sBuffer[CHUNK];
static char sReadBuffer[CHUNK];


static void
file_path(char* path, size_t size, int file)
{
	snprintf(path, size, "%s/double_indirect%d", sDirectory, file);
}


static void
fill(char* buffer, int file, off_t offset)
{
	for (int i = 0; i < CHUNK; i += 16) {
		uint64_t a = file, b = offset + i;
		memcpy(buffer + i, &a, 8);
		memcpy(buffer + i + 8, &b, 8);
	}
}


static int
append(int file, off_t offset)
{
	char path[1024];
	file_path(path, sizeof(path), file);
	int fd = open(path, O_WRONLY | O_APPEND);
	if (fd < 0) {
		printf("FAIL: open %s: %s\n", path, strerror(errno));
		return -1;
	}
	fill(sBuffer, file, offset);
	ssize_t written = write(fd, sBuffer, CHUNK);
	close(fd);
	if (written != CHUNK) {
		printf("FAIL: append to %s at %lld: %s\n", path, (long long)offset,
			written < 0 ? strerror(errno) : "short write");
		return -1;
	}
	return 0;
}


/*!	fsync()s the files in a child: on a broken volume, the pages the page
	writer cannot write keep fsync() from ever returning.
*/
static int
sync_files(int first, int count)
{
	fflush(stdout);
	pid_t child = fork();
	if (child < 0) {
		printf("FAIL: fork: %s\n", strerror(errno));
		return -1;
	}
	if (child == 0) {
		for (int file = first; file < first + count; file++) {
			char path[1024];
			file_path(path, sizeof(path), file);
			int fd = open(path, O_RDWR);
			if (fd < 0 || fsync(fd) != 0) {
				printf("FAIL: fsync %s: %s\n", path, strerror(errno));
				_exit(1);
			}
			close(fd);
		}
		_exit(0);
	}

	bigtime_t deadline = system_time() + 120000000LL;
	while (system_time() < deadline) {
		int status;
		pid_t result = waitpid(child, &status, WNOHANG);
		if (result == child)
			return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
		if (result < 0) {
			printf("FAIL: waitpid: %s\n", strerror(errno));
			return -1;
		}
		snooze(100000);
	}
	printf("FAIL: fsync did not return in 120 s (pages that cannot be "
		"written, see the syslog)\n");
	return -1;
}


/*!	The volume must have lost at least the blocks the files hold: a stream
	whose size is not backed by blocks is what this test looks for.
*/
static int
check_allocated(dev_t device, off_t freeBefore, off_t bytes, off_t blockSize)
{
	fs_info info;
	fs_stat_dev(device, &info);
	off_t used = freeBefore - info.free_blocks;
	if (used * blockSize < bytes) {
		printf("FAIL: the files hold %lld bytes, but only %lld blocks "
			"(%lld bytes) were allocated\n", (long long)bytes,
			(long long)used, (long long)(used * blockSize));
		return -1;
	}
	return 0;
}


static int
check_file(int file, off_t size)
{
	char path[1024];
	file_path(path, sizeof(path), file);
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		printf("FAIL: open %s: %s\n", path, strerror(errno));
		return -1;
	}
	struct stat st;
	fstat(fd, &st);
	if (st.st_size != size) {
		printf("FAIL: %s has %lld bytes, expected %lld\n", path,
			(long long)st.st_size, (long long)size);
		close(fd);
		return -1;
	}
	for (off_t offset = 0; offset < size; offset += CHUNK) {
		fill(sBuffer, file, offset);
		if (pread(fd, sReadBuffer, CHUNK, offset) != CHUNK
			|| memcmp(sBuffer, sReadBuffer, CHUNK) != 0) {
			printf("FAIL: %s differs at %lld\n", path, (long long)offset);
			close(fd);
			return -1;
		}
	}
	close(fd);
	return 0;
}


static void
timeout(int signal)
{
	printf("FAIL: did not finish in time\n");
	_exit(1);
}


int
main(int argc, char** argv)
{
	int argIndex = 1;
	bool exact = false;
	if (argc > argIndex && strcmp(argv[argIndex], "--exact") == 0) {
		exact = true;
		argIndex++;
	}
	if (argc <= argIndex) {
		fprintf(stderr, "usage: %s [--exact] directory [files [MiB]]\n",
			argv[0]);
		return 2;
	}
	sDirectory = argv[argIndex];
	int files = argc > argIndex + 1 ? atoi(argv[argIndex + 1]) : 8;
	off_t size = (argc > argIndex + 2 ? atol(argv[argIndex + 2]) : 24)
		* 1024 * 1024;
	if (files < 2 || files > MAX_FILES || size < CHUNK) {
		fprintf(stderr, "2 to %d files, 1 MiB at least\n", MAX_FILES);
		return 2;
	}

	signal(SIGALRM, timeout);
	alarm(900);

	struct stat st;
	if (stat(sDirectory, &st) != 0) {
		printf("FAIL: %s: %s\n", sDirectory, strerror(errno));
		return 1;
	}
	fs_info info;
	if (fs_stat_dev(st.st_dev, &info) != 0) {
		printf("FAIL: fs_stat_dev: %s\n", strerror(errno));
		return 1;
	}
	if (strcmp(info.fsh_name, "bfs") != 0) {
		printf("PASS: %s is not on BFS (%s): nothing to test\n", sDirectory,
			info.fsh_name);
		return 0;
	}
	if (info.free_blocks * info.block_size < files * size * 5 / 4) {
		printf("PASS: not enough room on %s: nothing to test\n", sDirectory);
		return 0;
	}
	// the double indirect range starts after 12 + 4 x block size / 8 runs
	off_t doubleIndirectStart = (12 + 4 * info.block_size / 8) * CHUNK;
	if (size < doubleIndirectStart + 2 * 1024 * 1024) {
		printf("FAIL: %lld MiB do not reach the double indirect range\n",
			(long long)(size >> 20));
		return 1;
	}
	sync();
	fs_stat_dev(st.st_dev, &info);
	off_t freeBefore = info.free_blocks;

	for (int file = 0; file < files; file++) {
		char path[1024];
		file_path(path, sizeof(path), file);
		int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
		if (fd < 0) {
			printf("FAIL: create %s: %s\n", path, strerror(errno));
			return 1;
		}
		close(fd);
	}

	bigtime_t start = system_time();
	for (off_t offset = 0; offset < size; offset += CHUNK) {
		for (int file = 0; file < files; file++) {
			if (append(file, offset) != 0)
				return 1;
		}
		// look early: once the pages cannot be written, every write waits
		// for the modified page quota, and the test would crawl
		if ((offset / CHUNK) % 64 == 63
			&& check_allocated(st.st_dev, freeBefore,
				files * (offset + CHUNK), info.block_size) != 0)
			return 1;
	}
	printf("%d files of %lld MiB appended in 8 KiB pieces in %.1f s\n",
		files, (long long)(size >> 20), (system_time() - start) / 1e6);

	if (check_allocated(st.st_dev, freeBefore, files * size, info.block_size)
			!= 0
		|| sync_files(0, files) != 0)
		return 1;
	for (int file = 0; file < files; file++) {
		if (check_file(file, size) != 0)
			return 1;
	}
	printf("written and fsynced: OK\n");

	// Cut each file at a different run boundary in the double indirect
	// range (the first one about 2 MiB in, where a second level array
	// starts if every append made one run), then
	// append to it again.
	off_t total = 0;
	off_t newSizes[MAX_FILES];
	for (int file = 0; file < files; file++) {
		off_t newSize = doubleIndirectStart
			+ (size - doubleIndirectStart) * file / files;
		if (file == 0)
			newSize = doubleIndirectStart + 2 * 1024 * 1024;
		newSize &= ~(off_t)(CHUNK - 1);
		char path[1024];
		file_path(path, sizeof(path), file);
		if (truncate(path, newSize) != 0) {
			printf("FAIL: truncate %s: %s\n", path, strerror(errno));
			return 1;
		}
		if (append(file, newSize) != 0 || append(file, newSize + CHUNK) != 0)
			return 1;
		newSizes[file] = newSize + 2 * CHUNK;
		total += newSizes[file];
	}
	if (check_allocated(st.st_dev, freeBefore, total, info.block_size) != 0
		|| sync_files(0, files) != 0)
		return 1;
	for (int file = 0; file < files; file++) {
		if (check_file(file, newSizes[file]) != 0)
			return 1;
	}
	printf("truncated, appended and fsynced: OK\n");

	for (int file = 0; file < files; file++) {
		char path[1024];
		file_path(path, sizeof(path), file);
		unlink(path);
	}
	sync();
	fs_stat_dev(st.st_dev, &info);
	off_t lost = freeBefore - info.free_blocks;
	printf("free blocks before %lld, after %lld\n", (long long)freeBefore,
		(long long)info.free_blocks);
	if (lost != 0) {
		if (exact) {
			printf("FAIL: %lld blocks were not freed\n", (long long)lost);
			return 1;
		}
		printf("(%lld blocks less: the volume is in use, or blocks leaked; "
			"--exact on a scratch volume tells)\n", (long long)lost);
	}

	printf("PASS\n");
	return 0;
}
