/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <bus/FDT.h>
#include <bus/PCI.h>
#include <bus/PCIInterrupts.h>
#include <arch/generic/msi.h>
#include <driver_settings.h>
#include <KernelExport.h>
#include <AutoDeleterDrivers.h>
#include <AutoDeleterOS.h>
#include <util/AutoLock.h>
#include <new>
#include <string.h>

#include "firmware_profile.h"
#include "intx_profile.h"
#include "msi_profile.h"

using namespace RK3588Firmware;

#define DRIVER_NAME "busses/pci/rk3588_firmware/driver_v1"
#define INTX_MODULE_NAME "busses/pci/rk3588_firmware/intx_v1"

static const uint32 kLegacyMask = 0x1c;

static uint32
ReadAPB(volatile uint8* base, uint32 offset)
{
	uint32 value = *(volatile uint32*)(base + offset);
	memory_full_barrier();
	return value;
}

static void
WriteAPB(volatile uint8* base, uint32 offset, uint32 value)
{
	memory_full_barrier();
	*(volatile uint32*)(base + offset) = value;
	memory_full_barrier();
}

static device_manager_info* sDeviceManager;

struct Controller {
	~Controller()
	{
		if (intxManaged)
			WriteAPB(apb, kLegacyMask, 0x000f0000 | originalIntxMask);
	}

	AreaDeleter rootArea;
	AreaDeleter endpointArea;
	AreaDeleter apbArea;
	volatile uint8* config[2]{};
	volatile uint8* apb = NULL;
	pci_resource_range memory{};
	uint32 intxIRQ = 0;
	uint32 originalIntxMask = 0;
	bool intxManaged = false;
	spinlock intxLock = B_SPINLOCK_INITIALIZER;
};


static bool
HasString(fdt_device_module_info* fdt, fdt_device* device, const char* property,
	const char* value)
{
	int length;
	const char* data = (const char*)fdt->get_prop(device, property, &length);
	while (data != NULL && length > 0) {
		const char* end = (const char*)memchr(data, 0, length);
		if (end == NULL)
			return false;
		if (strcmp(data, value) == 0)
			return true;
		length -= end - data + 1;
		data = end + 1;
	}
	return false;
}


static bool
ProfileEnabled(const PortProfile& port)
{
	void* settings = load_driver_settings("rk3588_pcie");
	if (settings == NULL)
		return false;
	const char* profile = get_driver_parameter(settings, "firmware_profile", "", "");
	bool enabled = ProfileAllowsPort(profile, port);
	unload_driver_settings(settings);
	return enabled;
}


static bool
IntxRequested(const PortProfile& port)
{
	IntxProfile profile;
	if (!FindIntxProfile(port.segment, profile))
		return false;
	void* settings = load_driver_settings("rk3588_pcie");
	if (settings == NULL)
		return false;
	bool enabled = get_driver_boolean_parameter(settings, "legacy_interrupts", false, false);
	unload_driver_settings(settings);
	return enabled;
}


static int
StringIndex(fdt_device_module_info* fdt, fdt_device* device, const char* property,
	const char* value)
{
	int length;
	const char* data = (const char*)fdt->get_prop(device, property, &length);
	for (int index = 0; data != NULL && length > 0; index++) {
		const char* end = (const char*)memchr(data, 0, length);
		if (end == NULL)
			return -1;
		if (strcmp(data, value) == 0)
			return index;
		length -= end - data + 1;
		data = end + 1;
	}
	return -1;
}


