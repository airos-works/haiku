/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	signal_frame_forge_test [case]: a signal handler that changes the saved
	processor state the kernel restores with its own privileges when the
	handler returns (airos-works/haiku#158).

	The saved context is on the user stack, and the program's to write. What
	the kernel loads from it in its return path must be something a program
	may have, or a fault there is the kernel's:
	- x86_64: the segment selectors and the instruction pointer for IRET (a
	  kernel code selector is a return to ring 0; a selector or address the
	  processor refuses is a general protection fault in the kernel, after
	  the switch to the user's GS base), the XSAVE area for XRSTOR (a header
	  with unknown or reserved bits, a reserved bit in MXCSR).
	- arm64: SPSR (the exception level to return to, the interrupt masks),
	  and, while at it, the immediate of an svc (the syscall number) past the
	  end of the syscall table.

	Every case runs in a child of its own. The child must either carry on in
	user mode with a sane state, or die of a signal; the kernel must still
	be there. Without an argument all cases run; with a case name only that
	one (a kernel that fails a case does not get to the next one).
*/


#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>


enum {
	kExitCarriedOn = 0,
	kExitWrongState = 1,
	kExitFault = 42
};

struct test_case {
	const char*	name;
	const char*	what;
	int			expected;	// exit code of the child
};

static const char* sCase;


static void
fault_handler(int signal)
{
	_exit(kExitFault);
}


#if defined(__x86_64__)


static const test_case kCases[] = {
	{ "plain", "nothing changed", kExitCarriedOn },
	{ "cs0", "cs = ss = 0 (a context the program built itself)",
		kExitCarriedOn },
	{ "kernelcs", "cs/ss = the kernel's selectors (ring 0)", kExitCarriedOn },
	{ "badcs", "cs = 0x1234, ss = 0x4321 (no such descriptors)",
		kExitCarriedOn },
	{ "rip", "rip = a non-canonical address", kExitFault },
	{ "xstate_bv", "XSAVE header: every state component bit set",
		kExitCarriedOn },
	{ "xcomp_bv", "XSAVE header: a compaction mask that fits nothing",
		kExitCarriedOn },
	{ "xsave_reserved", "XSAVE header: reserved bytes set", kExitCarriedOn },
	{ "mxcsr", "MXCSR = 0xffffffff (reserved bits)", kExitCarriedOn },
	{ "fpuzero", "the whole saved FPU area cleared", kExitCarriedOn },
	{ "fpuones", "the whole saved FPU area set to 0xff", kExitCarriedOn },
};


static void
forging_handler(int signal, siginfo_t* info, void* _context)
{
	ucontext_t* context = (ucontext_t*)_context;
	struct savefpu* fpu = &context->uc_mcontext.fpu;

	if (strcmp(sCase, "cs0") == 0) {
		fpu->fp_fxsave.cs = 0;
		fpu->fp_fxsave.ss = 0;
	} else if (strcmp(sCase, "kernelcs") == 0) {
		fpu->fp_fxsave.cs = 0x08;
		fpu->fp_fxsave.ss = 0x10;
	} else if (strcmp(sCase, "badcs") == 0) {
		fpu->fp_fxsave.cs = 0x1234;
		fpu->fp_fxsave.ss = 0x4321;
	} else if (strcmp(sCase, "rip") == 0) {
		context->uc_mcontext.rip = 0x0000800000001000UL;
	} else if (strcmp(sCase, "xstate_bv") == 0) {
		fpu->fp_xstate.bv = ~0UL;
	} else if (strcmp(sCase, "xcomp_bv") == 0) {
		fpu->fp_xstate.xcomp_bv = 0x8f0f00ff00ff0f00UL;
	} else if (strcmp(sCase, "xsave_reserved") == 0) {
		memset(fpu->fp_xstate._reserved, 0xff, sizeof(fpu->fp_xstate._reserved));
	} else if (strcmp(sCase, "mxcsr") == 0) {
		fpu->fp_fxsave.mxcsr = 0xffffffff;
	} else if (strcmp(sCase, "fpuzero") == 0) {
		memset(fpu, 0, sizeof(*fpu));
	} else if (strcmp(sCase, "fpuones") == 0) {
		memset(fpu, 0xff, sizeof(*fpu));
	}
}


static int
check_state_after_return()
{
	// still in ring 3, with the user's selectors?
	uint16 cs, ss;
	asm volatile("movw %%cs, %0" : "=r" (cs));
	asm volatile("movw %%ss, %0" : "=r" (ss));
	if ((cs & 3) != 3 || (ss & 3) != 3) {
		// (printing may not work anymore; the exit code says it)
		return kExitWrongState;
	}

	uint32 mxcsr;
	asm volatile("stmxcsr %0" : "=m" (mxcsr));
	if (strcmp(sCase, "mxcsr") == 0 && (mxcsr & 0xffff0000) != 0)
		return kExitWrongState;

	// the FPU state may be anything the handler asked for (all exceptions
	// unmasked, say): start over before using it
	mxcsr = 0x1f80;
	asm volatile("ldmxcsr %0; fninit" : : "m" (mxcsr));

	volatile double a = 1.5, b = 2.25;
	if (a * b != 3.375)
		return kExitWrongState;

	// and the kernel still takes syscalls from this thread
	if (getppid() <= 0 || find_thread(NULL) < 0)
		return kExitWrongState;

	printf("    child: carried on, cs %#x ss %#x\n", cs, ss);
	return kExitCarriedOn;
}


#elif defined(__aarch64__)


static const test_case kCases[] = {
	{ "plain", "nothing changed", kExitCarriedOn },
	{ "spsr_el1", "SPSR: EL1h, all interrupts masked", kExitCarriedOn },
	{ "spsr_ones", "SPSR: every bit set", kExitCarriedOn },
	{ "spsr_aarch32", "SPSR: AArch32 state", kExitCarriedOn },
	{ "svc", "svc with numbers past the syscall table", kExitCarriedOn },
};


static void
forging_handler(int signal, siginfo_t* info, void* _context)
{
	ucontext_t* context = (ucontext_t*)_context;

	if (strcmp(sCase, "spsr_el1") == 0)
		context->uc_mcontext.spsr |= 0x3c5;
	else if (strcmp(sCase, "spsr_ones") == 0)
		context->uc_mcontext.spsr = ~0UL;
	else if (strcmp(sCase, "spsr_aarch32") == 0)
		context->uc_mcontext.spsr |= 0x10;
}


#define BAD_SVC(number) \
	({ \
		register long x0 asm("x0") = 0; \
		asm volatile("svc #" #number : "+r" (x0) : : "memory", "cc", \
			"x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", \
			"x11", "x12", "x13", "x14", "x15", "x16", "x17"); \
		x0; \
	})


static int
check_state_after_return()
{
	if (strcmp(sCase, "svc") == 0) {
		long results[] = { BAD_SVC(0xffff), BAD_SVC(0x8000), BAD_SVC(0x4000),
			BAD_SVC(0x1000), BAD_SVC(0x0800), BAD_SVC(0x0400) };
		for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
			printf("    child: bad svc %zu returned %#lx\n", i, results[i]);
			if (results[i] >= 0)
				return kExitWrongState;
		}
	}

	// A thread at EL0 with interrupts masked would keep its CPU: with as
	// many of those as there are CPUs nothing else runs. One is enough to
	// see whether this one can still be preempted: it computes, and its
	// time slice ends.
	volatile uint64 count = 0;
	bigtime_t end = system_time() + 200000;
	while (system_time() < end)
		count++;

	if (getppid() <= 0 || find_thread(NULL) < 0)
		return kExitWrongState;

	printf("    child: carried on\n");
	return kExitCarriedOn;
}


