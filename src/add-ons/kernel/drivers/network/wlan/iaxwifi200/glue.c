/*
 * Copyright 2022, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <sys/haiku-module.h>

#include <driver_settings.h>
#include <stdlib.h>


HAIKU_FBSD_WLAN_DRIVER_GLUE(iaxwifi200, iwx, pci)
NO_HAIKU_FBSD_MII_DRIVER();
NO_HAIKU_REENABLE_INTERRUPTS();
HAIKU_DRIVER_REQUIREMENTS(OBSD_WLAN);
HAIKU_FIRMWARE_VERSION(0);
NO_HAIKU_FIRMWARE_NAME_MAP();


int
HAIKU_CHECK_DISABLE_INTERRUPTS(device_t dev)
{
	// We only support MSI(-X), so we handle all interrupts.
	return 1;
}


/*
 * Reads a number from the driver settings file "iaxwifi200"
 * (~/config/settings/kernel/drivers/iaxwifi200). The settings are lab
 * switches; see iwx_haiku_read_settings() in if_iwx.c.
 */
int
iwx_haiku_setting(const char* name, int defaultValue)
{
	void* settings = load_driver_settings("iaxwifi200");
	const char* value;
	int result = defaultValue;

	if (settings == NULL)
		return defaultValue;
	value = get_driver_parameter(settings, name, NULL, NULL);
	if (value != NULL)
		result = strtol(value, NULL, 0);
	unload_driver_settings(settings);
	return result;
}
