/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SUPPORT_DEFS_H
#define _SUPPORT_DEFS_H

// The few Haiku definitions the emulator uses, for testing it on another
// system.

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef int8_t int8;
typedef uint8_t uint8;
typedef int16_t int16;
typedef uint16_t uint16;
typedef int32_t int32;
typedef uint32_t uint32;
typedef int64_t int64;
typedef uint64_t uint64;
typedef int32 status_t;
typedef int64 bigtime_t;

#define B_OK			0
#define B_ERROR			(-1)
#define B_NO_MEMORY		(-2147483647 - 1)
#define B_BAD_VALUE		(-2147483647 + 4)
#define B_INFINITE_TIMEOUT	((bigtime_t)0x7fffffffffffffffLL)

#endif
