/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The SD card slot of the Qualcomm SC8280XP (SDC2, "qcom,sdhci-msm-v5";
	the Radxa Dragon Q8B's microSD slot): a standard SDHCI 3.0 controller
	with Qualcomm's additions, set up as Linux' sdhci-msm.c does:
	 - the block reset through GCC (GCC_SDCC2_BCR), its clocks on, the core
	   clock left at the rate the firmware set (GPLL9 / 4, 202 MHz);
	 - the vendor register at its power-on value with the pad voltage
	   switch on and at 3 V, 3.0 V and 8 bits in the capabilities;
	 - the card's supply (vmmc) and its pins' (vqmmc) on through RPMh (the
	   apps RSC's second active TCS) while the driver runs;
	 - the power interrupt ("pwr_irq"): the controller waits for each bus
	   power and I/O level change it makes to be acknowledged.
	The controller's own card detect is not wired: the slot's switch is GPIO
	131, low with a card in (seen with a card; schematic page 38), and a thread
	polls it and drives the SDHCI card detect test level, so the generic
	insertion path runs. Its DMA is ADMA2 only (it runs its ADMA engine
	whatever DMA mode is selected) and goes through the apps SMMU: in a
	context bank of its own that maps only the DMA payload and its
	descriptors (through the firmware's untranslated bank 2 its accesses to
	address 0, ADMA without a table, hung the board), and the bus is cache
	coherent ("dma-coherent").

	Settings (the "sdhci" driver settings file): qcom_sd false turns it off,
	qcom_cd_active_low false reads the switch the other way round. */


#include "sdhci.h"

#include <bus/FDT.h>
#include <driver_settings.h>
#include <arch/atomic.h>
#include <new>
#include <stdio.h>
#include <string.h>

#include <libfdt.h>

#include "qcom_rpmh.h"
#include "qcom_smmu.h"


#define SDHCI_QCOM_DRIVER "busses/mmc/sdhci/qcom/driver_v1"
#define SDHCI_QCOM_BUS "busses/mmc/sdhci/qcom/device/v1"

#define QCOM_TRACE(x...) dprintf("sdhci qcom: " x)


extern void* gFDT;

// the second active TCS of the apps RSC (qcom_adsp uses the first)
static const uint32 kSdTcs = 1;

// the one slot's state, for the bus' uninit (whose cookie is the SdhciBus)
static struct QcomMmc* sSlot;

// SDC2's GCC registers (gcc-sc8280xp.c)
static const uint32 kGccBcr = 0x14000;
static const uint32 kGccAppsClock = 0x14004;
static const uint32 kGccAhbClock = 0x14008;
static const uint32 kGccRcgCfg = 0x14010;
static const uint32 kGccRcgM = 0x14014;
static const uint32 kGccRcgN = 0x14018;

// sdhci-msm v5 (sdhci_msm_v5_offset)
static const uint32 kVendorSpec = 0x20c;
static const uint32 kVendorCapabilities = 0x21c;
static const uint32 kPowerStatus = 0x240;
static const uint32 kPowerMask = 0x244;
static const uint32 kPowerClear = 0x248;
static const uint32 kPowerControl = 0x24c;
static const uint32 kVendorSpecPowerOn = 0xa9c;
static const uint32 kPadPowerSwitchEnable = 1u << 15;
static const uint32 kPadPowerSwitch = 1u << 16;		// set: 1.8 V pads

// SDHCI host control 1: card detect signal selection and test level
static const uint8 kCardDetectTest = 1u << 7 | 1u << 6;


struct QcomMmc {
	device_node*	node;
	phys_addr_t		base;
	size_t			size;
	uint64			hcInterrupt;
	uint64			powerInterrupt;
	phys_addr_t		gccBase;
	phys_addr_t		tlmmBase;
	uint32			cdPin;
	bool			cdActiveLow;
	bool			readOnly;
	char			vmmc[16];
	uint32			vmmcMillivolts;
	char			vqmmc[16];
	uint32			vqmmcMillivolts;

	area_id			area;
	volatile uint8*	registers;
	area_id			gccArea;
	volatile uint8*	gcc;
	area_id			tlmmArea;
	volatile uint8*	tlmm;
	qcom_rpmh		rpmh;
	QcomSmmuDomain	smmu;			// its DMA's address space
	bool			smmuAttached;
	bool			powerHandler;
	thread_id		thread;
	volatile bool	quit;
	SdhciBus*		bus;
};


static inline uint32
read32(volatile uint8* base, uint32 offset)
{
	return *(volatile uint32*)(base + offset);
}


static inline void
write32(volatile uint8* base, uint32 offset, uint32 value)
{
	*(volatile uint32*)(base + offset) = value;
	memory_full_barrier();
}


// #pragma mark - device tree


static bool
fdt_reg(int node, phys_addr_t* _base, size_t* _size)
{
	int length;
	const fdt32_t* reg = (const fdt32_t*)fdt_getprop(gFDT, node, "reg",
		&length);
	if (reg == NULL || length < 16)
		return false;
	*_base = (uint64)fdt32_to_cpu(reg[0]) << 32 | fdt32_to_cpu(reg[1]);
	*_size = (uint64)fdt32_to_cpu(reg[2]) << 32 | fdt32_to_cpu(reg[3]);
	return true;
}


/*!	The RPMh resource of the regulator \a property points to, as
	rpmh-regulator names it: the node's name ("ldo9") with its PMIC's ID
	("c") after the type ("ldoc9"); and its highest voltage. */
static bool
fdt_regulator(int node, const char* property, char* name, size_t nameSize,
	uint32* _millivolts)
{
	const fdt32_t* phandle = (const fdt32_t*)fdt_getprop(gFDT, node, property,
		NULL);
	int regulator = phandle != NULL
		? fdt_node_offset_by_phandle(gFDT, fdt32_to_cpu(*phandle)) : -1;
	if (regulator < 0)
		return false;
	const char* nodeName = fdt_get_name(gFDT, regulator, NULL);
	const char* pmic = (const char*)fdt_getprop(gFDT,
		fdt_parent_offset(gFDT, regulator), "qcom,pmic-id", NULL);
	const fdt32_t* microvolts = (const fdt32_t*)fdt_getprop(gFDT, regulator,
		"regulator-max-microvolt", NULL);
	if (nodeName == NULL || pmic == NULL || microvolts == NULL
		|| strncmp(nodeName, "ldo", 3) != 0)
		return false;
	snprintf(name, nameSize, "ldo%s%s", pmic, nodeName + 3);
	*_millivolts = fdt32_to_cpu(*microvolts) / 1000;
	return true;
}


static bool
qcom_settings(bool& readOnly, bool& cdActiveLow)
{
	readOnly = false;
	cdActiveLow = true;
	void* handle = load_driver_settings("sdhci");
	if (handle == NULL)
		return true;
	bool enabled = get_driver_boolean_parameter(handle, "qcom_sd", true, true);
	cdActiveLow = get_driver_boolean_parameter(handle, "qcom_cd_active_low",
		true, true);
	unload_driver_settings(handle);
	return enabled;
}


// #pragma mark - the controller's Qualcomm side


/*!	The power interrupt: each bus power and I/O level change the controller
	makes waits for this acknowledgement (sdhci_msm_handle_pwr_irq()); the
	supplies are already on, so every change succeeds. */
static int32
power_interrupt(void* cookie)
{
	QcomMmc* info = (QcomMmc*)cookie;
	uint32 status = read32(info->registers, kPowerStatus) & 0xf;
	if (status == 0)
		return B_UNHANDLED_INTERRUPT;
	for (int retry = 0; retry < 10; retry++) {
		write32(info->registers, kPowerClear, status);
		if ((read32(info->registers, kPowerStatus) & status) == 0)
			break;
		spin(10);
	}
	uint32 ack = 0;
	bool ioHigh = false;
	if ((status & 1u << 1) != 0) {			// bus on: I/O high as well
		ack |= 1u << 0 | 1u << 2;
		ioHigh = true;
	}
	if ((status & 1u << 0) != 0)			// bus off
		ack |= 1u << 0 | 1u << 2;
	if ((status & 1u << 2) != 0)			// I/O low
		ack |= 1u << 2;
	if ((status & 1u << 3) != 0) {			// I/O high
		ack |= 1u << 2;
		ioHigh = true;
	}
	write32(info->registers, kPowerControl, ack);
	uint32 vendor = read32(info->registers, kVendorSpec);
	if (ioHigh)
		vendor &= ~kPadPowerSwitch;
	else if ((status & (1u << 2 | 1u << 0)) != 0)
		vendor |= kPadPowerSwitch;
	write32(info->registers, kVendorSpec, vendor);
	return B_HANDLED_INTERRUPT;
}


static uint32
core_clock_kilohertz(QcomMmc* info)
{
	// gcc_parent_map_15: XO, GPLL0, GPLL9, -, -, GPLL4, GPLL0 even; half
	// integer divider; M/N in dual edge mode
	static const uint64 kSources[8] = {19200000, 600000000, 808000000, 0, 0,
		806400000, 300000000, 0};
	uint32 cfg = read32(info->gcc, kGccRcgCfg);
	uint64 rate = kSources[(cfg >> 8) & 7];
	uint32 halfDivider = cfg & 0x1f;
	if (halfDivider != 0)
		rate = rate * 2 / (halfDivider + 1);
	if (((cfg >> 12) & 3) == 2) {
		uint32 m = read32(info->gcc, kGccRcgM) & 0xff;
		uint32 n = ((~read32(info->gcc, kGccRcgN)) & 0xff) + m;
		if (n != 0)
			rate = rate * m / n;
	}
	return (uint32)(rate / 1000);
}


/*!	The controller's stream (0x4e0, the device tree's iommus) in a context
	bank of its own that maps only the DMA payload, at its own address: the
	firmware's bank 2 passes everything through untranslated, and a stray
	access through it is suspected of hanging the board. */
static status_t
map_qcom_dma(void* cookie, phys_addr_t address, size_t size)
{
	QcomMmc* info = (QcomMmc*)cookie;
	static const uint32 kStreams[][2] = {{0x4e0, 0}};
	status_t status = B_OK;
	if (!info->smmuAttached) {
		status = info->smmu.Attach("sdhci qcom", kStreams, 1);
		info->smmuAttached = status == B_OK;
	}
	if (status == B_OK)
		status = info->smmu.Map(address, address, size, true);
	if (status != B_OK)
		dprintf("sdhci qcom: SDC2: no SMMU mapping for its DMA: %s\n",
			strerror(status));
	return status;
}


static status_t
set_qcom_clock(void* cookie, uint32 requested, uint32* baseClock)
{
	QcomMmc* info = (QcomMmc*)cookie;
	*baseClock = core_clock_kilohertz(info);
	return *baseClock != 0 ? B_OK : B_ERROR;
}


static bool
card_present(QcomMmc* info)
{
	bool high = (read32(info->tlmm, 4) & 1) != 0;
	return info->cdActiveLow ? !high : high;
}


/*!	Follows the slot's switch: the SDHCI card detect test level says what
	it does, which raises the controller's insertion and removal
	interrupts. A reset of the controller clears it; this puts it back. */
static status_t
card_detect_thread(void* cookie)
{
	QcomMmc* info = (QcomMmc*)cookie;
	int previous = -1;
	while (!info->quit) {
		bool present = card_present(info);
		if ((int)present != previous) {
			QCOM_TRACE("card %s (GPIO %" B_PRIu32 " %s)\n",
				present ? "inserted" : "removed", info->cdPin,
				(read32(info->tlmm, 4) & 1) != 0 ? "high" : "low");
			previous = present;
		}
		volatile uint8* hostControl = info->registers + 0x28;
		uint8 value = *hostControl;
		uint8 wanted = (value & ~kCardDetectTest)
			| (present ? kCardDetectTest : 1u << 7);
		if (value != wanted)
			*hostControl = wanted;
		// a DMA access its context bank refused is logged
		info->smmu.Faults("SDC2");
		snooze(250000);
	}
	return B_OK;
}


static status_t
qcom_controller_init(QcomMmc* info)
{
	// the clocks on, then the block reset
	write32(info->gcc, kGccAhbClock, read32(info->gcc, kGccAhbClock) | 1);
	write32(info->gcc, kGccAppsClock, read32(info->gcc, kGccAppsClock) | 1);
	spin(100);
	if ((read32(info->gcc, kGccAppsClock) & (1u << 31)) != 0)
		return B_DEVICE_NOT_FOUND;
	write32(info->gcc, kGccBcr, read32(info->gcc, kGccBcr) | 1);
	spin(200);
	write32(info->gcc, kGccBcr, read32(info->gcc, kGccBcr) & ~1u);
	spin(200);

	write32(info->registers, kVendorSpec, kVendorSpecPowerOn);
	// the capabilities the generic code reads: 3.0 V and 8 bits as well
	write32(info->registers, kVendorCapabilities,
		read32(info->registers, 0x40) | 1u << 25 | 1u << 18);
	write32(info->registers, kVendorSpec,
		(read32(info->registers, kVendorSpec) | kPadPowerSwitchEnable)
			& ~kPadPowerSwitch);

	// TLMM: the card detect pin a plain input, no bias (the board pulls it)
	write32(info->tlmm, 0, 0);

	status_t status = qcom_rpmh_init(info->rpmh);
	if (status == B_OK) {
		status = qcom_rpmh_ldo(info->rpmh, kSdTcs, info->vmmc, true,
			info->vmmcMillivolts);
	}
	if (status == B_OK) {
		status = qcom_rpmh_ldo(info->rpmh, kSdTcs, info->vqmmc, true,
			info->vqmmcMillivolts);
	}
	if (status != B_OK) {
		QCOM_TRACE("supplies %s/%s: %s\n", info->vmmc, info->vqmmc,
			strerror(status));
		return status;
	}
	snooze(20000);

	// anything pending from before, then the power interrupts on
	write32(info->registers, kPowerClear, 0xf);
	write32(info->registers, kPowerMask, 0xf);
	status = install_io_interrupt_handler(info->powerInterrupt,
		power_interrupt, info, 0);
	if (status != B_OK)
		return status;
	info->powerHandler = true;
	return B_OK;
}


static void
qcom_controller_uninit(QcomMmc* info)
{
	if (info->powerHandler) {
		write32(info->registers, kPowerMask, 0);
		remove_io_interrupt_handler(info->powerInterrupt, power_interrupt,
			info);
		info->powerHandler = false;
	}
	if (info->rpmh.rscArea >= 0) {
		qcom_rpmh_ldo(info->rpmh, kSdTcs, info->vmmc, false, 0);
		qcom_rpmh_uninit(info->rpmh);
	}
}


// #pragma mark - driver


static float
supports_qcom(device_node* parent)
{
#if !defined(__aarch64__)
	return 0;
#endif
	const char* bus;
	if (gFDT == NULL
		|| gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0)
		return 0;
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (gDeviceManager->get_driver(parent, (driver_module_info**)&fdt,
			(void**)&device) != B_OK)
		return 0;
	int size = 0;
	const char* compatible = (const char*)fdt->get_prop(device, "compatible",
		&size);
	const char* status = (const char*)fdt->get_prop(device, "status", NULL);
	uint64 base, length;
	// SDC2 only: its GCC registers are the ones this knows
	if (compatible == NULL || strcmp(compatible, "qcom,sc8280xp-sdhci") != 0
		|| (status != NULL && strcmp(status, "okay") != 0
			&& strcmp(status, "ok") != 0)
		|| !fdt->get_reg(device, 0, &base, &length) || base != 0x8804000)
		return 0;
	bool readOnly, cdActiveLow;
	return qcom_settings(readOnly, cdActiveLow) ? 0.9f : 0.0f;
}


static status_t
register_qcom(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "Qualcomm SD card slot"}},
		{}
	};
	return gDeviceManager->register_node(parent, SDHCI_QCOM_DRIVER, attrs, NULL,
		NULL);
}


