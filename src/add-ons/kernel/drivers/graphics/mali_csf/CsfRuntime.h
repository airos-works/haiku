/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_RUNTIME_H
#define MALI_CSF_RUNTIME_H

#include "CsfQueue.h"
#include "CsfResources.h"

// Create/destroy/close are serialized with legacy hardware cycles by the
// caller's hardware lock. Submit/wait/info must not hold that lock.
status_t ControlQueues(const MaliCSF::ResourceInfo& resources, void* client,
	uint32 op, void* user, size_t length, bool& needsRecovery, void* syncClient = NULL);
void CloseQueues(void* client, bool& needsRecovery);
// From the driver settings (#182): whether the scheduler sleeps on the job
// interrupt ("job_interrupt_wait"), and its thread's priority
// ("scheduler_priority"). Read by the next runtime that starts.
void SetRuntimeOptions(bool jobInterruptWait, int32 schedulerPriority);

#endif
