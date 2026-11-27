/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _FREEBSD_ASM_H
#define _FREEBSD_ASM_H

/*!	The macros of FreeBSD's <machine/asm.h> and amd64_archlevel.h that the
	string functions imported from FreeBSD use. Each of them has one
	implementation here, so an entry point is simply the function.
*/

#define CNAME(symbol)	symbol

// The strings may not cross into the next page before their end is known.
// 4 KiB is also the smallest page size, so it does no harm with larger ones.
#define PAGE_SIZE		4096

#define ARCHENTRY(name, level) \
	.text; \
	.p2align 4,0x90; \
	.globl name; \
	.type name, @function; \
	name:; \
	.cfi_startproc

#define ARCHEND(name, level) \
	.size name, . - name; \
	.cfi_endproc

#endif	// _FREEBSD_ASM_H