static status_t
InitIntx(Controller* controller, device_node* parent, const PortProfile& port)
{
	if (!IntxRequested(port))
		return B_OK;
	IntxProfile profile;
	if (!FindIntxProfile(port.segment, profile))
		return B_NOT_SUPPORTED;
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (sDeviceManager->get_driver(parent, (driver_module_info**)&fdt,
			(void**)&device) != B_OK) {
		return B_NOT_SUPPORTED;
	}
	// Match the root's named parent interrupt. The obsolete child node in this
	// firmware DT has an inconsistent edge flag; Linux also uses the root name.
	if (StringIndex(fdt, device, "reg-names", "apb") != 1
		|| StringIndex(fdt, device, "interrupt-names", "legacy") != 3
		|| fdt->get_prop(device, "interrupts-extended", NULL) != NULL) {
		return B_NOT_SUPPORTED;
	}
	uint64 base, size, irq;
	device_node* gicNode;
	int length;
	const uint32* interrupts = (const uint32*)fdt->get_prop(device, "interrupts", &length);
	if (!fdt->get_reg(device, 1, &base, &size) || interrupts == NULL || length != 80
		|| !fdt->get_interrupt(device, 3, &gicNode, &irq) || gicNode == NULL) {
		return B_NOT_SUPPORTED;
	}
	fdt_device_module_info* gic;
	fdt_device* gicDevice;
	uint64 gicBase, gicSize;
	if (sDeviceManager->get_driver(gicNode, (driver_module_info**)&gic,
			(void**)&gicDevice) != B_OK
		|| !HasString(gic, gicDevice, "compatible", "arm,gic-v3")
		|| !gic->get_reg(gicDevice, 0, &gicBase, &gicSize)) {
		return B_NOT_SUPPORTED;
	}
	const uint32* cells = (const uint32*)gic->get_prop(gicDevice, "#interrupt-cells", &length);
	if (cells == NULL || length != 4)
		return B_NOT_SUPPORTED;
	uint32 specifier[4];
	for (unsigned i = 0; i < 4; i++)
		specifier[i] = B_BENDIAN_TO_HOST_INT32(interrupts[12 + i]);
	if (!IntxResourcesMatch(profile, base, size, specifier,
			B_BENDIAN_TO_HOST_INT32(cells[0]), irq, gicBase)
		|| !ValidIntxEndpoint(1, 0, 0, controller->config[1][PCI_interrupt_pin])) {
		return B_NOT_SUPPORTED;
	}
	controller->apbArea.SetTo(map_physical_memory("RK3588 PCIe INTx", base,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&controller->apb));
	if (controller->apbArea.Get() < B_OK)
		return controller->apbArea.Get();
	uint32 mode = ReadAPB(controller->apb, 0);
	uint32 mask = ReadAPB(controller->apb, kLegacyMask);
	uint32 status = ReadAPB(controller->apb, 8);
	dprintf("rk3588_pcie: segment %u INTx APB %#" B_PRIx64
		" mode %#" B_PRIx32 " mask %#" B_PRIx32
		" status %#" B_PRIx32 " IRQ %" B_PRIu32 "\n",
		port.segment, base, mode, mask, status, profile.irq);
	// EN_LEGACY at 0x194 is reserved in the TRM and reads zero on this board.
	// It has no defined enable-state semantics; do not read or write it.
	// Interrupt gating uses the documented MASK_LEGACY register, as in Linux.
	if ((mode & 0xf0) != 0x40 || mask == UINT32_MAX)
		return B_NOT_SUPPORTED;
	controller->originalIntxMask = mask & 0xf;
	controller->intxManaged = true;
	// Firmware leaves these unmasked. Hold all four receive pins masked until
	// a driver installs its handler; only INTA can subsequently be enabled.
	WriteAPB(controller->apb, kLegacyMask, 0x000f000f);
	if ((ReadAPB(controller->apb, kLegacyMask) & 0xf) != 0xf)
		return B_IO_ERROR;
	controller->intxIRQ = profile.irq;
	return B_OK;
}


static bool
FirmwareIommuDescription(fdt_device_module_info* fdt, fdt_device* device,
	const PortProfile& port)
{
	int length;
	const uint32* map = (const uint32*)fdt->get_prop(device, "iommu-map", &length);
	if (map == NULL)
		return true;
	uint32 requesterBase = port.segment * 0x1000;
	if (length != 16 || B_BENDIAN_TO_HOST_INT32(map[0]) != requesterBase
		|| B_BENDIAN_TO_HOST_INT32(map[2]) != requesterBase
		|| B_BENDIAN_TO_HOST_INT32(map[3]) != 0x1000) {
		return false;
	}
	fdt_bus_module_info* busModule;
	fdt_bus* bus;
	if (sDeviceManager->get_driver(fdt->get_bus(device),
			(driver_module_info**)&busModule, (void**)&bus) != B_OK) {
		return false;
	}
	device_node* iommu = busModule->node_by_phandle(bus, B_BENDIAN_TO_HOST_INT32(map[1]));
	fdt_device_module_info* iommuModule;
	fdt_device* iommuDevice;
	if (iommu == NULL || sDeviceManager->get_driver(iommu,
			(driver_module_info**)&iommuModule, (void**)&iommuDevice) != B_OK) {
		return false;
	}
	uint64 base, size;
	// This describes Linux's future configuration, not the firmware DMA map.
	// EDK2 v1.1 uses NonCoherentIoMmuDxe/NonCoherentDmaLib with offset 0;
	// Haiku has no SMMUv3 driver that changes that handoff. The explicit profile
	// retains it, just as it retains the firmware's non-DT PCI config/ranges.
	return HasString(iommuModule, iommuDevice, "compatible", "arm,smmu-v3")
		&& iommuModule->get_reg(iommuDevice, 0, &base, &size)
		&& base == 0xfc900000 && size == 0x200000;
}


