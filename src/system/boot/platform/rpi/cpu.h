/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CPU_H
#define CPU_H


#include <SupportDefs.h>


void cpu_init();
void cpu_reset_after(bigtime_t timeout);
void watchdog_init(phys_addr_t base);
void watchdog_pet();
	// restarts the board through PSCI, where there is one


#endif	/* CPU_H */
