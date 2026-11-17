/*
 * Copyright 2018-2021 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		B Krishnan Iyer, krishnaniyer97@gmail.com
 *		Adrien Destugues, pulkomandy@pulkomandy.tk
 */
#include "mmc_bus.h"


#define	MMC_BUS_DEVICE_NAME "bus_managers/mmc_bus/device/v1"


device_manager_info* gDeviceManager = NULL;


static status_t
mmc_bus_init(device_node* node, void** _device)
{
	CALLED();
	MMCBus* device = new(std::nothrow) MMCBus(node);
	if (device == NULL) {
		ERROR("Unable to allocate MMC bus\n");
		return B_NO_MEMORY;
	}

	status_t result = device->InitCheck();
	if (result != B_OK) {
		TRACE("failed to set up mmc bus device object\n");
		return result;
	}
	TRACE("MMC bus object created\n");

	*_device = device;
	return B_OK;
}


static void
mmc_bus_uninit(void* _device)
{
	CALLED();
	MMCBus* device = (MMCBus*)_device;
	delete device;
}


static status_t
mmc_bus_register_child(void* _device)
{
	// Nothing to do, child devices are registered by the scanning thread
	return B_OK;
}


static void
mmc_bus_removed(void* _device)
{
	CALLED();
}


status_t
mmc_bus_added_device(device_node* parent)
{
	CALLED();

	device_attr attributes[] = {
		{ B_DEVICE_BUS, B_STRING_TYPE, { .string = "mmc"}},
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "MMC bus root"}},
		{ NULL }
	};

	return gDeviceManager->register_node(parent, MMC_BUS_DEVICE_NAME,
		attributes, NULL, NULL);
}


static status_t
mmc_bus_execute_command(device_node* node, void* cookie, uint16_t rca,
	uint8_t command, uint32_t argument, uint32_t* result)
{
	TRACE("In mmc_bus_execute_command\n");

	MMCBus* bus = (MMCBus*)cookie;

	bus->AcquireBus();
	status_t error = bus->ExecuteCommand(rca, command, argument, result);
	bus->ReleaseBus();

	return error;
}


static status_t
mmc_bus_do_io(device_node* node, void* cookie, uint16_t rca, uint8_t command,
	IOOperation* operation, bool offsetAsSectors)
{
	MMCBus* bus = (MMCBus*)cookie;
	status_t result = B_OK;

	bus->AcquireBus();
	result = bus->DoIO(rca, command, operation, offsetAsSectors);
	bus->ReleaseBus();

	return result;
}


static void
mmc_bus_set_width(device_node* node, void* cookie, int width)
{
	MMCBus* bus = (MMCBus*)cookie;

	bus->AcquireBus();
	bus->SetBusWidth(width);
	bus->ReleaseBus();
}


static status_t
std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
			// Nothing to do
		case B_MODULE_UNINIT:
			return B_OK;

		default:
			break;
	}

	return B_ERROR;
}


driver_module_info mmc_bus_device_module = {
	{
		MMC_BUS_DEVICE_NAME,
		0,
		std_ops
	},
	NULL, // supported devices
	NULL, // register node
	mmc_bus_init,
	mmc_bus_uninit,
	mmc_bus_register_child,
	NULL, // rescan
	mmc_bus_removed,
	NULL, // suspend
	NULL // resume
};


mmc_device_interface mmc_bus_controller_module = {
	{
		{
			MMC_BUS_MODULE_NAME,
			0,
			&std_ops
		},

		NULL, // supported devices
		mmc_bus_added_device,
		NULL,
		NULL,
		NULL
	},
	mmc_bus_execute_command,
	mmc_bus_do_io,
	mmc_bus_set_width
};


//	#pragma mark - SDIO functions


struct sdio_function_cookie {
	MMCBus*	bus;
	uint8	function;
};


static status_t
sdio_function_init(device_node* node, void** _cookie)
{
	uint8 function;
	if (gDeviceManager->get_attr_uint8(node, SDIO_FUNCTION_ATTRIBUTE,
			&function, false) != B_OK
		|| function == 0 || function >= SDIO_MAX_FUNCTIONS)
		return B_BAD_VALUE;

	device_node* parent = gDeviceManager->get_parent_node(node);
	MMCBus* bus = NULL;
	status_t status = gDeviceManager->get_driver(parent, NULL, (void**)&bus);
	gDeviceManager->put_node(parent);
	if (status != B_OK || bus == NULL)
		return status != B_OK ? status : B_NO_INIT;

	sdio_function_cookie* cookie = new(std::nothrow) sdio_function_cookie;
	if (cookie == NULL)
		return B_NO_MEMORY;
	cookie->bus = bus;
	cookie->function = function;
	*_cookie = cookie;
	return B_OK;
}


static void
sdio_function_uninit(void* _cookie)
{
	sdio_function_cookie* cookie = (sdio_function_cookie*)_cookie;
	cookie->bus->SdioSetInterruptHandler(cookie->function, NULL, NULL);
	delete cookie;
}


static status_t
sdio_read_byte(void* cookie, uint8 function, uint32 address, uint8* _value)
{
	return ((sdio_function_cookie*)cookie)->bus->SdioReadByte(function,
		address, _value);
}


static status_t
sdio_write_byte(void* cookie, uint8 function, uint32 address, uint8 value,
	uint8* _readBack)
{
	return ((sdio_function_cookie*)cookie)->bus->SdioWriteByte(function,
		address, value, _readBack);
}


static status_t
sdio_read(void* cookie, uint8 function, uint32 address, bool increment,
	void* buffer, size_t length)
{
	return ((sdio_function_cookie*)cookie)->bus->SdioTransfer(false, function,
		address, increment, (uint8*)buffer, length);
}


static status_t
sdio_write(void* cookie, uint8 function, uint32 address, bool increment,
	const void* buffer, size_t length)
{
	return ((sdio_function_cookie*)cookie)->bus->SdioTransfer(true, function,
		address, increment, (uint8*)buffer, length);
}


static status_t
sdio_set_block_size(void* cookie, uint8 function, uint16 size)
{
	return ((sdio_function_cookie*)cookie)->bus->SdioSetBlockSize(function,
		size);
}


static status_t
sdio_enable_function(void* cookie, uint8 function, bool enable)
{
	return ((sdio_function_cookie*)cookie)->bus->SdioEnableFunction(function,
		enable);
}


static status_t
sdio_set_interrupt_handler(void* cookie, uint8 function,
	sdio_interrupt_handler handler, void* data)
{
	return ((sdio_function_cookie*)cookie)->bus->SdioSetInterruptHandler(
		function, handler, data);
}


static status_t
sdio_get_card_info(void* cookie, sdio_card_info* info)
{
	return ((sdio_function_cookie*)cookie)->bus->SdioGetCardInfo(info);
}


sdio_function_interface sdio_function_module = {
	{
		{
			SDIO_FUNCTION_MODULE_NAME,
			0,
			&std_ops
		},

		NULL, // supported devices
		NULL, // register node
		sdio_function_init,
		sdio_function_uninit,
		NULL, // register child devices
		NULL, // rescan
		NULL, // removed
	},
	sdio_read_byte,
	sdio_write_byte,
	sdio_read,
	sdio_write,
	sdio_set_block_size,
	sdio_enable_function,
	sdio_set_interrupt_handler,
	sdio_get_card_info
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info **)&gDeviceManager },
	{}
};


module_info* modules[] = {
	(module_info*)&mmc_bus_controller_module,
	(module_info*)&mmc_bus_device_module,
	(module_info*)&sdio_function_module,
	NULL
};
