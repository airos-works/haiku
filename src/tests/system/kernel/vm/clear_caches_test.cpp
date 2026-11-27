/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	clear_caches() with addresses a program may pass (#238). On arm64 the
	kernel runs DC CVAU on every line of the range, and an address without
	a page (a hole, a kernel address, a page of a read+execute area that was
	never touched) used to panic the kernel with "unhandled page fault in
	kernel space". Each call must return, and code written into memory must
	run after clear_caches(), through the same mapping and through a second
	one.

	Usage: clear_caches_test [case]; without one, all cases in turn.
*/


#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <OS.h>
#include <image.h>


static int sFailures;


static void
check(bool ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok" : "FAILED", what);
	fflush(stdout);
	if (!ok)
		sFailures++;
}


static void
fatal_signal(int signal)
{
	char buffer[64];
	int length = snprintf(buffer, sizeof(buffer),
		"FAILED: signal %d\n", signal);
	write(STDOUT_FILENO, buffer, length);
	_exit(1);
}


//	#pragma mark - code generation


typedef int (*function_t)(void);
static const size_t kFunctionSize = 16;


static void
write_function(void* where, uint32 value)
{
	uint8* code = (uint8*)where;
#if defined(__aarch64__)
	uint32 instructions[2] = {
		0x52800000 | ((value & 0xffff) << 5),	// movz w0, #value
		0xd65f03c0								// ret
	};
	memcpy(code, instructions, sizeof(instructions));
#elif defined(__x86_64__) || defined(__i386__)
	code[0] = 0xb8;								// mov eax, imm32
	memcpy(code + 1, &value, 4);
	code[5] = 0xc3;								// ret
#elif defined(__riscv)
	uint32 instructions[2] = {
		0x00000513 | ((value & 0x7ff) << 20),	// li a0, value
		0x00008067								// ret
	};
	memcpy(code, instructions, sizeof(instructions));
#else
#	error add the instructions for this architecture
#endif
}


static uint32
function_value(int i)
{
	// fits the immediates of every architecture above
	return (i * 37 + 11) & 0x7ff;
}


//	#pragma mark - cases


static void
test_hole()
{
	// The address of an area that is gone.
	void* address = NULL;
	area_id area = create_area("clear caches hole", &address, B_ANY_ADDRESS,
		4 * B_PAGE_SIZE, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	check(area >= 0, "create an area for the hole");
	delete_area(area);

	clear_caches(address, 4 * B_PAGE_SIZE, B_INVALIDATE_ICACHE);
	check(true, "clear_caches() on a hole returns");
	clear_caches(address, 1, B_INVALIDATE_ICACHE | B_FLUSH_DCACHE);
	check(true, "clear_caches() of one byte of a hole returns");
}


static void
test_bad_ranges()
{
	clear_caches((void*)8, 1, B_INVALIDATE_ICACHE);
	check(true, "clear_caches((void*)8, 1) returns");

#if B_HAIKU_64_BIT
	clear_caches((void*)0xffffff0000000000UL, B_PAGE_SIZE,
		B_INVALIDATE_ICACHE);
	check(true, "clear_caches() on a kernel address returns");
#endif

	// wraps around the end of the address space
	char local = 0;
	clear_caches(&local, ~(size_t)0 - 16, B_INVALIDATE_ICACHE);
	check(true, "clear_caches() with a range that wraps returns");

	clear_caches(&local, 0, B_INVALIDATE_ICACHE);
	check(true, "clear_caches() with length 0 returns");
}


static void
test_untouched_read_execute()
{
	// Pages of a read+execute area that were never touched: the kernel has
	// to fault them in for a read, not refuse a "write".
	void* address = NULL;
	area_id area = create_area("clear caches rx", &address, B_ANY_ADDRESS,
		16 * B_PAGE_SIZE, B_NO_LOCK, B_READ_AREA | B_EXECUTE_AREA);
	check(area >= 0, "create an untouched read+execute area");
	clear_caches(address, 16 * B_PAGE_SIZE, B_INVALIDATE_ICACHE);
	check(true, "clear_caches() on an untouched read+execute area returns");
	delete_area(area);

	// A file mapped read+execute, at an address where none of its pages
	// is mapped yet.
	int fd = open("/boot/system/lib/libroot.so", O_RDONLY);
	check(fd >= 0, "open libroot.so");
	if (fd < 0)
		return;
	void* mapping = mmap(NULL, 64 * 1024, PROT_READ | PROT_EXEC, MAP_PRIVATE,
		fd, 0);
	close(fd);
	check(mapping != MAP_FAILED, "map libroot.so read+execute");
	if (mapping == MAP_FAILED)
		return;
	clear_caches((uint8*)mapping + B_PAGE_SIZE, 8 * B_PAGE_SIZE,
		B_INVALIDATE_ICACHE);
	check(true, "clear_caches() on an untouched file mapping returns");
	munmap(mapping, 64 * 1024);
}


static status_t
clear_huge(void* address)
{
	clear_caches(address, (size_t)1 << 40, B_INVALIDATE_ICACHE);
	return B_OK;
}


static void
test_huge_length()
{
	// Runs until the end of the memory and stops at the hole behind it (the
	// top half of the area, given back). QEMU executes DC CVAU and IC IVAU
	// without translating the address, so there it goes on for all of the
	// length: kept to 64 MiB.
	void* address = NULL;
	area_id area = create_area("clear caches huge", &address, B_ANY_ADDRESS,
		16 * B_PAGE_SIZE, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
	check(area >= 0, "create an area");
	if (area < 0)
		return;
	check(resize_area(area, 8 * B_PAGE_SIZE) == B_OK,
		"leave a hole behind the area");
	bigtime_t start = system_time();
	clear_caches(address, 64 * 1024 * 1024, B_INVALIDATE_ICACHE);
	check(true, "clear_caches() with a length of 64 MiB returns");
	printf("   (%" B_PRId64 " us)\n", system_time() - start);

	// A thread in a call that takes long can be killed: one that runs it
	// for 1 TiB (on hardware it stops at the hole, under QEMU it would take
	// hours) must be gone soon after kill_thread().
	thread_id thread = spawn_thread(clear_huge, "clear huge", B_LOW_PRIORITY,
		address);
	resume_thread(thread);
	snooze(200000);
	kill_thread(thread);
	status_t result;
	bool gone = wait_for_thread_etc(thread, B_RELATIVE_TIMEOUT, 10000000,
		&result) != B_TIMED_OUT;
	check(gone, "a thread in clear_caches() of 1 TiB can be killed");
	delete_area(area);
}


static void
test_generated_code()
{
	const int kRounds = 2000;

	// write and run through the same read+write+execute mapping
	void* address = NULL;
	area_id area = create_area("clear caches jit", &address, B_ANY_ADDRESS,
		4 * B_PAGE_SIZE, B_NO_LOCK,
		B_READ_AREA | B_WRITE_AREA | B_EXECUTE_AREA);
	check(area >= 0, "create a read+write+execute area");
	if (area < 0)
		return;

	int wrong = 0;
	for (int i = 0; i < kRounds; i++) {
		uint8* slot = (uint8*)address + (i % 4) * B_PAGE_SIZE
			+ (i * kFunctionSize) % B_PAGE_SIZE;
		write_function(slot, function_value(i));
		clear_caches(slot, kFunctionSize, B_INVALIDATE_ICACHE);
		if (((function_t)slot)() != (int)function_value(i))
			wrong++;
	}
	printf("   %d of %d wrong\n", wrong, kRounds);
	check(wrong == 0, "generated code runs after clear_caches() (one mapping)");

	// a writer and an executable clone of the same memory, the executable
	// one made read+execute with mprotect() as JITs do
	void* writerAddress = NULL;
	area_id writer = create_area("clear caches writer", &writerAddress,
		B_ANY_ADDRESS, B_PAGE_SIZE, B_FULL_LOCK, B_READ_AREA | B_WRITE_AREA);
	void* runnerAddress = NULL;
	area_id runner = clone_area("clear caches runner", &runnerAddress,
		B_ANY_ADDRESS, B_READ_AREA | B_EXECUTE_AREA, writer);
	check(writer >= 0 && runner >= 0, "create a writer and a runner alias");
	if (writer >= 0 && runner >= 0) {
		wrong = 0;
		for (int i = 0; i < kRounds; i++) {
			size_t offset = (i * kFunctionSize) % B_PAGE_SIZE;
			write_function((uint8*)writerAddress + offset, function_value(i));
			clear_caches((uint8*)runnerAddress + offset, kFunctionSize,
				B_INVALIDATE_ICACHE);
			if (((function_t)((uint8*)runnerAddress + offset))()
					!= (int)function_value(i)) {
				wrong++;
			}
		}
		printf("   %d of %d wrong\n", wrong, kRounds);
		check(wrong == 0, "generated code runs after clear_caches() (alias)");
	}
	delete_area(runner);
	delete_area(writer);

	// mprotect() between writing and running
	void* jit = mmap(NULL, B_PAGE_SIZE, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	check(jit != MAP_FAILED, "map a page for mprotect()");
	if (jit != MAP_FAILED) {
		wrong = 0;
		for (int i = 0; i < kRounds / 4; i++) {
			mprotect(jit, B_PAGE_SIZE, PROT_READ | PROT_WRITE);
			size_t offset = (i * kFunctionSize) % B_PAGE_SIZE;
			write_function((uint8*)jit + offset, function_value(i));
			mprotect(jit, B_PAGE_SIZE, PROT_READ | PROT_EXEC);
			clear_caches((uint8*)jit + offset, kFunctionSize,
				B_INVALIDATE_ICACHE);
			if (((function_t)((uint8*)jit + offset))()
					!= (int)function_value(i)) {
				wrong++;
			}
		}
		printf("   %d of %d wrong\n", wrong, kRounds / 4);
		check(wrong == 0, "generated code runs after mprotect()");
		munmap(jit, B_PAGE_SIZE);
	}
	delete_area(area);
}


int
main(int argc, char** argv)
{
	signal(SIGSEGV, fatal_signal);
	signal(SIGBUS, fatal_signal);
	signal(SIGILL, fatal_signal);
	signal(SIGALRM, fatal_signal);
	alarm(120);

	struct {
		const char* name;
		void (*function)();
	} cases[] = {
		{ "bad", test_bad_ranges },
		{ "hole", test_hole },
		{ "rx", test_untouched_read_execute },
		{ "huge", test_huge_length },
		{ "code", test_generated_code },
	};

	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (argc > 1 && strcmp(argv[1], cases[i].name) != 0)
			continue;
		printf("case %s\n", cases[i].name);
		fflush(stdout);
		cases[i].function();
		fflush(stdout);
	}

	printf("%s\n", sFailures == 0 ? "PASS" : "FAIL");
	return sFailures == 0 ? 0 : 1;
}
