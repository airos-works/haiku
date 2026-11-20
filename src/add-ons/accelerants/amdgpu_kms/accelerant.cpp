/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "accelerant.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <Drivers.h>
#include <graphic_driver.h>


#define TRACE(x...) debug_printf("amdgpu_kms: " x)


accelerant_info* gInfo;


/*!	The path of the device \a device is open on, for clones, which open it
	again: amdgpu_drm does not answer B_GET_PATH_FOR_DEVICE, so it is looked
	up among the graphics devices by its node. */
static void
get_device_path(int device, char* path, size_t size)
{
	if (ioctl(device, B_GET_PATH_FOR_DEVICE, path, size) == 0
		&& path[0] != '\0')
		return;
	path[0] = '\0';

	struct stat deviceStat;
	if (fstat(device, &deviceStat) != 0)
		return;
	const char* kDirectory = "/dev/graphics";
	DIR* directory = opendir(kDirectory);
	if (directory == NULL)
		return;
	while (dirent* entry = readdir(directory)) {
		if (entry->d_name[0] == '.')
			continue;
		char candidate[B_PATH_NAME_LENGTH];
		snprintf(candidate, sizeof(candidate), "%s/%s", kDirectory,
			entry->d_name);
		struct stat candidateStat;
		if (stat(candidate, &candidateStat) == 0
			&& candidateStat.st_dev == deviceStat.st_dev
			&& candidateStat.st_ino == deviceStat.st_ino) {
			strlcpy(path, candidate, size);
			break;
		}
	}
	closedir(directory);
}


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
	for (uint32 i = 0; i < kMaxOutputs; i++) {
		gInfo->outputRetraceSemaphores[i] = -1;
		gInfo->outputRetraceThreads[i] = -1;
	}
	for (uint32 c = 0; c < kMaxCards; c++) {
		gInfo->cards[c].fd = -1;
		gInfo->cards[c].frameBuffer.area = -1;
		gInfo->cards[c].cursor.area = -1;
	}
	gInfo->changePort = -1;
	gInfo->hotplugThread = -1;
	gInfo->dpmsMode = B_DPMS_ON;
	return B_OK;
}


static void
uninit_common(void)
{
	if (gInfo == NULL)
		return;
	free(gInfo->modeList);
	free(gInfo->cursorImage);
	free(gInfo);
	gInfo = NULL;
}


static uint32
first_object(const uint32* objects, uint32 count)
{
	return count > 0 ? objects[0] : 0;
}


