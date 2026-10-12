/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_RETRACE_H
#define AMDGPU_RETRACE_H

#include <OS.h>
#include <amdgpu_haiku.h>

// The boot frame buffer's vertical blanks (boot_frame_buffer_retrace.h).
// Both are called with the driver lock held; the registers stay mapped
// until amdgpu_retrace_stop().
status_t amdgpu_retrace_semaphore(volatile uint32* registers,
	const amdgpu_info& info, sem_id* _semaphore);
void amdgpu_retrace_stop();
#endif
