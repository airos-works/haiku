/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SYS_AUXV_H
#define _SYS_AUXV_H


#include <features.h>


/* getauxval() types, numbered as on other systems */
#define AT_PAGESZ	6
#define AT_HWCAP	16
#define AT_HWCAP2	26

#if defined(__aarch64__)
/* AT_HWCAP bits, as on Linux */
#	define HWCAP_FP			(1UL << 0)
#	define HWCAP_ASIMD		(1UL << 1)
#	define HWCAP_AES		(1UL << 3)
#	define HWCAP_PMULL		(1UL << 4)
#	define HWCAP_SHA1		(1UL << 5)
#	define HWCAP_SHA2		(1UL << 6)
#	define HWCAP_CRC32		(1UL << 7)
#	define HWCAP_ATOMICS	(1UL << 8)
#	define HWCAP_FPHP		(1UL << 9)
#	define HWCAP_ASIMDHP	(1UL << 10)
#	define HWCAP_ASIMDRDM	(1UL << 12)
#	define HWCAP_JSCVT		(1UL << 13)
#	define HWCAP_FCMA		(1UL << 14)
#	define HWCAP_LRCPC		(1UL << 15)
#	define HWCAP_DCPOP		(1UL << 16)
#	define HWCAP_SHA3		(1UL << 17)
#	define HWCAP_SM3		(1UL << 18)
#	define HWCAP_SM4		(1UL << 19)
#	define HWCAP_ASIMDDP	(1UL << 20)
#	define HWCAP_SHA512		(1UL << 21)
#	define HWCAP_ASIMDFHM	(1UL << 23)
#	define HWCAP_DIT		(1UL << 24)
#	define HWCAP_ILRCPC		(1UL << 26)
#	define HWCAP_FLAGM		(1UL << 27)
#	define HWCAP_SB			(1UL << 29)

/* AT_HWCAP2 bits */
#	define HWCAP2_DCPODP	(1UL << 0)
#	define HWCAP2_FLAGM2	(1UL << 7)
#	define HWCAP2_FRINT		(1UL << 8)
#	define HWCAP2_RNG		(1UL << 16)
#endif


#ifdef __cplusplus
extern "C" {
#endif

/* Returns the value of the given type, or 0 (with errno ENOENT) */
extern unsigned long getauxval(unsigned long type);

#ifdef __cplusplus
}
#endif


#endif	/* _SYS_AUXV_H */
