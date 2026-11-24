/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <asm_defs.h>


	.text
	.hidden	arch_lazy_binding_entry
	.hidden	lazy_binding_fixup
	.hidden	gLazyBindingSaveSize
	.hidden	gLazyBindingXsaveMask


/*	The PLT0 of an image bound lazily jumps here (GOT[2]) on the first call
	through one of its jump slots, with
		 0(%rsp)	GOT[1]: the image_t
		 8(%rsp)	the index of the jump slot's relocation
		16(%rsp)	the return address into the caller
	and every argument register still the caller's: %rdi, %rsi, %rdx, %rcx,
	%r8, %r9, %rax (the vector register count of a variadic call), %r10 (the
	static chain) and the vector registers, up to their AVX-512 width. All of
	them are saved around lazy_binding_fixup(), which writes the slot and
	returns the function, which is then jumped to as if called directly.
	%r11 is the only register changed; the ABI lets PLT stubs use it.
*/
	.p2align 4
FUNCTION(arch_lazy_binding_entry):
	.cfi_startproc
	.cfi_def_cfa_offset 24
	pushq	%rbp
	.cfi_def_cfa_offset 32
	.cfi_offset %rbp, -32
	movq	%rsp, %rbp
	.cfi_def_cfa_register %rbp

	pushq	%rax
	pushq	%rcx
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
	pushq	%r8
	pushq	%r9
	pushq	%r10

	subq	gLazyBindingSaveSize(%rip), %rsp
	andq	$-64, %rsp

	movl	gLazyBindingXsaveMask(%rip), %eax
	testl	%eax, %eax
	jz		1f

	// XRSTOR faults unless the reserved bytes of the XSAVE header (which XSAVE
	// leaves alone) are zero.
	xorl	%eax, %eax
	movq	%rax, 512(%rsp)
	movq	%rax, 520(%rsp)
	movq	%rax, 528(%rsp)
	movq	%rax, 536(%rsp)
	movq	%rax, 544(%rsp)
	movq	%rax, 552(%rsp)
	movq	%rax, 560(%rsp)
	movq	%rax, 568(%rsp)
	movl	gLazyBindingXsaveMask(%rip), %eax
	movl	gLazyBindingXsaveMask+4(%rip), %edx
	xsave64	(%rsp)
	jmp		2f
1:
	fxsave64 (%rsp)
2:

	movq	8(%rbp), %rdi
	movq	16(%rbp), %rsi
	call	lazy_binding_fixup
	movq	%rax, %r11

	movl	gLazyBindingXsaveMask(%rip), %eax
	testl	%eax, %eax
	jz		3f
	movl	gLazyBindingXsaveMask+4(%rip), %edx
	xrstor64 (%rsp)
	jmp		4f
3:
	fxrstor64 (%rsp)
4:

	leaq	-64(%rbp), %rsp
	popq	%r10
	popq	%r9
	popq	%r8
	popq	%rdi
	popq	%rsi
	popq	%rdx
	popq	%rcx
	popq	%rax
	popq	%rbp
	.cfi_def_cfa %rsp, 24
	addq	$16, %rsp
	.cfi_def_cfa_offset 8
	jmp		*%r11
	.cfi_endproc
FUNCTION_END(arch_lazy_binding_entry)

