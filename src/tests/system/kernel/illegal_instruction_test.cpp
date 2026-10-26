/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	An instruction the CPU does not know raises SIGILL with ILL_ILLOPC, as
	programs probing for CPU features with a SIGILL handler expect (OpenSSL's
	armcap, for one). On arm64 the signal used to be left uninitialized.
*/


#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>


static sigjmp_buf sJump;
static volatile int sSignal;
static volatile int sCode;


static void
handler(int signal, siginfo_t* info, void*)
{
	sSignal = signal;
	sCode = info->si_code;
	siglongjmp(sJump, 1);
}


static void
execute_undefined_instruction()
{
#if defined(__x86_64__) || defined(__i386__)
	asm volatile("ud2");
#elif defined(__aarch64__)
	asm volatile(".inst 0x00000000");
		// udf #0
#elif defined(__riscv)
	asm volatile(".word 0x00000000");
#else
#	error add an undefined instruction for this architecture
#endif
}


int
main()
{
	struct sigaction action = {};
	action.sa_sigaction = handler;
	action.sa_flags = SA_SIGINFO;
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	sigaction(SIGSEGV, &action, NULL);

	if (sigsetjmp(sJump, 1) == 0)
		execute_undefined_instruction();

	printf("undefined instruction: signal %d (%s), code %d\n", sSignal,
		sSignal != 0 ? strsignal(sSignal) : "none", sCode);

	if (sSignal != SIGILL || sCode != ILL_ILLOPC) {
		printf("FAILED: expected SIGILL with ILL_ILLOPC\n");
		return 1;
	}

	printf("passed\n");
	return 0;
}
