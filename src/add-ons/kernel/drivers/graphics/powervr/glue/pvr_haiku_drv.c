/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*	drm/imagination's pvr_drv.c, compiled unchanged: its ioctl handlers and
	its file open and postclose. Its platform driver half (probe, remove,
	power management) is left to the compiler to drop, since PvrDevice does
	that work on Haiku. The only addition is a way to reach the static
	struct drm_driver, for the dispatcher in pvr_haiku_drm.c. */


#include "pvr_drv.c"

#include "pvr_haiku_device.h"


const struct drm_driver*
pvr_haiku_drm_driver(void)
{
	return &pvr_drm_driver;
}