static bool
MatchesNode(device_node* node, const PortProfile** matchedPort)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(node, B_DEVICE_BUS, &bus, false) != B_OK
		|| strcmp(bus, "fdt") != 0) {
		return false;
	}
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (sDeviceManager->get_driver(node, (driver_module_info**)&fdt,
			(void**)&device) != B_OK
		|| !HasString(fdt, device, "compatible", "rockchip,rk3588-pcie")) {
		return false;
	}
	uint64 base, size;
	if (!fdt->get_reg(device, 0, &base, &size)) {
		dprintf("rk3588_pcie: compatible FDT root has no first register range\n");
		return false;
	}
	if (size != 0x400000) {
		dprintf("rk3588_pcie: unsupported FDT root at %#" B_PRIx64
			" size %#" B_PRIx64 "\n", base, size);
		return false;
	}
	const PortProfile* port = FindPort(base);
	if (port == NULL)
		return false;

	if (!FirmwareIommuDescription(fdt, device, *port)) {
		dprintf("rk3588_pcie: segment %u firmware IOMMU description rejected\n",
			port->segment);
		return false;
	}
	// Require an untranslated parent bus on the exact board. This explicit
	// firmware profile is not a general driver for the Linux DT resources.
	bool board = false;
	bool supported = true;
	device_node* current = node;
	// The caller owns node; get_parent_node() owns subsequent references.
	while (current != NULL) {
		if (sDeviceManager->get_attr_string(current, B_DEVICE_BUS, &bus, false) != B_OK
			|| strcmp(bus, "fdt") != 0) {
			if (current != node)
				sDeviceManager->put_node(current);
			break;
		}
		if (sDeviceManager->get_driver(current, (driver_module_info**)&fdt,
				(void**)&device) != B_OK) {
			if (current != node)
				sDeviceManager->put_node(current);
			return false;
		}
		if (fdt->get_prop(device, "status", NULL) != NULL
			&& !HasString(fdt, device, "status", "okay")
			&& !HasString(fdt, device, "status", "ok")) {
			supported = false;
		}
		const char* unsupported[] = {
			"iommus", "big-endian", "dma-coherent", NULL
		};
		for (unsigned i = 0; unsupported[i] != NULL; i++) {
			if (fdt->get_prop(device, unsupported[i], NULL) != NULL)
				supported = false;
		}
		if (current != node && fdt->get_prop(device, "iommu-map", NULL) != NULL)
			supported = false;
		int length;
		if (fdt->get_prop(device, "dma-ranges", &length) != NULL && length != 0)
			supported = false;
		if (fdt->get_name(device)[0] == 0) {
			board = HasString(fdt, device, "compatible", "radxa,rock-5-itx");
		} else if (current != node) {
			if (fdt->get_prop(device, "ranges", &length) == NULL || length != 0)
				supported = false;
		}
		device_node* next = sDeviceManager->get_parent_node(current);
		if (current != node)
			sDeviceManager->put_node(current);
		current = next;
	}
	if (!supported || !board) {
		dprintf("rk3588_pcie: segment %u FDT ancestry rejected: resources=%s board=%s\n",
			port->segment, supported ? "valid" : "invalid", board ? "match" : "mismatch");
		return false;
	}
	*matchedPort = port;
	return true;
}


static float
SupportsDevice(device_node* parent)
{
	const PortProfile* port;
	if (!MatchesNode(parent, &port))
		return 0.0f;
	if (!ProfileEnabled(*port)) {
		dprintf("rk3588_pcie: segment %u disabled by firmware profile setting\n",
			port->segment);
		return 0.0f;
	}
	return 1.0f;
}


