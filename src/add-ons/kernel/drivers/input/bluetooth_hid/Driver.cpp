/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Copyright 2008-2011 Michael Lotz <mmlr@mlotz.ch>
 * Distributed under the terms of the MIT License.
 */


/*!	Bluetooth HID devices, fed by the Bluetooth server; see
	<bluetooth/bluetooth_hid.h>. Each open of the control device can create
	one HID device; its keyboard, mouse, tablet and joystick parts are
	published by the shared HID protocol handlers until the control device is
	closed.
*/


#include "Driver.h"

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bluetooth_hid.h"
#include <fs/devfs.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>

#include "HIDDevice.h"
#include "ProtocolHandler.h"


int32 api_version = B_CUR_DRIVER_API_VERSION;

DeviceList* gDeviceList = NULL;
static mutex sDriverLock;


struct control_cookie {
	HIDDevice*	device;
};

struct device_cookie {
	ProtocolHandler*	handler;
	uint32				cookie;
};


static device_hooks sDeviceHooks;


// #pragma mark - HID devices


static void
remove_device(HIDDevice* device)
{
	// Unpublish outside of sDriverLock: devfs may call our free hook.
	char* paths[16];
	uint32 pathCount = 0;

	mutex_lock(&sDriverLock);
	for (uint32 i = 0;; i++) {
		ProtocolHandler* handler = device->ProtocolHandlerAt(i);
		if (handler == NULL)
			break;
		if (handler->PublishPath() == NULL)
			continue;

		gDeviceList->RemoveDevice(NULL, handler);
		if (pathCount < B_COUNT_OF(paths))
			paths[pathCount++] = strdup(handler->PublishPath());
	}
	device->Removed();
	// Nobody can open it any more; who deletes it is decided here, under
	// the lock: us, or the free hook of its last open handle.
	bool deleteNow = device->CountCookies() == 0;
	TRACE_ALWAYS("%s: removed%s\n", device->Name(),
		deleteNow ? "" : ", deleted when its last user closes it");
	mutex_unlock(&sDriverLock);

	for (uint32 i = 0; i < pathCount; i++) {
		if (paths[i] == NULL)
			continue;
		devfs_unpublish_device(paths[i], true);
		free(paths[i]);
	}

	if (deleteNow)
		delete device;
}


static status_t
create_device(control_cookie* cookie, void* buffer, size_t length)
{
	if (cookie->device != NULL)
		return B_BUSY;

	bluetooth_hid_create create;
	if (length < sizeof(create) || buffer == NULL)
		return B_BAD_VALUE;
	if (IS_USER_ADDRESS(buffer)) {
		if (user_memcpy(&create, buffer, sizeof(create)) != B_OK)
			return B_BAD_ADDRESS;
	} else
		memcpy(&create, buffer, sizeof(create));

	if (create.version != BLUETOOTH_HID_VERSION)
		return B_BAD_VALUE;
	if (create.descriptor_length == 0
		|| create.descriptor_length > BLUETOOTH_HID_MAX_DESCRIPTOR
		|| create.descriptor == NULL)
		return B_BAD_VALUE;
	create.name[sizeof(create.name) - 1] = '\0';

	uint8* descriptor = (uint8*)malloc(create.descriptor_length);
	if (descriptor == NULL)
		return B_NO_MEMORY;
	status_t status = B_OK;
	if (IS_USER_ADDRESS(create.descriptor)) {
		status = user_memcpy(descriptor, create.descriptor,
			create.descriptor_length);
	} else
		memcpy(descriptor, create.descriptor, create.descriptor_length);
	if (status != B_OK) {
		free(descriptor);
		return B_BAD_ADDRESS;
	}

	char name[64];
	snprintf(name, sizeof(name), "%s (%02x:%02x:%02x:%02x:%02x:%02x)",
		create.name[0] != '\0' ? create.name : "device",
		create.address[5], create.address[4], create.address[3],
		create.address[2], create.address[1], create.address[0]);

	HIDDevice* device = new(std::nothrow) HIDDevice(descriptor,
		create.descriptor_length, name);
	free(descriptor);
	if (device == NULL)
		return B_NO_MEMORY;
	status = device->InitCheck();
	if (status != B_OK) {
		delete device;
		return status;
	}

	// Publish every part under the first free number, as usb_hid does:
	// "keyboard/bluetooth/0" may be free while "1" is taken.
	char* paths[16];
	uint32 pathCount = 0;
	mutex_lock(&sDriverLock);
	for (uint32 i = 0;; i++) {
		ProtocolHandler* handler = device->ProtocolHandlerAt(i);
		if (handler == NULL)
			break;

		char path[B_DEV_NAME_LENGTH];
		for (int32 index = 0;; index++) {
			snprintf(path, sizeof(path), "%s%" B_PRId32, handler->BasePath(),
				index);
			if (gDeviceList->FindDevice(path) == NULL)
				break;
		}
		handler->SetPublishPath(strdup(path));
		gDeviceList->AddDevice(handler->PublishPath(), handler);
		if (pathCount < B_COUNT_OF(paths))
			paths[pathCount++] = strdup(path);
	}
	cookie->device = device;
	mutex_unlock(&sDriverLock);

	for (uint32 i = 0; i < pathCount; i++) {
		if (paths[i] == NULL)
			continue;
		status_t published = devfs_publish_device(paths[i], &sDeviceHooks);
		TRACE_ALWAYS("%s: /dev/%s%s\n", device->Name(), paths[i],
			published == B_OK ? "" : " could not be published");
		free(paths[i]);
	}

	return B_OK;
}