/*!	Sets up the display engine of the DRM device \a fd as \a card: atomic
	mode setting, its CRTCs and planes and the IDs of the properties set.
*/
status_t
init_card(Card& card, int fd)
{
	card.fd = fd;
	card.frameBuffer.area = -1;
	card.cursor.area = -1;

	// atomic mode setting and every plane
	if (Kms::SetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != B_OK
		|| Kms::SetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != B_OK) {
		TRACE("no atomic mode setting\n");
		return B_NOT_SUPPORTED;
	}
	// the first opener is the master already; ask in case it is not
	Kms::Ioctl(fd, DRM_IOCTL_SET_MASTER, NULL);

	// The driver: amdgpu's own ioctls (device copies) only on amdgpu; other
	// drivers' private ioctl numbers mean something else.
	drm_version version = {};
	version.name = card.driver;
	version.name_len = sizeof(card.driver) - 1;
	if (Kms::Ioctl(fd, DRM_IOCTL_VERSION, &version) != B_OK)
		card.driver[0] = '\0';
	card.isAmdgpu = strcmp(card.driver, "amdgpu") == 0;

	status_t status = Kms::GetResources(fd, card.resources);
	if (status == B_OK) {
		card.planeCount = Kms::kMaxObjects;
		status = Kms::GetPlanes(fd, card.planes, card.planeCount);
	}
	if (status != B_OK || card.resources.crtcCount == 0) {
		TRACE("no display resources: %s\n", strerror(status));
		return status != B_OK ? status : B_ERROR;
	}
	for (uint32 c = 0; c < card.resources.crtcCount; c++) {
		for (uint32 p = 0; p < card.planeCount; p++) {
			const Kms::Plane& plane = card.planes[p];
			if ((plane.possibleCrtcs & (1 << c)) == 0)
				continue;
			if (plane.type == Kms::kPlanePrimary
				&& card.crtcPrimaryPlane[c] == 0)
				card.crtcPrimaryPlane[c] = plane.id;
			else if (plane.type == Kms::kPlaneCursor
				&& card.crtcCursorPlane[c] == 0)
				card.crtcCursorPlane[c] = plane.id;
		}
	}

	uint32 connector = first_object(card.resources.connectors,
		card.resources.connectorCount);
	uint32 crtc = card.resources.crtcs[0];
	uint32 plane = card.crtcPrimaryPlane[0];
	card.connectorCrtcId = Kms::PropertyId(fd, connector,
		DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
	card.crtcModeId = Kms::PropertyId(fd, crtc, DRM_MODE_OBJECT_CRTC,
		"MODE_ID");
	card.crtcActive = Kms::PropertyId(fd, crtc, DRM_MODE_OBJECT_CRTC,
		"ACTIVE");
	card.planeFbId = Kms::PropertyId(fd, plane, DRM_MODE_OBJECT_PLANE,
		"FB_ID");
	card.planeCrtcId = Kms::PropertyId(fd, plane, DRM_MODE_OBJECT_PLANE,
		"CRTC_ID");
	card.planeSrcX = Kms::PropertyId(fd, plane, DRM_MODE_OBJECT_PLANE,
		"SRC_X");
	card.planeSrcY = Kms::PropertyId(fd, plane, DRM_MODE_OBJECT_PLANE,
		"SRC_Y");
	card.planeSrcW = Kms::PropertyId(fd, plane, DRM_MODE_OBJECT_PLANE,
		"SRC_W");
	card.planeSrcH = Kms::PropertyId(fd, plane, DRM_MODE_OBJECT_PLANE,
		"SRC_H");
	card.planeCrtcX = Kms::PropertyId(fd, plane, DRM_MODE_OBJECT_PLANE,
		"CRTC_X");
	card.planeCrtcY = Kms::PropertyId(fd, plane, DRM_MODE_OBJECT_PLANE,
		"CRTC_Y");
	card.planeCrtcW = Kms::PropertyId(fd, plane, DRM_MODE_OBJECT_PLANE,
		"CRTC_W");
	card.planeCrtcH = Kms::PropertyId(fd, plane, DRM_MODE_OBJECT_PLANE,
		"CRTC_H");
	if (card.connectorCrtcId == 0 || card.crtcModeId == 0
		|| card.crtcActive == 0 || card.planeFbId == 0
		|| card.planeCrtcId == 0 || plane == 0) {
		TRACE("mode setting properties missing\n");
		return B_NOT_SUPPORTED;
	}
	TRACE("%s: %" B_PRIu32 " CRTC(s), %" B_PRIu32 " plane(s), %" B_PRIu32
		" connector(s)\n", card.driver[0] != '\0' ? card.driver : "DRM",
		card.resources.crtcCount, card.planeCount,
		card.resources.connectorCount);
	return B_OK;
}


/*!	A card's buffers go; a card this accelerant opened is closed, which
	leaves its monitors dark. */
void
uninit_card(Card& card)
{
	if (card.fd < 0)
		return;
	Kms::DestroyBuffer(card.fd, card.cursor);
	if (card.owned) {
		for (uint32 c = 0; c < Kms::kMaxObjects; c++)
			Kms::DestroyBlob(card.fd, card.modeBlobs[c]);
		Kms::DestroyBuffer(card.fd, card.frameBuffer);
		close(card.fd);
	}
	card.fd = -1;
}


/*!	The other GPUs this accelerant serves: every graphics device that names
	it, but the one app_server opened. Each one's monitors join the layout.
	The file ~/config/settings/amdgpu_kms_single_card keeps to the one
	app_server opened. */
void
open_other_cards(void)
{
	struct stat ignored;
	if (stat("/boot/home/config/settings/amdgpu_kms_single_card", &ignored)
			== 0) {
		TRACE("one card only (amdgpu_kms_single_card)\n");
		return;
	}
	struct stat firstStat;
	if (fstat(gInfo->device, &firstStat) != 0)
		return;
	const char* kDirectory = "/dev/graphics";
	DIR* directory = opendir(kDirectory);
	if (directory == NULL)
		return;
	while (dirent* entry = readdir(directory)) {
		if (gInfo->cardCount >= kMaxCards)
			break;
		if (entry->d_name[0] == '.')
			continue;
		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "%s/%s", kDirectory, entry->d_name);
		struct stat candidateStat;
		if (stat(path, &candidateStat) != 0
			|| (candidateStat.st_dev == firstStat.st_dev
				&& candidateStat.st_ino == firstStat.st_ino))
			continue;
		// only devices that name this accelerant: the others are not DRM
		// devices (the framebuffer or VESA device, nvidia_rm's)
		int fd = open(path, B_READ_WRITE | O_CLOEXEC);
		if (fd < 0)
			continue;
		char signature[B_FILE_NAME_LENGTH] = {};
		if (ioctl(fd, B_GET_ACCELERANT_SIGNATURE, signature,
				sizeof(signature)) != 0
			|| strcmp(signature, "amdgpu_kms.accelerant") != 0) {
			close(fd);
			continue;
		}
		Card& card = gInfo->cards[gInfo->cardCount];
		memset(&card, 0, sizeof(card));
		if (init_card(card, fd) != B_OK) {
			TRACE("%s: not used\n", path);
			close(fd);
			card.fd = -1;
			continue;
		}
		card.owned = true;
		gInfo->cardCount++;
		TRACE("%s joins the desktop as card %" B_PRIu32 "\n", path,
			gInfo->cardCount);
	}
	closedir(directory);
}


