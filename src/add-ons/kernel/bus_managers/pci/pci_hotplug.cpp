/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * PCI Express hot plug: devices appearing behind, or vanishing from, hot
 * plug capable root and switch downstream ports. The firmware leaves bus
 * numbers and address windows on such ports for whatever gets plugged in;
 * we number the bridges found behind them, size and assign their windows
 * and BARs out of that space and publish the new devices.
 */


#include <string.h>

#include <lock.h>
#include <util/AutoLock.h>

#include <drivers/pci_hotplug.h>

#include "pci.h"
#include "pci_private.h"


#define PCIE_CAPABILITIES		0x02
#define PCIE_LINK_CAPABILITIES	0x0c
#define PCIE_LINK_STATUS		0x12
#define PCIE_SLOT_CAPABILITIES	0x14
#define PCIE_SLOT_STATUS		0x1a

#define PCIE_TYPE_ROOT_PORT		0x4
#define PCIE_TYPE_DOWNSTREAM	0x6

#define HOTPLUG_MEMORY_SLACK	(2 * 1024 * 1024)
	// what a hot plug port below another one gets for its own devices

#define MB						(1024 * 1024)


static mutex sHotplugLock = MUTEX_INITIALIZER("pci hotplug");


static uint64
round_up_power_of_two(uint64 value)
{
	if (value == 0)
		return 0;
	uint64 result = 1;
	while (result < value)
		result <<= 1;
	return result;
}


static uint64
round_up(uint64 value, uint64 alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}


static bool
is_pci_bridge(PCIDev* dev)
{
	return dev->info.class_base == PCI_bridge
		&& dev->info.class_sub == PCI_pci
		&& (dev->info.header_type & PCI_header_type_mask)
			== PCI_header_type_PCI_to_PCI_bridge;
}


bool
PCI::_IsHotplugPort(PCIDev* dev, uint8* _capability)
{
	if (dev->child == NULL || !is_pci_bridge(dev))
		return false;

	uint8 capability;
	if (FindCapability(dev, PCI_cap_id_pcie, &capability) != B_OK)
		return false;

	uint16 flags = ReadConfig(dev, capability + PCIE_CAPABILITIES, 2);
	uint8 type = (flags >> 4) & 0xf;
	if (type != PCIE_TYPE_ROOT_PORT && type != PCIE_TYPE_DOWNSTREAM)
		return false;
	if ((flags & (1 << 8)) == 0)
		return false;

	uint32 slot = ReadConfig(dev, capability + PCIE_SLOT_CAPABILITIES, 4);
	if ((slot & (1 << 6)) == 0)
		return false;

	if (_capability != NULL)
		*_capability = capability;
	return true;
}


bool
PCI::_IsPortLinkUp(PCIDev* dev, uint8 capability)
{
	uint32 linkCapabilities = ReadConfig(dev,
		capability + PCIE_LINK_CAPABILITIES, 4);
	if ((linkCapabilities & (1 << 20)) != 0) {
		// data link layer link active
		return (ReadConfig(dev, capability + PCIE_LINK_STATUS, 2)
			& (1 << 13)) != 0;
	}
	// presence detect
	return (ReadConfig(dev, capability + PCIE_SLOT_STATUS, 2) & (1 << 6))
		!= 0;
}


