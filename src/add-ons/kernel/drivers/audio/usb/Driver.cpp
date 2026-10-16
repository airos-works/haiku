/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Distributed under the terms of the MIT license.
 *
 */

#include "Driver.h"

#include <stdio.h>

#include <AutoLock.h>
#include <usb/USB_audio.h>

#include "Device.h"
#include "Settings.h"


static const char* sDeviceBaseName = "audio/hmulti/usb/";

usb_module_info* gUSBModule = NULL;

Device* gDevices[MAX_DEVICES];
char* gDeviceNames[MAX_DEVICES + 1];

mutex gDriverLock;
int32 api_version = B_CUR_DRIVER_API_VERSION;


status_t
usb_audio_device_added(usb_device device, void** cookie)
{
	*cookie = NULL;

	MutexLocker _(gDriverLock);

	// A device plugged in again is a new one, with a name of its own: one
	// that was unplugged while it was open stays until its last close (and
	// setting it up again on top of what it had doubled its streams).
	Device* audioDevice = new(std::nothrow) Device(device);
	if (audioDevice == 0)
		return ENODEV;

	status_t status = audioDevice->InitCheck();
	if (status < B_OK) {
		delete audioDevice;
		return status;
	}

	status = audioDevice->SetupDevice(false);
	if (status < B_OK) {
		delete audioDevice;
		return status;
	}

	for (int32 i = 0; i < MAX_DEVICES; i++) {
		if (gDevices[i] != NULL)
			continue;

		gDevices[i] = audioDevice;
		*cookie = audioDevice;

		TRACE(INF, "New device is added at %ld.\n", i);
		return B_OK;
	}

	// no space for the device
	TRACE(ERR, "Error: no more device entries availble.\n");

	delete audioDevice;
	return B_ERROR;
}


status_t
usb_audio_device_removed(void* cookie)
{
	MutexLocker _(gDriverLock);

	Device* device = (Device*)cookie;
	for (int32 i = 0; i < MAX_DEVICES; i++) {
		if (gDevices[i] == device) {
			if (device->IsOpen()) {
				// the device will be deleted upon being freed; it is no
				// longer published, so that its users let it go
				device->Removed();
			} else {
				gDevices[i] = NULL;
				delete device;
				TRACE(INF, "Device at %ld deleted.\n", i);
			}
			break;
		}
	}

	return B_OK;
}


status_t
init_hardware()
{
	return B_OK;
}


status_t
init_driver()
{
	status_t status = get_module(B_USB_MODULE_NAME,
		(module_info**)&gUSBModule);
	if (status < B_OK)
		return status;

	load_settings();

	TRACE(INF, "%s\n", kVersion);

	for (int32 i = 0; i < MAX_DEVICES; i++)
		gDevices[i] = NULL;

	gDeviceNames[0] = NULL;
	mutex_init(&gDriverLock, DRIVER_NAME "_devices");

	static usb_notify_hooks notifyHooks = {
		&usb_audio_device_added,
		&usb_audio_device_removed
	};

	static usb_support_descriptor supportedDevices[] = {
		{ USB_AUDIO_INTERFACE_AUDIO_CLASS, 0, 0, 0, 0 }
	};

	gUSBModule->register_driver(DRIVER_NAME, supportedDevices, 0, NULL);
	gUSBModule->install_notify(DRIVER_NAME, &notifyHooks);
	return B_OK;
}


void
uninit_driver()
{
	gUSBModule->uninstall_notify(DRIVER_NAME);
	mutex_lock(&gDriverLock);

	for (int32 i = 0; i < MAX_DEVICES; i++) {
		if (gDevices[i]) {
			delete gDevices[i];
			gDevices[i] = NULL;
		}
	}

	for (int32 i = 0; gDeviceNames[i]; i++) {
		free(gDeviceNames[i]);
		gDeviceNames[i] = NULL;
	}

	mutex_destroy(&gDriverLock);
	put_module(B_USB_MODULE_NAME);

	release_settings();
}


static status_t
usb_audio_open(const char* name, uint32 flags, void** cookie)
{
	MutexLocker _(gDriverLock);

	*cookie = NULL;
	status_t status = ENODEV;
	for (int32 i = 0; i < MAX_DEVICES; i++) {
		// publish_devices() names the device in slot i "usb/<i + 1>";
		// gDeviceNames[] has no gaps, gDevices[] can have them
		if (gDevices[i] == NULL || gDevices[i]->IsRemoved())
			continue;

		char deviceName[64];
		snprintf(deviceName, sizeof(deviceName), "%s%" B_PRId32,
			sDeviceBaseName, i + 1);
		if (strcmp(deviceName, name) == 0) {
			status = gDevices[i]->Open(flags);
			*cookie = gDevices[i];
			break;
		}
	}

	return status;
}


static status_t
usb_audio_read(void* cookie, off_t position, void* buffer, size_t* numBytes)
{
	Device* device = (Device*)cookie;
	return device->Read((uint8*)buffer, numBytes);
}


static status_t
usb_audio_write(void* cookie, off_t position, const void* buffer,
	size_t* numBytes)
{
	Device* device = (Device*)cookie;
	return device->Write((const uint8*)buffer, numBytes);
}


static status_t
usb_audio_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	Device* device = (Device*)cookie;
	return device->Control(op, buffer, length);
}


static status_t
usb_audio_close(void* cookie)
{
	Device* device = (Device*)cookie;
	return device->Close();
}


static status_t
usb_audio_free(void* cookie)
{
	Device* device = (Device*)cookie;

	MutexLocker _(gDriverLock);
	status_t status = device->Free();
	if (device->IsRemoved()) {
		// Unplugged while it was open: devfs disconnected its user when it
		// was no longer published. That user may still hold its buffers.
		device->OrphanBuffers();
		for (int32 i = 0; i < MAX_DEVICES; i++) {
			if (gDevices[i] == device) {
				gDevices[i] = NULL;
				break;
			}
		}
		delete device;
		TRACE(INF, "Unplugged device freed.\n");
	}
	return status;
}


const char**
publish_devices()
{
	MutexLocker _(gDriverLock);

	for (int32 i = 0; gDeviceNames[i]; i++) {
		free(gDeviceNames[i]);
		gDeviceNames[i] = NULL;
	}

	int32 deviceCount = 0;
	for (size_t i = 0; i < MAX_DEVICES; i++) {
		if (gDevices[i] == NULL || gDevices[i]->IsRemoved())
			continue;

		gDeviceNames[deviceCount] = (char*)malloc(strlen(sDeviceBaseName) + 4);
		if (gDeviceNames[deviceCount]) {
			sprintf(gDeviceNames[deviceCount], "%s%ld", sDeviceBaseName, i + 1);
			TRACE(INF, "publishing %s\n", gDeviceNames[deviceCount]);
			deviceCount++;
		} else
			TRACE(ERR, "Error: out of memory during allocating device name.\n");
	}

	gDeviceNames[deviceCount] = NULL;
	return (const char**)&gDeviceNames[0];
}


device_hooks*
find_device(const char* name)
{
	static device_hooks deviceHooks = {
		usb_audio_open,
		usb_audio_close,
		usb_audio_free,
		usb_audio_control,
		usb_audio_read,
		usb_audio_write,
		NULL,				// select
		NULL				// deselect
	};

	return &deviceHooks;
}

