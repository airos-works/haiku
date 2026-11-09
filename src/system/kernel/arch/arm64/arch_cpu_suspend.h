/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARCH_ARM64_CPU_SUSPEND_H
#define ARCH_ARM64_CPU_SUSPEND_H


// arm64_suspend_context layout, shared with arch_cpu_suspend.S
#define CTX_X19				0
#define CTX_X21				16
#define CTX_X23				32
#define CTX_X25				48
#define CTX_X27				64
#define CTX_X29				80
#define CTX_SP				96
#define CTX_D8				104
#define CTX_D10				120
#define CTX_D12				136
#define CTX_D14				152
#define CTX_SP_EL0			168
#define CTX_TPIDR_EL0		176
#define CTX_TPIDRRO_EL0		184
#define CTX_TPIDR_EL1		192
#define CTX_TPIDR_EL2		200
#define CTX_HCR_EL2			208
#define CTX_CNTHCTL_EL2		216
#define CTX_CNTVOFF_EL2		224
#define CTX_MDCR_EL2		232
#define CTX_ICC_SRE_EL2		240
#define CTX_VTTBR_EL2		248
#define CTX_SCTLR			256
#define CTX_TCR				264
#define CTX_MAIR			272
#define CTX_TTBR0			280
#define CTX_TTBR1			288
#define CTX_VBAR			296
#define CTX_CONTEXTIDR		304
#define CTX_CNTKCTL			312
#define CTX_CPACR			320
#define CTX_ICC_PMR			328
#define CTX_ICC_BPR1		336
#define CTX_ICC_CTLR		344
#define CTX_ICC_IGRPEN1		352
#define CTX_ICC_SRE_EL1		360
#define CTX_MDSCR			368
#define CTX_DAIF			376
#define CTX_IS_EL2			384
#define CTX_IDMAP_TTBR0		392
#define CTX_IDMAP_TCR		400
#define CTX_RESUME_VA		408
#define CTX_VA				416
#define CTX_CNTP_CTL		424
#define CTX_CNTP_CVAL		432
#define CTX_CNTV_CTL		440
#define CTX_CNTV_CVAL		448
#define CTX_SIZE			512


#ifndef __ASSEMBLER__

#include <SupportDefs.h>

typedef struct arm64_suspend_context {
	uint64	values[CTX_SIZE / 8];
} __attribute__((aligned(64))) arm64_suspend_context;

#ifdef __cplusplus
extern "C" {
#endif

int64 arm64_cpu_suspend(arm64_suspend_context* context, uint64 powerState,
	uint64 entryPhysical, uint64 contextPhysical, uint64 smc);
void arm64_cpu_resume(void);
extern char arm64_cpu_resume_end[];
void arm64_cpu_resume_virtual(void);

#ifdef __cplusplus
}
#endif

#endif	// !__ASSEMBLER__


#endif	// ARCH_ARM64_CPU_SUSPEND_H
