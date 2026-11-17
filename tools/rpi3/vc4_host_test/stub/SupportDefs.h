// Host stand-in for the Haiku kernel header (tools/rpi3/vc4_host_test).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <errno.h>
typedef int8_t int8; typedef uint8_t uint8; typedef int16_t int16; typedef uint16_t uint16;
typedef int32_t int32; typedef uint32_t uint32; typedef int64_t int64; typedef uint64_t uint64;
typedef int32 status_t; typedef uint64 phys_addr_t; typedef int32 area_id; typedef int32 team_id; typedef uintptr_t addr_t;
#define B_OK 0
#define B_ERROR (-1)
#define B_BAD_VALUE (-2147483643)
#define B_NO_MEMORY (-2147483646)
#define B_INTERRUPTED (-2147483638)
#define B_BAD_ADDRESS (-2147478783)
#define B_COUNT_OF(a) (sizeof(a) / sizeof(a[0]))