/*!	Gives every bridge below \a bus numbers out of what is left up to
	\a limit, depth first like the firmware does.
*/
void
PCI::_HotplugNumberBuses(uint8 domain, uint8 bus, uint8& last, uint8 limit)
{
	int maxBusDevices = _GetDomainData(domain)->max_bus_devices;
	for (int dev = 0; dev < maxBusDevices; dev++) {
		if (ReadConfig(domain, bus, dev, 0, PCI_vendor_id, 2) == 0xffff)
			continue;

		int functions = _NumFunctions(domain, bus, dev);
		for (int function = 0; function < functions; function++) {
			if (ReadConfig(domain, bus, dev, function, PCI_device_id, 2)
					== 0xffff)
				continue;
			if (ReadConfig(domain, bus, dev, function, PCI_class_base, 1)
					!= PCI_bridge
				|| ReadConfig(domain, bus, dev, function, PCI_class_sub, 1)
					!= PCI_pci
				|| (ReadConfig(domain, bus, dev, function, PCI_header_type, 1)
					& PCI_header_type_mask)
						!= PCI_header_type_PCI_to_PCI_bridge)
				continue;

			// no decoding while the numbers change
			uint16 command = ReadConfig(domain, bus, dev, function,
				PCI_command, 2);
			WriteConfig(domain, bus, dev, function, PCI_command, 2,
				command & ~(PCI_command_io | PCI_command_memory
					| PCI_command_master));

			if (last >= limit) {
				dprintf("PCI: hot plug: no bus number left for the bridge at "
					"%02x:%02x.%x\n", bus, dev, function);
				WriteConfig(domain, bus, dev, function, PCI_primary_bus, 1,
					bus);
				WriteConfig(domain, bus, dev, function, PCI_secondary_bus, 1,
					0);
				WriteConfig(domain, bus, dev, function, PCI_subordinate_bus, 1,
					0);
				continue;
			}

			uint8 secondary = ++last;
			WriteConfig(domain, bus, dev, function, PCI_primary_bus, 1, bus);
			WriteConfig(domain, bus, dev, function, PCI_secondary_bus, 1,
				secondary);
			WriteConfig(domain, bus, dev, function, PCI_subordinate_bus, 1,
				limit);

			_HotplugNumberBuses(domain, secondary, last, limit);

			WriteConfig(domain, bus, dev, function, PCI_subordinate_bus, 1,
				last);
		}
	}
}


void
PCI::_HotplugDiscoverBus(PCIBus* bus)
{
	int maxBusDevices = _GetDomainData(bus->domain)->max_bus_devices;
	for (int dev = 0; dev < maxBusDevices; dev++) {
		if (ReadConfig(bus->domain, bus->bus, dev, 0, PCI_vendor_id, 2)
				== 0xffff)
			continue;

		int functions = _NumFunctions(bus->domain, bus->bus, dev);
		for (int function = 0; function < functions; function++) {
			if (ReadConfig(bus->domain, bus->bus, dev, function,
					PCI_device_id, 2) == 0xffff)
				continue;
			PCIDev* newDev = _CreateDevice(bus, dev, function);
			if (newDev == NULL)
				continue;
			memset(newDev->window, 0, sizeof(newDev->window));
			memset(newDev->bar, 0, sizeof(newDev->bar));

			if (!is_pci_bridge(newDev))
				continue;
			uint8 secondary = ReadConfig(bus->domain, bus->bus, dev, function,
				PCI_secondary_bus, 1);
			if (secondary == 0 || secondary <= bus->bus)
				continue;
			PCIBus* newBus = _CreateBus(newDev, bus->domain, secondary);
			if (newBus != NULL)
				_HotplugDiscoverBus(newBus);
		}
	}
}


struct bus_needs {
	uint64	memory;
	uint64	prefetchable;
	uint64	io;
};


/*!	What the devices on \a bus need of each kind of address space. Bridge
	windows are sized to powers of two so that, allocated largest first, all
	items of a bus pack without gaps.
*/
void
PCI::_HotplugSizeBus(PCIBus* bus, bus_needs& needs)
{
	needs.memory = needs.prefetchable = needs.io = 0;

	for (PCIDev* dev = bus->child; dev != NULL; dev = dev->next) {
		if (dev->child != NULL) {
			bus_needs child;
			_HotplugSizeBus(dev->child, child);
			if (_IsHotplugPort(dev, NULL)) {
				child.memory = max_c(child.memory, HOTPLUG_MEMORY_SLACK);
				child.prefetchable = max_c(child.prefetchable,
					HOTPLUG_MEMORY_SLACK);
			}
			dev->window_size[0] = child.memory != 0
				? round_up_power_of_two(round_up(child.memory, MB)) : 0;
			dev->window_size[1] = child.prefetchable != 0
				? round_up_power_of_two(round_up(child.prefetchable, MB)) : 0;
			dev->window_size[2] = child.io != 0
				? round_up_power_of_two(round_up(child.io, 4096)) : 0;
			needs.memory += dev->window_size[0];
			needs.prefetchable += dev->window_size[1];
			needs.io += dev->window_size[2];
			continue;
		}

		for (int i = 0, next = 1; i < 6; i = next, next++) {
			uint32 flags = dev->info.u.h0.base_register_flags[i];
			uint64 size = dev->info.u.h0.base_register_sizes[i];
			if ((flags & PCI_address_type) == PCI_address_type_64) {
				size |= (uint64)dev->info.u.h0.base_register_sizes[i + 1] << 32;
				next++;
			}
			if (size == 0)
				continue;
			if ((flags & PCI_address_space) != 0)
				needs.io += size;
			else if ((flags & PCI_address_prefetchable) != 0)
				needs.prefetchable += size;
			else
				needs.memory += size;
		}
	}
}


