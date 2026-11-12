/*
 * Copyright 2022, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */


#include "ECAMPCIController.h"

#include <AutoDeleterDrivers.h>

#include <string.h>

extern "C" {
#include <libfdt.h>
}

extern void* gFDT;


static uint32
GetCell(const void* data, uint32 index)
{
	return B_BENDIAN_TO_HOST_INT32(((const uint32*)data)[index]);
}


/*!	The MSI controller of the host bridge, from its "msi-map" (QEMU's virt
	machine: <0 &its 0 0x10000>): a GICv3 ITS, whose DeviceID for a function
	is its-base + RID. Fills three attributes, or none.
*/
/*static*/ void
ECAMPCIControllerFDT::GetMSIAttributes(device_node* fdtNode, device_attr* attrs)
{
	fdt_device_module_info* fdtModule;
	fdt_device* fdtDev;
	if (gFDT == NULL || gDeviceManager->get_driver(fdtNode,
			(driver_module_info**)&fdtModule, (void**)&fdtDev) != B_OK) {
		return;
	}
	int length;
	const void* map = fdtModule->get_prop(fdtDev, "msi-map", &length);
	if (map == NULL || length != 16 || GetCell(map, 0) != 0
		|| fdtModule->get_prop(fdtDev, "msi-map-mask", NULL) != NULL) {
		return;
	}
	int its = fdt_node_offset_by_phandle(gFDT, GetCell(map, 1));
	if (its < 0 || fdt_node_check_compatible(gFDT, its, "arm,gic-v3-its") != 0)
		return;
	int regLength;
	const uint32* reg = (const uint32*)fdt_getprop(gFDT, its, "reg", &regLength);
	int gic = fdt_parent_offset(gFDT, its);
	const uint32* cells = gic >= 0
		? (const uint32*)fdt_getprop(gFDT, gic, "#address-cells", NULL) : NULL;
	uint32 addressCells = cells != NULL ? fdt32_to_cpu(*cells) : 2;
	if (reg == NULL || (addressCells != 1 && addressCells != 2)
		|| regLength < (int)(addressCells * 4)) {
		return;
	}
	uint64 itsBase = addressCells == 2
		? ((uint64)fdt32_to_cpu(reg[0]) << 32) | fdt32_to_cpu(reg[1])
		: fdt32_to_cpu(reg[0]);
	attrs[0] = { B_PCI_MSI_CONTROLLER_ADDRESS, B_UINT64_TYPE, {.ui64 = itsBase} };
	attrs[1] = { B_PCI_MSI_REQUESTER_BASE, B_UINT32_TYPE, {.ui32 = GetCell(map, 2)} };
	attrs[2] = { B_PCI_MSI_REQUESTER_COUNT, B_UINT32_TYPE, {.ui32 = GetCell(map, 3)} };
	dprintf("  MSIs: GICv3 ITS %#" B_PRIx64 ", DeviceID %#" B_PRIx32 " + RID\n",
		itsBase, GetCell(map, 2));
}


