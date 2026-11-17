/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	The Wi-Fi chip's SDIO card, see bwfm_host.h. All of it goes through an
	sdio_function_interface: the MMC bus' or, on the Raspberry Pi 4, one made
	of the driver's own host. */


#include "bwfm_host.h"

#include <string.h>

#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>
#include <sdio.h>

#include "sdio_host.h"


#define ERROR(x...)	dprintf("broadcomfmac: " x)
#define INFO(x...)	dprintf("broadcomfmac: " x)


static const uint16 kVendors[] = {
	0x02d0,		// Broadcom
	0x04b4,		// Cypress (Infineon)
};

static sdio_function_interface* sSdio;
static void* sCookie;
static device_node* sNode;
static device_manager_info* sDeviceManager;


//	#pragma mark - the Raspberry Pi 4's port, as an SDIO function interface


static sem_id sRpiInterruptSemaphore = -1;
static thread_id sRpiInterruptThread = -1;
static sdio_interrupt_handler sRpiHandlers[3];
static void* sRpiHandlerData[3];


static void
rpi_card_interrupt(void* data)
{
	// interrupt context; the host has turned the card interrupt off
	release_sem_etc(sRpiInterruptSemaphore, 1, B_DO_NOT_RESCHEDULE);
}


static status_t
rpi_interrupt_thread(void* data)
{
	while (acquire_sem(sRpiInterruptSemaphore) == B_OK) {
		for (int i = 1; i < 3; i++) {
			if (sRpiHandlers[i] == NULL || (i == 2
					&& sRpiHandlers[2] == sRpiHandlers[1]
					&& sRpiHandlerData[2] == sRpiHandlerData[1]))
				continue;
			sRpiHandlers[i](sRpiHandlerData[i]);
		}
		rpi_sdio_enable_card_interrupt();
	}
	return B_OK;
}


static status_t
rpi_read_byte(void* cookie, uint8 function, uint32 address, uint8* _value)
{
	return rpi_sdio_rw_byte(false, function, address, _value);
}


static status_t
rpi_write_byte(void* cookie, uint8 function, uint32 address, uint8 value,
	uint8* _readBack)
{
	// the host always reads back (RAW)
	status_t status = rpi_sdio_rw_byte(true, function, address, &value);
	if (status == B_OK && _readBack != NULL)
		*_readBack = value;
	return status;
}


static status_t
rpi_read(void* cookie, uint8 function, uint32 address, bool increment,
	void* buffer, size_t length)
{
	return rpi_sdio_rw_extended(false, function, address, (uint8*)buffer,
		length, increment);
}


static status_t
rpi_write(void* cookie, uint8 function, uint32 address, bool increment,
	const void* buffer, size_t length)
{
	return rpi_sdio_rw_extended(true, function, address, (uint8*)buffer,
		length, increment);
}


static status_t
rpi_set_block_size(void* cookie, uint8 function, uint16 size)
{
	return rpi_sdio_set_block_size(function, size);
}


static status_t
rpi_enable_function(void* cookie, uint8 function, bool enable)
{
	return rpi_sdio_enable_function(function, enable);
}


static status_t
rpi_set_interrupt_handler(void* cookie, uint8 function,
	sdio_interrupt_handler handler, void* data)
{
	if (function == 0 || function >= 3)
		return B_BAD_VALUE;

	if (handler != NULL && sRpiInterruptThread < 0) {
		sRpiInterruptSemaphore = create_sem(0, "bwfm sdio interrupt");
		if (sRpiInterruptSemaphore < 0)
			return sRpiInterruptSemaphore;
		sRpiInterruptThread = spawn_kernel_thread(rpi_interrupt_thread,
			"bwfm sdio interrupt", B_URGENT_DISPLAY_PRIORITY, NULL);
		if (sRpiInterruptThread < 0) {
			delete_sem(sRpiInterruptSemaphore);
			sRpiInterruptSemaphore = -1;
			return sRpiInterruptThread;
		}
		resume_thread(sRpiInterruptThread);
		status_t status = rpi_sdio_set_interrupt_handler(rpi_card_interrupt,
			NULL);
		if (status != B_OK)
			return status;
		sRpiHandlerData[function] = data;
		sRpiHandlers[function] = handler;
		rpi_sdio_enable_card_interrupt();
		return B_OK;
	}

	sRpiHandlerData[function] = data;
	sRpiHandlers[function] = handler;

	if (sRpiHandlers[1] == NULL && sRpiHandlers[2] == NULL
		&& sRpiInterruptThread >= 0) {
		// the thread ends after the handler that may be running
		rpi_sdio_set_interrupt_handler(NULL, NULL);
		delete_sem(sRpiInterruptSemaphore);
		sRpiInterruptSemaphore = -1;
		status_t result;
		wait_for_thread(sRpiInterruptThread, &result);
		sRpiInterruptThread = -1;
	}
	return B_OK;
}


static sdio_function_interface sRpiFunctions = {
	{},
	rpi_read_byte,
	rpi_write_byte,
	rpi_read,
	rpi_write,
	rpi_set_block_size,
	rpi_enable_function,
	rpi_set_interrupt_handler,
	NULL	// get_card_info
};


//	#pragma mark - finding the chip