static status_t
RegisterDevice(device_node* parent)
{
	const PortProfile* port;
	if (!MatchesNode(parent, &port))
		return B_NOT_SUPPORTED;
	bool msi = false;
	MsiProfile msiProfile;
	if (FindMsiProfile(port->segment, msiProfile)) {
		fdt_device_module_info* fdt;
		fdt_device* device;
		if (sDeviceManager->get_driver(parent, (driver_module_info**)&fdt,
				(void**)&device) == B_OK) {
			int length;
			const uint32* map = (const uint32*)fdt->get_prop(device, "msi-map", &length);
			uint32 decodedMap[4];
			if (map != NULL && length == 16) {
				for (unsigned i = 0; i < 4; i++)
					decodedMap[i] = B_BENDIAN_TO_HOST_INT32(map[i]);
			}
			if (map != NULL && length == 16
				&& fdt->get_prop(device, "msi-map-mask", NULL) == NULL) {
				fdt_bus_module_info* busModule;
				fdt_bus* bus;
				if (sDeviceManager->get_driver(fdt->get_bus(device),
						(driver_module_info**)&busModule, (void**)&bus) == B_OK) {
					device_node* itsNode = busModule->node_by_phandle(bus,
						B_BENDIAN_TO_HOST_INT32(map[1]));
					fdt_device_module_info* itsModule;
					fdt_device* itsDevice;
					uint64 base, size;
					if (itsNode != NULL && sDeviceManager->get_driver(itsNode,
							(driver_module_info**)&itsModule, (void**)&itsDevice) == B_OK) {
						msi = HasString(itsModule, itsDevice, "compatible", "arm,gic-v3-its")
							&& itsModule->get_reg(itsDevice, 0, &base, &size)
							&& MsiResourcesMatch(msiProfile, decodedMap, length, base, size);
					}
				}
			}
		}
	}
	device_attr attrs[7] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{.string = "RK3588 EDK2 v1.1 PCIe host"} },
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{.string = "bus_managers/pci/root/driver_v1"} },
		// Even an opt-out controller advertises its provider, so consumers never
		// treat a retained firmware interrupt-line byte as a valid GIC route.
		{ B_PCI_INTX_CONTROLLER_MODULE, B_STRING_TYPE, {.string = INTX_MODULE_NAME} }
	};
	if (msi) {
		attrs[3] = { B_PCI_MSI_CONTROLLER_ADDRESS, B_UINT64_TYPE,
			{.ui64 = msiProfile.controller} };
		attrs[4] = { B_PCI_MSI_REQUESTER_BASE, B_UINT32_TYPE,
			{.ui32 = msiProfile.requesterBase} };
		attrs[5] = { B_PCI_MSI_REQUESTER_COUNT, B_UINT32_TYPE,
			{.ui32 = msiProfile.requesterCount} };
	}
	return sDeviceManager->register_node(parent, DRIVER_NAME, attrs, NULL, NULL);
}


static void
ReadSnapshot(volatile uint8* config, uint32* snapshot)
{
	// Device-nGnRnE, individual 32-bit config transactions (never memcpy MMIO).
	for (unsigned i = 0; i < 64; i++)
		snapshot[i] = ((volatile uint32*)config)[i];
	memory_full_barrier();
}


static status_t
WaitForRootLink(volatile uint8* config, uint32* snapshot,
	uint64& memoryBase, uint64& memorySize, const PortProfile& port)
{
	// The captured ASM1164 root can still report training at early attachment.
	// Wait only for that otherwise valid, active link; never retrain or reset it.
	const bigtime_t start = system_time();
	const bigtime_t deadline = start + 1000000;
	const uint32 initialLink = snapshot[0x80 / 4];
	unsigned polls = 0;
	while (!RootMatches(snapshot, memoryBase, memorySize, port)) {
		if (!RootConfigurationMatches(snapshot, memoryBase, memorySize, port)
			|| !RootLinkActive(snapshot)
			|| (snapshot[0x80 / 4] & 0x08000000) == 0) {
			return B_NOT_SUPPORTED;
		}
		if (polls == 0) {
			dprintf("rk3588_pcie: segment %u waiting for firmware link training, "
				"link=%08" B_PRIx32 "\n", port.segment, initialLink);
		}
		bigtime_t remaining = deadline - system_time();
		if (remaining <= 0)
			return B_TIMED_OUT;
		status_t status = snooze(remaining < 1000 ? remaining : 1000);
		if (status != B_OK)
			return status;
		if (system_time() > deadline)
			return B_TIMED_OUT;
		// Check the ID before every full root snapshot, as on the initial read.
		uint32 id = *(volatile uint32*)config;
		if (id != 0x35881d87) {
			memset(snapshot, 0, 64 * sizeof(uint32));
			snapshot[0] = id;
			return B_NOT_SUPPORTED;
		}
		ReadSnapshot(config, snapshot);
		polls++;
	}
	if (polls != 0) {
		dprintf("rk3588_pcie: segment %u firmware link training settled after "
			"%" B_PRIdBIGTIME " us (%u polls), link=%08" B_PRIx32 " -> %08"
			B_PRIx32 "\n", port.segment, system_time() - start, polls,
			initialLink, snapshot[0x80 / 4]);
	}
	return B_OK;
}