status_t
ECAMPCIControllerFDT::ReadResourceInfo()
{
	DeviceNodePutter<&gDeviceManager> fdtNode(gDeviceManager->get_parent_node(fNode));

	fdt_device_module_info *fdtModule;
	fdt_device* fdtDev;
	CHECK_RET(gDeviceManager->get_driver(fdtNode.Get(),
		(driver_module_info**)&fdtModule, (void**)&fdtDev));

	const void* prop;
	int propLen;

	prop = fdtModule->get_prop(fdtDev, "bus-range", &propLen);
	if (prop != NULL && propLen == 8) {
		uint32 busBeg = B_BENDIAN_TO_HOST_INT32(*((uint32*)prop + 0));
		uint32 busEnd = B_BENDIAN_TO_HOST_INT32(*((uint32*)prop + 1));
		dprintf("  bus-range: %" B_PRIu32 " - %" B_PRIu32 "\n", busBeg, busEnd);
	}

	prop = fdtModule->get_prop(fdtDev, "ranges", &propLen);
	if (prop == NULL) {
		dprintf("  \"ranges\" property not found");
		return B_ERROR;
	}
	dprintf("  ranges:\n");
	for (uint32_t *it = (uint32_t*)prop; (uint8_t*)it - (uint8_t*)prop < propLen; it += 7) {
		dprintf("    ");
		uint32_t type      = B_BENDIAN_TO_HOST_INT32(*(it + 0));
		uint64_t childAdr  = B_BENDIAN_TO_HOST_INT64(*(uint64_t*)(it + 1));
		uint64_t parentAdr = B_BENDIAN_TO_HOST_INT64(*(uint64_t*)(it + 3));
		uint64_t len       = B_BENDIAN_TO_HOST_INT64(*(uint64_t*)(it + 5));

		pci_resource_range range = {};
		range.host_address = parentAdr;
		range.pci_address = childAdr;
		range.size = len;

		if ((type & fdtPciRangePrefechable) != 0)
			range.address_type |= PCI_address_prefetchable;

		switch (type & fdtPciRangeTypeMask) {
		case fdtPciRangeIoPort:
			range.type = B_IO_PORT;
			fResourceRanges.Add(range);
			break;
		case fdtPciRangeMmio32Bit:
			range.type = B_IO_MEMORY;
			range.address_type |= PCI_address_type_32;
			fResourceRanges.Add(range);
			break;
		case fdtPciRangeMmio64Bit:
			range.type = B_IO_MEMORY;
			range.address_type |= PCI_address_type_64;
			fResourceRanges.Add(range);
			break;
		}

		switch (type & fdtPciRangeTypeMask) {
		case fdtPciRangeConfig:    dprintf("CONFIG"); break;
		case fdtPciRangeIoPort:    dprintf("IOPORT"); break;
		case fdtPciRangeMmio32Bit: dprintf("MMIO32"); break;
		case fdtPciRangeMmio64Bit: dprintf("MMIO64"); break;
		}

		dprintf(" (0x%08" B_PRIx32 "): ", type);
		dprintf("child: %08" B_PRIx64, childAdr);
		dprintf(", parent: %08" B_PRIx64, parentAdr);
		dprintf(", len: %" B_PRIx64 "\n", len);
	}

	uint64 regs = 0;
	if (!fdtModule->get_reg(fdtDev, 0, &regs, &fRegsLen))
		return B_ERROR;

	fRegsArea.SetTo(map_physical_memory("PCI Config MMIO", regs, fRegsLen, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fRegs));
	CHECK_RET(fRegsArea.Get());

	return B_OK;
}


status_t
ECAMPCIControllerFDT::Finalize()
{
	dprintf("finalize PCI controller from FDT\n");

	DeviceNodePutter<&gDeviceManager> parent(gDeviceManager->get_parent_node(fNode));

	fdt_device_module_info* parentModule;
	fdt_device* parentDev;

	CHECK_RET(gDeviceManager->get_driver(parent.Get(), (driver_module_info**)&parentModule,
		(void**)&parentDev));

	struct fdt_interrupt_map* interruptMap = parentModule->get_interrupt_map(parentDev);
	parentModule->print_interrupt_map(interruptMap);

	for (int bus = 0; bus < 8; bus++) {
		// TODO: Proper multiple domain handling. (domain, bus) pair should be converted to virtual
		// bus before calling PCI module interface.
		for (int device = 0; device < 32; device++) {
			uint32 vendorID = gPCI->read_pci_config(bus, device, 0, PCI_vendor_id, 2);
			if ((vendorID != 0xffffffff) && (vendorID != 0xffff)) {
				uint32 headerType = gPCI->read_pci_config(bus, device, 0, PCI_header_type, 1);
				if ((headerType & 0x80) != 0) {
					for (int function = 0; function < 8; function++) {
						FinalizeInterrupts(parentModule, interruptMap, bus, device, function);
					}
				} else {
					FinalizeInterrupts(parentModule, interruptMap, bus, device, 0);
				}
			}
		}
	}

	return B_OK;
}


void
ECAMPCIControllerFDT::FinalizeInterrupts(fdt_device_module_info* fdtModule,
	struct fdt_interrupt_map* interruptMap, int bus, int device, int function)
{
	uint32 childAddr = ((bus & 0xff) << 16) | ((device & 0x1f) << 11) | ((function & 0x07) << 8);
	uint32 interruptPin = gPCI->read_pci_config(bus, device, function, PCI_interrupt_pin, 1);

	if (interruptPin == 0xffffffff) {
		dprintf("Error: Unable to read interrupt pin!\n");
		return;
	}

	uint32 irq = fdtModule->lookup_interrupt_map(interruptMap, childAddr, interruptPin);
	if (irq == 0xffffffff) {
		dprintf("no interrupt mapping for childAddr: (%d:%d:%d), childIrq: %d)\n",
			bus, device, function, interruptPin);
	} else {
		dprintf("configure interrupt (%d,%d,%d) --> %d\n",
			bus, device, function, irq);
		gPCI->update_interrupt_line(bus, device, function, irq);
	}
}
