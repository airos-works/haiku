/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "accelerant.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Drivers.h>


#define TRACE(x...) debug_printf("amdgpu_kms: " x)


accelerant_info* gInfo;


static status_t
init_common(int device, bool isClone)
{
	gInfo = (accelerant_info*)calloc(1, sizeof(accelerant_info));
	if (gInfo == NULL)
		return B_NO_MEMORY;
	gInfo->device = device;
	gInfo->isClone = isClone;
	gInfo->sharedArea = -1;
	gInfo->clonedFrameBuffer = -1;
	gInfo->retraceSemaphore = -1;
	gInfo->retraceThread = -1;
	gInfo->frameBuffer.area = -1;
	gInfo->cursor.area = -1;
	gInfo->changePort = -1;
	gInfo->dpmsMode = B_DPMS_ON;
	return B_OK;
}


static void
uninit_common(void)
{
	if (gInfo == NULL)
		return;
	free(gInfo->modeList);
	free(gInfo);
	gInfo = NULL;
}


static uint32
first_object(const uint32* objects, uint32 count)
{
	return count > 0 ? objects[0] : 0;
}


extern "C" status_t
amdgpu_init_accelerant(int device)
{
	status_t status = init_common(device, false);
	if (status != B_OK)
		return status;

	// atomic mode setting and every plane
	if (Kms::SetClientCap(device, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != B_OK
		|| Kms::SetClientCap(device, DRM_CLIENT_CAP_ATOMIC, 1) != B_OK) {
		TRACE("no atomic mode setting\n");
		uninit_common();
		return B_NOT_SUPPORTED;
	}
	// the first opener is the master already; ask in case it is not
	Kms::Ioctl(device, DRM_IOCTL_SET_MASTER, NULL);

	status = Kms::GetResources(device, gInfo->resources);
	if (status == B_OK) {
		gInfo->planeCount = Kms::kMaxObjects;
		status = Kms::GetPlanes(device, gInfo->planes, gInfo->planeCount);
	}
	if (status != B_OK || gInfo->resources.crtcCount == 0) {
		TRACE("no display resources: %s\n", strerror(status));
		uninit_common();
		return status != B_OK ? status : B_ERROR;
	}
	for (uint32 c = 0; c < gInfo->resources.crtcCount; c++) {
		for (uint32 p = 0; p < gInfo->planeCount; p++) {
			if (gInfo->planes[p].type == Kms::kPlanePrimary
				&& (gInfo->planes[p].possibleCrtcs & (1 << c)) != 0) {
				gInfo->crtcPrimaryPlane[c] = gInfo->planes[p].id;
				break;
			}
		}
	}

	uint32 connector = first_object(gInfo->resources.connectors,
		gInfo->resources.connectorCount);
	uint32 crtc = gInfo->resources.crtcs[0];
	uint32 plane = gInfo->crtcPrimaryPlane[0];
	gInfo->connectorCrtcId = Kms::PropertyId(device, connector,
		DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
	gInfo->crtcModeId = Kms::PropertyId(device, crtc, DRM_MODE_OBJECT_CRTC,
		"MODE_ID");
	gInfo->crtcActive = Kms::PropertyId(device, crtc, DRM_MODE_OBJECT_CRTC,
		"ACTIVE");
	gInfo->planeFbId = Kms::PropertyId(device, plane, DRM_MODE_OBJECT_PLANE,
		"FB_ID");
	gInfo->planeCrtcId = Kms::PropertyId(device, plane, DRM_MODE_OBJECT_PLANE,
		"CRTC_ID");
	gInfo->planeSrcX = Kms::PropertyId(device, plane, DRM_MODE_OBJECT_PLANE,
		"SRC_X");
	gInfo->planeSrcY = Kms::PropertyId(device, plane, DRM_MODE_OBJECT_PLANE,
		"SRC_Y");
	gInfo->planeSrcW = Kms::PropertyId(device, plane, DRM_MODE_OBJECT_PLANE,
		"SRC_W");
	gInfo->planeSrcH = Kms::PropertyId(device, plane, DRM_MODE_OBJECT_PLANE,
		"SRC_H");
	gInfo->planeCrtcX = Kms::PropertyId(device, plane, DRM_MODE_OBJECT_PLANE,
		"CRTC_X");
	gInfo->planeCrtcY = Kms::PropertyId(device, plane, DRM_MODE_OBJECT_PLANE,
		"CRTC_Y");
	gInfo->planeCrtcW = Kms::PropertyId(device, plane, DRM_MODE_OBJECT_PLANE,
		"CRTC_W");
	gInfo->planeCrtcH = Kms::PropertyId(device, plane, DRM_MODE_OBJECT_PLANE,
		"CRTC_H");
	if (gInfo->connectorCrtcId == 0 || gInfo->crtcModeId == 0
		|| gInfo->crtcActive == 0 || gInfo->planeFbId == 0
		|| gInfo->planeCrtcId == 0 || plane == 0) {
		TRACE("mode setting properties missing\n");
		uninit_common();
		return B_NOT_SUPPORTED;
	}

	status = probe_outputs();
	if (status != B_OK) {
		uninit_common();
		return status;
	}

	SharedInfo* shared;
	gInfo->sharedArea = create_area("amdgpu_kms shared info", (void**)&shared,
		B_ANY_ADDRESS, (sizeof(SharedInfo) + B_PAGE_SIZE - 1) & ~(B_PAGE_SIZE - 1),
		B_FULL_LOCK,
		B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
	if (gInfo->sharedArea < 0) {
		uninit_common();
		return gInfo->sharedArea;
	}
	gInfo->shared = shared;
	memset(shared, 0, sizeof(SharedInfo));
	shared->frameBufferArea = -1;
	// for clones, which open the device again
	if (ioctl(device, B_GET_PATH_FOR_DEVICE, shared->devicePath,
			sizeof(shared->devicePath)) != 0)
		shared->devicePath[0] = '\0';

	create_mode_list();
	TRACE("%" B_PRIu32 " output(s), %" B_PRIu32 " CRTC(s), %" B_PRIu32
		" plane(s)\n", gInfo->outputCount, gInfo->resources.crtcCount,
		gInfo->planeCount);
	return B_OK;
}


extern "C" ssize_t
amdgpu_accelerant_clone_info_size(void)
{
	return sizeof(area_id);
}


extern "C" void
amdgpu_get_accelerant_clone_info(void* info)
{
	*(area_id*)info = gInfo->sharedArea;
}


extern "C" status_t
amdgpu_clone_accelerant(void* info)
{
	SharedInfo* shared;
	area_id sharedArea = clone_area("amdgpu_kms cloned shared info",
		(void**)&shared, B_ANY_ADDRESS, B_READ_AREA, *(area_id*)info);
	if (sharedArea < 0)
		return sharedArea;
	int device = open(shared->devicePath, B_READ_WRITE);
	if (device < 0) {
		delete_area(sharedArea);
		return errno;
	}
	status_t status = init_common(device, true);
	if (status != B_OK) {
		close(device);
		delete_area(sharedArea);
		return status;
	}
	gInfo->sharedArea = sharedArea;
	gInfo->shared = shared;
	if (shared->frameBufferArea >= 0) {
		gInfo->clonedFrameBuffer = clone_area("amdgpu_kms cloned frame buffer",
			&gInfo->cloneFrameBuffer, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA,
			shared->frameBufferArea);
	}
	gInfo->mode = shared->mode;
	gInfo->modeSet = true;
	return B_OK;
}


static void stop_retrace(void);


extern "C" void
amdgpu_uninit_accelerant(void)
{
	if (gInfo == NULL)
		return;
	stop_retrace();
	if (gInfo->isClone) {
		if (gInfo->clonedFrameBuffer >= 0)
			delete_area(gInfo->clonedFrameBuffer);
		delete_area(gInfo->sharedArea);
		close(gInfo->device);
	} else {
		// the display shows what it showed; app_server is going away
		Kms::DestroyBuffer(gInfo->device, gInfo->cursor);
		if (gInfo->sharedArea >= 0)
			delete_area(gInfo->sharedArea);
	}
	uninit_common();
}


extern "C" status_t
amdgpu_get_accelerant_device_info(accelerant_device_info* info)
{
	info->version = B_ACCELERANT_VERSION;
	strlcpy(info->name, "AMD Radeon (amdgpu_drm)", sizeof(info->name));
	strlcpy(info->chipset, "amdgpu", sizeof(info->chipset));
	strlcpy(info->serial_no, "", sizeof(info->serial_no));
	info->memory = 0;
	info->dac_speed = 0;
	return B_OK;
}


// #pragma mark - retrace


static status_t
retrace_thread(void*)
{
	while (!gInfo->quitRetrace) {
		Output* primary = primary_output();
		if (primary == NULL || gInfo->dpmsMode != B_DPMS_ON
			|| Kms::WaitVBlank(gInfo->device, primary->crtcIndex) != B_OK) {
			// nothing being scanned out: a 60 Hz clock instead
			snooze(16667);
		}
		release_sem_etc(gInfo->retraceSemaphore, 1,
			B_RELEASE_ALL | B_DO_NOT_RESCHEDULE);
	}
	return B_OK;
}


extern "C" sem_id
amdgpu_accelerant_retrace_semaphore(void)
{
	if (gInfo->isClone)
		return -1;
	if (gInfo->retraceSemaphore < 0) {
		gInfo->retraceSemaphore = create_sem(0, "amdgpu_kms retrace");
		if (gInfo->retraceSemaphore < 0)
			return -1;
		gInfo->quitRetrace = false;
		gInfo->retraceThread = spawn_thread(retrace_thread,
			"amdgpu_kms retrace", B_REAL_TIME_DISPLAY_PRIORITY, NULL);
		if (gInfo->retraceThread < 0) {
			delete_sem(gInfo->retraceSemaphore);
			gInfo->retraceSemaphore = -1;
			return -1;
		}
		resume_thread(gInfo->retraceThread);
	}
	return gInfo->retraceSemaphore;
}


static void
stop_retrace(void)
{
	if (gInfo->retraceThread < 0)
		return;
	gInfo->quitRetrace = true;
	status_t result;
	wait_for_thread(gInfo->retraceThread, &result);
	gInfo->retraceThread = -1;
	delete_sem(gInfo->retraceSemaphore);
	gInfo->retraceSemaphore = -1;
}
