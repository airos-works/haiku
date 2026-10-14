/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef POWERVR_A733_POWER_H
#define POWERVR_A733_POWER_H


#include <OS.h>


namespace powervr {


/*	The Allwinner A733's GPU (BXM-4-64 MC1): power domains, core clock
	(800 or 600 MHz, from pll-peri0-800m or -600m), bus gate and reset, as
	Allwinner's BSP does it. The 0.8 V supply is AXP8191 DCDC4. */
status_t	a733_gpu_power_on(uint32 megahertz, uint32* _coreClock);


}	// namespace powervr


#endif	// POWERVR_A733_POWER_H
