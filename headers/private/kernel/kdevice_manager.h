/*
 * Copyright 2004-2008, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_DEVICE_MANAGER_H
#define _KERNEL_DEVICE_MANAGER_H


#include <device_manager.h>
#include <lock.h>

struct kernel_args;


#ifdef __cplusplus
extern "C" {
#endif

void legacy_driver_add_preloaded(struct kernel_args *args);

status_t device_manager_probe(const char *path, uint32 updateCycle);
status_t device_manager_init(struct kernel_args *args);
status_t device_manager_init_post_modules(struct kernel_args *args);

recursive_lock* device_manager_get_lock();

typedef status_t (*device_manager_power_hook)(void* cookie, bool resume,
	int32 state);

status_t device_manager_add_power_hook(device_manager_power_hook hook,
	void* cookie, const char* name);
status_t device_manager_add_power_hook_etc(device_manager_power_hook hook,
	void* cookie, const char* name, uint32 flags);
void device_manager_set_suspend_verbose(bool verbose);
void device_manager_suspend_trace(const char* format, ...);
void device_manager_clear_suspend_trace(void);
size_t device_manager_get_suspend_trace(char* buffer, size_t size);
status_t device_manager_remove_power_hook(device_manager_power_hook hook,
	void* cookie);
enum {
	DEVICE_MANAGER_SKIP_POWER_HOOKS	= 0x01,
	DEVICE_MANAGER_SKIP_DEVICE_TREE	= 0x02,
		// debugging aids for narrowing down a driver that hangs
	DEVICE_MANAGER_LIGHT_SLEEP		= 0x04,
		// leaves out the hooks only for a deep sleep
};

// flags for device_manager_add_power_hook_etc()
enum {
	DEVICE_MANAGER_POWER_HOOK_DEEP_SLEEP	= 0x01,
		// only called for a deep sleep: the device takes long to come back
};

status_t device_manager_suspend(int32 state, uint32 flags);
status_t device_manager_resume(uint32 flags);

void device_manager_wakeup_event(const char* source);
int32 device_manager_wakeup_events(void);

#ifdef __cplusplus
}
#endif

#endif	/* _KERNEL_DEVICE_MANAGER_H */
