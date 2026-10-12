/*
 * Copyright 2005-2008, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Copyright 2016, Jessica Hamilton, jessica.l.hamilton@gmail.com.
 * Distributed under the terms of the MIT License.
 */


#include "accelerant_protos.h"
#include "accelerant.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <syslog.h>

#include <AutoDeleterOS.h>
#include <boot_frame_buffer_retrace.h>


//#define TRACE_ACCELERANT
#ifdef TRACE_ACCELERANT
extern "C" void _sPrintf(const char *format, ...);
#	define TRACE(x) _sPrintf x
#else
#	define TRACE(x) ;
#endif


struct accelerant_info *gInfo;


//	#pragma mark -


/*!	This is the common accelerant_info initializer. It is called by
	both, the first accelerant and all clones.
*/
static status_t
init_common(int device, bool isClone)
{
	// initialize global accelerant info structure

	gInfo = (accelerant_info *)malloc(sizeof(accelerant_info));
	MemoryDeleter infoDeleter(gInfo);
	if (gInfo == NULL)
		return B_NO_MEMORY;

	memset(gInfo, 0, sizeof(accelerant_info));

	gInfo->is_clone = isClone;
	gInfo->device = device;
	gInfo->current_mode = UINT16_MAX;
	gInfo->frame_buffer_area = -1;
	gInfo->retrace_device = -1;

	// get basic info from driver

	area_id sharedArea;
	if (ioctl(device, VESA_GET_PRIVATE_DATA, &sharedArea, sizeof(area_id)) != 0)
		return B_ERROR;

	AreaDeleter sharedDeleter(clone_area("vesa shared info",
		(void **)&gInfo->shared_info, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, sharedArea));
	status_t status = gInfo->shared_info_area = sharedDeleter.Get();
	if (status < B_OK)
		return status;

	infoDeleter.Detach();
	sharedDeleter.Detach();
	return B_OK;
}


/*!	Cleans up everything done by a successful init_common(). */
static void
uninit_common(void)
{
	delete_area(gInfo->frame_buffer_area);
	delete_area(gInfo->shared_info_area);
	gInfo->shared_info_area = -1;
	gInfo->shared_info = NULL;

	if (gInfo->retrace_device >= 0)
		close(gInfo->retrace_device);

	// close the file handle ONLY if we're the clone
	// (this is what Be tells us ;)
	if (gInfo->is_clone)
		close(gInfo->device);

	free(gInfo);
}


//	#pragma mark - public accelerant functions


/*!	Init primary accelerant */
status_t
framebuffer_init_accelerant(int device)
{
	TRACE(("framebuffer_init_accelerant()\n"));

	status_t status = init_common(device, false);
	if (status != B_OK)
		return status;

	status = create_mode_list();
	if (status != B_OK) {
		uninit_common();
		return status;
	}

	area_info info;
	status = ioctl(gInfo->device, VESA_CLONE_FRAME_BUFFER, &info, sizeof(info));
	if (status != B_OK) {
		uninit_common();
		return status;
	}

	gInfo->frame_buffer_area = info.area;
	gInfo->frame_buffer = info.address;

	return B_OK;
}


ssize_t
framebuffer_accelerant_clone_info_size(void)
{
	// clone info is device name, so return its maximum size
	return B_PATH_NAME_LENGTH;
}


void
framebuffer_get_accelerant_clone_info(void *info)
{
	ioctl(gInfo->device, VESA_GET_DEVICE_NAME, info, B_PATH_NAME_LENGTH);
}


status_t
framebuffer_clone_accelerant(void *info)
{
	TRACE(("framebuffer_clone_accelerant()\n"));

	// create full device name
	char path[MAXPATHLEN];
	strcpy(path, "/dev/");
	strcat(path, (const char *)info);

	int fd = open(path, B_READ_WRITE);
	if (fd < 0)
		return errno;

	status_t status = init_common(fd, true);
	if (status != B_OK)
		goto err1;

	// get read-only clone of supported display modes
	status = gInfo->mode_list_area = clone_area(
		"framebuffer cloned modes", (void **)&gInfo->mode_list,
		B_ANY_ADDRESS, B_READ_AREA, gInfo->shared_info->mode_list_area);
	if (status < B_OK)
		goto err2;

	return B_OK;

err2:
	uninit_common();
err1:
	close(fd);
	return status;
}


/*!	This function is called for both, the primary accelerant and all of
	its clones.
*/
void
framebuffer_uninit_accelerant(void)
{
	TRACE(("framebuffer_uninit_accelerant()\n"));

	// delete accelerant instance data
	delete_area(gInfo->mode_list_area);
	gInfo->mode_list = NULL;

	uninit_common();
}


status_t
framebuffer_get_accelerant_device_info(accelerant_device_info *info)
{
	info->version = B_ACCELERANT_VERSION;
	strcpy(info->name, "Framebuffer Driver");
	strcpy(info->chipset, "Framebuffer");
		// ToDo: provide some more insight here...
	strcpy(info->serial_no, "None");

#if 0
	info->memory = ???
	info->dac_speed = ???
#endif

	return B_OK;
}


/*!	The boot frame buffer is shown by display hardware this driver knows
	nothing about, but a driver for that hardware may still know its blanks
	(boot_frame_buffer_retrace.h). These are the ones that do; the device
	stays open, the driver releases the semaphore only while it is.
*/
static const char* const kRetraceProviders[] = {
	"/dev/dri/amdgpu/0",	// Radeon Pro WX 5100
};


sem_id
framebuffer_accelerant_retrace_semaphore()
{
	for (size_t i = 0; i < B_COUNT_OF(kRetraceProviders); i++) {
		int device = open(kRetraceProviders[i], O_RDWR);
		if (device < 0)
			continue;
		sem_id semaphore;
		if (ioctl(device, B_GET_BOOT_FRAME_BUFFER_RETRACE_SEMAPHORE,
				&semaphore, sizeof(semaphore)) == 0) {
			if (gInfo->retrace_device >= 0)
				close(gInfo->retrace_device);
			gInfo->retrace_device = device;
			return semaphore;
		}
		close(device);
	}
	return -1;
}