static status_t
init_qcom(device_node* node, void** cookie)
{
	device_node* parent = gDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = gDeviceManager->get_driver(parent,
		(driver_module_info**)&fdt, (void**)&device);
	gDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	QcomMmc* info = new(std::nothrow) QcomMmc{};
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;
	info->area = info->gccArea = info->tlmmArea = -1;
	info->rpmh.cmdDbArea = info->rpmh.rscArea = -1;
	info->thread = -1;
	qcom_settings(info->readOnly, info->cdActiveLow);

	uint64 base, length;
	device_node* controller;
	bool valid = fdt->get_reg(device, 0, &base, &length)
		&& fdt->get_interrupt(device, 0, &controller, &info->hcInterrupt)
		&& fdt->get_interrupt(device, 1, &controller, &info->powerInterrupt);
	info->base = base;
	info->size = length;

	int sd = fdt_node_offset_by_compatible(gFDT, -1, "qcom,sc8280xp-sdhci");
	int gcc = fdt_node_offset_by_compatible(gFDT, -1, "qcom,gcc-sc8280xp");
	size_t size;
	valid &= sd >= 0 && gcc >= 0 && fdt_reg(gcc, &info->gccBase, &size)
		&& fdt_regulator(sd, "vmmc-supply", info->vmmc, sizeof(info->vmmc),
			&info->vmmcMillivolts)
		&& fdt_regulator(sd, "vqmmc-supply", info->vqmmc, sizeof(info->vqmmc),
			&info->vqmmcMillivolts);
	int cdLength = 0;
	const fdt32_t* cd = sd >= 0
		? (const fdt32_t*)fdt_getprop(gFDT, sd, "cd-gpios", &cdLength) : NULL;
	int tlmm = cd != NULL && cdLength >= 12
		? fdt_node_offset_by_phandle(gFDT, fdt32_to_cpu(cd[0])) : -1;
	valid &= tlmm >= 0 && fdt_reg(tlmm, &info->tlmmBase, &size);
	if (!valid) {
		QCOM_TRACE("incomplete device tree description\n");
		delete info;
		return B_NOT_SUPPORTED;
	}
	info->cdPin = fdt32_to_cpu(cd[1]);
	*cookie = info;
	return B_OK;
}


