/*
 * Copyright 2018-2025 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		B Krishnan Iyer, krishnaniyer97@gmail.com
 *		Adrien Destugues, pulkomandy@pulkomandy.tk
 *		Ron Ben Aroya, sed4906birdie@gmail.com
 */
#include <algorithm>
#include <new>
#include <stdio.h>
#include <string.h>

#include <bus/PCI.h>
#include <ACPI.h>
#include "acpi.h"

#include <KernelExport.h>

#include "IOSchedulerSimple.h"
#include "mmc.h"
#include "sdhci.h"
#include "sdhci_intel.h"


//#define TRACE_SDHCI
#ifdef TRACE_SDHCI
#	define TRACE(x...) dprintf("\33[33msdhci:\33[0m " x)
#else
#	define TRACE(x...) ;
#endif
#define TRACE_ALWAYS(x...)	dprintf("\33[33msdhci:\33[0m " x)
#define ERROR(x...)			dprintf("\33[33msdhci:\33[0m " x)
#define CALLED(x...)		TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


#define SDHCI_DEVICE_MODULE_NAME "busses/mmc/sdhci/driver_v1"
#define SDHCI_ACPI_MMC_BUS_MODULE_NAME "busses/mmc/sdhci/acpi/device/v2"

static acpi_status
sdhci_acpi_scan_parse_callback(ACPI_RESOURCE *res, void *context)
{
	struct sdhci_crs* crs = (struct sdhci_crs*)context;

	if (res->Type == ACPI_RESOURCE_TYPE_FIXED_MEMORY32) {
		crs->addr_bas = res->Data.FixedMemory32.Address;
		crs->addr_len = res->Data.FixedMemory32.AddressLength;
	} else if (res->Type == ACPI_RESOURCE_TYPE_IRQ) {
		crs->irq = res->Data.Irq.Interrupt;
		//crs->irq_triggering = res->Data.Irq.Triggering;
		//crs->irq_polarity = res->Data.Irq.Polarity;
		//crs->irq_shareable = res->Data.Irq.Shareable;
	} else if (res->Type == ACPI_RESOURCE_TYPE_EXTENDED_IRQ) {
		crs->irq = res->Data.ExtendedIrq.Interrupt;
		//crs->irq_triggering = res->Data.ExtendedIrq.Triggering;
		//crs->irq_polarity = res->Data.ExtendedIrq.Polarity;
		//crs->irq_shareable = res->Data.ExtendedIrq.Shareable;
	}

	return B_OK;
}

