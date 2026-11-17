/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
/*	Host test stand-in for Haiku's device_manager.h: what sdio.h's interface
	definitions need. */
#ifndef _DEVICE_MANAGER_H
#define _DEVICE_MANAGER_H

#include <SupportDefs.h>

typedef struct device_node device_node;
typedef struct driver_module_info {
	void* info[4];
} driver_module_info;

#endif
