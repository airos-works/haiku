/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _USB_NCM_DRIVER_H_
#define _USB_NCM_DRIVER_H_


#include <device_manager.h>
#include <Drivers.h>
#include <KernelExport.h>
#include <OS.h>
#include <USB3.h>
#include <usb/USB_cdc.h>

#include <net_buffer.h>
#include <util/kernel_cpp.h>


#define DRIVER_NAME	"usb_ncm"


extern usb_module_info* gUSBModule;
extern device_manager_info* gDeviceManager;
extern net_buffer_module_info* gBufferModule;


#ifdef TRACE_NCM
#	define TRACE(x...)		dprintf(DRIVER_NAME ": " x)
#else
#	define TRACE(x...)
#endif
#define TRACE_ALWAYS(x...)	dprintf(DRIVER_NAME ": " x)


#endif	// _USB_NCM_DRIVER_H_
