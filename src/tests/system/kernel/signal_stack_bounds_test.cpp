/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	signal_stack_bounds_test [case]: signals delivered to a thread whose
	stack pointer, or alternate signal stack, is not user memory it may write
	(airos-works/haiku#224).

	The kernel writes the signal frame where the stack pointer points. That
	must be user memory: a kernel address is refused (the frame is not
	written) and the program dies of SIGSEGV, like on Linux. So does a stack
	pointer at unmapped memory, where the kernel used to return to the
	faulting instruction and loop for ever.

	To see a write to kernel memory without damaging the kernel, the "kernel
	address" used is the kernel's mapping of all physical memory, at the
	physical address of a locked buffer of the test's own (from
	/dev/misc/poke, so it has to run as root): a frame written there shows
	up in the buffer. Without poke, an unmapped part of that mapping is used
	and only the SIGSEGV is checked.

	Every case runs in a child of its own, with the debugger disabled, so that
	a crash does not wait for the crash dialog. Without an argument all cases
	run; with a case name only that one.
*/


#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>

#include <poke.h>


#if defined(__x86_64__) || defined(__aarch64__)

// the kernel's mapping of all physical memory (KERNEL_PMAP_BASE), and an
// address in it far beyond any RAM (KERNEL_PMAP_SIZE is 512 GiB)
static const addr_t kPhysicalMapBase = 0xffffff0000000000UL;
static const addr_t kUnmappedPhysical = 0x7fffff0000UL;

#if defined(__x86_64__)
// the last page below USER_TOP (0x7fffffdfffff)
static const addr_t kLastUserPage = 0x7fffffdff000UL;
#else
// the last page below USER_TOP (0xffffffffffff)
static const addr_t kLastUserPage = 0xfffffffff000UL;
#endif

#endif

// in a register when the signal is raised, so it is in the frame
static const uint64 kMagic = 0x5349474652414d45ULL;
static const size_t kBufferSize = 64 * 1024;

enum {
	kExitHandlerRan = 0,
	kExitHung = 2,
	kExitWritten = 3,
	kExitRefused = 4,
	kExitSetupFailed = 5
};

enum expectation {
	kExpectHandler,		// the handler runs on a valid stack
	kExpectSIGSEGV,		// the team is killed by SIGSEGV
	kExpectRefused		// sigaltstack() refuses the stack
};

struct test_case {
	const char*	name;
	const char*	what;
	expectation	expected;
};

static const test_case kCases[] = {
	{ "plain", "SIGILL handler on the thread's own stack", kExpectHandler },
	{ "altstack", "SIGILL handler on a valid alternate stack",
		kExpectHandler },
	{ "kernelsp", "stack pointer = a writable kernel address",
		kExpectSIGSEGV },
	{ "altkernel", "alternate stack from user space into kernel space",
		kExpectRefused },
	{ "lowsp", "stack pointer = 0x1000 (the frame below it wraps around)",
		kExpectSIGSEGV },
	{ "unmapped", "stack pointer = unmapped user memory", kExpectSIGSEGV },
	{ "overflow", "stack overflow with a SIGSEGV handler and no alternate "
		"stack", kExpectSIGSEGV },
};

static uint8* sBuffer;
static addr_t sBufferPhysical;
static addr_t sAltStackBase;
static size_t sAltStackSize;


static void
exiting_handler(int signal)
{
	_exit(kExitHandlerRan);
}


static void
altstack_handler(int signal)
{
	// must run on the alternate stack
	addr_t here = (addr_t)&signal;
	if (here < sAltStackBase || here >= sAltStackBase + sAltStackSize)
		_exit(kExitSetupFailed);
	_exit(kExitHandlerRan);
}


/*!	Raises SIGILL with the given stack pointer and kMagic in a register that
	the frame saves.
*/
static void __attribute__((noreturn))
raise_sigill_with_stack(addr_t stackPointer)
{
#if defined(__x86_64__)
	__asm__ volatile(
		"mov %1, %%rbx\n\t"
		"mov %0, %%rsp\n\t"
		"ud2\n\t"
		: : "r"(stackPointer), "r"(kMagic) : "rbx", "memory");
#elif defined(__aarch64__)
	__asm__ volatile(
		"mov x19, %1\n\t"
		"mov sp, %0\n\t"
		"udf #0\n\t"
		: : "r"(stackPointer), "r"(kMagic) : "x19", "memory");
#endif
	while (true)
		;
}


