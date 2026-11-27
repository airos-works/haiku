/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Once munmap(), mprotect(), delete_area() or fork() have returned, no
	thread of the team on any CPU may still reach the memory the old way:
	a TLB entry left behind is memory corruption. Threads on the other CPUs
	keep reading or writing the memory while the main thread changes it, and
	check every access they make after it said it was done (#236, which
	batches those invalidations on arm64). Also checks that new memory reads
	as zero (#237: pages are cleared with DC ZVA on arm64).

	Usage: tlb_shootdown_test [rounds]
*/


#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>


static const int kThreads = 3;

static int sFailures;

static int32 sGeneration;
	// even: the memory is there, odd: it is gone (or read-only)
static uint8* volatile sMemory;
static size_t sMemorySize;
static int32 sMode;
	// 0 read, 1 write
static int32 sQuit;
static int32 sStaleAccesses;
static int32 sFaults;
static int32 sAccesses;

static __thread sigjmp_buf tJump;
static __thread volatile bool tJumpSet;


static void
check(bool ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok" : "FAILED", what);
	fflush(stdout);
	if (!ok)
		sFailures++;
}


static void
fault_handler(int signal)
{
	if (tJumpSet)
		siglongjmp(tJump, 1);

	char buffer[64];
	int length = snprintf(buffer, sizeof(buffer),
		"FAILED: unexpected signal %d\n", signal);
	write(STDOUT_FILENO, buffer, length);
	_exit(1);
}


static void
timeout_handler(int)
{
	const char message[] = "FAILED: timeout\n";
	write(STDOUT_FILENO, message, sizeof(message) - 1);
	_exit(1);
}


static status_t
accessor(void*)
{
	uint32 index = find_thread(NULL);
	while (atomic_get(&sQuit) == 0) {
		int32 generation = atomic_get(&sGeneration);
		uint8* memory = sMemory;
		if (memory == NULL)
			continue;

		size_t offset = (index * 7919 * B_PAGE_SIZE) % sMemorySize;
		index++;

		tJumpSet = true;
		if (sigsetjmp(tJump, 1) != 0) {
			tJumpSet = false;
			atomic_add(&sFaults, 1);
			continue;
		}
		if (atomic_get(&sMode) == 0)
			(void)*(volatile uint8*)(memory + offset);
		else
			*(volatile uint8*)(memory + offset) = 1;
		tJumpSet = false;

		atomic_add(&sAccesses, 1);
		if ((generation & 1) != 0
			&& atomic_get(&sGeneration) == generation) {
			// it was gone before this access began, and nothing new has
			// been mapped since
			atomic_add(&sStaleAccesses, 1);
		}
	}
	return B_OK;
}


static void
wait_for_accesses(int32 count)
{
	int32 start = atomic_get(&sAccesses);
	bigtime_t deadline = system_time() + 2000000;
	while (atomic_get(&sAccesses) - start < count && system_time() < deadline)
		snooze(100);
}


enum {
	kUnmap,
	kProtect,
	kDeleteArea,
	kUnmapPart
};


static void
test_round(int what, size_t pages, int rounds)
{
	atomic_set(&sStaleAccesses, 0);
	atomic_set(&sFaults, 0);
	size_t size = pages * B_PAGE_SIZE;
	bigtime_t startTime = system_time();

	for (int round = 0; round < rounds; round++) {
		area_id area = -1;
		uint8* memory;
		if (what == kDeleteArea) {
			void* address = NULL;
			area = create_area("tlb test", &address, B_ANY_ADDRESS, size,
				B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
			memory = area >= 0 ? (uint8*)address : (uint8*)MAP_FAILED;
		} else {
			memory = (uint8*)mmap(NULL, size, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		}
		if (memory == MAP_FAILED) {
			check(false, "allocate memory");
			return;
		}
		for (size_t i = 0; i < size; i += B_PAGE_SIZE)
			memory[i] = 1;

		atomic_set(&sMode, what == kProtect ? 1 : 0);
		sMemorySize = what == kUnmapPart ? size / 2 : size;
		sMemory = memory;
		wait_for_accesses(50);

		switch (what) {
			case kUnmap:
				munmap(memory, size);
				break;
			case kUnmapPart:
				// the front half: UnmapPages() of a part of the area
				munmap(memory, size / 2);
				break;
			case kProtect:
				mprotect(memory, size, PROT_READ);
				break;
			case kDeleteArea:
				delete_area(area);
				break;
		}
		atomic_add(&sGeneration, 1);

		wait_for_accesses(20);
		while (atomic_get(&sFaults) == 0
			&& atomic_get(&sStaleAccesses) < 1000) {
			snooze(100);
		}
		sMemory = NULL;
		atomic_add(&sGeneration, 1);
		snooze(1000);

		if (what == kProtect)
			munmap(memory, size);
		else if (what == kUnmapPart)
			munmap(memory + size / 2, size / 2);
	}

	static const char* kNames[] = { "munmap()", "mprotect()",
		"delete_area()", "munmap() of a part" };
	char text[128];
	snprintf(text, sizeof(text), "%s of %zu pages, %d rounds: %" B_PRId32
		" stale accesses, %" B_PRId32 " faults, %" B_PRId64 " ms",
		kNames[what], pages, rounds, atomic_get(&sStaleAccesses),
		atomic_get(&sFaults), (system_time() - startTime) / 1000);
	check(atomic_get(&sStaleAccesses) == 0 && atomic_get(&sFaults) > 0, text);
}


static int32 sWriterQuit;


static status_t
writer(void* data)
{
	uint32* memory = (uint32*)data;
	uint32 value = 0;
	while (atomic_get(&sWriterQuit) == 0) {
		for (size_t i = 0; i < 256 * B_PAGE_SIZE / sizeof(uint32);
				i += B_PAGE_SIZE / sizeof(uint32)) {
			memory[i] = value;
		}
		value++;
	}
	return B_OK;
}


static void
test_fork(int rounds)
{
	// Threads keep writing to the memory while the team forks: the child
	// must see a snapshot, which the parent's writes after the copy-on-write
	// protection do not change.
	const size_t size = 256 * B_PAGE_SIZE;
	uint32* memory = (uint32*)mmap(NULL, size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED) {
		check(false, "allocate memory for fork()");
		return;
	}
	memset(memory, 0, size);

	atomic_set(&sWriterQuit, 0);
	thread_id writers[kThreads];
	for (int i = 0; i < kThreads; i++) {
		writers[i] = spawn_thread(writer, "writer", B_NORMAL_PRIORITY, memory);
		resume_thread(writers[i]);
	}
	snooze(10000);

	int bad = 0;
	for (int round = 0; round < rounds; round++) {
		pid_t child = fork();
		if (child == 0) {
			alarm(30);
			uint32 first[256];
			for (int i = 0; i < 256; i++)
				first[i] = memory[i * B_PAGE_SIZE / sizeof(uint32)];
			snooze(20000);
			for (int i = 0; i < 256; i++) {
				if (memory[i * B_PAGE_SIZE / sizeof(uint32)] != first[i])
					_exit(1);
			}
			_exit(0);
		}
		int status = 0;
		if (child < 0 || waitpid(child, &status, 0) != child
			|| !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			bad++;
		}
	}

	atomic_set(&sWriterQuit, 1);
	for (int i = 0; i < kThreads; i++) {
		status_t result;
		wait_for_thread(writers[i], &result);
	}
	munmap(memory, size);

	char text[128];
	snprintf(text, sizeof(text),
		"fork() while threads write: %d of %d children saw it change", bad,
		rounds);
	check(bad == 0, text);
}


static void
test_zeroed(int rounds)
{
	// new memory reads as zero, also when its pages were used before
	const size_t size = 32 * 1024 * 1024;
	int bad = 0;
	for (int round = 0; round < rounds; round++) {
		uint64* memory = (uint64*)mmap(NULL, size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (memory == MAP_FAILED) {
			check(false, "allocate memory");
			return;
		}
		for (size_t i = 0; i < size / sizeof(uint64); i++) {
			if (memory[i] != 0)
				bad++;
			memory[i] = ~(uint64)i;
		}
		munmap(memory, size);
	}
	char text[128];
	snprintf(text, sizeof(text), "new memory is zero: %d non-zero words in "
		"%d rounds of %zu MiB", bad, rounds, size / 1024 / 1024);
	check(bad == 0, text);
}


int
main(int argc, char** argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 20;

	struct sigaction action = {};
	action.sa_handler = fault_handler;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	signal(SIGALRM, timeout_handler);
	alarm(120 + rounds * 20);
		// emulated (QEMU TCG) machines are slow

	thread_id threads[kThreads];
	for (int i = 0; i < kThreads; i++) {
		threads[i] = spawn_thread(accessor, "accessor", B_NORMAL_PRIORITY,
			NULL);
		resume_thread(threads[i]);
	}

	test_round(kUnmap, 16, rounds);
	test_round(kUnmap, 1024, rounds / 4 + 1);
		// above the batch limit: the whole ASID
	test_round(kUnmapPart, 64, rounds);
	test_round(kProtect, 16, rounds);
	test_round(kProtect, 1024, rounds / 4 + 1);
	test_round(kDeleteArea, 64, rounds);
	test_round(kDeleteArea, 2048, rounds / 4 + 1);

	atomic_set(&sQuit, 1);
	for (int i = 0; i < kThreads; i++) {
		status_t result;
		wait_for_thread(threads[i], &result);
	}

	test_fork(rounds);
	test_zeroed(rounds / 4 + 1);

	printf("%s\n", sFailures == 0 ? "PASS" : "FAIL");
	return sFailures == 0 ? 0 : 1;
}