static status_t
InitDriver(device_node* node, void** cookie)
{
	DeviceNodePutter<&sDeviceManager> parent(sDeviceManager->get_parent_node(node));
	const PortProfile* port;
	if (!MatchesNode(parent.Get(), &port) || !ProfileEnabled(*port))
		return B_NOT_SUPPORTED;
	ObjectDeleter<Controller> controller(new(std::nothrow) Controller);
	if (!controller.IsSet())
		return B_NO_MEMORY;
	controller->rootArea.SetTo(map_physical_memory("RK3588 root config", port->rootConfig,
		kConfigSize, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&controller->config[0]));
	if (controller->rootArea.Get() < B_OK)
		return controller->rootArea.Get();
	uint32 snapshot[64];
	uint64 memoryBase, memorySize;
	// Check the ID before reading the rest of the root configuration.
	if (*(volatile uint32*)controller->config[0] != 0x35881d87)
		return B_NOT_SUPPORTED;
	ReadSnapshot(controller->config[0], snapshot);
	status_t status = WaitForRootLink(controller->config[0], snapshot,
		memoryBase, memorySize, *port);
	if (status != B_OK) {
		dprintf("rk3588_pcie: segment %u firmware root configuration "
			"does not match profile: %" B_PRId32 "\n", port->segment, status);
		// Record the snapshot already read for validation; do not issue further
		// register reads or relax the firmware profile after rejection.
		dprintf("rk3588_pcie: root snapshot id=%08" B_PRIx32
			" command=%08" B_PRIx32 " class=%08" B_PRIx32 " header=%08" B_PRIx32
			" buses=%08" B_PRIx32 " memory=%08" B_PRIx32
			" capability=%08" B_PRIx32 " link=%08" B_PRIx32 "\n",
			snapshot[0], snapshot[1], snapshot[2], snapshot[3], snapshot[6],
			snapshot[8], snapshot[0x70 / 4], snapshot[0x80 / 4]);
		return status;
	}
	controller->endpointArea.SetTo(map_physical_memory("RK3588 endpoint config", port->endpointConfig,
		kConfigSize, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&controller->config[1]));
	if (controller->endpointArea.Get() < B_OK)
		return controller->endpointArea.Get();
	uint32 endpointId = *(volatile uint32*)controller->config[1];
	if (!EndpointIdMatches(endpointId, *port)) {
		dprintf("rk3588_pcie: segment %u endpoint %04" B_PRIx32 ":%04" B_PRIx32
			" is not in the firmware profile\n", port->segment,
			endpointId & 0xffff, endpointId >> 16);
		return B_NOT_SUPPORTED;
	}
	ReadSnapshot(controller->config[1], snapshot);
	if (!EndpointMatches(snapshot, memoryBase, memorySize, *port)) {
		dprintf("rk3588_pcie: segment %u firmware endpoint configuration "
			"does not match profile\n",
			port->segment);
		dprintf("rk3588_pcie: endpoint snapshot id=%08" B_PRIx32
			" command=%08" B_PRIx32 " class=%08" B_PRIx32 " header=%08" B_PRIx32
			" bars=%08" B_PRIx32 ",%08" B_PRIx32 ",%08" B_PRIx32
			",%08" B_PRIx32 ",%08" B_PRIx32 ",%08" B_PRIx32 "\n",
			snapshot[0], snapshot[1], snapshot[2], snapshot[3], snapshot[4],
			snapshot[5], snapshot[6], snapshot[7], snapshot[8], snapshot[9]);
		return B_NOT_SUPPORTED;
	}
	controller->memory.type = B_IO_MEMORY;
	controller->memory.address_type = PCI_address_type_32;
	controller->memory.host_address = memoryBase;
	controller->memory.pci_address = memoryBase;
	controller->memory.size = memorySize;
	status = InitIntx(controller.Get(), parent.Get(), *port);
	if (status != B_OK) {
		dprintf("rk3588_pcie: segment %u INTx profile rejected: %" B_PRId32 "\n",
			port->segment, status);
		return status;
	}
	dprintf("rk3588_pcie: EDK2 v1.1 segment %u, buses 0..1, %s; "
		"MMIO %#" B_PRIx64 "+%#" B_PRIx64 "; retaining firmware PHY/clocks/iATU, "
		"identity noncoherent DMA; INTx IRQ %" B_PRIu32 ", MSI provider %s\n",
		port->segment, port->endpointName, memoryBase, memorySize,
		controller->intxIRQ, msi_supported() ? "available" : "unavailable");
	*cookie = controller.Detach();
	return B_OK;
}


