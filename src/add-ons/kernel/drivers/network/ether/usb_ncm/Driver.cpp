/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Copyright 2008, Michael Lotz <mmlr@mlotz.ch> (usb_ecm, which this follows)
 * Distributed under the terms of the MIT License.
 */


#include <fcntl.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bus/USB.h>
#include <driver_settings.h>

#include "Driver.h"
#include "NCMDevice.h"


#define DEVICE_BASE_NAME "net/usb_ncm/"
#define USB_NCM_DRIVER_MODULE_NAME "drivers/network/usb_ncm/driver_v1"
#define USB_NCM_DEVICE_MODULE_NAME "drivers/network/usb_ncm/device_v1"
#define USB_NCM_DEVICE_ID_GENERATOR "usb_ncm/device_id"


usb_module_info* gUSBModule = NULL;
device_manager_info* gDeviceManager = NULL;
net_buffer_module_info* gBufferModule = NULL;


typedef struct {
	device_node*	node;
	NCMDevice*		device;
} usb_ncm_driver_info;


/*!	An open for reading only does not take the device: it can only ask for
	USB_NCM_GET_INFO, while the network stack has the device open.
*/
typedef struct {
	NCMDevice*		device;
	bool			passive;
} usb_ncm_cookie;


//	#pragma mark - device module API


static status_t
usb_ncm_init_device(void* _info, void** _cookie)
{
	usb_ncm_driver_info* info = (usb_ncm_driver_info*)_info;

	usb_device device;
	if (gDeviceManager->get_attr_uint32(info->node, USB_DEVICE_ID_ITEM,
			&device, true) != B_OK) {
		return B_ERROR;
	}

	NCMDevice* ncmDevice = new(std::nothrow) NCMDevice(device);
	if (ncmDevice == NULL)
		return B_NO_MEMORY;

	status_t status = ncmDevice->InitCheck();
	if (status < B_OK) {
		delete ncmDevice;
		return status;
	}

	info->device = ncmDevice;
	*_cookie = info;
	return B_OK;
}


static void
usb_ncm_uninit_device(void* _cookie)
{
	usb_ncm_driver_info* info = (usb_ncm_driver_info*)_cookie;
	delete info->device;
	info->device = NULL;
}


static void
usb_ncm_device_removed(void* _cookie)
{
	usb_ncm_driver_info* info = (usb_ncm_driver_info*)_cookie;
	if (info->device != NULL)
		info->device->Removed();
}


static status_t
usb_ncm_open(void* _info, const char* path, int openMode, void** _cookie)
{
	usb_ncm_driver_info* info = (usb_ncm_driver_info*)_info;

	usb_ncm_cookie* cookie = (usb_ncm_cookie*)malloc(sizeof(usb_ncm_cookie));
	if (cookie == NULL)
		return B_NO_MEMORY;
	cookie->device = info->device;
	cookie->passive = (openMode & O_ACCMODE) == O_RDONLY;

	if (!cookie->passive) {
		status_t status = info->device->Open();
		if (status != B_OK) {
			free(cookie);
			return status;
		}
	}

	*_cookie = cookie;
	return B_OK;
}


static status_t
usb_ncm_read(void* _cookie, off_t position, void* buffer, size_t* numBytes)
{
	usb_ncm_cookie* cookie = (usb_ncm_cookie*)_cookie;
	if (cookie->passive) {
		*numBytes = 0;
		return B_NOT_ALLOWED;
	}
	return cookie->device->Read((uint8*)buffer, numBytes);
}


static status_t
usb_ncm_write(void* _cookie, off_t position, const void* buffer,
	size_t* numBytes)
{
	usb_ncm_cookie* cookie = (usb_ncm_cookie*)_cookie;
	if (cookie->passive) {
		*numBytes = 0;
		return B_NOT_ALLOWED;
	}
	return cookie->device->Write((const uint8*)buffer, numBytes);
}


static status_t
usb_ncm_control(void* _cookie, uint32 op, void* buffer, size_t length)
{
	usb_ncm_cookie* cookie = (usb_ncm_cookie*)_cookie;
	if (cookie->passive) {
		if (op == USB_NCM_GET_INFO)
			return cookie->device->GetInfo(buffer, length);
		return B_NOT_ALLOWED;
	}
	return cookie->device->Control(op, buffer, length);
}


