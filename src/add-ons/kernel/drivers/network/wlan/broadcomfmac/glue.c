/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <sys/bus.h>
#include <sys/haiku-module.h>

#include <OS.h>
#include <driver_settings.h>
#include <stdlib.h>

#ifdef BWFM_SDIO
#include "bwfm_host.h"
#endif


HAIKU_FBSD_WLAN_DRIVERS_GLUE(broadcomfmac)
NO_HAIKU_FBSD_MII_DRIVER();
NO_HAIKU_REENABLE_INTERRUPTS();
HAIKU_DRIVER_REQUIREMENTS(OBSD_WLAN);
HAIKU_FIRMWARE_VERSION(0);
NO_HAIKU_FIRMWARE_NAME_MAP();

extern driver_t* DRIVER_MODULE_NAME(bwfm, pci);
#ifdef BWFM_SDIO
extern driver_t* DRIVER_MODULE_NAME(bwfm, sdio);
#endif


/*!	A lab switch in ~/config/settings/kernel/drivers/broadcomfmac:
	"attach_until <seconds since 1970>" lets the driver attach only until
	then, so that a driver under test that brings the system down at boot
	does so only until the deadline.
*/
static bool
attach_allowed()
{
	void* settings = load_driver_settings("broadcomfmac");
	if (settings == NULL)
		return true;

	bool allowed = true;
	const char* until = get_driver_parameter(settings, "attach_until", NULL,
		NULL);
	if (until != NULL && (uint32)real_time_clock() > strtoul(until, NULL, 10)) {
		dprintf("broadcomfmac: attach_until %s has passed, not attaching\n",
			until);
		allowed = false;
	}
	unload_driver_settings(settings);
	return allowed;
}


void
__haiku_init_hardware()
{
	if (!attach_allowed())
		return;

	// PCIe chips (such as the BCM43602 of Apple's computers) on the PCI bus
	driver_t* drivers[] = {
		DRIVER_MODULE_NAME(bwfm, pci),
		NULL
	};
	_fbsd_init_hardware_pci(drivers);

#ifdef BWFM_SDIO
	// The SDIO chip is not on a bus of the compatibility layer: it is an
	// SDIO card, of the MMC bus (the Raspberry Pi 5's port) or of the
	// Raspberry Pi 4's port. The attach routine talks to it.
	if (bwfm_host_present())
		_fbsd_init_hardware_fixed(DRIVER_MODULE_NAME(bwfm, sdio));
#endif
}


int
HAIKU_CHECK_DISABLE_INTERRUPTS(device_t dev)
{
	// Only the PCIe chips use the compatibility layer's interrupts, and
	// only with an MSI, which is theirs (the SDIO card's handler is the
	// host's).
	return 1;
}