// #pragma mark - HID device hooks


static status_t
device_open(const char* name, uint32 flags, void** _cookie)
{
	device_cookie* cookie = new(std::nothrow) device_cookie();
	if (cookie == NULL)
		return B_NO_MEMORY;

	MutexLocker locker(sDriverLock);
	ProtocolHandler* handler = (ProtocolHandler*)gDeviceList->FindDevice(name);
	if (handler == NULL) {
		delete cookie;
		return B_ENTRY_NOT_FOUND;
	}

	cookie->handler = handler;
	cookie->cookie = 0;
	status_t status = handler->Open(flags, &cookie->cookie);
	if (status != B_OK) {
		delete cookie;
		return status;
	}

	handler->Device()->AcquireCookie();
	*_cookie = cookie;
	return B_OK;
}


static status_t
device_close(void* _cookie)
{
	device_cookie* cookie = (device_cookie*)_cookie;
	return cookie->handler->Close(&cookie->cookie);
}


static status_t
device_free(void* _cookie)
{
	device_cookie* cookie = (device_cookie*)_cookie;

	mutex_lock(&sDriverLock);
	HIDDevice* device = cookie->handler->Device();
	bool deleteNow = device->ReleaseCookie() && device->IsRemoved();
	mutex_unlock(&sDriverLock);

	// The control device was closed while this was open: the last one out
	// deletes the device.
	if (deleteNow)
		delete device;

	delete cookie;
	return B_OK;
}


static status_t
device_read(void* _cookie, off_t position, void* buffer, size_t* _length)
{
	device_cookie* cookie = (device_cookie*)_cookie;
	return cookie->handler->Read(&cookie->cookie, position, buffer, _length);
}


static status_t
device_write(void* _cookie, off_t position, const void* buffer,
	size_t* _length)
{
	device_cookie* cookie = (device_cookie*)_cookie;
	return cookie->handler->Write(&cookie->cookie, position, buffer, _length);
}


static status_t
device_control(void* _cookie, uint32 op, void* buffer, size_t length)
{
	device_cookie* cookie = (device_cookie*)_cookie;
	return cookie->handler->Control(&cookie->cookie, op, buffer, length);
}


// #pragma mark - control device hooks


static status_t
control_open(const char* name, uint32 flags, void** _cookie)
{
	control_cookie* cookie = new(std::nothrow) control_cookie;
	if (cookie == NULL)
		return B_NO_MEMORY;

	cookie->device = NULL;
	*_cookie = cookie;
	return B_OK;
}


