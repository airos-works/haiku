/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * USB4 and Thunderbolt host interface driver with a software connection
 * manager. Each host interface becomes a domain; /dev/bus/usb4/<n> shows
 * its routers and tunnels and takes commands ("start", "stop", "rescan",
 * "debug <level>").
 */


#include <ACPI.h>
#include <KernelExport.h>
#include <PCI.h>
#include <bus/PCI.h>
#include <device_manager.h>
#include <driver_settings.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <drivers/pci_hotplug.h>

#include "Domain.h"
#include "Nhi.h"
#include "usb4_debug.h"


#define USB4_DRIVER_MODULE_NAME		"busses/usb/usb4/driver_v1"
#define USB4_DEVICE_MODULE_NAME		"busses/usb/usb4/device_v1"

#define DUMP_BUFFER_SIZE			(64 * 1024)


int32 gUSB4DebugLevel = 0;

static device_manager_info* sDeviceManager;
static int32 sNextIndex = 0;
static bool sOSCDone = false;
static uint32 sOSCControl = USB4_CONTROL_ALL;
static bool sOSCNative = false;


struct usb4_device {
	device_node*			node;
	pci_device_module_info*	pci;
	pci_device*				device;
	Nhi*					nhi;
	Domain*					domain;
	int32					index;
	uint32					denied;
		// tunnel kinds turned off with "deny"
};


struct usb4_handle {
	usb4_device*			device;
	char*					text;
	size_t					length;
};


// #pragma mark - ACPI


/*!	Asks the platform for native control of USB4 (ACPI 6.4 "Operating System
	Capabilities for USB"): first the platform wide _OSC with the native
	USB4 bit, then the USB4 _OSC for the kinds of tunnels. Without it the
	firmware keeps control of what it may, and we tunnel everything.
*/
static void
negotiate_usb4_control()
{
	if (sOSCDone)
		return;
	sOSCDone = true;

	acpi_module_info* acpi;
	if (get_module(B_ACPI_MODULE_NAME, (module_info**)&acpi) != B_OK)
		return;

	acpi_handle handle;
	if (acpi->get_handle(NULL, "\\_SB", &handle) != B_OK) {
		put_module(B_ACPI_MODULE_NAME);
		return;
	}

	// 0811B06E-4A27-44F9-8D60-3CBBC22E7B48
	static uint8 platformUUID[16] = {
		0x6e, 0xb0, 0x11, 0x08, 0x27, 0x4a, 0xf9, 0x44,
		0x8d, 0x60, 0x3c, 0xbb, 0xc2, 0x2e, 0x7b, 0x48
	};
	// 23A0D13A-26AB-486C-9C5F-0FFA525A575A
	static uint8 usbUUID[16] = {
		0x3a, 0xd1, 0xa0, 0x23, 0xab, 0x26, 0x6c, 0x48,
		0x9c, 0x5f, 0x0f, 0xfa, 0x52, 0x5a, 0x57, 0x5a
	};
	const uint32 kQuery = 0x1;
	const uint32 kNativeUSB4 = 1u << 18;

	auto runOSC = [&](uint8* uuid, uint32* caps, uint32 count) -> bool {
		acpi_object_type args[4];
		args[0].object_type = ACPI_TYPE_BUFFER;
		args[0].buffer.length = 16;
		args[0].buffer.buffer = uuid;
		args[1].object_type = ACPI_TYPE_INTEGER;
		args[1].integer.integer = 1;
		args[2].object_type = ACPI_TYPE_INTEGER;
		args[2].integer.integer = count;
		args[3].object_type = ACPI_TYPE_BUFFER;
		args[3].buffer.length = count * 4;
		args[3].buffer.buffer = caps;
		acpi_objects objects;
		objects.count = 4;
		objects.pointer = args;

		acpi_data result;
		result.pointer = NULL;
		result.length = ACPI_ALLOCATE_BUFFER;
		if (acpi->evaluate_method(handle, "_OSC", &objects, &result) != B_OK)
			return false;
		acpi_object_type* object = (acpi_object_type*)result.pointer;
		bool ok = object != NULL && object->object_type == ACPI_TYPE_BUFFER
			&& object->buffer.length >= count * 4;
		if (ok)
			memcpy(caps, object->buffer.buffer, count * 4);
		free(result.pointer);
		return ok;
	};

	uint32 platform[2] = { kQuery, kNativeUSB4 };
	bool native = false;
	if (runOSC(platformUUID, platform, 2)) {
		uint32 supported = platform[1];
		platform[0] = 0;
		platform[1] = supported & kNativeUSB4;
		if (platform[1] != 0 && runOSC(platformUUID, platform, 2))
			native = (platform[1] & kNativeUSB4) != 0;
	}

	sOSCNative = native;
	if (!native) {
		TRACE_ALWAYS("_OSC: the platform does not hand over USB4\n");
		put_module(B_ACPI_MODULE_NAME);
		return;
	}

	uint32 usb[3] = { kQuery, 0, USB4_CONTROL_ALL };
	if (runOSC(usbUUID, usb, 3)) {
		uint32 granted = usb[2];
		usb[0] = 0;
		usb[1] = 0;
		usb[2] = granted;
		if (runOSC(usbUUID, usb, 3))
			sOSCControl = USB4_CONTROL_ALL & usb[2];
	}

	TRACE_ALWAYS("_OSC: native USB4 control: USB3 %s, DisplayPort %s, PCIe "
		"%s, host to host %s\n",
		(sOSCControl & USB4_CONTROL_USB3) != 0 ? "yes" : "no",
		(sOSCControl & USB4_CONTROL_DP) != 0 ? "yes" : "no",
		(sOSCControl & USB4_CONTROL_PCIE) != 0 ? "yes" : "no",
		(sOSCControl & USB4_CONTROL_XDOMAIN) != 0 ? "yes" : "no");
	put_module(B_ACPI_MODULE_NAME);
}


