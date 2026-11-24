/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	The floating-point control state belongs to the program across a system
	call: the rounding mode, the exception masks, flush-to-zero and
	denormals-are-zero in MXCSR, and the x87 control word (precision,
	rounding). On processors with AVX-512 the kernel reset all of them on
	every return from a system call, with the XRSTOR that clears the vector
	registers (airos-works/haiku#167).

	Sets each of them, makes system calls (also from another thread), and
	reads them back.

	fpu_control_syscall_test signal: also has a signal handled on the way out
	of a system call. That loses the control state on every processor, and
	is another matter: the signal frame of a thread in a system call is made
	from the kernel's initial FPU state, control state included, and
	restored when the handler returns.
*/


#include <fenv.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>


#if defined(__x86_64__)


static uint32
get_mxcsr()
{
	uint32 value;
	asm volatile("stmxcsr %0" : "=m" (value));
	return value;
}


static void
set_mxcsr(uint32 value)
{
	asm volatile("ldmxcsr %0" : : "m" (value));
}


static uint16
get_x87_control()
{
	uint16 value;
	asm volatile("fnstcw %0" : "=m" (value));
	return value;
}


static void
set_x87_control(uint16 value)
{
	asm volatile("fldcw %0" : : "m" (value));
}


static bool sWithSignal;


static void
handler(int)
{
}


static void
make_syscalls()
{
	// plain ones, one that blocks, one that takes its arguments from the
	// stack
	for (int i = 0; i < 100; i++) {
		getuid();
		find_thread(NULL);
	}
	snooze(2000);
	thread_info info;
	get_thread_info(find_thread(NULL), &info);
	area_id area = create_area("fpu test", NULL, B_ANY_ADDRESS, B_PAGE_SIZE,
		B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	delete_area(area);
	if (sWithSignal)
		raise(SIGUSR1);
	getpid();
}


static bool
check(const char* what, uint32 expectedMXCSR, uint16 expectedControl)
{
	set_mxcsr(expectedMXCSR);
	set_x87_control(expectedControl);

	make_syscalls();

	// (the status flags, the low six bits of MXCSR, are not control state)
	uint32 mxcsr = get_mxcsr();
	uint16 control = get_x87_control();
	bool ok = (mxcsr & ~0x3fu) == (expectedMXCSR & ~0x3fu)
		&& control == expectedControl;
	printf("%-44s MXCSR %#06x -> %#06x, x87 control %#06x -> %#06x: %s\n",
		what, expectedMXCSR, mxcsr, expectedControl, control,
		ok ? "kept" : "LOST");
	return ok;
}


static status_t
other_thread(void* _ok)
{
	if (!check("in another thread: FTZ, round down", 0x1f80 | 0x8000 | 0x2000,
			0x077f)) {
		*(bool*)_ok = false;
	}
	return B_OK;
}


int
main(int argc, char** argv)
{
	sWithSignal = argc > 1 && strcmp(argv[1], "signal") == 0;
	signal(SIGUSR1, handler);

	uint32 eax = 7, ebx = 0, ecx = 0, edx = 0;
	asm volatile("cpuid" : "+a" (eax), "=b" (ebx), "+c" (ecx), "=d" (edx));
	printf("this processor has %sAVX-512\n",
		(ebx & (1 << 16)) != 0 ? "" : "no ");

	bool ok = true;

	// the defaults first
	ok &= check("defaults", 0x1f80, 0x037f);
	// flush-to-zero and denormals-are-zero (what audio code sets)
	ok &= check("FTZ + DAZ", 0x1f80 | 0x8040, 0x037f);
	// rounding modes
	ok &= check("round up", 0x1f80 | 0x4000, 0x0b7f);
	ok &= check("round towards zero", 0x1f80 | 0x6000, 0x0f7f);
	// an exception unmasked (invalid operation), x87 at double precision
	ok &= check("invalid unmasked, x87 double precision", 0x1f80 & ~0x80u,
		0x027e);

	// through the C library
	set_mxcsr(0x1f80);
	set_x87_control(0x037f);
	fesetround(FE_UPWARD);
	make_syscalls();
	bool kept = fegetround() == FE_UPWARD;
	printf("%-44s %s\n", "fesetround(FE_UPWARD)", kept ? "kept" : "LOST");
	ok &= kept;
	fesetround(FE_TONEAREST);

	thread_id thread = spawn_thread(other_thread, "fpu test", B_NORMAL_PRIORITY,
		&ok);
	resume_thread(thread);
	wait_for_thread(thread, NULL);

	set_mxcsr(0x1f80);
	set_x87_control(0x037f);

	printf("%s\n", ok ? "PASSED" : "FAILED");
	return ok ? 0 : 1;
}


#else


int
main()
{
	printf("x86_64 only\n");
	return 0;
}


#endif
