/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _HOST_OS_H
#define _HOST_OS_H

#include <SupportDefs.h>
#include <time.h>

static inline bigtime_t
system_time()
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return (bigtime_t)time.tv_sec * 1000000 + time.tv_nsec / 1000;
}

#endif	// _HOST_OS_H
