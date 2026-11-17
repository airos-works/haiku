/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	SDIO function drivers: the MMC bus manager finds the cards and publishes
	a device node per I/O function; this finds those of the vendors a driver
	names, probes the driver's FreeBSD-style probe method against each and
	reports the matches to driver.c, which attaches them like PCI and USB
	devices. The driver talks to the card through the calls of
	compat/dev/sdio/sdio_subr.h, which go to the node's
	sdio_function_interface.
*/


extern "C" {
#include <sys/bus.h>
#include <sys/mutex.h>
#include <sys/systm.h>

#include <dev/sdio/sdio_subr.h>

#include "device.h"
}

#include <device_manager.h>
#include "sdio.h"
	// headers/private/drivers, as a local header (Jamfile)

#include <stdlib.h>
#include <string.h>


static device_manager_info* sDeviceManager = NULL;


static status_t
init_sdio()
{
	if (sDeviceManager != NULL)
		return B_OK;

	if (get_module(B_DEVICE_MANAGER_MODULE_NAME,
			(module_info**)&sDeviceManager) != B_OK) {
		dprintf("%s: cannot get the device manager\n", gDriverName);
		sDeviceManager = NULL;
		return B_ERROR;
	}
	return B_OK;
}


extern "C" void
uninit_sdio()
{
	if (sDeviceManager == NULL)
		return;

	put_module(B_DEVICE_MANAGER_MODULE_NAME);
	sDeviceManager = NULL;
}


static int
map_sdio_error(status_t status)
{
	switch (status) {
		case B_OK:
			return 0;
		case B_TIMED_OUT:
			return ETIMEDOUT;
		case B_NO_MEMORY:
			return ENOMEM;
		case B_BAD_VALUE:
			return EINVAL;
		case B_DEV_NOT_READY:
		case B_DEV_NO_MEDIA:
			return ENXIO;
		default:
			return EIO;
	}
}


static inline sdio_function_interface*
sdio_interface(struct sdio_func* func)
{
	return (sdio_function_interface*)func->haiku_interface;
}


//	#pragma mark - finding the functions


struct compat_sdio_device {
	struct sdio_func func;
};


static void
free_compat_sdio_device(void* cookie)
{
	compat_sdio_device* device = (compat_sdio_device*)cookie;
	if (device == NULL)
		return;
	if (device->func.haiku_node != NULL && sDeviceManager != NULL)
		sDeviceManager->put_node((device_node*)device->func.haiku_node);
	free(device);
}


static void
prepare_sdio_attach(void* cookie, device_t device)
{
	compat_sdio_device* compatDevice = (compat_sdio_device*)cookie;

	struct root_device_softc* rootSoftc
		= (struct root_device_softc*)device->parent->softc;
	rootSoftc->sdio_func = &compatDevice->func;

	compatDevice->func.dev = device;
	device_set_ivars(device, &compatDevice->func);
}


/*!	Fills in \a func for the function \a node; the node stays acquired. */
static status_t
get_sdio_func(device_node* node, struct sdio_func* func)
{
	memset(func, 0, sizeof(*func));

	sdio_function_interface* interface;
	void* cookie;
	status_t status = sDeviceManager->get_driver(node,
		(driver_module_info**)&interface, &cookie);
	if (status != B_OK)
		return status;
	if (cookie == NULL)
		return B_NO_INIT;

	uint8 function = 0;
	sDeviceManager->get_attr_uint16(node, SDIO_VENDOR_ATTRIBUTE, &func->vendor,
		false);
	sDeviceManager->get_attr_uint16(node, SDIO_DEVICE_ATTRIBUTE, &func->device,
		false);
	sDeviceManager->get_attr_uint8(node, SDIO_FUNCTION_ATTRIBUTE, &function,
		false);
	sDeviceManager->get_attr_uint8(node, SDIO_CLASS_ATTRIBUTE, &func->fclass,
		false);
	func->fn = function;

	sdio_card_info info;
	if (interface->get_card_info(cookie, &info) == B_OK) {
		if (function < SDIO_MAX_FUNCTIONS) {
			func->max_blksize = info.max_block_size[function];
			func->cur_blksize = info.block_size[function];
		}
		func->bus_width = info.bus_width;
		func->high_speed = info.high_speed;
		func->capabilities = info.capabilities;
		func->clock = info.clock;
	}

	func->haiku_node = node;
	func->haiku_interface = interface;
	func->haiku_cookie = cookie;
	return B_OK;
}