#else


static const test_case kCases[] = {
	{ "plain", "nothing changed", kExitCarriedOn },
};


static void
forging_handler(int signal, siginfo_t* info, void* _context)
{
}


static int
check_state_after_return()
{
	return kExitCarriedOn;
}


#endif


static int
run_child()
{
	signal(SIGSEGV, fault_handler);
	signal(SIGBUS, fault_handler);
	signal(SIGILL, fault_handler);
	signal(SIGFPE, fault_handler);

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = forging_handler;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGUSR1, &action, NULL);

	raise(SIGUSR1);

	return check_state_after_return();
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
	if (child == 0) {
		sCase = testCase.name;
		_exit(run_child());
	}

	// a deadline for the child
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
			printf("  FAILED: the child neither returned nor died\n");
			kill(child, SIGKILL);
			return false;
		}
		snooze(10000);
	}

	if (WIFSIGNALED(status)) {
		// killed by the kernel: a refusal, and fine for a forged frame
		printf("  child killed by signal %d\n", WTERMSIG(status));
		return testCase.expected != kExitCarriedOn
			|| strcmp(testCase.name, "plain") != 0;
	}

	int code = WEXITSTATUS(status);
	if (code == kExitFault) {
		printf("  child got a fault signal in user mode\n");
		// acceptable for every forged frame, required for some
		return strcmp(testCase.name, "plain") != 0;
	}
	if (code == kExitCarriedOn) {
		printf("  child carried on\n");
		if (testCase.expected != kExitCarriedOn) {
			printf("  FAILED: it should not have\n");
			return false;
		}
		return true;
	}

	printf("  FAILED: child left with a wrong state (exit code %d)\n", code);
	return false;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);

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