status_t
init_bus_acpi(device_node* node, void** bus_cookie)
{
	CALLED();

	// Get the ACPI driver and device
	acpi_device_module_info* acpi;
	acpi_device device;

	device_node* parent = gDeviceManager->get_parent_node(node);
	device_node* acpiParent = gDeviceManager->get_parent_node(parent);
	gDeviceManager->get_driver(acpiParent, (driver_module_info**)&acpi,
			(void**)&device);

	// Power the controller and what hangs off it (the Wi-Fi chip of the SDIO
	// slot), as Linux does for every ACPI SD host
	// (acpi_device_fix_up_power_extended() in sdhci_acpi_probe()), and on
	// Bay Trail clear the OCP timeout (sdhci_acpi_byt_setting()). Only for
	// Intel's controllers, so far: the others work without.
	const char* hid = NULL;
	const char* uid = NULL;
	const char* path = NULL;
	gDeviceManager->get_attr_string(acpiParent, ACPI_DEVICE_HID_ITEM, &hid,
		false);
	gDeviceManager->get_attr_string(acpiParent, ACPI_DEVICE_UID_ITEM, &uid,
		false);
	if (sdhci_intel_acpi_is_slot(hid, uid)) {
		if (gDeviceManager->get_attr_string(acpiParent, ACPI_DEVICE_PATH_ITEM,
				&path, false) == B_OK)
			sdhci_acpi_fix_up_power(path);
		sdhci_intel_byt_ocp_fixup();
	}

	gDeviceManager->put_node(acpiParent);
	gDeviceManager->put_node(parent);

	uint32 slotQuirks = 0;
	gDeviceManager->get_attr_uint32(node, SDHCI_SLOT_QUIRKS_ATTRIBUTE,
		&slotQuirks, false);

	// Ignore invalid bars
	TRACE("Register SD bus\n");

	struct sdhci_crs crs = {};
	if(acpi->walk_resources(device, (ACPI_STRING)"_CRS",
			sdhci_acpi_scan_parse_callback, &crs) != B_OK) {
		ERROR("Couldn't scan ACPI register set\n");
		return B_IO_ERROR;
	}

	TRACE("addr: %" B_PRIx32 " len: %" B_PRIx32 "\n", crs.addr_bas, crs.addr_len);

	if (crs.addr_bas == 0 || crs.addr_len == 0) {
		ERROR("No registers to map\n");
		return B_IO_ERROR;
	}

	// map the slot registers
	area_id	regs_area;
	struct registers* _regs;
	regs_area = map_physical_memory("sdhc_regs_map",
		crs.addr_bas, crs.addr_len, B_ANY_KERNEL_BLOCK_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&_regs);

	if (regs_area < B_OK) {
		ERROR("Could not map registers\n");
		return B_BAD_VALUE;
	}

	// the interrupt is shared between all busses in an SDHC controller, but
	// they each register an handler. Not a problem, we will just test the
	// interrupt registers for all busses one after the other and find no
	// interrupts on the idle busses.
	uint32_t irq = crs.irq;
	TRACE("irq interrupt line: %d\n", irq);

	SdhciBus* bus = new(std::nothrow) SdhciBus(_regs, irq, true, NULL,
		slotQuirks);

	status_t status = B_NO_MEMORY;
	if (bus != NULL)
		status = bus->InitCheck();

	if (status != B_OK) {
		if (bus != NULL)
			delete bus;
		else
			delete_area(regs_area);
		return status;
	}

	// Store the created object as a cookie, allowing users of the bus to
	// locate it.
	*bus_cookie = bus;

	return status;
}

status_t
register_child_devices_acpi(void* cookie)
{
	CALLED();
	SdhciDevice* context = (SdhciDevice*)cookie;
	device_node* parent = gDeviceManager->get_parent_node(context->fNode);
	acpi_device_module_info* acpi;
	acpi_device* device;

	gDeviceManager->get_driver(parent, (driver_module_info**)&acpi,
		(void**)&device);

	TRACE("register_child_devices\n");

	const char* hid = NULL;
	const char* uid = NULL;
	gDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid, false);
	gDeviceManager->get_attr_string(parent, ACPI_DEVICE_UID_ITEM, &uid, false);
	uint32 slotQuirks = sdhci_intel_acpi_slot_quirks(hid, uid);
	gDeviceManager->put_node(parent);
	if (slotQuirks != 0) {
		TRACE_ALWAYS("%s (_UID %s): Intel SDIO slot\n", hid,
			uid != NULL ? uid : "-");
	}

	char prettyName[25];

	sprintf(prettyName, (slotQuirks & SDHCI_SLOT_NON_REMOVABLE) != 0
		? "SDHC bus (SDIO)" : "SDHC bus");
	device_attr attrs[] = {
		// properties of this controller for mmc bus manager
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = prettyName } },
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{.string = MMC_BUS_MODULE_NAME} },
		{ B_DEVICE_BUS, B_STRING_TYPE, {.string = "mmc"} },
		// standard SDHCI: SD high speed with the capability bit (SetClock)
		{ kMmcSdHighSpeedAttribute, B_UINT8_TYPE, { .ui8 = 1 }},
		{ kMmcSdioHighSpeedAttribute, B_UINT8_TYPE, { .ui8 = 1 }},
		{ kMmcNonRemovableAttribute, B_UINT8_TYPE,
			{ .ui8 = (uint8)((slotQuirks & SDHCI_SLOT_NON_REMOVABLE) != 0) }},
		{ SDHCI_SLOT_QUIRKS_ATTRIBUTE, B_UINT32_TYPE, { .ui32 = slotQuirks }},

		// DMA properties
		// The high alignment is to force access only to complete sectors
		// These constraints could be removed by using ADMA which allows
		// use of the full 64bit address space and can do scatter-gather.
		{ B_DMA_ALIGNMENT, B_UINT32_TYPE, { .ui32 = 511 }},
		{ B_DMA_HIGH_ADDRESS, B_UINT64_TYPE, { .ui64 = 0x100000000LL }},
		{ B_DMA_BOUNDARY, B_UINT32_TYPE, { .ui32 = (1 << 19) - 1 }},
		{ B_DMA_MAX_SEGMENT_COUNT, B_UINT32_TYPE, { .ui32 = 1 }},
		{ B_DMA_MAX_SEGMENT_BLOCKS, B_UINT32_TYPE, { .ui32 = (1 << 10) - 1 }},

		// private data to identify device
		{ NULL }
	};
	device_node* node;
	if (gDeviceManager->register_node(context->fNode,
			SDHCI_ACPI_MMC_BUS_MODULE_NAME, attrs, NULL,
			&node) != B_OK)
		return B_BAD_VALUE;
	return B_OK;
}

