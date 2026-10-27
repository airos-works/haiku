/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef USB_FDT_H
#define USB_FDT_H


#include <ByteOrder.h>
#include <bus/FDT.h>
#include <KernelExport.h>

#include <string.h>


/*!	Shared by the FDT attachments of the USB host controller drivers: which
	nodes they accept, and the register, interrupt and DMA resources of one.
	As with EHCI, the firmware's PHY, clock and power configuration is kept.
*/


struct usb_fdt_resources {
	phys_addr_t	register_base;
	size_t		register_size;
	uint32		interrupt;
	bool		dma_coherent;
};


static inline bool
usb_fdt_has_string(fdt_device_module_info* fdt, fdt_device* device,
	const char* property, const char* value)
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


static inline bool
usb_fdt_is_enabled(fdt_device_module_info* fdt, fdt_device* device)
{
	return fdt->get_prop(device, "status", NULL) == NULL
		|| usb_fdt_has_string(fdt, device, "status", "okay")
		|| usb_fdt_has_string(fdt, device, "status", "ok");
}


/*!	Returns the FDT device of \a parent if it is an enabled node that is
	compatible with one of \a compatibles (a NULL terminated list).
*/
static inline bool
usb_fdt_get_device(device_manager_info* manager, device_node* parent,
	const char* const* compatibles, fdt_device_module_info** _fdt,
	fdt_device** _device)
{
#ifndef __aarch64__
	// The noncoherent DMA allocator currently implements ARM64 only.
	return false;
#endif
	const char* bus;
	if (manager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK
		|| strcmp(bus, "fdt") != 0) {
		return false;
	}
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (manager->get_driver(parent, (driver_module_info**)&fdt,
			(void**)&device) != B_OK) {
		return false;
	}
	bool compatible = false;
	for (int32 i = 0; compatibles[i] != NULL && !compatible; i++)
		compatible = usb_fdt_has_string(fdt, device, "compatible", compatibles[i]);
	if (!compatible || !usb_fdt_is_enabled(fdt, device))
		return false;
	if (_fdt != NULL)
		*_fdt = fdt;
	if (_device != NULL)
		*_device = device;
	return true;
}


/*!	Whether the "ranges" of the bus \a device (node \a node) map addresses
	1:1, which is what get_reg() assumes: empty, or every entry with equal
	child and parent addresses, as the A733's
	"ranges = <0x0 0x0 0x0 0x40000000>".
*/
static inline bool
usb_fdt_ranges_are_identity(device_manager_info* manager, device_node* node,
	fdt_device_module_info* fdt, fdt_device* device)
{
	int length;
	const uint32* ranges = (const uint32*)fdt->get_prop(device, "ranges",
		&length);
	if (ranges == NULL)
		return false;
	if (length == 0)
		return true;

	struct Cells {
		static uint32 Read(fdt_device_module_info* fdt, fdt_device* device,
			const char* name, uint32 defaultValue)
		{
			int length;
			const uint32* value = (const uint32*)fdt->get_prop(device, name,
				&length);
			if (value == NULL || length != 4)
				return defaultValue;
			return B_BENDIAN_TO_HOST_INT32(*value);
		}
		static uint64 Address(const uint32* cells, uint32 count)
		{
			uint64 address = 0;
			for (uint32 i = 0; i < count; i++)
				address = (address << 32) | B_BENDIAN_TO_HOST_INT32(cells[i]);
			return address;
		}
	};

	uint32 childCells = Cells::Read(fdt, device, "#address-cells", 2);
	uint32 sizeCells = Cells::Read(fdt, device, "#size-cells", 1);
	uint32 parentCells = 2;
	device_node* parentNode = manager->get_parent_node(node);
	if (parentNode != NULL) {
		const char* bus;
		fdt_device_module_info* parentFDT;
		fdt_device* parentDevice;
		if (manager->get_attr_string(parentNode, B_DEVICE_BUS, &bus, false)
				== B_OK && strcmp(bus, "fdt") == 0
			&& manager->get_driver(parentNode,
				(driver_module_info**)&parentFDT, (void**)&parentDevice)
				== B_OK) {
			parentCells = Cells::Read(parentFDT, parentDevice,
				"#address-cells", 2);
		}
		manager->put_node(parentNode);
	}

	uint32 entryCells = childCells + parentCells + sizeCells;
	if (childCells > 2 || parentCells > 2 || length % (entryCells * 4) != 0)
		return false;
	for (int i = 0; i < length / 4; i += entryCells) {
		if (Cells::Address(ranges + i, childCells)
				!= Cells::Address(ranges + i + childCells, parentCells)) {
			return false;
		}
	}
	return true;
}


