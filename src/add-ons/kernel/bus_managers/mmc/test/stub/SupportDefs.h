/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
/*	Host test stand-in for Haiku's SupportDefs.h: the types and error codes
	sdio_card.cpp uses. */
#ifndef _SUPPORT_DEFS_H
#define _SUPPORT_DEFS_H

#include <stddef.h>
#include <stdint.h>

typedef int32_t status_t;
typedef int64_t bigtime_t;
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int32_t int32;

#define B_OK				0
#define B_ERROR				(-1)
#define B_GENERAL_ERROR_BASE	(-2147483647 - 1)
#define B_NO_MEMORY			(B_GENERAL_ERROR_BASE + 0)
#define B_IO_ERROR			(B_GENERAL_ERROR_BASE + 1)
#define B_BAD_VALUE			(B_GENERAL_ERROR_BASE + 5)
#define B_TIMED_OUT			(B_GENERAL_ERROR_BASE + 9)
#define B_NOT_SUPPORTED		(B_GENERAL_ERROR_BASE + 0x1000 + 21)
#define B_BAD_DATA			(B_GENERAL_ERROR_BASE + 0x1000 + 22)
#define B_DEVICE_NOT_FOUND	(B_GENERAL_ERROR_BASE + 0x6000 + 14)
#define B_ENTRY_NOT_FOUND	(B_GENERAL_ERROR_BASE + 0x6000 + 3)

#define B_PRIx32 "x"
#define B_PRIu32 "u"

#endif
