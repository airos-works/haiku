/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _OS_H
#define _OS_H

#include <time.h>

#include <SupportDefs.h>

static inline bigtime_t
system_time()
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return (bigtime_t)time.tv_sec * 1000000 + time.tv_nsec / 1000;
}

#endif
