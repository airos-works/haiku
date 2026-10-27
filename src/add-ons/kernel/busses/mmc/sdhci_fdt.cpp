/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "sdhci.h"
#include "rk3588_profile.h"

#include <bus/FDT.h>
#include <driver_settings.h>
#include <arch/atomic.h>
#include <new>
#include <stdio.h>

#define SDHCI_FDT_DRIVER "busses/mmc/sdhci/fdt/driver_v1"
#define SDHCI_FDT_BUS "busses/mmc/sdhci/fdt/device/v2"

struct FdtMmc {
	device_node* node;
	RK3588Mmc::Resources resources;
	bool readOnly;
	uint64 cpuAddressFloor;
	bool enableCache;
	area_id cruArea;
	volatile uint8* cru;
	volatile uint8* registers;
	uint32 originalClock;
	bool clockChanged;
};

static bool
has(fdt_device_module_info* fdt, fdt_device* device, const char* name, const char* value)
{
	int size = 0;
	const void* data = fdt->get_prop(device, name, &size);
	return RK3588Mmc::HasString(data, size, value);
}

static uint32
number(fdt_device_module_info* fdt, fdt_device* device, const char* name)
{
	int size = 0;
	const void* data = fdt->get_prop(device, name, &size);
	return data != NULL && size == 4 ? RK3588Mmc::Read32(data) : 0;
}

static bool
enabled(fdt_device_module_info* fdt, fdt_device* device)
{
	return fdt->get_prop(device, "status", NULL) == NULL
		|| has(fdt, device, "status", "okay") || has(fdt, device, "status", "ok");
}

static bool
settings(bool& readOnly, bool& forceHighCpuBuffers, bool& enableCache)
{
	void* handle = load_driver_settings("sdhci");
	if (handle == NULL)
		return false;
	const char* profile = get_driver_parameter(handle, "firmware_profile", "", "");
	bool admitted = strcmp(profile, RK3588Mmc::kProfile) == 0;
	readOnly = get_driver_boolean_parameter(handle, "read_only", true, true);
	forceHighCpuBuffers = get_driver_boolean_parameter(handle,
		"force_high_cpu_buffers", false, false);
	enableCache = get_driver_boolean_parameter(handle, "enable_cache", false, false);
	unload_driver_settings(handle);
	return admitted;
}

static float
supports_fdt(device_node* parent)
{
#if !defined(__aarch64__)
	return 0;
#endif
	bool readOnly, forceHighCpuBuffers, enableCache;
	const char* bus;
	if (!settings(readOnly, forceHighCpuBuffers, enableCache)
		|| gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK
		|| strcmp(bus, "fdt") != 0)
		return 0;
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (gDeviceManager->get_driver(parent, (driver_module_info**)&fdt, (void**)&device) != B_OK)
		return 0;
	return enabled(fdt, device) && has(fdt, device, "compatible", "rockchip,rk3588-dwcmshc")
		? 0.9f : 0.0f;
}

static status_t
register_fdt(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "RK3588 eMMC"}}, {}
	};
	return gDeviceManager->register_node(parent, SDHCI_FDT_DRIVER, attrs, NULL, NULL);
}

