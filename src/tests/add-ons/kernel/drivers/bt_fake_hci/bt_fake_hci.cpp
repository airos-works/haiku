/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	A Bluetooth transport driver with no radio behind it, for testing the
	HCI module's receive path (#228) where there is no Bluetooth hardware:
	it registers a device with the HCI module and hands it whatever a test
	program writes, as a USB radio's completion callbacks do with what the
	radio sends. What the stack sends to the radio is dropped.
*/


#include <string.h>

#include <Drivers.h>
#include <KernelExport.h>
#include <OS.h>

#include <net_buffer.h>

#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_transport.h>

#include "bt_fake_hci.h"


int32 api_version = B_CUR_DRIVER_API_VERSION;

static const char* sDevices[] = { BT_FAKE_HCI_DEVICE, NULL };
static bt_hci_module_info* sHci;
static net_buffer_module_info* sBufferModule;


static status_t
fake_send(hci_id id, net_buffer* buffer)
{
	// as if the radio had taken it
	sBufferModule->free(buffer);
	return B_OK;
}


static bt_hci_transport_hooks sHooks = {
	fake_send,
	fake_send,
	fake_send,
	NULL,
	NULL,
	H2
};


static status_t
fake_open(const char* name, uint32 flags, void** cookie)
{
	*cookie = NULL;
	return B_OK;
}


static status_t
fake_close(void* cookie)
{
	return B_OK;
}


static status_t
fake_free(void* cookie)
{
	return B_OK;
}


static status_t
fake_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	switch (op) {
		case BT_FAKE_REGISTER:
		{
			bluetooth_device* device;
			status_t status = sHci->RegisterDriver(&sHooks, &device);
			if (status != B_OK)
				return status;
			int32 id = device->index;
			return user_memcpy(buffer, &id, sizeof(id));
		}

		case BT_FAKE_UNREGISTER:
		{
			int32 id;
			if (user_memcpy(&id, buffer, sizeof(id)) != B_OK)
				return B_BAD_ADDRESS;
			return sHci->UnregisterDriver(id);
		}

		case BT_FAKE_POST:
		{
			bt_fake_post post;
			if (user_memcpy(&post, buffer, sizeof(post)) != B_OK)
				return B_BAD_ADDRESS;
			if (post.size > sizeof(post.data))
				return B_BAD_VALUE;

			bigtime_t start = system_time();
			post.status = sHci->PostTransportPacket(post.id,
				(bt_packet_t)post.type, post.data, post.size);
			post.duration = system_time() - start;
			return user_memcpy(buffer, &post, sizeof(post));
		}
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
fake_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_OK;
}


static status_t
fake_write(void* cookie, off_t position, const void* buffer, size_t* _length)
{
	return B_NOT_SUPPORTED;
}


static device_hooks sDeviceHooks = {
	fake_open,
	fake_close,
	fake_free,
	fake_control,
	fake_read,
	fake_write
};


status_t
init_hardware()
{
	return B_OK;
}


status_t
init_driver()
{
	status_t status = get_module(NET_BUFFER_MODULE_NAME,
		(module_info**)&sBufferModule);
	if (status != B_OK)
		return status;

	status = get_module(BT_HCI_MODULE_NAME, (module_info**)&sHci);
	if (status != B_OK) {
		put_module(NET_BUFFER_MODULE_NAME);
		return status;
	}

	return B_OK;
}


void
uninit_driver()
{
	put_module(BT_HCI_MODULE_NAME);
	put_module(NET_BUFFER_MODULE_NAME);
}


const char**
publish_devices()
{
	return sDevices;
}


device_hooks*
find_device(const char* name)
{
	return &sDeviceHooks;
}