extern "C" status_t
amdgpu_init_accelerant(int device)
{
	status_t status = init_common(device, false);
	if (status != B_OK)
		return status;

	status = init_card(gInfo->cards[0], device);
	if (status != B_OK) {
		uninit_common();
		return status;
	}
	gInfo->cardCount = 1;
	gInfo->isAmdgpu = gInfo->cards[0].isAmdgpu;
	TRACE("DRM driver \"%s\"%s\n", gInfo->cards[0].driver,
		gInfo->isAmdgpu ? "" : ": no copies by the GPU");
	open_other_cards();

	status = probe_outputs();
	if (status != B_OK) {
		for (uint32 c = 1; c < gInfo->cardCount; c++)
			uninit_card(gInfo->cards[c]);
		uninit_common();
		return status;
	}
	if (gInfo->isAmdgpu)
		Kms::VramLostCounter(device, gInfo->vramLost);

	SharedInfo* shared;
	gInfo->sharedArea = create_area("amdgpu_kms shared info", (void**)&shared,
		B_ANY_ADDRESS, (sizeof(SharedInfo) + B_PAGE_SIZE - 1) & ~(B_PAGE_SIZE - 1),
		B_FULL_LOCK,
		B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
	if (gInfo->sharedArea < 0) {
		status = gInfo->sharedArea;
		for (uint32 c = 1; c < gInfo->cardCount; c++)
			uninit_card(gInfo->cards[c]);
		uninit_common();
		return status;
	}
	gInfo->shared = shared;
	memset(shared, 0, sizeof(SharedInfo));
	shared->frameBufferArea = -1;
	// for clones, which open the device again
	get_device_path(device, shared->devicePath, sizeof(shared->devicePath));
	if (shared->devicePath[0] == '\0')
		TRACE("the device's path was not found: no clones\n");

	create_mode_list();
	TRACE("%" B_PRIu32 " output(s) on %" B_PRIu32 " card(s)\n",
		gInfo->outputCount, gInfo->cardCount);
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
	stop_hotplug();
	if (gInfo->isClone) {
		if (gInfo->clonedFrameBuffer >= 0)
			delete_area(gInfo->clonedFrameBuffer);
		delete_area(gInfo->sharedArea);
		close(gInfo->device);
	} else {
		// the display shows what it showed; app_server is going away
		uninit_device_copies();
		for (uint32 c = 0; c < gInfo->cardCount; c++)
			uninit_card(gInfo->cards[c]);
		if (gInfo->sharedArea >= 0)
			delete_area(gInfo->sharedArea);
	}
	uninit_common();
}


extern "C" status_t
amdgpu_get_accelerant_device_info(accelerant_device_info* info)
{
	info->version = B_ACCELERANT_VERSION;
	const char* driver = gInfo->isClone || gInfo->cards[0].driver[0] == '\0'
		? "amdgpu" : gInfo->cards[0].driver;
	if (strcmp(driver, "nouveau") == 0)
		strlcpy(info->name, "NVIDIA (nouveau_drm)", sizeof(info->name));
	else if (strcmp(driver, "amdgpu") == 0)
		strlcpy(info->name, "AMD Radeon (amdgpu_drm)", sizeof(info->name));
	else
		snprintf(info->name, sizeof(info->name), "%s (DRM)", driver);
	strlcpy(info->chipset, driver, sizeof(info->chipset));
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
			|| Kms::WaitVBlank(card_of(*primary).fd, primary->crtcIndex)
				!= B_OK) {
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


/*!	One output's blanks: its CRTC's while it is driven, a 60 Hz clock
	otherwise. The output is looked up by its connector every time, since a
	probe rebuilds the outputs. */
static status_t
output_retrace_thread(void* data)
{
	uint32 index = (uint32)(addr_t)data;
	uint32 id = gInfo->outputRetraceConnectors[index];
	while (!gInfo->quitRetrace) {
		Output* output = NULL;
		for (uint32 i = 0; i < gInfo->outputCount; i++) {
			if (gInfo->outputs[i].id == id) {
				output = &gInfo->outputs[i];
				break;
			}
		}
		if (output == NULL || !output->enabled || output->crtcIndex < 0
			|| gInfo->dpmsMode != B_DPMS_ON
			|| Kms::WaitVBlank(card_of(*output).fd, output->crtcIndex)
				!= B_OK) {
			snooze(16667);
		}
		release_sem_etc(gInfo->outputRetraceSemaphores[index], 1,
			B_RELEASE_ALL | B_DO_NOT_RESCHEDULE);
	}
	return B_OK;
}


extern "C" sem_id
amdgpu_get_display_output_retrace_semaphore(uint32 id)
{
	if (gInfo->isClone)
		return B_UNSUPPORTED;
	refresh_outputs();
	int32 index = -1;
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		if (gInfo->outputs[i].id == id) {
			index = i;
			break;
		}
	}
	if (index < 0)
		return B_BAD_VALUE;
	// an output keeps its slot: a probe keeps the order of the connectors
	for (uint32 i = 0; i < kMaxOutputs; i++) {
		if (gInfo->outputRetraceThreads[i] >= 0
			&& gInfo->outputRetraceConnectors[i] == id)
			return gInfo->outputRetraceSemaphores[i];
	}
	for (uint32 i = 0; i < kMaxOutputs; i++) {
		if (gInfo->outputRetraceThreads[i] >= 0)
			continue;
		sem_id semaphore = create_sem(0, "amdgpu_kms output retrace");
		if (semaphore < 0)
			return semaphore;
		gInfo->outputRetraceConnectors[i] = id;
		gInfo->outputRetraceSemaphores[i] = semaphore;
		gInfo->quitRetrace = false;
		thread_id thread = spawn_thread(output_retrace_thread,
			"amdgpu_kms output retrace", B_REAL_TIME_DISPLAY_PRIORITY,
			(void*)(addr_t)i);
		if (thread < 0) {
			delete_sem(semaphore);
			gInfo->outputRetraceSemaphores[i] = -1;
			return thread;
		}
		gInfo->outputRetraceThreads[i] = thread;
		resume_thread(thread);
		return semaphore;
	}
	return B_NO_MEMORY;
}


/*!	The scan's row in the output's region now: from the end of the last
	blank, the pixel clock and the line length, scaled from the monitor's
	rows to the region's. Negative while the blank lasts. */
extern "C" status_t
amdgpu_get_display_output_scan_line(uint32 id, int32* _line, int32* _lines,
	bigtime_t* _scanStart, bigtime_t* _period)
{
	if (gInfo->isClone)
		return B_UNSUPPORTED;
	Output* output = NULL;
	for (uint32 i = 0; i < gInfo->outputCount; i++) {
		if (gInfo->outputs[i].id == id) {
			output = &gInfo->outputs[i];
			break;
		}
	}
	if (output == NULL)
		return B_BAD_VALUE;
	if (!output->enabled || output->crtcIndex < 0
		|| gInfo->dpmsMode != B_DPMS_ON || output->timing.clock == 0
		|| output->timing.htotal == 0 || output->timing.vdisplay == 0)
		return B_NOT_ALLOWED;

	bigtime_t scanStart;
	status_t status = Kms::LastVBlank(card_of(*output).fd, output->crtcIndex,
		&scanStart);
	if (status != B_OK)
		return status;
	bigtime_t now = system_time();
	// clock is in kHz: lines = us * kHz / 1000 / htotal
	int64 line = (now - scanStart) * output->timing.clock
		/ (1000LL * output->timing.htotal);
	int32 total = output->timing.vtotal;
	bigtime_t period = (bigtime_t)output->timing.htotal * total * 1000
		/ output->timing.clock;
	if (line >= total) {
		// a stale blank (its interrupt off): wrap, it is periodic
		int64 frames = line / total;
		line -= frames * total;
		scanStart += frames * period;
	}
	if (line >= output->timing.vdisplay) {
		// in the blank before the next scan
		line -= total;
		scanStart += period;
	}
	*_line = (int32)(line * (int64)output->height / output->timing.vdisplay);
	*_lines = output->height;
	*_scanStart = scanStart;
	*_period = period;
	return B_OK;
}


static void
stop_retrace(void)
{
	gInfo->quitRetrace = true;
	status_t result;
	if (gInfo->retraceThread >= 0) {
		wait_for_thread(gInfo->retraceThread, &result);
		gInfo->retraceThread = -1;
		delete_sem(gInfo->retraceSemaphore);
		gInfo->retraceSemaphore = -1;
	}
	for (uint32 i = 0; i < kMaxOutputs; i++) {
		if (gInfo->outputRetraceThreads[i] < 0)
			continue;
		wait_for_thread(gInfo->outputRetraceThreads[i], &result);
		gInfo->outputRetraceThreads[i] = -1;
		delete_sem(gInfo->outputRetraceSemaphores[i]);
		gInfo->outputRetraceSemaphores[i] = -1;
	}
}
