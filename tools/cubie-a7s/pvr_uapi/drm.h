/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef PVR_UAPI_DRM_H
#define PVR_UAPI_DRM_H


/*	What the driver's pvr_drm.h needs of Linux's drm.h, for the lab tools
	that talk to the powervr driver without libdrm. */


#include <linux/types.h>


#define DRM_COMMAND_BASE	0x40


#endif	/* PVR_UAPI_DRM_H */