static status_t
init_fdt(device_node* node, void** cookie)
{
	bool readOnly, forceHighCpuBuffers, enableCache;
	if (!settings(readOnly, forceHighCpuBuffers, enableCache))
		return B_NOT_SUPPORTED;
	device_node* parent = gDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = gDeviceManager->get_driver(parent, (driver_module_info**)&fdt, (void**)&device);
	if (status != B_OK) {
		gDeviceManager->put_node(parent);
		return status;
	}
	RK3588Mmc::Resources p = {};
	device_node* interruptController = NULL;
	bool valid = enabled(fdt, device) && has(fdt, device, "compatible", "rockchip,rk3588-dwcmshc")
		&& fdt->get_reg(device, 0, &p.base, &p.size)
		&& fdt->get_interrupt(device, 0, &interruptController, &p.interrupt);
	int size = 0, namesSize = 0;
	const void* data = fdt->get_prop(device, "interrupts", &size);
	p.levelHigh = RK3588Mmc::Interrupt(data, size);
	if (interruptController != NULL) {
		fdt_device_module_info* irqFdt;
		fdt_device* irqDevice;
		valid &= gDeviceManager->get_driver(interruptController,
			(driver_module_info**)&irqFdt, (void**)&irqDevice) == B_OK
			&& has(irqFdt, irqDevice, "compatible", "arm,gic-v3");
	}
	p.width = number(fdt, device, "bus-width");
	p.maxFrequency = number(fdt, device, "max-frequency");
	p.nonRemovable = fdt->get_prop(device, "non-removable", NULL) != NULL;
	p.noSD = fdt->get_prop(device, "no-sd", NULL) != NULL;
	p.noSDIO = fdt->get_prop(device, "no-sdio", NULL) != NULL;
	data = fdt->get_prop(device, "clocks", &size);
	const void* names = fdt->get_prop(device, "clock-names", &namesSize);
	valid &= RK3588Mmc::ClockReferences(data, size, names, namesSize, false, p.clockPhandle);
	data = fdt->get_prop(device, "resets", &size);
	names = fdt->get_prop(device, "reset-names", &namesSize);
	valid &= RK3588Mmc::ClockReferences(data, size, names, namesSize, true, p.clockPhandle);
	fdt_bus_module_info* busModule;
	fdt_bus* bus;
	if (gDeviceManager->get_driver(fdt->get_bus(device), (driver_module_info**)&busModule,
			(void**)&bus) == B_OK && p.clockPhandle != 0) {
		device_node* cruNode = busModule->node_by_phandle(bus, p.clockPhandle);
		fdt_device_module_info* cruFdt;
		fdt_device* cruDevice;
		if (cruNode != NULL && gDeviceManager->get_driver(cruNode,
				(driver_module_info**)&cruFdt, (void**)&cruDevice) == B_OK) {
			p.cru = has(cruFdt, cruDevice, "compatible", "rockchip,rk3588-cru")
				&& enabled(cruFdt, cruDevice) && number(cruFdt, cruDevice, "#clock-cells") == 1
				&& number(cruFdt, cruDevice, "#reset-cells") == 1
				&& cruFdt->get_reg(cruDevice, 0, &p.cruBase, &p.cruSize);
		}
	}
	// The FDT API has no address translation here. Require the retained
	// firmware's identity buses, and reject DMA/IOMMU transformations.
	p.identity = true;
	device_node* current = parent;
	while (current != NULL) {
		const char* busName;
		if (gDeviceManager->get_attr_string(current, B_DEVICE_BUS, &busName, false) != B_OK
			|| strcmp(busName, "fdt") != 0) {
			gDeviceManager->put_node(current);
			break;
		}
		fdt_device_module_info* ancestorFdt;
		fdt_device* ancestor;
		if (gDeviceManager->get_driver(current, (driver_module_info**)&ancestorFdt,
				(void**)&ancestor) != B_OK) {
			p.identity = false;
			gDeviceManager->put_node(current);
			break;
		}
		p.identity &= enabled(ancestorFdt, ancestor);
		static const char* const kUnsupported[] = {
			"iommus", "iommu-map", "big-endian", "big-endian-regs"
		};
		for (const char* name : kUnsupported)
			p.identity &= ancestorFdt->get_prop(ancestor, name, NULL) == NULL;
		data = ancestorFdt->get_prop(ancestor, "dma-ranges", &size);
		p.identity &= data == NULL || size == 0;
		if (ancestorFdt->get_name(ancestor)[0] == 0)
			p.board = has(ancestorFdt, ancestor, "compatible", "radxa,rock-5-itx")
				&& has(ancestorFdt, ancestor, "compatible", "rockchip,rk3588");
		else if (current != parent) {
			data = ancestorFdt->get_prop(ancestor, "ranges", &size);
			p.identity &= data != NULL && size == 0;
		}
		device_node* next = gDeviceManager->get_parent_node(current);
		gDeviceManager->put_node(current);
		current = next;
	}
	if (!valid || !RK3588Mmc::Allows(p)) {
		dprintf("sdhci: RK3588 firmware resource profile rejected\n");
		return B_NOT_SUPPORTED;
	}
	FdtMmc* info = new(std::nothrow) FdtMmc{};
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;
	info->resources = p;
	info->readOnly = readOnly;
	// This constrains the scheduler's CPU vectors, not the private SDMA
	// payload. The latter must still fit the controller's 32-bit interface.
	info->cpuAddressFloor = forceHighCpuBuffers ? UINT64_C(0x100000000) : 0;
	info->enableCache = enableCache;
	info->cruArea = -1;
	*cookie = info;
	return B_OK;
}

