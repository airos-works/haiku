/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _HOST_SUPPORT_DEFS_H
#define _HOST_SUPPORT_DEFS_H

//	Just enough of Haiku's SupportDefs.h to build the Neural Kit's graph,
//	loader and interpreter on the build host for testing.

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef int8_t		int8;
typedef uint8_t		uint8;
typedef int16_t		int16;
typedef uint16_t	uint16;
typedef int32_t		int32;
typedef uint32_t	uint32;
typedef int64_t		int64;
typedef uint64_t	uint64;
typedef int32		status_t;
typedef int64		bigtime_t;
typedef uintptr_t	addr_t;

#define _EXPORT

#endif	// _HOST_SUPPORT_DEFS_H