struct hotplug_item {
	PCIDev*	dev;
	int		index;
		// BAR, or for bridges 0 memory, 1 prefetchable, 2 I/O window
	bool	window;
	uint64	size;
	uint32	flags;
};


static void
program_bridge_window(PCI* pci, PCIDev* dev, int index,
	const pci_resource_range* range)
{
	switch (index) {
		case 0:
			if (range == NULL) {
				pci->WriteConfig(dev, PCI_memory_base, 2, 0xfff0);
				pci->WriteConfig(dev, PCI_memory_limit, 2, 0);
			} else {
				uint64 end = range->pci_address + range->size - 1;
				pci->WriteConfig(dev, PCI_memory_base, 2,
					(range->pci_address >> 16) & 0xfff0);
				pci->WriteConfig(dev, PCI_memory_limit, 2,
					(end >> 16) & 0xfff0);
			}
			break;
		case 1:
			if (range == NULL) {
				pci->WriteConfig(dev, PCI_prefetchable_memory_base, 2, 0xfff0);
				pci->WriteConfig(dev, PCI_prefetchable_memory_limit, 2, 0);
				pci->WriteConfig(dev, PCI_prefetchable_memory_base_upper32, 4,
					0);
				pci->WriteConfig(dev, PCI_prefetchable_memory_limit_upper32, 4,
					0);
			} else {
				uint64 end = range->pci_address + range->size - 1;
				pci->WriteConfig(dev, PCI_prefetchable_memory_base_upper32, 4,
					range->pci_address >> 32);
				pci->WriteConfig(dev, PCI_prefetchable_memory_limit_upper32, 4,
					end >> 32);
				pci->WriteConfig(dev, PCI_prefetchable_memory_base, 2,
					(range->pci_address >> 16) & 0xfff0);
				pci->WriteConfig(dev, PCI_prefetchable_memory_limit, 2,
					(end >> 16) & 0xfff0);
			}
			break;
		case 2:
			if (range == NULL) {
				pci->WriteConfig(dev, PCI_io_base, 1, 0xf0);
				pci->WriteConfig(dev, PCI_io_limit, 1, 0);
				pci->WriteConfig(dev, PCI_io_base_upper16, 2, 0);
				pci->WriteConfig(dev, PCI_io_limit_upper16, 2, 0);
			} else {
				uint64 end = range->pci_address + range->size - 1;
				pci->WriteConfig(dev, PCI_io_base_upper16, 2,
					range->pci_address >> 16);
				pci->WriteConfig(dev, PCI_io_limit_upper16, 2, end >> 16);
				pci->WriteConfig(dev, PCI_io_base, 1,
					(range->pci_address >> 8) & 0xf0);
				pci->WriteConfig(dev, PCI_io_limit, 1, (end >> 8) & 0xf0);
			}
			break;
	}
}