static void __attribute__((noreturn))
raise_sigill()
{
#if defined(__x86_64__)
	__asm__ volatile("ud2");
#elif defined(__aarch64__)
	__asm__ volatile("udf #0");
#endif
	while (true)
		;
}


static bool
install_handler(int signal, void (*handler)(int), int flags)
{
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = handler;
	action.sa_flags = flags;
	return sigaction(signal, &action, NULL) == 0;
}


static bool
buffer_has_magic()
{
	uint64* words = (uint64*)sBuffer;
	for (size_t i = 0; i < kBufferSize / sizeof(uint64); i++) {
		if (words[i] == kMagic)
			return true;
	}
	return false;
}


static int __attribute__((noinline))
recurse(int depth)
{
	volatile char block[16384];
	if (depth > 1000000)
		return 0;
	block[0] = (char)depth;
	block[sizeof(block) - 1] = (char)depth;
	return recurse(depth + 1) + block[0] + block[sizeof(block) - 1];
}


static status_t
bad_thread(void* data)
{
	const char* name = (const char*)data;

	if (strcmp(name, "plain") == 0)
		raise_sigill();

	if (strcmp(name, "altstack") == 0) {
		sAltStackSize = 4 * SIGSTKSZ;
		void* stack = malloc(sAltStackSize);
		sAltStackBase = (addr_t)stack;
		stack_t altStack;
		altStack.ss_sp = stack;
		altStack.ss_size = sAltStackSize;
		altStack.ss_flags = 0;
		if (stack == NULL || sigaltstack(&altStack, NULL) != 0)
			_exit(kExitSetupFailed);
		install_handler(SIGILL, altstack_handler, SA_ONSTACK);
		raise_sigill();
	}

#if defined(__x86_64__) || defined(__aarch64__)
	if (strcmp(name, "kernelsp") == 0)
		raise_sigill_with_stack(kPhysicalMapBase + sBufferPhysical + 32768);

	if (strcmp(name, "altkernel") == 0) {
		// An alternate stack that starts in user space and ends in the
		// buffer, seen through the kernel's mapping. It must not include
		// this thread's stack, or the kernel takes it for "already on the
		// alternate stack".
		addr_t top = kPhysicalMapBase + sBufferPhysical + 49152;
		if ((addr_t)&name >= kLastUserPage)
			_exit(kExitSetupFailed);
		stack_t altStack;
		altStack.ss_sp = (void*)kLastUserPage;
		altStack.ss_size = top - kLastUserPage;
		altStack.ss_flags = 0;
		if (sigaltstack(&altStack, NULL) != 0) {
			printf("  sigaltstack(): %s\n", strerror(errno));
			_exit(errno == EINVAL ? kExitRefused : kExitSetupFailed);
		}
		install_handler(SIGILL, exiting_handler, SA_ONSTACK);
		raise_sigill_with_stack((addr_t)__builtin_frame_address(0) - 4096);
	}
#endif

	if (strcmp(name, "lowsp") == 0)
		raise_sigill_with_stack(0x1000);

	if (strcmp(name, "unmapped") == 0) {
		void* address;
		area_id area = create_area("gone", &address, B_ANY_ADDRESS, 65536,
			B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
		if (area < 0)
			_exit(kExitSetupFailed);
		delete_area(area);
		raise_sigill_with_stack((addr_t)address + 32768);
	}

	if (strcmp(name, "overflow") == 0) {
		install_handler(SIGSEGV, exiting_handler, 0);
		recurse(0);
	}

	_exit(kExitSetupFailed);
	return 0;
}


static int
run_child(const char* name)
{
	disable_debugger(1);

	// With a handler for SIGSEGV as well, a frame the kernel did write keeps
	// being written: the handler cannot run on that stack, faults, and the
	// next frame goes to the same place. Without one the team would die of
	// that fault before it could be seen.
	if (!install_handler(SIGILL, exiting_handler, 0)
		|| !install_handler(SIGSEGV, exiting_handler, 0)) {
		return kExitSetupFailed;
	}

	// a buffer the kernel's physical memory mapping can point into
	area_id area = create_area("signal frame target", (void**)&sBuffer,
		B_ANY_ADDRESS, kBufferSize, B_CONTIGUOUS,
		B_READ_AREA | B_WRITE_AREA);
	if (area < 0)
		return kExitSetupFailed;
	memset(sBuffer, 0, kBufferSize);
	sBufferPhysical = kUnmappedPhysical;
	int poke = open(POKE_DEVICE_FULLNAME, O_RDWR);
	mem_map_args args;
	memset(&args, 0, sizeof(args));
	args.signature = POKE_SIGNATURE;
	args.address = sBuffer;
	args.size = kBufferSize;
	if (poke >= 0 && ioctl(poke, POKE_GET_PHYSICAL_ADDRESS, &args,
			sizeof(args)) == 0 && args.size >= kBufferSize) {
		sBufferPhysical = args.physical_address;
	} else {
		printf("  no physical address from %s: the target is unmapped\n",
			POKE_DEVICE_FULLNAME);
	}
	if (poke >= 0)
		close(poke);

	thread_id thread = spawn_thread(bad_thread, "bad stack",
		B_NORMAL_PRIORITY, (void*)name);
	if (thread < 0 || resume_thread(thread) != B_OK)
		return kExitSetupFailed;

	// The thread ends the team one way or another. Watch for a frame
	// written to the buffer through the kernel mapping meanwhile.
	bigtime_t end = system_time() + 2000000;
	while (system_time() < end) {
		if (buffer_has_magic()) {
			printf("  the kernel wrote the signal frame through a kernel "
				"address\n");
			return kExitWritten;
		}
		snooze(1000);
	}
	return kExitHung;
}


static bool
run_case(const test_case& testCase)
{
	printf("%s: %s\n", testCase.name, testCase.what);
	fflush(stdout);

	pid_t child = fork();
	if (child < 0) {
		printf("  fork failed: %s\n", strerror(errno));
		return false;
	}
	if (child == 0)
		_exit(run_child(testCase.name));

	int status = 0;
	bigtime_t end = system_time() + 20000000;
	while (true) {
		pid_t result = waitpid(child, &status, WNOHANG);
		if (result == child)
			break;
		if (result < 0 && errno != EINTR) {
			printf("  waitpid failed: %s\n", strerror(errno));
			return false;
		}
		if (system_time() > end) {
			printf("  FAILED: the child did not end\n");
			kill(child, SIGKILL);
			waitpid(child, &status, 0);
			return false;
		}
		snooze(10000);
	}

	if (WIFSIGNALED(status)) {
		printf("  child killed by signal %d\n", WTERMSIG(status));
		if (testCase.expected == kExpectSIGSEGV
			&& WTERMSIG(status) == SIGSEGV) {
			return true;
		}
		printf("  FAILED: expected %s\n",
			testCase.expected == kExpectSIGSEGV ? "SIGSEGV"
				: "a normal exit");
		return false;
	}

	int code = WEXITSTATUS(status);
	switch (code) {
		case kExitHandlerRan:
			printf("  the handler ran\n");
			break;
		case kExitHung:
			printf("  the thread neither died nor got to its handler "
				"(signal delivery loops)\n");
			break;
		case kExitWritten:
			break;
		case kExitRefused:
			printf("  sigaltstack() refused the stack\n");
			break;
		default:
			printf("  the test could not set the case up (exit code %d)\n",
				code);
			return false;
	}

	if ((testCase.expected == kExpectHandler && code == kExitHandlerRan)
		|| (testCase.expected == kExpectRefused && code == kExitRefused)) {
		return true;
	}

	printf("  FAILED: expected %s\n",
		testCase.expected == kExpectSIGSEGV ? "death by SIGSEGV"
			: testCase.expected == kExpectRefused
				? "sigaltstack() to refuse" : "the handler to run");
	return false;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

#if !defined(__x86_64__) && !defined(__aarch64__)
	printf("only x86_64 and arm64 are covered\n");
	return 0;
#endif

	bool ok = true;
	int ran = 0;
	for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
		if (argc > 1 && strcmp(argv[1], kCases[i].name) != 0)
			continue;
		ran++;
		if (!run_case(kCases[i]))
			ok = false;
	}

	if (ran == 0) {
		printf("no such case: %s\n", argc > 1 ? argv[1] : "");
		return 1;
	}

	printf("%s\n", ok ? "PASSED" : "FAILED");
	return ok ? 0 : 1;
}
