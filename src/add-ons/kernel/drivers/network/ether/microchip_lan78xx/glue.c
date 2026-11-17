/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

/*	Microchip LAN7800/LAN7801/LAN7850 USB gigabit Ethernet (FreeBSD's
	if_muge): the Raspberry Pi 3 B+'s LAN7515 hub has a LAN7800 behind it. */


#include <sys/bus.h>

#include <rpi_firmware.h>


extern driver_t* DRIVER_MODULE_NAME(muge, uhub);

extern driver_t *DRIVER_MODULE_NAME(ukphy, miibus);

HAIKU_FBSD_DRIVERS_GLUE(microchip_lan78xx)
HAIKU_DRIVER_REQUIREMENTS(0);
NO_HAIKU_CHECK_DISABLE_INTERRUPTS();
NO_HAIKU_REENABLE_INTERRUPTS();


#define RPI_FIRMWARE_GET_BOARD_MAC	0x00010003


/*!	The board's Ethernet address from the Raspberry Pi firmware; nonzero
	where there is no such firmware. */
int
haiku_lan78xx_board_mac(uint8_t *address)
{
	rpi_firmware_module_info *firmware;
	uint32_t value[2] = { 0, 0 };
	status_t status;

	if (get_module(RPI_FIRMWARE_MODULE_NAME, (module_info **)&firmware)
			!= B_OK) {
		return -1;
	}
	status = firmware->property(RPI_FIRMWARE_GET_BOARD_MAC, value,
		sizeof(value));
	put_module(RPI_FIRMWARE_MODULE_NAME);
	if (status != B_OK)
		return -1;

	memcpy(address, value, 6);
	if ((address[0] | address[1] | address[2] | address[3] | address[4]
			| address[5]) == 0 || (address[0] & 1) != 0) {
		return -1;
	}
	return 0;
}


driver_t *
__haiku_select_miibus_driver(device_t dev)
{
	driver_t *drivers[] = {
		DRIVER_MODULE_NAME(ukphy, miibus),
		NULL
	};

	return __haiku_probe_drivers(dev, drivers);
}


void
__haiku_init_hardware()
{
	driver_t *usb_drivers[] = {
		DRIVER_MODULE_NAME(muge, uhub),
		NULL
	};
	_fbsd_init_hardware_uhub(usb_drivers);
}
