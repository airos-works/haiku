/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	madvise(MADV_FREE) and madvise(MADV_DONTNEED) on memory that is not the
	caller's alone: a shared file mapping, shared anonymous memory a forked
	child has too, an area with a clone, and private memory a forked child
	still reads through (airos-works/haiku#162).

	Such memory cannot be freed, and the call has to leave it as it is.
	MADV_FREE returned from discard_area_range() with the area's cache still
	locked and referenced: the next page fault on the area, and for a file the
	next read() of it by anyone, then waited for that lock for good, in a
	thread no signal gets out of.

	Every case runs in a team of its own with a deadline, so that a hang is
	reported instead of hanging the test. A case that hangs leaves teams
	behind that cannot be killed: run this in a virtual machine when the
	kernel under test may have the bug.

	madvise_shared_cache_test [directory]   (default: /tmp)
*/


#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>


static const size_t kPages = 4;
static const size_t kSize = kPages * B_PAGE_SIZE;
static const bigtime_t kDeadline = 10 * 1000000LL;

static const char* sDirectory = "/tmp";


#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			printf("  FAILED %s:%d: ", __func__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
			fflush(stdout); \
			return 1; \
		} \
	} while (false)


static bool
all_bytes(const uint8* data, size_t size, uint8 value)
{
	for (size_t i = 0; i < size; i++) {
		if (data[i] != value)
			return false;
	}
	return true;
}


/*!	What every case does with its memory: the advice succeeds, the data is
	still there, and the memory can be written.
*/
static int
advise_and_touch(uint8* data, int advice, bool writable)
{
	CHECK(madvise(data, kSize, advice) == 0, "madvise: %s", strerror(errno));
	CHECK(all_bytes(data, kSize, 0x5a), "the data changed");
	if (writable) {
		memset(data, 0x33, kSize);
		CHECK(all_bytes(data, kSize, 0x33), "rewrite");
	}
	return 0;
}


static int
test_shared_file(int advice)
{
	char path[B_PATH_NAME_LENGTH];
	snprintf(path, sizeof(path), "%s/madvise_shared_cache_test.%d",
		sDirectory, (int)getpid());
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	CHECK(fd >= 0, "open %s: %s", path, strerror(errno));
	unlink(path);

	uint8 buffer[kSize];
	memset(buffer, 0x5a, kSize);
	CHECK(write(fd, buffer, kSize) == (ssize_t)kSize, "write");

	uint8* data = (uint8*)mmap(NULL, kSize, PROT_READ | PROT_WRITE,
		MAP_SHARED, fd, 0);
	CHECK(data != MAP_FAILED, "mmap: %s", strerror(errno));

	if (advise_and_touch(data, advice, true) != 0)
		return 1;

	// the file's cache is the one that stayed locked
	CHECK(pread(fd, buffer, kSize, 0) == (ssize_t)kSize, "pread");
	CHECK(all_bytes(buffer, kSize, 0x33), "the file does not have the data");

	munmap(data, kSize);
	close(fd);
	return 0;
}