status_t
PCI::_HotplugAssignBus(PCIBus* bus)
{
	hotplug_item items[64];
	int count = 0;

	for (PCIDev* dev = bus->child; dev != NULL && count < 64;
			dev = dev->next) {
		if (dev->child != NULL) {
			static const uint32 kWindowFlags[3] = {
				0,
				PCI_address_prefetchable | PCI_address_type_64,
				PCI_address_space
			};
			for (int i = 0; i < 3 && count < 64; i++) {
				program_bridge_window(this, dev, i, NULL);
				if (dev->window_size[i] == 0)
					continue;
				items[count++] = { dev, i, true, dev->window_size[i],
					kWindowFlags[i] };
			}
			continue;
		}

		for (int i = 0, next = 1; i < 6 && count < 64; i = next, next++) {
			uint32 flags = dev->info.u.h0.base_register_flags[i];
			uint64 size = dev->info.u.h0.base_register_sizes[i];
			if ((flags & PCI_address_type) == PCI_address_type_64) {
				size |= (uint64)dev->info.u.h0.base_register_sizes[i + 1] << 32;
				next++;
			}
			if (size == 0)
				continue;
			items[count++] = { dev, i, false, size, flags };
		}
	}

	// largest first
	for (int i = 1; i < count; i++) {
		hotplug_item item = items[i];
		int j = i - 1;
		while (j >= 0 && items[j].size < item.size) {
			items[j + 1] = items[j];
			j--;
		}
		items[j + 1] = item;
	}

	status_t result = B_OK;
	for (int i = 0; i < count; i++) {
		hotplug_item& item = items[i];
		bool io = (item.flags & PCI_address_space) != 0;
		PCIResourceWindow& window = io ? bus->io_window : bus->memory_window;
		uint64 alignment = item.window ? (io ? 4096 : MB) : item.size;
		alignment = max_c(alignment, item.size);

		pci_resource_range range = {};
		if (window.AllocateResource(item.size, alignment, range,
				item.flags & (PCI_address_prefetchable | PCI_address_type_64
					| PCI_address_space)) != B_OK) {
			dprintf("PCI: hot plug: no room for %s %d of %02x:%02x.%x (%#"
				B_PRIx64 " bytes)\n", item.window ? "window" : "BAR",
				item.index, item.dev->bus, item.dev->device,
				item.dev->function, item.size);
			result = B_NO_MEMORY;
			continue;
		}

		PCIDev* dev = item.dev;
		if (item.window) {
			dev->window[item.index] = range;
			program_bridge_window(this, dev, item.index, &range);
			PCIResourceWindow& childWindow = io ? dev->child->io_window
				: dev->child->memory_window;
			childWindow.FreeResource(range);
			dprintf("PCI: hot plug: %02x:%02x.%x window %d at %#" B_PRIx64
				", %#" B_PRIx64 " bytes\n", dev->bus, dev->device,
				dev->function, item.index, (uint64)range.pci_address,
				item.size);
		} else {
			dev->bar[item.index] = range;
			WriteConfig(dev, PCI_base_registers + item.index * 4, 4,
				(uint32)range.pci_address);
			if ((item.flags & PCI_address_type) == PCI_address_type_64) {
				WriteConfig(dev, PCI_base_registers + (item.index + 1) * 4, 4,
					(uint32)((uint64)range.pci_address >> 32));
			}
			dprintf("PCI: hot plug: %02x:%02x.%x BAR %d at %#" B_PRIx64
				", %#" B_PRIx64 " bytes\n", dev->bus, dev->device,
				dev->function, item.index, (uint64)range.pci_address,
				item.size);
		}
	}

	// bridges decode their windows; the devices below wait for their
	// drivers
	for (PCIDev* dev = bus->child; dev != NULL; dev = dev->next) {
		if (dev->child == NULL)
			continue;
		uint16 command = ReadConfig(dev, PCI_command, 2);
		command |= PCI_command_memory | PCI_command_master;
		if (dev->window_size[2] != 0)
			command |= PCI_command_io;
		WriteConfig(dev, PCI_command, 2, command);
		if (_HotplugAssignBus(dev->child) != B_OK)
			result = B_NO_MEMORY;
	}

	return result;
}