/*!	Reads the resources of the controller whose FDT node is \a parent. Only
	what the drivers can honour is accepted: one level-triggered SPI routed to
	the GICv3, and identity-mapped buses without an IOMMU.
*/
static inline status_t
usb_fdt_get_resources(device_manager_info* manager, device_node* parent,
	const char* driverName, usb_fdt_resources* resources)
{
	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = manager->get_driver(parent, (driver_module_info**)&fdt,
		(void**)&device);
	if (status != B_OK)
		return status;

	const char* name = fdt->get_name(device);
	*resources = usb_fdt_resources();
	uint64 base, size, irq;
	device_node* interruptController = NULL;
	bool supported = fdt->get_reg(device, 0, &base, &size)
		&& fdt->get_interrupt(device, 0, &interruptController, &irq)
		&& base <= UINT64_MAX - size && size >= 0x100 && size <= SIZE_MAX
		&& irq >= 32 && irq < 1020;

	// The current ARM64 GIC setup uses level-sensitive SPIs. Require that
	// encoding until the FDT API can propagate trigger/polarity information.
	int irqLength = 0;
	const uint8* spec = (const uint8*)fdt->get_prop(device, "interrupts",
		&irqLength);
	if (spec == NULL || (irqLength != 12 && irqLength != 16)
		|| spec[0] != 0 || spec[1] != 0 || spec[2] != 0 || spec[3] != 0
		|| spec[8] != 0 || spec[9] != 0 || spec[10] != 0 || spec[11] != 4) {
		supported = false;
	}
	// FDT nodes are registered in source order. The controller node may not
	// exist yet, although get_interrupt() can already decode its DT specifier.
	if (interruptController != NULL) {
		fdt_device_module_info* controllerFDT;
		fdt_device* controller;
		if (manager->get_driver(interruptController,
				(driver_module_info**)&controllerFDT, (void**)&controller) != B_OK
			|| !usb_fdt_has_string(controllerFDT, controller, "compatible",
				"arm,gic-v3")) {
			supported = false;
		}
	}
	const char* unsupported[] = {
		"big-endian", "big-endian-regs", "big-endian-desc", "iommus",
		"iommu-map", NULL
	};
	for (size_t i = 0; unsupported[i] != NULL; i++) {
		if (fdt->get_prop(device, unsupported[i], NULL) != NULL)
			supported = false;
	}

	// get_reg() currently returns bus addresses without translating ranges.
	// Accept identity buses only, and do not invent a DMA/IOMMU translation.
	// The walk starts at the caller's node, whose reference stays the
	// caller's; the ancestors' references are released here.
	device_node* current = parent;
	bool owned = false;
	while (current != NULL) {
		const char* bus;
		if (manager->get_attr_string(current, B_DEVICE_BUS, &bus, false) != B_OK
			|| strcmp(bus, "fdt") != 0) {
			break;
		}
		fdt_device_module_info* ancestorFDT;
		fdt_device* ancestor;
		if (manager->get_driver(current, (driver_module_info**)&ancestorFDT,
				(void**)&ancestor) != B_OK) {
			supported = false;
			break;
		}
		int length;
		const void* ranges = ancestorFDT->get_prop(ancestor, "dma-ranges",
			&length);
		if (ranges != NULL && length != 0)
			supported = false;
		if (ancestorFDT->get_prop(ancestor, "dma-coherent", NULL) != NULL)
			resources->dma_coherent = true;
		if (!usb_fdt_is_enabled(ancestorFDT, ancestor))
			supported = false;
		if (current != parent && ancestorFDT->get_name(ancestor)[0] != 0
			&& !usb_fdt_ranges_are_identity(manager, current, ancestorFDT,
				ancestor)) {
			supported = false;
		}
		device_node* next = manager->get_parent_node(current);
		if (owned)
			manager->put_node(current);
		current = next;
		owned = true;
	}
	if (owned && current != NULL)
		manager->put_node(current);
	if (!supported) {
		dprintf("%s: unsupported FDT resources for %s\n", driverName, name);
		return B_NOT_SUPPORTED;
	}
	resources->register_base = base;
	resources->register_size = size;
	resources->interrupt = irq;
	return B_OK;
}


#endif	// USB_FDT_H