struct RkMmcIO {
	FdtMmc* info;
	uint8 Read8(unsigned offset) { return info->registers[offset]; }
	uint16 Read16(unsigned offset) { return *(volatile uint16*)(info->registers + offset); }
	void Write8(unsigned offset, uint8 value) { info->registers[offset] = value; }
	void Write16(unsigned offset, uint16 value) { *(volatile uint16*)(info->registers + offset) = value; }
	void Write32(unsigned offset, uint32 value) { *(volatile uint32*)(info->registers + offset) = value; }
	uint32 ReadClock() { return *(volatile uint32*)(info->cru + RK3588Mmc::kClockOffset); }
	void WriteClock(uint32 value) {
		*(volatile uint32*)(info->cru + RK3588Mmc::kClockOffset) = value;
		info->clockChanged = true;
	}
	void Barrier() { memory_full_barrier(); }
};

static status_t
set_rk3588_clock(void* cookie, uint32 requested, uint32* baseClock)
{
	if (requested != 400 && requested != 25000)
		return B_NOT_SUPPORTED;
	RkMmcIO io = {(FdtMmc*)cookie};
	if (!RK3588Mmc::ConfigureLegacy(io, requested == 400))
		return B_IO_ERROR;
	// Lower the external PHY/core source for identification as the installed
	// firmware does. The host divider alone did not identify this native card.
	*baseClock = requested == 400 ? 375 : 24000;
	return B_OK;
}

static void
uninit_fdt(void* cookie)
{
	FdtMmc* info = (FdtMmc*)cookie;
	if (info->cruArea >= B_OK) {
		if (info->clockChanged) {
			*(volatile uint32*)(info->cru + RK3588Mmc::kClockOffset)
				= RK3588Mmc::ClockWrite(info->originalClock);
			memory_full_barrier();
		}
		delete_area(info->cruArea);
	}
	delete info;
}

static status_t
register_children(void* cookie)
{
	FdtMmc* info = (FdtMmc*)cookie;
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "RK3588 eMMC legacy SDR"}},
		{B_DEVICE_BUS, B_STRING_TYPE, {.string = "mmc"}},
		{B_DEVICE_FIXED_CHILD, B_STRING_TYPE, {.string = MMC_BUS_MODULE_NAME}},
		{kMmcReadOnlyAttribute, B_UINT8_TYPE, {.ui8 = uint8(info->readOnly)}},
		{kMmcNonRemovableAttribute, B_UINT8_TYPE, {.ui8 = 1}},
		{kMmcMaxBusWidthAttribute, B_UINT8_TYPE, {.ui8 = uint8(info->resources.width)}},
		{kMmcEnableCacheAttribute, B_UINT8_TYPE, {.ui8 = uint8(info->enableCache)}},
		{B_DMA_LOW_ADDRESS, B_UINT64_TYPE, {.ui64 = info->cpuAddressFloor}},
		{B_DMA_ALIGNMENT, B_UINT32_TYPE, {.ui32 = 511}},
		{B_DMA_BOUNDARY, B_UINT32_TYPE, {.ui32 = (1 << 19) - 1}},
		{B_DMA_MAX_SEGMENT_COUNT, B_UINT32_TYPE, {.ui32 = 1}},
		{B_DMA_MAX_SEGMENT_BLOCKS, B_UINT32_TYPE, {.ui32 = (1 << 10) - 1}},
		{}
	};
	return gDeviceManager->register_node(info->node, SDHCI_FDT_BUS, attrs, NULL, NULL);
}