float
supports_device_acpi(device_node* parent)
{
	const char* hid;
	const char* uid;
	const char* cid;
	uint32 type;

	if (gDeviceManager->get_attr_uint32(parent, ACPI_DEVICE_TYPE_ITEM, &type, false)
		|| type != ACPI_TYPE_DEVICE) {
		return 0.0f;
	}

	if (gDeviceManager->get_attr_string(parent, ACPI_DEVICE_HID_ITEM, &hid, false)) {
		TRACE("No hid attribute\n");
		return 0.0f;
	}

	if (gDeviceManager->get_attr_string(parent, ACPI_DEVICE_UID_ITEM, &uid, false))
		uid = NULL;
	if (gDeviceManager->get_attr_string(parent, ACPI_DEVICE_CID_ITEM, &cid, false))
		cid = NULL;

	// The HID and UID determine whether a given host controller is SD, SDIO, or eMMC.
	// The CID PNP0D40 says that an ACPI device is a standard host controller;
	// Intel's are known by their HID too, as Linux knows them (they need not
	// have the CID).
	if ((cid == NULL || strcmp(cid, "PNP0D40") != 0)
		&& strcmp(hid, "PNP0D40") != 0
		&& !sdhci_intel_acpi_is_slot(hid, uid)) {
		return 0.0f;
	}

	acpi_device_module_info* acpi;
	acpi_device* device;
	gDeviceManager->get_driver(parent, (driver_module_info**)&acpi,
		(void**)&device);
	TRACE("SDHCI Device found! hid: %s, uid: %s\n", hid,
		uid != NULL ? uid : "-");

	return 0.8f;
}

// Device node registered for each SD slot. It implements the MMC operations so
// the bus manager can use it to communicate with SD cards.
mmc_bus_interface gSDHCIACPIDeviceModule = {
	.info = {
		.info = {
			.name = SDHCI_ACPI_MMC_BUS_MODULE_NAME,
		},

		.init_driver = init_bus_acpi,
		.uninit_driver = uninit_bus,
		.device_removed = bus_removed,
	},

	.set_clock = set_clock,
	.execute_command = execute_command,
	.do_io = do_io,
	.set_scan_semaphore = set_scan_semaphore,
	.set_bus_width = set_bus_width,
	.terminate_bus = terminate_bus,
	.set_card_type = set_card_type,
	.read_extended_csd = read_extended_csd,
	.switch_sd_function = switch_sd_function,
	.sdio_transfer = sdio_transfer,
	.set_sdio_interrupt_handler = set_sdio_interrupt_handler,
	.enable_sdio_interrupt = enable_sdio_interrupt,
};