static void
uninit_qcom(void* cookie)
{
	delete (QcomMmc*)cookie;
}


static status_t
register_children_qcom(void* cookie)
{
	QcomMmc* info = (QcomMmc*)cookie;
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "Qualcomm SD card"}},
		{B_DEVICE_BUS, B_STRING_TYPE, {.string = "mmc"}},
		{B_DEVICE_FIXED_CHILD, B_STRING_TYPE, {.string = MMC_BUS_MODULE_NAME}},
		{kMmcReadOnlyAttribute, B_UINT8_TYPE, {.ui8 = uint8(info->readOnly)}},
		{kMmcNonRemovableAttribute, B_UINT8_TYPE, {.ui8 = 0}},
		{kMmcMaxBusWidthAttribute, B_UINT8_TYPE, {.ui8 = 4}},
		{B_DMA_ALIGNMENT, B_UINT32_TYPE, {.ui32 = 511}},
		{B_DMA_BOUNDARY, B_UINT32_TYPE, {.ui32 = (1 << 19) - 1}},
		{B_DMA_MAX_SEGMENT_COUNT, B_UINT32_TYPE, {.ui32 = 1}},
		{B_DMA_MAX_SEGMENT_BLOCKS, B_UINT32_TYPE, {.ui32 = (1 << 10) - 1}},
		{}
	};
	return gDeviceManager->register_node(info->node, SDHCI_QCOM_BUS, attrs,
		NULL, NULL);
}