static int
test_shared_anonymous_after_fork(int advice)
{
	uint8* data = (uint8*)mmap(NULL, kSize, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	CHECK(data != MAP_FAILED, "mmap: %s", strerror(errno));
	memset(data, 0x5a, kSize);

	// a child that keeps its mapping of the memory while we give the advice
	int pipeFDs[2];
	CHECK(pipe(pipeFDs) == 0, "pipe");
	pid_t child = fork();
	CHECK(child >= 0, "fork");
	if (child == 0) {
		close(pipeFDs[1]);
		char c;
		read(pipeFDs[0], &c, 1);
		_exit(all_bytes(data, kSize, 0x33) ? 0 : 1);
	}
	close(pipeFDs[0]);

	int result = advise_and_touch(data, advice, true);

	close(pipeFDs[1]);
	int status;
	CHECK(waitpid(child, &status, 0) == child, "waitpid");
	CHECK(result != 0 || (WIFEXITED(status) && WEXITSTATUS(status) == 0),
		"the child does not see what was written after the advice");
	return result;
}


static int
test_cloned_area(int advice)
{
	uint8* data;
	area_id area = create_area("madvise test", (void**)&data, B_ANY_ADDRESS,
		kSize, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	CHECK(area >= 0, "create_area: %s", strerror(area));
	memset(data, 0x5a, kSize);

	uint8* cloneData;
	area_id clone = clone_area("madvise test clone", (void**)&cloneData,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, area);
	CHECK(clone >= 0, "clone_area: %s", strerror(clone));

	if (advise_and_touch(data, advice, true) != 0)
		return 1;
	CHECK(all_bytes(cloneData, kSize, 0x33),
		"the clone does not see what was written after the advice");

	delete_area(clone);
	delete_area(area);
	return 0;
}


/*!	A forked child reads the parent's read-only pages through its own cache:
	the parent's cache has a consumer.
*/
static int
test_read_only_after_fork(int advice)
{
	uint8* data = (uint8*)mmap(NULL, kSize, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(data != MAP_FAILED, "mmap: %s", strerror(errno));
	memset(data, 0x5a, kSize);
	CHECK(mprotect(data, kSize, PROT_READ) == 0, "mprotect");

	int pipeFDs[2];
	CHECK(pipe(pipeFDs) == 0, "pipe");
	pid_t child = fork();
	CHECK(child >= 0, "fork");
	if (child == 0) {
		close(pipeFDs[1]);
		char c;
		read(pipeFDs[0], &c, 1);
		_exit(all_bytes(data, kSize, 0x5a) ? 0 : 1);
	}
	close(pipeFDs[0]);

	int result = 0;
	if (advice == MADV_DONTNEED) {
		// MADV_DONTNEED zeroes what it can write to; this it cannot
		if (madvise(data, kSize, advice) != 0) {
			printf("  FAILED: madvise: %s\n", strerror(errno));
			result = 1;
		}
		if (!all_bytes(data, kSize, 0x5a)) {
			printf("  FAILED: read-only data changed\n");
			result = 1;
		}
	} else
		result = advise_and_touch(data, advice, false);

	close(pipeFDs[1]);
	int status;
	CHECK(waitpid(child, &status, 0) == child, "waitpid");
	CHECK(result != 0 || (WIFEXITED(status) && WEXITSTATUS(status) == 0),
		"the child lost the data");
	return result;
}


struct test_case {
	const char*	name;
	int			(*function)(int advice);
};

static const test_case kCases[] = {
	{ "shared file mapping", test_shared_file },
	{ "shared anonymous memory after fork()", test_shared_anonymous_after_fork },
	{ "area with a clone", test_cloned_area },
	{ "read-only private memory after fork()", test_read_only_after_fork },
};


static void
crash_handler(int signal)
{
	// without a handler the crash dialog comes up and the team stays
	static const char kMessage[] = "  FAILED: crashed\n";
	write(STDOUT_FILENO, kMessage, sizeof(kMessage) - 1);
	_exit(2);
}


/*!	Runs the case in a child and waits for it until the deadline. */
static bool
run_case(const test_case& testCase, int advice)
{
	const char* adviceName = advice == MADV_FREE ? "MADV_FREE" : "MADV_DONTNEED";
	printf("%s, %s: ", testCase.name, adviceName);
	fflush(stdout);

	pid_t child = fork();
	if (child < 0) {
		printf("FAILED: fork: %s\n", strerror(errno));
		return false;
	}
	if (child == 0) {
		signal(SIGSEGV, crash_handler);
		signal(SIGBUS, crash_handler);
		int result = testCase.function(advice);
		fflush(stdout);
		_exit(result);
	}

	const bigtime_t deadline = system_time() + kDeadline;
	int status;
	while (true) {
		pid_t result = waitpid(child, &status, WNOHANG);
		if (result == child)
			break;
		if (result < 0) {
			printf("FAILED: waitpid: %s\n", strerror(errno));
			return false;
		}
		if (system_time() > deadline) {
			// The team is left as it is. Killing it would make things
			// worse: a team that exits deletes its areas with its address
			// space write-locked, waits for the same cache lock, and then
			// blocks the fork() of every team it shares an area with (this
			// one), and a waitpid() for it, even with WNOHANG.
			printf("FAILED: no answer after %d s (team %d is left behind)\n",
				(int)(kDeadline / 1000000), (int)child);
			return false;
		}
		snooze(20000);
	}

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		if (WIFSIGNALED(status))
			printf("FAILED: killed by signal %d\n", WTERMSIG(status));
		return false;
	}

	printf("ok\n");
	return true;
}


int
main(int argc, char** argv)
{
	if (argc > 1)
		sDirectory = argv[1];

	setvbuf(stdout, NULL, _IOLBF, 0);

	int failures = 0;
	for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
		if (!run_case(kCases[i], MADV_FREE))
			failures++;
		if (!run_case(kCases[i], MADV_DONTNEED))
			failures++;
	}

	if (failures != 0) {
		printf("%d case(s) FAILED\n", failures);
		return 1;
	}

	printf("all cases passed\n");
	return 0;
}