/*!	Walks the published functions of \a vendor (all function numbers).
	\a node: the one found last (released here), NULL to start; the one
	returned is acquired.
*/
static device_node*
next_sdio_function(uint16 vendor, device_node* root, device_node* node)
{
	device_attr attributes[] = {
		{ SDIO_VENDOR_ATTRIBUTE, B_UINT16_TYPE, { .ui16 = vendor } },
		{}
	};
	while (sDeviceManager->find_child_node(root, attributes, &node) == B_OK) {
		uint8 function;
		if (sDeviceManager->get_attr_uint8(node, SDIO_FUNCTION_ATTRIBUTE,
				&function, false) == B_OK && function > 0) {
			return node;
		}
	}
	return NULL;
}


/*!	Whether a card of \a vendor with one of the \a devices is there, without
	attaching anything (\a deviceCount 0: any).
*/
extern "C" status_t
_fbsd_sdio_find(uint16 vendor, const uint16* devices, size_t deviceCount,
	uint16* _device, uint8* _function)
{
	bool hadManager = sDeviceManager != NULL;
	if (init_sdio() != B_OK)
		return B_ERROR;

	device_node* root = sDeviceManager->get_root_node();
	device_node* node = NULL;
	status_t status = B_ENTRY_NOT_FOUND;
	while ((node = next_sdio_function(vendor, root, node)) != NULL) {
		uint16 device = 0;
		uint8 function = 0;
		sDeviceManager->get_attr_uint16(node, SDIO_DEVICE_ATTRIBUTE, &device,
			false);
		sDeviceManager->get_attr_uint8(node, SDIO_FUNCTION_ATTRIBUTE,
			&function, false);
		bool match = deviceCount == 0;
		for (size_t i = 0; i < deviceCount && !match; i++)
			match = devices[i] == device;
		if (match) {
			if (_device != NULL)
				*_device = device;
			if (_function != NULL)
				*_function = function;
			sDeviceManager->put_node(node);
			status = B_OK;
			break;
		}
	}
	sDeviceManager->put_node(root);
	if (!hadManager)
		uninit_sdio();
	return status;
}


/*!	Probes \a drivers against the published SDIO functions of \a vendors.
	The MMC bus finds cards in the background at boot; with \a wait, this
	looks again for up to that long when none of a driver's are there yet.
*/
extern "C" status_t
_fbsd_init_hardware_sdio(driver_t* drivers[], const uint16* vendors,
	size_t vendorCount, bigtime_t wait)
{
	const int BUS_sdio = root_device_softc::BUS_sdio;

	status_t status = init_sdio();
	if (status != B_OK)
		return status;

	device_t root;
	status = init_root_device(&root, BUS_sdio);
	if (status != B_OK) {
		uninit_sdio();
		return status;
	}

	// The device manager has no call to acquire a node once more: a node a
	// device keeps is left acquired by the search that found it, and the
	// search starts over (skipping the kept ones) instead of going on from
	// it, which would release it.
	device_node* kept[MAX_DEVICES];
	int keptCount = 0;
	device_node* rejected[MAX_DEVICES * 2];
	int rejectedCount = 0;

	bigtime_t deadline = system_time() + wait;
	bool any = false;
	while (true) {
		device_node* busRoot = sDeviceManager->get_root_node();
		for (size_t v = 0; v < vendorCount && keptCount < MAX_DEVICES; v++) {
			device_node* node = NULL;
			while ((node = next_sdio_function(vendors[v], busRoot, node))
					!= NULL) {
				any = true;
				bool seen = false;
				for (int i = 0; i < keptCount && !seen; i++)
					seen = kept[i] == node;
				for (int i = 0; i < rejectedCount && !seen; i++)
					seen = rejected[i] == node;
				if (seen)
					continue;

				compat_sdio_device* compatDevice
					= (compat_sdio_device*)calloc(1,
						sizeof(compat_sdio_device));
				driver_t* driver = NULL;
				if (compatDevice != NULL
					&& get_sdio_func(node, &compatDevice->func) == B_OK) {
					struct device device = {};
					device.parent = root;
					device.root = root;
					device_set_ivars(&device, &compatDevice->func);
					driver = __haiku_probe_drivers(&device, drivers);
				}
				if (driver == NULL) {
					free(compatDevice);
					if (rejectedCount < (int)B_COUNT_OF(rejected))
						rejected[rejectedCount++] = node;
					continue;
				}

				dprintf("%s: SDIO function %u of card %04x:%04x, %u bit, "
					"%" B_PRIu32 " kHz\n", gDriverName, compatDevice->func.fn,
					compatDevice->func.vendor, compatDevice->func.device,
					compatDevice->func.bus_width, compatDevice->func.clock);

				// the device keeps the node, acquired; search again
				compatDevice->func.haiku_node = node;
				kept[keptCount++] = node;
				report_probed_device(BUS_sdio, compatDevice, driver,
					prepare_sdio_attach, free_compat_sdio_device);
				node = NULL;
				if (keptCount == MAX_DEVICES)
					break;
			}
		}
		sDeviceManager->put_node(busRoot);

		if (keptCount > 0 || any || system_time() >= deadline)
			break;
		snooze(250000);
	}
	bool found = keptCount > 0;

	device_delete_child(NULL, root);

	if (found)
		return B_OK;

	uninit_sdio();
	return B_NOT_SUPPORTED;
}


