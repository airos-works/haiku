/*
 * Copyright 2019 Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef _SYSTEM_ARCH_ARM64_COMMPAGE_DEFS_H
#define _SYSTEM_ARCH_ARM64_COMMPAGE_DEFS_H

#ifndef _SYSTEM_COMMPAGE_DEFS_H
#	error Must not be included directly. Include <commpage_defs.h> instead!
#endif

#define COMMPAGE_ENTRY_ARM64_THREAD_EXIT (COMMPAGE_ENTRY_FIRST_ARCH_SPECIFIC + 0)
#define COMMPAGE_ENTRY_ARM64_SIGNAL_HANDLER (COMMPAGE_ENTRY_FIRST_ARCH_SPECIFIC + 1)
#define COMMPAGE_ENTRY_ARM64_HWCAPS (COMMPAGE_ENTRY_FIRST_ARCH_SPECIFIC + 2)
	// two uint64: the AT_HWCAP and AT_HWCAP2 bits all CPUs share

#endif	/* _SYSTEM_ARCH_ARM64_COMMPAGE_DEFS_H */
