// Host stand-in for the Haiku kernel header (tools/rpi3/vc4_host_test).
#pragma once
#include <string.h>
#define ROUNDDOWN(a, b)	(((a) / (b)) * (b))
#define ROUNDUP(a, b)	ROUNDDOWN((a) + (b) - 1, b)
#define IS_USER_ADDRESS(x) true
static inline int user_memcpy(void* to, const void* from, size_t size) { memcpy(to, from, size); return 0; }
