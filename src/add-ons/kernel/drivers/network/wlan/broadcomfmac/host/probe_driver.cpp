/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab driver: brings the SDIO host up and reads the Wi-Fi chip's identity
	through the backplane window. In no image. */


#include <Drivers.h>
#include <KernelExport.h>

#include <string.h>

#include "bwfm_host.h"


int32 api_version = B_CUR_DRIVER_API_VERSION;


status_t
init_hardware()
{
	if (!bwfm_host_present()) {
		dprintf("broadcomfmac probe: no Wi-Fi chip\n");
		return B_ERROR;
	}

	status_t status = bwfm_host_init();
	dprintf("broadcomfmac probe: init: %s\n", strerror(status));
	if (status == B_OK)
		status = bwfm_host_enable_function(1, true);
	dprintf("broadcomfmac probe: function 1: %s\n", strerror(status));
	if (status == B_OK) {
		// backplane window to the chip common core at 0x18000000
		uint8 window[3] = { 0x00, 0x00, 0x18 };
		for (int i = 0; i < 3 && status == B_OK; i++)
			status = bwfm_host_rw_byte(true, 1, 0x1000a + i, &window[i]);
		uint32 id = 0;
		if (status == B_OK) {
			status = bwfm_host_rw_extended(false, 1, 0x8000, (uint8*)&id, 4,
				true);
		}
		dprintf("broadcomfmac probe: chip id register %#" B_PRIx32 " (%s): "
			"chip %#" B_PRIx32 " revision %" B_PRIu32 "\n", id,
			strerror(status), id & 0xffff, (id >> 16) & 0xf);
	}
	bwfm_host_uninit();
	return B_ERROR;
}


status_t
init_driver()
{
	return B_ERROR;
}


void
uninit_driver()
{
}


const char**
publish_devices()
{
	static const char* names[] = { NULL };
	return names;
}


device_hooks*
find_device(const char* name)
{
	return NULL;
}
