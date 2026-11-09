/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Powering a core down while it is idle (PSCI CPU_SUSPEND with a power-down
 * state) and bringing it back: the firmware restarts the core at a physical
 * address, with its MMU off and its system registers reset, in the
 * exception level CPU_SUSPEND was called from.
 */
#include <asm_defs.h>

#include "arch_cpu_suspend.h"


#define ICC_PMR_EL1			S3_0_C4_C6_0
#define ICC_BPR1_EL1		S3_0_C12_C12_3
#define ICC_CTLR_EL1		S3_0_C12_C12_4
#define ICC_SRE_EL1			S3_0_C12_C12_5
#define ICC_IGRPEN1_EL1		S3_0_C12_C12_7
#define ICC_SRE_EL2			S3_4_C12_C9_5

#define PSCI_CPU_SUSPEND64	0xc4000001


.text

/*	int64 arm64_cpu_suspend(arm64_suspend_context* context (x0, virtual),
		uint64 powerState (x1), uint64 entryPhysical (x2),
		uint64 contextPhysical (x3), uint64 smc (x4))

	Called with interrupts disabled. Returns what CPU_SUSPEND returned when
	the core did not power down (a pending interrupt, for one: 0 or an
	error), and 1 when it did and came back through arm64_cpu_resume.
*/
FUNCTION(arm64_cpu_suspend):
	stp		x19, x20, [x0, #CTX_X19]
	stp		x21, x22, [x0, #CTX_X21]
	stp		x23, x24, [x0, #CTX_X23]
	stp		x25, x26, [x0, #CTX_X25]
	stp		x27, x28, [x0, #CTX_X27]
	stp		x29, x30, [x0, #CTX_X29]
	mov		x9, sp
	str		x9, [x0, #CTX_SP]
	stp		d8, d9, [x0, #CTX_D8]
	stp		d10, d11, [x0, #CTX_D10]
	stp		d12, d13, [x0, #CTX_D12]
	stp		d14, d15, [x0, #CTX_D14]

	mrs		x9, sp_el0
	str		x9, [x0, #CTX_SP_EL0]
	mrs		x9, tpidr_el0
	str		x9, [x0, #CTX_TPIDR_EL0]
	mrs		x9, tpidrro_el0
	str		x9, [x0, #CTX_TPIDRRO_EL0]
	mrs		x9, tpidr_el1
	str		x9, [x0, #CTX_TPIDR_EL1]

	// EL2 (VHE) state: the *_EL1 names below then mean the EL2 registers
	ldr		x9, [x0, #CTX_IS_EL2]
	cbz		x9, 1f
	mrs		x9, tpidr_el2
	str		x9, [x0, #CTX_TPIDR_EL2]
	mrs		x9, hcr_el2
	str		x9, [x0, #CTX_HCR_EL2]
	mrs		x9, cnthctl_el2
	str		x9, [x0, #CTX_CNTHCTL_EL2]
	mrs		x9, cntvoff_el2
	str		x9, [x0, #CTX_CNTVOFF_EL2]
	mrs		x9, mdcr_el2
	str		x9, [x0, #CTX_MDCR_EL2]
	mrs		x9, ICC_SRE_EL2
	str		x9, [x0, #CTX_ICC_SRE_EL2]
	mrs		x9, vttbr_el2
	str		x9, [x0, #CTX_VTTBR_EL2]
1:
	mrs		x9, sctlr_el1
	str		x9, [x0, #CTX_SCTLR]
	mrs		x9, tcr_el1
	str		x9, [x0, #CTX_TCR]
	mrs		x9, mair_el1
	str		x9, [x0, #CTX_MAIR]
	mrs		x9, ttbr0_el1
	str		x9, [x0, #CTX_TTBR0]
	mrs		x9, ttbr1_el1
	str		x9, [x0, #CTX_TTBR1]
	mrs		x9, vbar_el1
	str		x9, [x0, #CTX_VBAR]
	mrs		x9, contextidr_el1
	str		x9, [x0, #CTX_CONTEXTIDR]
	mrs		x9, cntkctl_el1
	str		x9, [x0, #CTX_CNTKCTL]
	mrs		x9, cpacr_el1
	str		x9, [x0, #CTX_CPACR]
	mrs		x9, mdscr_el1
	str		x9, [x0, #CTX_MDSCR]
	mrs		x9, daif
	str		x9, [x0, #CTX_DAIF]

	mrs		x9, ICC_PMR_EL1
	str		x9, [x0, #CTX_ICC_PMR]
	mrs		x9, ICC_BPR1_EL1
	str		x9, [x0, #CTX_ICC_BPR1]
	mrs		x9, ICC_CTLR_EL1
	str		x9, [x0, #CTX_ICC_CTLR]
	mrs		x9, ICC_IGRPEN1_EL1
	str		x9, [x0, #CTX_ICC_IGRPEN1]
	mrs		x9, ICC_SRE_EL1
	str		x9, [x0, #CTX_ICC_SRE_EL1]

	mrs		x9, cntp_ctl_el0
	str		x9, [x0, #CTX_CNTP_CTL]
	mrs		x9, cntp_cval_el0
	str		x9, [x0, #CTX_CNTP_CVAL]
	mrs		x9, cntv_ctl_el0
	str		x9, [x0, #CTX_CNTV_CTL]
	mrs		x9, cntv_cval_el0
	str		x9, [x0, #CTX_CNTV_CVAL]

	// The trampoline reads the context with the MMU and caches off: clean
	// it to the point of coherency.
	mov		x9, x0
	add		x10, x0, #CTX_SIZE
2:	dc		civac, x9
	add		x9, x9, #64
	cmp		x9, x10
	b.lo	2b
	dsb		sy

	mov		x10, x0
	mov		x11, x4
	mov		x0, #(PSCI_CPU_SUSPEND64 & 0xffff)
	movk	x0, #(PSCI_CPU_SUSPEND64 >> 16), lsl #16
	// x1 = power state, x2 = entry point, x3 = context id
	cbz		x11, 3f
	smc		#0
	b		4f
3:	hvc		#0
4:
	// The core did not power down: everything is still in place.
	ret
FUNCTION_END(arm64_cpu_suspend)


/*	Where the firmware restarts the core: x0 = the context's physical
	address, MMU and caches off. This code is copied to a physically
	contiguous page that an identity mapping covers, and must not refer to
	anything by address.
*/
.balign 64
FUNCTION(arm64_cpu_resume):
	mov		x19, x0

	mrs		x1, CurrentEL
	lsr		x1, x1, #2
	cmp		x1, #2
	b.ne	1f

	// EL2: host mode (VHE) first, so that the *_EL1 names mean EL2 again
	ldr		x2, [x19, #CTX_HCR_EL2]
	msr		hcr_el2, x2
	isb
	ldr		x2, [x19, #CTX_CNTHCTL_EL2]
	msr		cnthctl_el2, x2
	ldr		x2, [x19, #CTX_CNTVOFF_EL2]
	msr		cntvoff_el2, x2
	ldr		x2, [x19, #CTX_MDCR_EL2]
	msr		mdcr_el2, x2
	ldr		x2, [x19, #CTX_VTTBR_EL2]
	msr		vttbr_el2, x2
	ldr		x2, [x19, #CTX_ICC_SRE_EL2]
	msr		ICC_SRE_EL2, x2
	isb
	ldr		x2, [x19, #CTX_TPIDR_EL2]
	msr		tpidr_el2, x2
1:
	ldr		x2, [x19, #CTX_MAIR]
	msr		mair_el1, x2
	ldr		x2, [x19, #CTX_IDMAP_TCR]
	msr		tcr_el1, x2
	ldr		x2, [x19, #CTX_IDMAP_TTBR0]
	msr		ttbr0_el1, x2
	ldr		x2, [x19, #CTX_TTBR1]
	msr		ttbr1_el1, x2
	ldr		x2, [x19, #CTX_CPACR]
	msr		cpacr_el1, x2
	ldr		x2, [x19, #CTX_VBAR]
	msr		vbar_el1, x2
	isb

	cmp		x1, #2
	b.ne	2f
	tlbi	alle2
	b		3f
2:	tlbi	vmalle1
3:	dsb		nsh
	isb

	// what comes after the MMU is on, read while it is off
	ldr		x3, [x19, #CTX_RESUME_VA]
	ldr		x0, [x19, #CTX_VA]

	ldr		x2, [x19, #CTX_SCTLR]
	msr		sctlr_el1, x2
	isb
	// now running from the identity mapping
	br		x3
FUNCTION_END(arm64_cpu_resume)

.globl arm64_cpu_resume_end
arm64_cpu_resume_end:


/*	Back in the kernel's address space: x0 = the context's virtual address. */
FUNCTION(arm64_cpu_resume_virtual):
	ldr		x2, [x0, #CTX_TCR]
	msr		tcr_el1, x2
	ldr		x2, [x0, #CTX_TTBR0]
	msr		ttbr0_el1, x2
	isb
	ldr		x1, [x0, #CTX_IS_EL2]
	cbz		x1, 1f
	tlbi	alle2
	b		2f
1:	tlbi	vmalle1
2:	dsb		nsh
	isb

	ldr		x2, [x0, #CTX_SP_EL0]
	msr		sp_el0, x2
	ldr		x2, [x0, #CTX_TPIDR_EL0]
	msr		tpidr_el0, x2
	ldr		x2, [x0, #CTX_TPIDRRO_EL0]
	msr		tpidrro_el0, x2
	ldr		x2, [x0, #CTX_TPIDR_EL1]
	msr		tpidr_el1, x2
	ldr		x2, [x0, #CTX_CONTEXTIDR]
	msr		contextidr_el1, x2
	ldr		x2, [x0, #CTX_CNTKCTL]
	msr		cntkctl_el1, x2
	ldr		x2, [x0, #CTX_MDSCR]
	msr		mdscr_el1, x2

	ldr		x2, [x0, #CTX_ICC_SRE_EL1]
	msr		ICC_SRE_EL1, x2
	isb
	ldr		x2, [x0, #CTX_ICC_PMR]
	msr		ICC_PMR_EL1, x2
	ldr		x2, [x0, #CTX_ICC_BPR1]
	msr		ICC_BPR1_EL1, x2
	ldr		x2, [x0, #CTX_ICC_CTLR]
	msr		ICC_CTLR_EL1, x2
	ldr		x2, [x0, #CTX_ICC_IGRPEN1]
	msr		ICC_IGRPEN1_EL1, x2
	isb

	ldr		x2, [x0, #CTX_CNTP_CVAL]
	msr		cntp_cval_el0, x2
	ldr		x2, [x0, #CTX_CNTP_CTL]
	msr		cntp_ctl_el0, x2
	ldr		x2, [x0, #CTX_CNTV_CVAL]
	msr		cntv_cval_el0, x2
	ldr		x2, [x0, #CTX_CNTV_CTL]
	msr		cntv_ctl_el0, x2
	isb

	ldp		x19, x20, [x0, #CTX_X19]
	ldp		x21, x22, [x0, #CTX_X21]
	ldp		x23, x24, [x0, #CTX_X23]
	ldp		x25, x26, [x0, #CTX_X25]
	ldp		x27, x28, [x0, #CTX_X27]
	ldp		x29, x30, [x0, #CTX_X29]
	ldp		d8, d9, [x0, #CTX_D8]
	ldp		d10, d11, [x0, #CTX_D10]
	ldp		d12, d13, [x0, #CTX_D12]
	ldp		d14, d15, [x0, #CTX_D14]
	ldr		x2, [x0, #CTX_SP]
	mov		sp, x2
	ldr		x2, [x0, #CTX_DAIF]
	msr		daif, x2

	// tell the caller it came back from a power-down
	mov		x0, #1
	ret
FUNCTION_END(arm64_cpu_resume_virtual)