// #pragma mark - PCIe hot plug


/*!	When a PCIe tunnel comes up or goes away the PCI bus manager has to find
	what appeared behind (or vanished from) the PCIe port at the host. The
	link needs a moment to train, so look a little later, and again in case
	it took longer.
*/
class PCIeRescanner : public PCIeTunnelListener {
public:
	virtual void PCIeTunnelChanged(Domain* domain, bool up)
	{
		if (atomic_add(&fPending, 1) > 0)
			return;
		thread_id thread = spawn_kernel_thread(_Rescan, "usb4 pci rescan",
			B_NORMAL_PRIORITY, this);
		if (thread < 0) {
			atomic_add(&fPending, -1);
			return;
		}
		resume_thread(thread);
	}

private:
	static status_t _Rescan(void* data)
	{
		PCIeRescanner* self = (PCIeRescanner*)data;
		pci_hotplug_module_info* hotplug;
		if (get_module(B_PCI_HOTPLUG_MODULE_NAME, (module_info**)&hotplug)
				!= B_OK) {
			TRACE_ERROR("no PCI hot plug support\n");
			atomic_set(&self->fPending, 0);
			return B_ERROR;
		}

		static const bigtime_t kDelays[] = { 500000, 1500000, 3000000 };
		for (size_t i = 0; i < B_COUNT_OF(kDelays); i++) {
			snooze(kDelays[i]);
			atomic_set(&self->fPending, 1);
			uint32 added = 0, removed = 0;
			hotplug->rescan(&added, &removed);
			if (added != 0 || removed != 0) {
				TRACE_ALWAYS("PCIe: %" B_PRIu32 " port(s) gained devices, %"
					B_PRIu32 " lost them\n", added, removed);
			}
		}

		put_module(B_PCI_HOTPLUG_MODULE_NAME);
		atomic_set(&self->fPending, 0);
		return B_OK;
	}

	int32	fPending = 0;
};


static PCIeRescanner* sPCIeRescanner = NULL;


// #pragma mark - settings


struct usb4_settings {
	bool	autoStart;
	bool	usb3;
	bool	dp;
	bool	pcie;
	bool	resetHost;
};


static usb4_settings
read_settings()
{
	usb4_settings settings = { false, true, true, true, false };

	void* handle = load_driver_settings("usb4");
	if (handle == NULL)
		return settings;

	settings.autoStart = get_driver_boolean_parameter(handle, "auto_start",
		settings.autoStart, true);
	settings.usb3 = get_driver_boolean_parameter(handle, "usb3_tunneling",
		settings.usb3, true);
	settings.dp = get_driver_boolean_parameter(handle, "dp_tunneling",
		settings.dp, true);
	settings.pcie = get_driver_boolean_parameter(handle, "pcie_tunneling",
		settings.pcie, true);
	settings.resetHost = get_driver_boolean_parameter(handle, "reset_host",
		settings.resetHost, true);
	const char* debug = get_driver_parameter(handle, "debug", NULL, NULL);
	if (debug != NULL)
		gUSB4DebugLevel = strtol(debug, NULL, 0);

	unload_driver_settings(handle);
	return settings;
}


