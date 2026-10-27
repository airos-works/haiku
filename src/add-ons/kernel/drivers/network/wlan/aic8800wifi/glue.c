/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <sys/bus.h>
#include <sys/haiku-module.h>

#include "aic_usb.h"


HAIKU_FBSD_WLAN_DRIVERS_GLUE(aic8800wifi)
NO_HAIKU_FBSD_MII_DRIVER();
NO_HAIKU_REENABLE_INTERRUPTS();
HAIKU_DRIVER_REQUIREMENTS(OBSD_WLAN);
HAIKU_FIRMWARE_VERSION(0);
NO_HAIKU_FIRMWARE_NAME_MAP();

extern driver_t* DRIVER_MODULE_NAME(aic, usb);

void uninit_hardware(void);


void
__haiku_init_hardware()
{
	// The chip has to be running its firmware, which takes a load and a
	// trip off the bus and back; the USB side sees to that and loads this
	// driver again when it is done. Then there is one device.
	if (aic_usb_init_hardware() == B_OK)
		_fbsd_init_hardware_fixed(DRIVER_MODULE_NAME(aic, usb));
}


void
uninit_hardware(void)
{
	aic_usb_uninit_hardware();
}


int
HAIKU_CHECK_DISABLE_INTERRUPTS(device_t dev)
{
	// the driver has no interrupt handler of the compatibility layer's
	return 0;
}