static bool
find_function()
{
	if (sSdio != NULL)
		return true;
	if (sDeviceManager == NULL
		&& get_module(B_DEVICE_MANAGER_MODULE_NAME,
			(module_info**)&sDeviceManager) != B_OK) {
		return false;
	}

	for (size_t i = 0; i < B_COUNT_OF(kVendors); i++) {
		if (sdio_find_function(sDeviceManager, kVendors[i], NULL, 0, 1, &sNode,
				&sSdio, &sCookie) == B_OK) {
			uint16 device = 0;
			sDeviceManager->get_attr_uint16(sNode, SDIO_DEVICE_ATTRIBUTE,
				&device, false);
			INFO("SDIO card %04x:%04x on the MMC bus\n", kVendors[i], device);
			return true;
		}
	}
	sSdio = NULL;
	return false;
}


/*!	A board whose Wi-Fi port the MMC bus drives: the Raspberry Pi 5 (its
	BCM2712's "non-removable" SD host controller).
*/
static bool
mmc_bus_has_port()
{
	if (sDeviceManager == NULL
		&& get_module(B_DEVICE_MANAGER_MODULE_NAME,
			(module_info**)&sDeviceManager) != B_OK) {
		return false;
	}

	device_attr attributes[] = {
		{ "fdt/compatible", B_STRING_TYPE,
			{ .string = "brcm,bcm2712-sdhci" } },
		{}
	};
	bool found = false;
	device_node* root = sDeviceManager->get_root_node();
	device_node* node = NULL;
	while (!found && sDeviceManager->find_child_node(root, attributes, &node)
			== B_OK) {
		fdt_device_module_info* module;
		fdt_device* device;
		found = sDeviceManager->get_driver(node, (driver_module_info**)&module,
				(void**)&device) == B_OK
			&& module->get_prop(device, "non-removable", NULL) != NULL;
	}
	if (found)
		sDeviceManager->put_node(node);
	sDeviceManager->put_node(root);
	return found;
}


static void
put_device_manager()
{
	if (sNode != NULL)
		sDeviceManager->put_node(sNode);
	sNode = NULL;
	if (sDeviceManager != NULL)
		put_module(B_DEVICE_MANAGER_MODULE_NAME);
	sDeviceManager = NULL;
}


bool
bwfm_host_present(void)
{
	if (find_function())
		return true;

	if (mmc_bus_has_port()) {
		// The bus powers the chip and reads its CIS at boot, about a third
		// of a second; the network stack may ask before.
		bigtime_t deadline = system_time() + 5000000;
		while (system_time() < deadline) {
			snooze(100000);
			if (find_function())
				return true;
		}
		ERROR("the MMC bus has not found the Wi-Fi chip\n");
		put_device_manager();
		return false;
	}
	put_device_manager();

	return rpi_sdio_present();
}


status_t
bwfm_host_init(void)
{
	if (find_function()) {
		// the bus has set the card up
		return B_OK;
	}
	put_device_manager();

	status_t status = rpi_sdio_init();
	if (status != B_OK)
		return status;
	sSdio = &sRpiFunctions;
	sCookie = NULL;
	return B_OK;
}


void
bwfm_host_uninit(void)
{
	if (sSdio == NULL)
		return;

	if (sSdio == &sRpiFunctions) {
		rpi_sdio_uninit();
	} else {
		// The bus keeps the card; its functions are turned off, as the
		// chip is after the Pi 4's host powers it down.
		sSdio->enable_function(sCookie, 2, false);
		sSdio->enable_function(sCookie, 1, false);
	}
	sSdio = NULL;
	sCookie = NULL;
	put_device_manager();
}


status_t
bwfm_host_rw_byte(bool write, uint32 function, uint32 address, uint8* _value)
{
	if (sSdio == NULL)
		return B_NO_INIT;
	if (write)
		return sSdio->write_byte(sCookie, function, address, *_value, _value);
	return sSdio->read_byte(sCookie, function, address, _value);
}


status_t
bwfm_host_rw_extended(bool write, uint32 function, uint32 address,
	uint8* buffer, size_t length, bool increment)
{
	if (sSdio == NULL)
		return B_NO_INIT;
	if (write)
		return sSdio->write(sCookie, function, address, increment, buffer,
			length);
	return sSdio->read(sCookie, function, address, increment, buffer, length);
}


status_t
bwfm_host_set_block_size(uint32 function, uint16 size)
{
	if (sSdio == NULL)
		return B_NO_INIT;
	return sSdio->set_block_size(sCookie, function, size);
}


status_t
bwfm_host_enable_function(uint32 function, bool enable)
{
	if (sSdio == NULL)
		return B_NO_INIT;
	return sSdio->enable_function(sCookie, function, enable);
}


status_t
bwfm_host_set_interrupt_handler(void (*handler)(void*), void* data)
{
	if (sSdio == NULL)
		return B_NO_INIT;

	// The chip signals function 1 (mailbox) and function 2 (frames)
	// events; the driver serves both in one go.
	if (handler == NULL) {
		sSdio->set_interrupt_handler(sCookie, 2, NULL, NULL);
		return sSdio->set_interrupt_handler(sCookie, 1, NULL, NULL);
	}
	status_t status = sSdio->set_interrupt_handler(sCookie, 1, handler, data);
	if (status == B_OK)
		status = sSdio->set_interrupt_handler(sCookie, 2, handler, data);
	if (status != B_OK)
		sSdio->set_interrupt_handler(sCookie, 1, NULL, NULL);
	return status;
}