static status_t
start_domain(usb4_device* device)
{
	if (device->domain != NULL)
		return device->domain->IsRunning() ? B_OK : device->domain->Start();

	usb4_settings settings = read_settings();
	negotiate_usb4_control();

	// Intel controllers whose platform keeps control run a firmware
	// connection manager; we must not fight it
	if (!sOSCNative && device->nhi->Info().vendor_id == 0x8086) {
		TRACE_ALWAYS("domain %" B_PRId32 ": the firmware connection manager "
			"is in charge\n", device->index);
		return B_NOT_SUPPORTED;
	}

	uint32 control = sOSCControl;
	if (!settings.usb3)
		control &= ~USB4_CONTROL_USB3;
	if (!settings.dp)
		control &= ~USB4_CONTROL_DP;
	if (!settings.pcie)
		control &= ~USB4_CONTROL_PCIE;
	control &= ~device->denied;

	device->domain = new(std::nothrow) Domain(device->nhi, device->index,
		control);
	if (device->domain == NULL)
		return B_NO_MEMORY;
	device->nhi->SetDomain(device->domain);
	if (sPCIeRescanner == NULL)
		sPCIeRescanner = new(std::nothrow) PCIeRescanner;
	device->domain->SetPCIeListener(sPCIeRescanner);
	device->domain->SetResetHost(settings.resetHost);
	return device->domain->Start();
}


static void
stop_domain(usb4_device* device)
{
	if (device->domain == NULL)
		return;
	device->domain->Stop();
	device->nhi->SetDomain(NULL);
	delete device->domain;
	device->domain = NULL;
}


// #pragma mark - driver module


static float
usb4_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "pci") != 0)
		return -1.0f;

	uint16 type, subType, interface;
	if (sDeviceManager->get_attr_uint16(parent, B_DEVICE_TYPE, &type, false)
			!= B_OK
		|| sDeviceManager->get_attr_uint16(parent, B_DEVICE_SUB_TYPE, &subType,
			false) != B_OK
		|| sDeviceManager->get_attr_uint16(parent, B_DEVICE_INTERFACE,
			&interface, false) != B_OK)
		return -1.0f;

	// USB4 host interface
	if (type == PCI_serial_bus && subType == PCI_usb && interface == 0x40)
		return 0.8f;

	return 0.0f;
}


static status_t
usb4_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "USB4 host interface" }},
		{ NULL }
	};
	return sDeviceManager->register_node(parent, USB4_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
usb4_init_driver(device_node* node, void** _cookie)
{
	usb4_device* device = (usb4_device*)calloc(1, sizeof(usb4_device));
	if (device == NULL)
		return B_NO_MEMORY;

	device_node* parent = sDeviceManager->get_parent_node(node);
	sDeviceManager->get_driver(parent, (driver_module_info**)&device->pci,
		(void**)&device->device);
	sDeviceManager->put_node(parent);

	pci_info info;
	device->pci->get_pci_info(device->device, &info);

	device->node = node;
	device->index = atomic_add(&sNextIndex, 1);
	device->nhi = new(std::nothrow) Nhi(device->pci, device->device, info);
	if (device->nhi == NULL) {
		free(device);
		return B_NO_MEMORY;
	}

	status_t status = device->nhi->Init();
	if (status != B_OK) {
		TRACE_ERROR("host interface %04x:%04x: %s\n", info.vendor_id,
			info.device_id, strerror(status));
		delete device->nhi;
		free(device);
		return status;
	}

	usb4_settings settings = read_settings();
	if (settings.autoStart)
		start_domain(device);
	else {
		TRACE_ALWAYS("domain %" B_PRId32 ": connection manager not started "
			"(auto_start is off; write \"start\" to /dev/bus/usb4/%" B_PRId32
			")\n", device->index, device->index);
	}

	*_cookie = device;
	return B_OK;
}


static void
usb4_uninit_driver(void* cookie)
{
	usb4_device* device = (usb4_device*)cookie;
	stop_domain(device);
	delete device->nhi;
	free(device);
}


static status_t
usb4_register_child_devices(void* cookie)
{
	usb4_device* device = (usb4_device*)cookie;
	char name[64];
	snprintf(name, sizeof(name), "bus/usb4/%" B_PRId32, device->index);
	return sDeviceManager->publish_device(device->node, name,
		USB4_DEVICE_MODULE_NAME);
}


// #pragma mark - device module


static status_t
usb4_init_device(void* driverCookie, void** _deviceCookie)
{
	*_deviceCookie = driverCookie;
	return B_OK;
}


static void
usb4_uninit_device(void* deviceCookie)
{
}


static status_t
usb4_open(void* deviceCookie, const char* path, int openMode, void** _cookie)
{
	usb4_handle* handle = (usb4_handle*)calloc(1, sizeof(usb4_handle));
	if (handle == NULL)
		return B_NO_MEMORY;
	handle->device = (usb4_device*)deviceCookie;
	*_cookie = handle;
	return B_OK;
}


static status_t
usb4_close(void* cookie)
{
	return B_OK;
}


static status_t
usb4_free(void* cookie)
{
	usb4_handle* handle = (usb4_handle*)cookie;
	free(handle->text);
	free(handle);
	return B_OK;
}


static status_t
usb4_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	usb4_handle* handle = (usb4_handle*)cookie;

	if (handle->text == NULL || position == 0) {
		free(handle->text);
		handle->text = (char*)malloc(DUMP_BUFFER_SIZE);
		if (handle->text == NULL)
			return B_NO_MEMORY;

		usb4_device* device = handle->device;
		if (device->domain != NULL)
			handle->length = device->domain->Dump(handle->text,
				DUMP_BUFFER_SIZE);
		else {
			const pci_info& info = device->nhi->Info();
			handle->length = snprintf(handle->text, DUMP_BUFFER_SIZE,
				"domain %" B_PRId32 ": host interface %04x:%04x at %02x:%02x."
				"%x, connection manager not started\n", device->index,
				info.vendor_id, info.device_id, info.bus, info.device,
				info.function);
			handle->length = min_c(handle->length, DUMP_BUFFER_SIZE);
		}
	}

	if (position < 0 || (size_t)position >= handle->length) {
		*_length = 0;
		return B_OK;
	}
	size_t length = min_c(*_length, handle->length - (size_t)position);
	if (user_memcpy(buffer, handle->text + position, length) != B_OK)
		return B_BAD_ADDRESS;
	*_length = length;
	return B_OK;
}


