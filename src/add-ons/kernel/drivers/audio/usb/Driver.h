/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Distributed under the terms of the MIT license.
 *
 */
#ifndef _USB_AUDIO_DRIVER_H_
#define _USB_AUDIO_DRIVER_H_


#include <Drivers.h>
#include <USB3.h>


#define DRIVER_NAME	"usb_audio"
#define MAX_DEVICES	8

const char* const kVersion = "ver.0.0.5";

// initial buffer size in samples
const uint32 kSamplesBufferSize = 2048;
// [sub]buffers count
const uint32 kSamplesBufferCount = 2;
// most row pointers B_MULTI_GET_BUFFERS takes from a caller (multi_audio
// asks for its MAX_BUFFERS, 32)
const int32 kMaxRequestBuffers = 32;
// most mix values or controls one mixer ioctl takes from a caller
const int32 kMaxMixItems = 4096;


extern usb_module_info* gUSBModule;

extern "C" status_t usb_audio_device_added(usb_device device, void** cookie);
extern "C" status_t usb_audio_device_removed(void* cookie);

extern "C" status_t init_hardware();
extern "C" void uninit_driver();

extern "C" const char** publish_devices();
extern "C" device_hooks *find_device(const char* name);


#endif // _USB_AUDIO_DRIVER_H_