static status_t
control_close(void* _cookie)
{
	control_cookie* cookie = (control_cookie*)_cookie;
	if (cookie->device != NULL) {
		// Wakes a thread blocked in read(); the device goes in free.
		cookie->device->Removed();
	}
	return B_OK;
}


static status_t
control_free(void* _cookie)
{
	control_cookie* cookie = (control_cookie*)_cookie;
	if (cookie->device != NULL)
		remove_device(cookie->device);
	delete cookie;
	return B_OK;
}


static status_t
control_read(void* _cookie, off_t position, void* buffer, size_t* _length)
{
	control_cookie* cookie = (control_cookie*)_cookie;
	if (cookie->device == NULL)
		return B_NO_INIT;

	uint8 output[sizeof(bluetooth_hid_output) + BLUETOOTH_HID_MAX_REPORT];
	size_t length = sizeof(output);
	status_t status = cookie->device->ReadOutputReport(output, &length);
	if (status != B_OK) {
		*_length = 0;
		return status;
	}
	if (*_length < length) {
		*_length = 0;
		return B_BUFFER_OVERFLOW;
	}

	if (IS_USER_ADDRESS(buffer)) {
		if (user_memcpy(buffer, output, length) != B_OK)
			return B_BAD_ADDRESS;
	} else
		memcpy(buffer, output, length);
	*_length = length;
	return B_OK;
}


static status_t
control_write(void* _cookie, off_t position, const void* buffer,
	size_t* _length)
{
	control_cookie* cookie = (control_cookie*)_cookie;
	if (cookie->device == NULL)
		return B_NO_INIT;

	size_t length = *_length;
	if (length == 0 || length > BLUETOOTH_HID_MAX_REPORT) {
		*_length = 0;
		return B_BAD_VALUE;
	}

	uint8 report[BLUETOOTH_HID_MAX_REPORT];
	if (IS_USER_ADDRESS(buffer)) {
		if (user_memcpy(report, buffer, length) != B_OK)
			return B_BAD_ADDRESS;
	} else
		memcpy(report, buffer, length);

	status_t status = cookie->device->InputReport(report, length);
	if (status != B_OK)
		*_length = 0;
	return status;
}


static status_t
control_control(void* _cookie, uint32 op, void* buffer, size_t length)
{
	control_cookie* cookie = (control_cookie*)_cookie;
	switch (op) {
		case BLUETOOTH_HID_CREATE:
			return create_device(cookie, buffer, length);
	}
	return B_DEV_INVALID_IOCTL;
}


// #pragma mark - driver hooks


status_t
init_hardware()
{
	return B_OK;
}


status_t
init_driver()
{
	gDeviceList = new(std::nothrow) DeviceList();
	if (gDeviceList == NULL)
		return B_NO_MEMORY;

	mutex_init(&sDriverLock, "bluetooth hid driver");

	sDeviceHooks.open = device_open;
	sDeviceHooks.close = device_close;
	sDeviceHooks.free = device_free;
	sDeviceHooks.control = device_control;
	sDeviceHooks.read = device_read;
	sDeviceHooks.write = device_write;
	return B_OK;
}


void
uninit_driver()
{
	delete gDeviceList;
	gDeviceList = NULL;
	mutex_destroy(&sDriverLock);
}


const char**
publish_devices()
{
	// Only the control device: the HID devices come and go with their
	// connections and are published with devfs_publish_device().
	static const char* sDevices[] = {
		BLUETOOTH_HID_CONTROL_NAME,
		NULL
	};
	return sDevices;
}


device_hooks*
find_device(const char* name)
{
	static device_hooks sControlHooks = {
		control_open,
		control_close,
		control_free,
		control_control,
		control_read,
		control_write,
		NULL,	// select
		NULL	// deselect
	};

	if (strcmp(name, BLUETOOTH_HID_CONTROL_NAME) == 0)
		return &sControlHooks;
	return NULL;
}
