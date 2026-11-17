/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TEST_STUB_OS_H
#define _TEST_STUB_OS_H

#include <SupportDefs.h>
#include <time.h>
#include <unistd.h>

static inline bigtime_t
system_time()
{
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (bigtime_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static inline void
snooze(bigtime_t time)
{
	usleep(time);
}

#endif