static status_t
usb4_write(void* cookie, off_t position, const void* buffer, size_t* _length)
{
	usb4_handle* handle = (usb4_handle*)cookie;
	usb4_device* device = handle->device;

	char command[64];
	size_t length = min_c(*_length, sizeof(command) - 1);
	if (user_memcpy(command, buffer, length) != B_OK)
		return B_BAD_ADDRESS;
	command[length] = '\0';
	while (length > 0 && (command[length - 1] == '\n'
			|| command[length - 1] == ' '))
		command[--length] = '\0';

	status_t status = B_OK;
	if (strcmp(command, "start") == 0)
		status = start_domain(device);
	else if (strcmp(command, "stop") == 0)
		stop_domain(device);
	else if (strcmp(command, "rescan") == 0) {
		if (device->domain != NULL)
			device->domain->QueueRescan();
	} else if (strncmp(command, "allow ", 6) == 0
		|| strncmp(command, "deny ", 5) == 0) {
		bool allow = command[0] == 'a';
		const char* what = command + (allow ? 6 : 5);
		uint32 bits = strcmp(what, "usb3") == 0 ? USB4_CONTROL_USB3
			: strcmp(what, "dp") == 0 ? USB4_CONTROL_DP
			: strcmp(what, "pcie") == 0 ? USB4_CONTROL_PCIE
			: strcmp(what, "all") == 0 ? USB4_CONTROL_USB3 | USB4_CONTROL_DP
				| USB4_CONTROL_PCIE : 0;
		if (bits == 0)
			return B_BAD_VALUE;
		if (allow)
			device->denied &= ~bits;
		else
			device->denied |= bits;
		if (device->domain != NULL) {
			uint32 control = device->domain->Control();
			if (allow)
				control |= bits & sOSCControl;
			else
				control &= ~bits;
			device->domain->SetControl(control);
		}
	} else if (strncmp(command, "debug ", 6) == 0)
		gUSB4DebugLevel = strtol(command + 6, NULL, 0);
	else
		status = B_BAD_VALUE;

	if (status != B_OK)
		return status;
	return B_OK;
}


static status_t
usb4_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	return B_DEV_INVALID_IOCTL;
}


static driver_module_info sUSB4Driver = {
	{
		USB4_DRIVER_MODULE_NAME,
		0,
		NULL
	},

	usb4_supports_device,
	usb4_register_device,
	usb4_init_driver,
	usb4_uninit_driver,
	usb4_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};


static device_module_info sUSB4Device = {
	{
		USB4_DEVICE_MODULE_NAME,
		0,
		NULL
	},

	usb4_init_device,
	usb4_uninit_device,
	NULL,	// removed

	usb4_open,
	usb4_close,
	usb4_free,
	usb4_read,
	usb4_write,
	NULL,	// io
	usb4_control,
	NULL,	// select
	NULL	// deselect
};


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


module_info* modules[] = {
	(module_info*)&sUSB4Driver,
	(module_info*)&sUSB4Device,
	NULL
};
