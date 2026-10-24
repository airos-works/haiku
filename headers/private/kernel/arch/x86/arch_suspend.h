/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_ARCH_X86_SUSPEND_H
#define _KERNEL_ARCH_X86_SUSPEND_H


#include <SupportDefs.h>


#define X86_SUSPEND_SYSCALLS			"x86_suspend"
#define X86_SUSPEND_SYSCALLS_VERSION	1

// generic syscall functions
enum {
	X86_SUSPEND_RESTART_CPU		= 1,
		// int32 cpu: park the CPU and restart it through the wakeup
		// trampoline, without sleeping
	X86_SUSPEND_ENTER_S3		= 2,
		// uint32 flags or struct x86_suspend_s3_args
	X86_SUSPEND_GET_TRACE		= 3,
		// char buffer: the trace of the last suspend and resume
	X86_SUSPEND_ENTER_S2IDLE	= 4,
		// struct x86_suspend_s2idle_args: sleeps in S0 until the SCI (lid,
		// power button) or the timeout, and reports how it went
};

struct x86_suspend_s2idle_args {
	uint32	flags;
		// X86_SUSPEND_SKIP_DEVICES, _SKIP_POWER_HOOKS, _SKIP_DEVICE_TREE
	uint32	timeout_ms;
		// 0 sleeps until the SCI

	// returned
	uint32	wake_reason;
	uint32	idle_rounds;
	uint64	slept_us;
	uint64	s0i3_us;
		// how long the SoC stayed in its deepest state (AMD), last time
	uint32	s0i3_entered;
	uint32	pmc_idle_mask;
		// AMD: which IP blocks were still busy
	uint32	s0i3_blockers;
		// AMD: the IP blocks that kept the SoC out of S0i3, as the power
		// management firmware logged it (bit 0 display, 2 GFX, 5 VCN, 9 and
		// 10 the USB 3 controllers, ...)
};

// wake_reason
enum {
	X86_S2IDLE_WAKE_SCI		= 1,
	X86_S2IDLE_WAKE_TIMEOUT	= 2,
};

// Debugging aid: without working devices after resume, the only observable
// progress is the power state. When power_off_checkpoint is non-zero, the
// resume path powers the machine off once it reaches that checkpoint.
struct x86_suspend_s3_args {
	uint32	flags;
	uint32	power_off_checkpoint;
};

// flags for X86_SUSPEND_ENTER_S3
enum {
	X86_SUSPEND_POWER_OFF_AFTER_RESUME	= 0x01,
		// power the machine off a few seconds after resuming (for testing
		// the core resume path without working devices)
	X86_SUSPEND_SKIP_DEVICES			= 0x02,
		// don't suspend and resume devices
	X86_SUSPEND_SKIP_POWER_HOOKS		= 0x08,
		// don't call the drivers registered outside the device tree
	X86_SUSPEND_SKIP_DEVICE_TREE		= 0x10,
		// don't suspend and resume the device tree
	X86_SUSPEND_REBOOT_AFTER_RESUME		= 0x20,
		// reboot a few seconds after resuming; the trace of the resume is
		// kept in memory and printed by the next boot
	X86_SUSPEND_POWER_OFF_ON_CPU_ERROR	= 0x40,
		// power off when an application processor did not come back
	X86_SUSPEND_VERBOSE					= 0x04,
		// log every step and pause afterwards, so that the log is written
		// before a step that hangs
	X86_SUSPEND_LIGHT_SLEEP				= 0x100,
		// s2idle: leaves out the power hooks only for a deep sleep (the
		// graphics)
	X86_SUSPEND_IGNORE_LID				= 0x200,
		// s2idle, for testing: sleeps on through SCIs as with the lid
		// closed
	X86_SUSPEND_RTC_WAKE_ONLY			= 0x80,
		// s2idle, for testing: only the RTC alarm ends the timeout (no local
		// APIC deadline, which stops in S0i3 anyway)
};


#ifdef __cplusplus
extern "C" {
#endif

status_t x86_suspend_init(void);
status_t x86_suspend_restart_cpu(int32 cpu);
status_t x86_suspend_enter_s3(const struct x86_suspend_s3_args* args);
status_t x86_suspend_enter_s2idle(struct x86_suspend_s2idle_args* args);
void x86_suspend_save_trace(void);

#ifdef __cplusplus
}
#endif


#endif	// _KERNEL_ARCH_X86_SUSPEND_H
