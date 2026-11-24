/*
 * Copyright 2018-2020, Jérôme Duval, jerome.duval@gmail.com.
 * Distributed under the terms of the MIT License.
 */


#include <asm_defs.h>

#include "asm_offsets.h"


.section .rodata

FUNCTION(_stac):
	stac
FUNCTION_END(_stac)

FUNCTION(_clac):
	clac
FUNCTION_END(_clac)

FUNCTION(_xsave):
	xsave64		(%rdi)
FUNCTION_END(_xsave)

FUNCTION(_xsavec):
	xsavec64	(%rdi)
FUNCTION_END(_xsavec)

FUNCTION(_xrstor):
	xrstor64	(%rdi)
FUNCTION_END(_xrstor)

FUNCTION(_vzeroall):
	vzeroall
FUNCTION_END(_vzeroall)

// Clears the vector registers of a processor with more state than AVX
// (AVX-512): VZEROALL for XMM/YMM0-15 (and with them the upper halves of
// ZMM0-15), XRSTOR from the initial state for everything else. The x87, SSE
// and AVX components are left out of the XRSTOR mask (bits 0-2): with any of
// them it would also load the x87 control word or MXCSR, which belong to the
// program across a system call (rounding mode, exception masks, flush to
// zero).
FUNCTION(_vzeroall_xrstor_initial):
	vzeroall
	movabsq		$gInitialState + ARCH_THREAD_user_fpu_state, %rdi
	mov 		$0xFFFFFFF8, %eax
	mov 		$0xFFFFFFFF, %edx
	xrstor64	(%rdi)
FUNCTION_END(_vzeroall_xrstor_initial)