static status_t
ReadConfig(void* cookie, uint8 bus, uint8 device, uint8 function, uint16 offset,
	uint8 size, uint32* value)
{
	*value = 0xffffffff;
	if (!ValidAccess(bus, device, function, offset, size))
		return B_BAD_VALUE;
	volatile uint8* address = ((Controller*)cookie)->config[bus] + offset;
	switch (size) {
		case 1: *value = *address; break;
		case 2: *value = *(volatile uint16*)address; break;
		case 4: *value = *(volatile uint32*)address; break;
	}
	memory_full_barrier();
	return B_OK;
}


static status_t
WriteConfig(void* cookie, uint8 bus, uint8 device, uint8 function, uint16 offset,
	uint8 size, uint32 value)
{
	if (!ValidAccess(bus, device, function, offset, size))
		return B_BAD_VALUE;
	volatile uint8* address = ((Controller*)cookie)->config[bus] + offset;
	memory_full_barrier();
	switch (size) {
		case 1: *address = value; break;
		case 2: *(volatile uint16*)address = value; break;
		case 4: *(volatile uint32*)address = value; break;
	}
	memory_full_barrier();
	return B_OK;
}


static pci_controller_module_info sController = {
	.info = {
		.info = { .name = DRIVER_NAME },
		.supports_device = SupportsDevice,
		.register_device = RegisterDevice,
		.init_driver = InitDriver,
		.uninit_driver = [](void* cookie) { delete (Controller*)cookie; },
	},
	.read_pci_config = ReadConfig,
	.write_pci_config = WriteConfig,
	.get_max_bus_devices = [](void*, int32* count) { *count = 1; return B_OK; },
	.read_pci_irq = [](void*, uint8, uint8, uint8, uint8, uint8*) {
		return B_NOT_SUPPORTED;
	},
	.write_pci_irq = [](void*, uint8, uint8, uint8, uint8, uint8) {
		return B_NOT_SUPPORTED;
	},
	.get_range = [](void* cookie, uint32 index, pci_resource_range* range) {
		// I/O ports and prefetchable windows are not part of these profiles.
		if (index != 0)
			return B_BAD_INDEX;
		*range = ((Controller*)cookie)->memory;
		return B_OK;
	},
	.finalize = [](void*) { return B_OK; }
};

static status_t
GetIntxIRQ(void* cookie, uint8 bus, uint8 device, uint8 function, uint8 pin, uint32* irq)
{
	if (irq == NULL)
		return B_BAD_VALUE;
	*irq = 0;
	Controller* controller = (Controller*)cookie;
	if (!ValidIntxEndpoint(bus, device, function, pin) || controller->intxIRQ == 0)
		return B_NOT_SUPPORTED;
	*irq = controller->intxIRQ;
	return B_OK;
}


static pci_intx_controller_module_info sIntxController = {
	.info = { .name = INTX_MODULE_NAME },
	.get_irq = GetIntxIRQ,
	.set_enabled = [](void* cookie, uint8 bus, uint8 device, uint8 function,
			uint8 pin, bool enabled) {
		uint32 irq;
		status_t status = GetIntxIRQ(cookie, bus, device, function, pin, &irq);
		if (status != B_OK)
			return status;
		Controller* controller = (Controller*)cookie;
		BPrivate::InterruptsSpinLocker locker(controller->intxLock);
		WriteAPB(controller->apb, kLegacyMask, enabled ? 0x00010000 : 0x00010001);
		uint32 mask = ReadAPB(controller->apb, kLegacyMask) & 0xf;
		if (mask != (enabled ? 0xeu : 0xfu)) {
			WriteAPB(controller->apb, kLegacyMask, 0x000f000f);
			return B_IO_ERROR;
		}
		dprintf("rk3588_pcie: INTx IRQ %" B_PRIu32 " %s, receive mask %#" B_PRIx32 "\n",
			irq, enabled ? "enabled" : "disabled", mask);
		return B_OK;
	}
};


_EXPORT module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};

_EXPORT module_info* modules[] = {
	(module_info*)&sController,
	(module_info*)&sIntxController,
	NULL
};
