/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <asm_defs.h>


	.text
	.hidden	arch_lazy_binding_entry
	.hidden	lazy_binding_fixup


/*	The PLT0 of an image bound lazily jumps here (GOT[2]) on the first call
	through one of its jump slots, with
		x16			&GOT[2]
		x17			this function
		[sp]		&GOT[n], the jump slot called through (n >= 3)
		[sp, #8]	the caller's x30, which x30 still holds as well
	and every argument register still the caller's: x0-x7, x8 (the indirect
	result location) and q0-q7. They are saved around lazy_binding_fixup(),
	which writes the slot and returns the function, which is then branched to
	as if called directly. x16 and x17 are the only registers changed; the
	ABI lets PLT stubs use them.
*/
	.p2align 4
FUNCTION(arch_lazy_binding_entry):
	.cfi_startproc
	.cfi_def_cfa_offset 16
	.cfi_offset x30, -8
	stp		x29, x30, [sp, #-224]!
	.cfi_def_cfa_offset 240
	mov		x29, sp
	stp		x0, x1, [sp, #16]
	stp		x2, x3, [sp, #32]
	stp		x4, x5, [sp, #48]
	stp		x6, x7, [sp, #64]
	str		x8, [sp, #80]
	stp		q0, q1, [sp, #96]
	stp		q2, q3, [sp, #128]
	stp		q4, q5, [sp, #160]
	stp		q6, q7, [sp, #192]

	// GOT[1] is the image_t; slot n belongs to PLT relocation n - 3.
	ldr		x0, [x16, #-8]
	ldr		x1, [sp, #224]
	sub		x1, x1, x16
	sub		x1, x1, #8
	lsr		x1, x1, #3
	bl		lazy_binding_fixup
	mov		x16, x0

	ldp		q6, q7, [sp, #192]
	ldp		q4, q5, [sp, #160]
	ldp		q2, q3, [sp, #128]
	ldp		q0, q1, [sp, #96]
	ldr		x8, [sp, #80]
	ldp		x6, x7, [sp, #64]
	ldp		x4, x5, [sp, #48]
	ldp		x2, x3, [sp, #32]
	ldp		x0, x1, [sp, #16]
	ldp		x29, x30, [sp], #224
	.cfi_def_cfa_offset 16
	// drop &GOT[n], take back the caller's x30
	ldp		x17, x30, [sp], #16
	.cfi_def_cfa_offset 0
	br		x16
	.cfi_endproc
FUNCTION_END(arch_lazy_binding_entry)