static status_t
usb_ncm_close(void* _cookie)
{
	usb_ncm_cookie* cookie = (usb_ncm_cookie*)_cookie;
	if (cookie->passive)
		return B_OK;
	return cookie->device->Close();
}


static status_t
usb_ncm_free(void* _cookie)
{
	usb_ncm_cookie* cookie = (usb_ncm_cookie*)_cookie;
	status_t status = B_OK;
	if (!cookie->passive)
		status = cookie->device->Free();
	free(cookie);
	return status;
}


//	#pragma mark - driver module API


static float
usb_ncm_supports_device(device_node* parent)
{
	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK
		|| strcmp(bus, "usb") != 0) {
		return 0.0f;
	}

	// Only a function that can be driven: usb_ecm leaves a device that also
	// offers ECM to this driver when it says yes here.
	usb_device device;
	if (gDeviceManager->get_attr_uint32(parent, USB_DEVICE_ID_ITEM, &device,
			false) != B_OK) {
		return 0.0f;
	}

	ncm_function function;
	if (!NCMDevice::FindFunction(device, &function))
		return 0.0f;

	return 0.6f;
}


static status_t
usb_ncm_register_device(device_node* node)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "USB NCM" } },
		{ NULL }
	};

	return gDeviceManager->register_node(node, USB_NCM_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
usb_ncm_init_driver(device_node* node, void** cookie)
{
	usb_ncm_driver_info* info
		= (usb_ncm_driver_info*)malloc(sizeof(usb_ncm_driver_info));
	if (info == NULL)
		return B_NO_MEMORY;

	memset(info, 0, sizeof(*info));
	info->node = node;
	*cookie = info;
	return B_OK;
}


static void
usb_ncm_uninit_driver(void* _cookie)
{
	free(_cookie);
}


static status_t
usb_ncm_register_child_devices(void* _cookie)
{
	usb_ncm_driver_info* info = (usb_ncm_driver_info*)_cookie;

	int32 id = gDeviceManager->create_id(USB_NCM_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	char name[64];
	snprintf(name, sizeof(name), DEVICE_BASE_NAME "%" B_PRId32, id);

	status_t status = gDeviceManager->publish_device(info->node, name,
		USB_NCM_DEVICE_MODULE_NAME);
	if (status != B_OK)
		gDeviceManager->free_id(USB_NCM_DEVICE_ID_GENERATOR, id);
	return status;
}


/*!	"disable true" in the driver settings file keeps the driver from loading.
	usb_ecm then takes the devices that offer ECM next to NCM again: it only
	steps aside while this module loads.
*/
static status_t
usb_ncm_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		{
			void* handle = load_driver_settings(DRIVER_NAME);
			bool disabled = get_driver_boolean_parameter(handle, "disable",
				false, true);
			unload_driver_settings(handle);
			return disabled ? B_NOT_ALLOWED : B_OK;
		}

		case B_MODULE_UNINIT:
			return B_OK;
	}

	return B_ERROR;
}


//	#pragma mark -


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{ B_USB_MODULE_NAME, (module_info**)&gUSBModule },
	{ NET_BUFFER_MODULE_NAME, (module_info**)&gBufferModule },
	{ NULL }
};


struct device_module_info sUsbNcmDevice = {
	{
		USB_NCM_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	usb_ncm_init_device,
	usb_ncm_uninit_device,
	usb_ncm_device_removed,

	usb_ncm_open,
	usb_ncm_close,
	usb_ncm_free,
	usb_ncm_read,
	usb_ncm_write,
	NULL,	// io
	usb_ncm_control,

	NULL,	// select
	NULL,	// deselect
};


struct driver_module_info sUsbNcmDriver = {
	{
		USB_NCM_DRIVER_MODULE_NAME,
		0,
		usb_ncm_std_ops
	},

	usb_ncm_supports_device,
	usb_ncm_register_device,
	usb_ncm_init_driver,
	usb_ncm_uninit_driver,
	usb_ncm_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


module_info* modules[] = {
	(module_info*)&sUsbNcmDriver,
	(module_info*)&sUsbNcmDevice,
	NULL
};