//!	Makes sure the bus behind \a port knows the windows the port decodes.
void
PCI::_HotplugInitWindows(PCIDev* port)
{
	PCIBus* bus = port->child;
	if (!bus->memory_window.IsEmpty() || !bus->io_window.IsEmpty())
		return;

	// the firmware programmed the port, but the windows were never
	// reserved at boot: take them as they are
	_ReadHeaderInfo(port);
	pci_info& info = port->info;

	uint32 memoryBase = ((uint32)info.u.h1.memory_base & 0xfff0) << 16;
	uint32 memoryLimit = (((uint32)info.u.h1.memory_limit & 0xfff0) << 16)
		+ 0xfffff;
	if (memoryLimit > memoryBase) {
		pci_resource_range range = {};
		range.type = B_IO_MEMORY;
		range.pci_address = memoryBase;
		range.host_address = pci_ram_address(port->domain, memoryBase);
		range.size = (uint64)memoryLimit + 1 - memoryBase;
		bus->memory_window.FreeResource(range);
	}

	uint64 prefetchBase = (((uint64)info.u.h1.prefetchable_memory_base
			& 0xfff0) << 16)
		| ((uint64)info.u.h1.prefetchable_memory_base_upper32 << 32);
	uint64 prefetchLimit = ((((uint64)info.u.h1.prefetchable_memory_limit
			& 0xfff0) << 16)
		| ((uint64)info.u.h1.prefetchable_memory_limit_upper32 << 32))
		+ 0xfffff;
	if (prefetchLimit > prefetchBase) {
		pci_resource_range range = {};
		range.type = B_IO_MEMORY;
		range.address_type = PCI_address_prefetchable | PCI_address_type_64;
		range.pci_address = prefetchBase;
		range.host_address = pci_ram_address(port->domain, prefetchBase);
		range.size = prefetchLimit + 1 - prefetchBase;
		bus->memory_window.FreeResource(range);
	}

	uint32 ioBase = (((uint32)info.u.h1.io_base & 0xf0) << 8)
		| ((uint32)info.u.h1.io_base_upper16 << 16);
	uint32 ioLimit = ((((uint32)info.u.h1.io_limit & 0xf0) << 8)
		| ((uint32)info.u.h1.io_limit_upper16 << 16)) + 0xfff;
	if (ioLimit > ioBase) {
		pci_resource_range range = {};
		range.type = B_IO_PORT;
		range.pci_address = ioBase;
		range.host_address = ioBase;
		range.size = ioLimit + 1 - ioBase;
		bus->io_window.FreeResource(range);
	}

	dprintf("PCI: hot plug: %02x:%02x.%x windows from the firmware: memory %#"
		B_PRIx32 "-%#" B_PRIx32 ", prefetchable %#" B_PRIx64 "-%#" B_PRIx64
		", I/O %#" B_PRIx32 "-%#" B_PRIx32 "\n", port->bus, port->device,
		port->function, memoryBase, memoryLimit, prefetchBase, prefetchLimit,
		ioBase, ioLimit);
}


status_t
PCI::_HotplugAdd(PCIDev* port)
{
	PCIBus* bus = port->child;
	uint8 secondary = ReadConfig(port, PCI_secondary_bus, 1);
	uint8 subordinate = ReadConfig(port, PCI_subordinate_bus, 1);
	if (secondary == 0 || secondary != bus->bus) {
		dprintf("PCI: hot plug: %02x:%02x.%x has no usable bus number\n",
			port->bus, port->device, port->function);
		return B_BAD_VALUE;
	}

	// the device may take a while to answer after the link came up
	bigtime_t deadline = system_time() + 1000000;
	while (true) {
		uint16 vendor = ReadConfig(port->domain, secondary, 0, 0,
			PCI_vendor_id, 2);
		if (vendor != 0xffff && vendor != 0x0001)
			break;
		if (system_time() > deadline) {
			dprintf("PCI: hot plug: nothing answers behind %02x:%02x.%x\n",
				port->bus, port->device, port->function);
			return B_TIMED_OUT;
		}
		snooze(20000);
	}

	dprintf("PCI: hot plug: link up behind %02x:%02x.%x, buses %u-%u\n",
		port->bus, port->device, port->function, secondary, subordinate);

	uint8 last = secondary;
	_HotplugNumberBuses(port->domain, secondary, last, subordinate);
	_HotplugDiscoverBus(bus);
	_ConfigureBridges(bus);
	_RefreshDeviceInfo(bus);

	_HotplugInitWindows(port);
	bus_needs needs;
	_HotplugSizeBus(bus, needs);
	dprintf("PCI: hot plug: devices need memory %#" B_PRIx64 ", prefetchable %#"
		B_PRIx64 ", I/O %#" B_PRIx64 "\n", needs.memory, needs.prefetchable,
		needs.io);
	if (_HotplugAssignBus(bus) != B_OK)
		dprintf("PCI: hot plug: some devices got no address space\n");
	_RefreshDeviceInfo(bus);

	// make the devices known to the device manager
	pci_register_devices(_GetDomainData(port->domain)->root_node, bus);
	return B_OK;
}