//	#pragma mark - the function interface


extern "C" int
sdio_enable_func(struct sdio_func* func)
{
	return map_sdio_error(sdio_interface(func)->enable_function(
		func->haiku_cookie, func->fn, true));
}


extern "C" int
sdio_disable_func(struct sdio_func* func)
{
	return map_sdio_error(sdio_interface(func)->enable_function(
		func->haiku_cookie, func->fn, false));
}


extern "C" int
sdio_set_block_size(struct sdio_func* func, uint16_t size)
{
	status_t status = sdio_interface(func)->set_block_size(func->haiku_cookie,
		func->fn, size);
	if (status != B_OK)
		return map_sdio_error(status);

	sdio_card_info info;
	if (sdio_interface(func)->get_card_info(func->haiku_cookie, &info)
			== B_OK && func->fn < SDIO_MAX_FUNCTIONS) {
		func->cur_blksize = info.block_size[func->fn];
	} else
		func->cur_blksize = size;
	return 0;
}


static uint8_t
read_byte(struct sdio_func* func, uint8 function, uint32_t address, int* _error)
{
	uint8 value = 0xff;
	status_t status = sdio_interface(func)->read_byte(func->haiku_cookie,
		function, address, &value);
	if (_error != NULL)
		*_error = map_sdio_error(status);
	return status == B_OK ? value : 0xff;
}


static void
write_byte(struct sdio_func* func, uint8 function, uint32_t address,
	uint8_t value, int* _error)
{
	status_t status = sdio_interface(func)->write_byte(func->haiku_cookie,
		function, address, value, NULL);
	if (_error != NULL)
		*_error = map_sdio_error(status);
}


extern "C" uint8_t
sdio_read_1(struct sdio_func* func, uint32_t address, int* _error)
{
	return read_byte(func, func->fn, address, _error);
}


extern "C" void
sdio_write_1(struct sdio_func* func, uint32_t address, uint8_t value,
	int* _error)
{
	write_byte(func, func->fn, address, value, _error);
}


extern "C" uint8_t
sdio_f0_read_1(struct sdio_func* func, uint32_t address, int* _error)
{
	return read_byte(func, 0, address, _error);
}


extern "C" void
sdio_f0_write_1(struct sdio_func* func, uint32_t address, uint8_t value,
	int* _error)
{
	write_byte(func, 0, address, value, _error);
}


extern "C" int
sdio_read_extended(struct sdio_func* func, uint32_t address, bool increment,
	void* buffer, size_t length)
{
	return map_sdio_error(sdio_interface(func)->read(func->haiku_cookie,
		func->fn, address, increment, buffer, length));
}


extern "C" int
sdio_write_extended(struct sdio_func* func, uint32_t address, bool increment,
	const void* buffer, size_t length)
{
	return map_sdio_error(sdio_interface(func)->write(func->haiku_cookie,
		func->fn, address, increment, buffer, length));
}


extern "C" int
sdio_setup_intr(struct sdio_func* func, void (*handler)(void*), void* arg)
{
	if (handler == NULL)
		return EINVAL;
	return map_sdio_error(sdio_interface(func)->set_interrupt_handler(
		func->haiku_cookie, func->fn, handler, arg));
}


extern "C" int
sdio_teardown_intr(struct sdio_func* func)
{
	return map_sdio_error(sdio_interface(func)->set_interrupt_handler(
		func->haiku_cookie, func->fn, NULL, NULL));
}
