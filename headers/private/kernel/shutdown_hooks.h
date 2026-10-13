/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_SHUTDOWN_HOOKS_H
#define _KERNEL_SHUTDOWN_HOOKS_H


#include <SupportDefs.h>


/*!	Called when the system is about to be powered off or rebooted, after the
	teams have been told to go and the file systems were synced: for drivers
	whose hardware must be put into a known state that a reboot does not
	reset by itself (GPUs integrated into the processor, for instance). The
	hooks run in the reverse order of their registration, in a normal thread.
*/
typedef void (*shutdown_hook)(bool reboot, void* cookie);


#ifdef __cplusplus
extern "C" {
#endif

status_t register_shutdown_hook(shutdown_hook hook, void* cookie);
status_t unregister_shutdown_hook(shutdown_hook hook, void* cookie);

#ifdef __cplusplus
}
#endif


#endif	/* _KERNEL_SHUTDOWN_HOOKS_H */
