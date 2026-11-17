/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 *
 * Just enough of Haiku's SupportDefs.h to build the SMP initiator on a host.
 */
#ifndef _TEST_STUB_SUPPORT_DEFS_H
#define _TEST_STUB_SUPPORT_DEFS_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef int32_t status_t;
typedef int64_t bigtime_t;
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef int32_t int32;
typedef uint64_t uint64;
typedef int64_t int64;

#define B_COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

enum {
	B_OK = 0,
	B_ERROR = -1,
	B_NO_MEMORY = -0x7fffffff,
	B_IO_ERROR,
	B_PERMISSION_DENIED,
	B_BAD_VALUE,
	B_TIMED_OUT,
	B_NOT_ALLOWED,
	B_BAD_DATA,
	B_DEV_NOT_READY,
	B_NOT_SUPPORTED,
	B_CANCELED,
	B_BUSY
};

#endif