void
PCI::_HotplugRelease(PCIBus* bus)
{
	for (PCIDev* dev = bus->child; dev != NULL; dev = dev->next) {
		if (dev->child != NULL) {
			_HotplugRelease(dev->child);
			for (int i = 0; i < 3; i++) {
				if (dev->window[i].size == 0)
					continue;
				PCIResourceWindow& window = i == 2 ? bus->io_window
					: bus->memory_window;
				window.FreeResource(dev->window[i]);
				dev->window[i].size = 0;
			}
			continue;
		}
		for (int i = 0; i < 6; i++) {
			if (dev->bar[i].size == 0)
				continue;
			PCIResourceWindow& window = dev->bar[i].type == B_IO_PORT
				? bus->io_window : bus->memory_window;
			window.FreeResource(dev->bar[i]);
			dev->bar[i].size = 0;
		}
	}
}


void
PCI::_HotplugRemove(PCIDev* port)
{
	PCIBus* bus = port->child;
	dprintf("PCI: hot plug: link down behind %02x:%02x.%x\n", port->bus,
		port->device, port->function);

	pci_unregister_devices(_GetDomainData(port->domain)->root_node, bus);
	_HotplugRelease(bus);

	// The device structures stay allocated: drivers and device nodes that
	// still hold them must not touch freed memory.
	bus->child = NULL;
}


void
PCI::_HotplugScan(PCIBus* bus, uint32& added, uint32& removed)
{
	for (PCIDev* dev = bus->child; dev != NULL; dev = dev->next) {
		uint8 capability;
		if (_IsHotplugPort(dev, &capability)) {
			bool up = _IsPortLinkUp(dev, capability);
			bool populated = dev->child->child != NULL;
			if (up && !populated) {
				if (_HotplugAdd(dev) == B_OK)
					added++;
			} else if (!up && populated) {
				_HotplugRemove(dev);
				removed++;
				continue;
			}
		}
		if (dev->child != NULL)
			_HotplugScan(dev->child, added, removed);
	}
}


status_t
PCI::RescanHotplug(uint32* _added, uint32* _removed)
{
	MutexLocker locker(sHotplugLock);

	uint32 added = 0, removed = 0;
	for (uint32 domain = 0; domain < fDomainCount; domain++)
		_HotplugScan(fDomainData[domain].bus, added, removed);

	if (_added != NULL)
		*_added = added;
	if (_removed != NULL)
		*_removed = removed;
	return B_OK;
}


// #pragma mark - module


static status_t
pci_hotplug_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		{
			module_info* pci;
			return get_module(B_PCI_MODULE_NAME, &pci);
		}
		case B_MODULE_UNINIT:
			return put_module(B_PCI_MODULE_NAME);
	}
	return B_BAD_VALUE;
}


pci_hotplug_module_info gPCIHotplugModule = {
	.info = { B_PCI_HOTPLUG_MODULE_NAME, 0, pci_hotplug_std_ops },
	.rescan = [](uint32* added, uint32* removed) {
		return gPCI->RescanHotplug(added, removed);
	},
};