static status_t
init_bus_fdt(device_node* node, void** cookie)
{
	device_node* parent = gDeviceManager->get_parent_node(node);
	FdtMmc* info;
	status_t status = gDeviceManager->get_driver(parent, NULL, (void**)&info);
	gDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;
	void* mapping;
	area_id area = map_physical_memory("RK3588 eMMC registers", info->resources.base,
		info->resources.size, B_ANY_KERNEL_BLOCK_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &mapping);
	if (area < B_OK)
		return area;
	registers* regs = (registers*)mapping;
	volatile uint32* words = (volatile uint32*)mapping;
	if (!RK3588Mmc::Controller(words[0x40 / 4], words[0x44 / 4],
			regs->host_controller_version.specVersion, words[0xe8 / 4])) {
		dprintf("sdhci: RK3588 capabilities rejected: %#x %#x v%u vendor %#x\n",
			words[0x40 / 4], words[0x44 / 4], regs->host_controller_version.specVersion,
			words[0xe8 / 4]);
		delete_area(area);
		return B_NOT_SUPPORTED;
	}
	if (info->cruArea < B_OK) {
		info->cruArea = map_physical_memory("RK3588 eMMC clock", info->resources.cruBase,
			B_PAGE_SIZE, B_ANY_KERNEL_BLOCK_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&info->cru);
		if (info->cruArea < B_OK) {
			delete_area(area);
			return info->cruArea;
		}
		info->originalClock = *(volatile uint32*)(info->cru + RK3588Mmc::kClockOffset);
	}
	info->registers = (volatile uint8*)regs;
	sdhci_platform_info platform = {set_rk3588_clock, info, info->readOnly, true, 375,
		info->cpuAddressFloor};
	SdhciBus* controller = new(std::nothrow) SdhciBus(regs, info->resources.interrupt, false, &platform);
	if (controller == NULL) {
		delete_area(area);
		info->registers = NULL;
		return B_NO_MEMORY;
	}
	status = controller->InitCheck();
	if (status != B_OK) {
		delete controller;
		info->registers = NULL;
		return status;
	}
	*cookie = controller;
	dprintf("sdhci: RK3588 eMMC IRQ 237, external clock 375 kHz identification / "
		"24 MHz legacy SDR, nonzero host divider, DMA32, %s\n",
		info->readOnly ? "read-only" : "writable");
	return B_OK;
}

driver_module_info gSDHCIFDTDriverModule = {
	{SDHCI_FDT_DRIVER, 0, NULL},
	supports_fdt, register_fdt, init_fdt, uninit_fdt, register_children, NULL, NULL
};

mmc_bus_interface gSDHCIFDTDeviceModule = {
	.info = {.info = {.name = SDHCI_FDT_BUS}, .init_driver = init_bus_fdt,
		.uninit_driver = uninit_bus, .device_removed = bus_removed},
	.set_clock = set_clock, .execute_command = execute_command, .do_io = do_io,
	.set_scan_semaphore = set_scan_semaphore, .set_bus_width = set_bus_width,
	.terminate_bus = terminate_bus, .set_card_type = set_card_type,
	.read_extended_csd = read_extended_csd,
	.switch_sd_function = switch_sd_function
};
