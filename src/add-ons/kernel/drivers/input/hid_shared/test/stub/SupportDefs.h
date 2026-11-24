#ifndef _TEST_SUPPORT_DEFS_H
#define _TEST_SUPPORT_DEFS_H
#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include <stdio.h>
typedef int8_t int8; typedef uint8_t uint8; typedef int16_t int16; typedef uint16_t uint16;
typedef int32_t int32; typedef uint32_t uint32; typedef int64_t int64; typedef uint64_t uint64;
typedef int32 status_t; typedef int64 bigtime_t;
#define B_PRIu32 PRIu32
#define B_PRId32 PRId32
#define B_PRIx32 PRIx32
#define B_PRIuSIZE "zu"
#define _PACKED __attribute__((packed))
enum { B_OK = 0, B_ERROR = -1, B_NO_MEMORY = -2147483646, B_BAD_VALUE = -2147483643,
	B_BUSY = -2147483634, B_BAD_DATA = -2147483632, B_NO_INIT = -2147483635 };
#endif
