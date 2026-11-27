/*-
 * Written by Mateusz Guzik <mjg@freebsd.org>
 * Copyright (c) 2023 The FreeBSD Foundation
 *
 * Portions of this software were developed by Robert Clausecker
 * <fuz@FreeBSD.org> under sponsorship from the FreeBSD Foundation.
 *
 * Public domain.
 */

// Imported from FreeBSD's lib/libc/amd64/string/strlen.S, src revision
// 05f2f4129779ccb0857b05ed38ea5b825bae0107.
// Only the baseline (SSE2) implementation, without the scalar one and the
// dispatch between them: SSE2 is part of the x86_64 baseline. The instruction
// sequence is unchanged.

#include "freebsd_asm.h"

#define ALIGN_TEXT      .p2align 4,0x90 /* 16-byte alignment, nop filled */

ARCHENTRY(strlen, baseline)
	mov	%rdi, %rcx
	pxor	%xmm1, %xmm1
	and	$~0xf, %rdi			# align string
	pcmpeqb	(%rdi), %xmm1			# compare head (with junk before string)
	mov	%rcx, %rsi			# string pointer copy for later
	and	$0xf, %ecx			# amount of bytes rdi is past 16 byte alignment
	pmovmskb %xmm1, %eax
	add	$32, %rdi			# advance to next iteration
	shr	%cl, %eax			# clear out matches in junk bytes
	test	%eax, %eax			# any match? (can't use ZF from SHR as CL=0 is possible)
	jnz	2f

	ALIGN_TEXT
1:	pxor	%xmm1, %xmm1
	pcmpeqb	-16(%rdi), %xmm1		# find NUL bytes
	pmovmskb %xmm1, %eax
	test	%eax, %eax			# were any NUL bytes present?
	jnz	3f

	/* the same unrolled once more */
	pxor	%xmm1, %xmm1
	pcmpeqb	(%rdi), %xmm1
	pmovmskb %xmm1, %eax
	add	$32, %rdi			# advance to next iteration
	test	%eax, %eax
	jz	1b

	/* match found in loop body */
	sub	$16, %rdi			# undo half the advancement
3:	tzcnt	%eax, %eax			# find the first NUL byte
	sub	%rsi, %rdi			# string length until beginning of (%rdi)
	lea	-16(%rdi, %rax, 1), %rax	# that plus loc. of NUL byte: full string length
	ret

	/* match found in head */
2:	tzcnt	%eax, %eax			# compute string length
	ret
ARCHEND(strlen, baseline)

	.section .note.GNU-stack,"",%progbits