static void
cleanup_bus(QcomMmc* info)
{
	if (info->thread >= 0) {
		info->quit = true;
		status_t result;
		wait_for_thread(info->thread, &result);
		info->thread = -1;
	}
	qcom_controller_uninit(info);
	if (info->tlmmArea >= 0)
		delete_area(info->tlmmArea);
	if (info->gccArea >= 0)
		delete_area(info->gccArea);
	if (info->area >= 0)
		delete_area(info->area);
	info->tlmmArea = info->gccArea = info->area = -1;
	info->registers = NULL;
}


static status_t
init_bus_qcom(device_node* node, void** cookie)
{
	device_node* parent = gDeviceManager->get_parent_node(node);
	QcomMmc* info;
	status_t status = gDeviceManager->get_driver(parent, NULL, (void**)&info);
	gDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	void* mapping;
	area_id area = map_physical_memory("Qualcomm SDHCI registers", info->base,
		info->size, B_ANY_KERNEL_BLOCK_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &mapping);
	if (area < B_OK)
		return area;
	info->area = area;
	info->registers = (volatile uint8*)mapping;
	info->gccArea = map_physical_memory("Qualcomm SDHCI clocks",
		info->gccBase, 0x20000,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&info->gcc);
	info->tlmmArea = map_physical_memory("Qualcomm SDHCI card detect",
		info->tlmmBase + info->cdPin * 0x1000, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&info->tlmm);
	if (info->gccArea < B_OK || info->tlmmArea < B_OK) {
		cleanup_bus(info);
		return B_NO_MEMORY;
	}
	status = qcom_controller_init(info);
	if (status != B_OK) {
		cleanup_bus(info);
		return status;
	}

	sdhci_platform_info platform = {set_qcom_clock, info, info->readOnly,
		false, 0, 0, true, map_qcom_dma, true};
	SdhciBus* bus = new(std::nothrow) SdhciBus((registers*)mapping,
		info->hcInterrupt, false, &platform);
	if (bus == NULL || bus->InitCheck() != B_OK) {
		status = bus != NULL ? bus->InitCheck() : B_NO_MEMORY;
		delete bus;
		cleanup_bus(info);
		return status;
	}
	info->bus = bus;
	sSlot = info;
	info->quit = false;
	info->thread = spawn_kernel_thread(card_detect_thread,
		"Qualcomm SD card detect", B_LOW_PRIORITY, info);
	if (info->thread >= 0)
		resume_thread(info->thread);
	*cookie = bus;
	QCOM_TRACE("SDC2: core clock %" B_PRIu32 " kHz, supplies %s %" B_PRIu32
		" mV / %s %" B_PRIu32 " mV, card detect GPIO %" B_PRIu32 "%s, %s\n",
		core_clock_kilohertz(info), info->vmmc, info->vmmcMillivolts,
		info->vqmmc, info->vqmmcMillivolts, info->cdPin,
		info->cdActiveLow ? " (active low)" : "",
		info->readOnly ? "read-only" : "writable");
	return B_OK;
}


static void
uninit_bus_qcom(void* cookie)
{
	QcomMmc* info = sSlot;
	// the card detect thread touches the registers: stop it first
	if (info != NULL && info->thread >= 0) {
		info->quit = true;
		status_t result;
		wait_for_thread(info->thread, &result);
		info->thread = -1;
	}
	uninit_bus(cookie);
	if (info != NULL) {
		info->bus = NULL;
		cleanup_bus(info);
	}
	sSlot = NULL;
}


driver_module_info gSDHCIQcomDriverModule = {
	{SDHCI_QCOM_DRIVER, 0, NULL},
	supports_qcom, register_qcom, init_qcom, uninit_qcom,
	register_children_qcom, NULL, NULL
};

mmc_bus_interface gSDHCIQcomDeviceModule = {
	.info = {.info = {.name = SDHCI_QCOM_BUS}, .init_driver = init_bus_qcom,
		.uninit_driver = uninit_bus_qcom, .device_removed = bus_removed},
	.set_clock = set_clock, .execute_command = execute_command, .do_io = do_io,
	.set_scan_semaphore = set_scan_semaphore, .set_bus_width = set_bus_width,
	.terminate_bus = terminate_bus, .set_card_type = set_card_type,
	.read_extended_csd = read_extended_csd
};
