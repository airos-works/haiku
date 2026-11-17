/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <sys/bus.h>
#include <sys/haiku-module.h>

#include "bwfm_host.h"


HAIKU_FBSD_WLAN_DRIVERS_GLUE(broadcomfmac)
NO_HAIKU_FBSD_MII_DRIVER();
NO_HAIKU_REENABLE_INTERRUPTS();
HAIKU_DRIVER_REQUIREMENTS(OBSD_WLAN);
HAIKU_FIRMWARE_VERSION(0);
NO_HAIKU_FIRMWARE_NAME_MAP();

extern driver_t* DRIVER_MODULE_NAME(bwfm, sdio);


void
__haiku_init_hardware()
{
	// The chip is not on a bus of the compatibility layer: it is an SDIO
	// card, of the MMC bus (the Raspberry Pi 5's port) or of the Raspberry
	// Pi 4's port. The attach routine talks to it.
	if (bwfm_host_present())
		_fbsd_init_hardware_fixed(DRIVER_MODULE_NAME(bwfm, sdio));
}


int
HAIKU_CHECK_DISABLE_INTERRUPTS(device_t dev)
{
	// the driver has no interrupt handler of the compatibility layer's
	return 0;
}
