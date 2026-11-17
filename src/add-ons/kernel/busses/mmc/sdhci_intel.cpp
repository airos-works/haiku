/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Intel's SD host controllers of the Atom SoCs (Bay Trail, Cherry Trail and
	Braswell, and the later SDIO slots), in ACPI or PCI mode: which slot is
	the SDIO one, and what it needs. Linux is the reference for all of it
	(drivers/mmc/host/sdhci-acpi.c, sdhci-pci-core.c); the behaviour is
	reimplemented here, each part named after the Linux code it follows. */


#include "sdhci_intel.h"

#include <stdio.h>
#include <string.h>

#include <ACPI.h>
#include "acpi.h"
#include <KernelExport.h>
#include <OS.h>
#include <PCI.h>

#include "sdhci.h"


#define TRACE_ALWAYS(x...)	dprintf("\33[33msdhci:\33[0m " x)


// The SDIO slots by ACPI _HID (and _UID where the same _HID has several
// slots): Linux's sdhci_acpi_uids[] entries with sdhci_acpi_slot_int_sdio.
static const struct {
	const char*	hid;
	const char*	uid;
} kIntelAcpiSdioSlots[] = {
	{ "80860F14", "2" },	// Bay Trail, Cherry Trail, Braswell
	{ "INT33BB", "2" },		// Bay Trail
	{ "INT33C6", NULL },	// Lynx Point
	{ "INT3436", NULL },	// Wildcat Point
	{ "INT344D", NULL },	// Sunrise Point
	{ "80865AD0", NULL },	// Broxton/Apollo Lake
};

// Their other slots, for matching controllers that have no PNP0D40 _CID:
// eMMC and SD card. Their handling stays the generic one.
static const struct {
	const char*	hid;
	const char*	uid;
} kIntelAcpiOtherSlots[] = {
	{ "80860F14", "1" },
	{ "80860F14", "3" },
	{ "80860F16", NULL },
	{ "INT33BB", "3" },
	{ "80865ACA", NULL },
	{ "80865ACC", NULL },
};

// The SDIO slots in PCI mode: Linux's sdhci_intel_byt_sdio users
static const uint16 kIntelPciSdioDevices[] = {
	0x0f15,		// Bay Trail
	0x2295,		// Braswell, Cherry Trail
	0x9d2c,		// Sunrise Point
	0x0ad0,		// Broxton
	0x1ad0,		// Broxton-M
	0x5ad0,		// Apollo Lake
	0x31d0,		// Gemini Lake
};

// Bay Trail's slots in PCI mode, for its OCP timeout (byt_ocp_setting)
static const uint16 kIntelPciBaytrailDevices[] = {
	0x0f14, 0x0f15, 0x0f16, 0x0f50
};


static bool
uid_matches(const char* wanted, const char* uid)
{
	return wanted == NULL || (uid != NULL && strcmp(wanted, uid) == 0);
}


bool
sdhci_intel_acpi_is_sdio_slot(const char* hid, const char* uid)
{
	if (hid == NULL)
		return false;
	for (size_t i = 0; i < B_COUNT_OF(kIntelAcpiSdioSlots); i++) {
		if (strcmp(hid, kIntelAcpiSdioSlots[i].hid) == 0
			&& uid_matches(kIntelAcpiSdioSlots[i].uid, uid))
			return true;
	}
	return false;
}


bool
sdhci_intel_acpi_is_slot(const char* hid, const char* uid)
{
	if (sdhci_intel_acpi_is_sdio_slot(hid, uid))
		return true;
	if (hid == NULL)
		return false;
	for (size_t i = 0; i < B_COUNT_OF(kIntelAcpiOtherSlots); i++) {
		if (strcmp(hid, kIntelAcpiOtherSlots[i].hid) == 0
			&& uid_matches(kIntelAcpiOtherSlots[i].uid, uid))
			return true;
	}
	return false;
}


uint32
sdhci_intel_acpi_slot_quirks(const char* hid, const char* uid)
{
	// sdhci_acpi_slot_int_sdio: MMC_CAP_NONREMOVABLE and broken card
	// detection. (Its ops have no set_power override: no power retry.)
	if (sdhci_intel_acpi_is_sdio_slot(hid, uid))
		return SDHCI_SLOT_NON_REMOVABLE;
	return 0;
}


uint32
sdhci_intel_pci_slot_quirks(uint16 vendor, uint16 device)
{
	if (vendor != 0x8086)
		return 0;
	for (size_t i = 0; i < B_COUNT_OF(kIntelPciSdioDevices); i++) {
		if (device == kIntelPciSdioDevices[i]) {
			// byt_sdio_probe_slot: MMC_CAP_NONREMOVABLE; the byt ops:
			// sdhci_intel_set_power, sdhci_intel_set_uhs_signaling
			return SDHCI_SLOT_NON_REMOVABLE | SDHCI_SLOT_POWER_RETRY
				| SDHCI_SLOT_HS_AS_SDR25;
		}
	}
	return 0;
}


bool
sdhci_intel_pci_is_baytrail(uint16 vendor, uint16 device)
{
	if (vendor != 0x8086)
		return false;
	for (size_t i = 0; i < B_COUNT_OF(kIntelPciBaytrailDevices); i++) {
		if (device == kIntelPciBaytrailDevices[i])
			return true;
	}
	return false;
}


//	#pragma mark - Bay Trail OCP timeout


#if defined(__i386__) || defined(__x86_64__)

// IOSF sideband through the host bridge's message bus registers (Linux:
// arch/x86/platform/intel/iosf_mbi.c)
#define MBI_MCR			0xd0
#define MBI_MDR			0xd4
#define MBI_MCRX		0xd8
#define MBI_ENABLE		0xf0
#define MBI_CR_READ		0x06
#define MBI_CR_WRITE	0x07

#define BYT_IOSF_SCCEP				0x63
#define BYT_IOSF_OCP_NETCTRL0		0x1078
#define BYT_IOSF_OCP_TIMEOUT_BASE	(7u << 8)


static bool
is_baytrail()
{
	// soc_intel_is_byt(): an Intel family 6 model 0x37 (Silvermont) CPU
	cpuid_info info;
	if (get_cpuid(&info, 0, 0) != B_OK
		|| strncmp(info.eax_0.vendor_id, "GenuineIntel", 12) != 0
		|| get_cpuid(&info, 1, 0) != B_OK)
		return false;
	uint32 model = (info.eax_1.extended_model << 4) | info.eax_1.model;
	return info.eax_1.family == 6 && model == 0x37;
}


static uint32
mbi_command(uint8 opcode, uint8 port, uint32 offset)
{
	return ((uint32)opcode << 24) | ((uint32)port << 16)
		| ((offset & 0xff) << 8) | MBI_ENABLE;
}


/*!	Bay Trail's SD controllers time out on their OCP bus under load unless
	the timeout base is cleared (Linux: sdhci_acpi_byt_setting(),
	byt_ocp_setting()). Does nothing on other CPUs.
*/
void
sdhci_intel_byt_ocp_fixup()
{
	if (!is_baytrail())
		return;

	pci_module_info* pci;
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&pci) != B_OK)
		return;

	static spinlock sLock = B_SPINLOCK_INITIALIZER;
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sLock);

	uint32 high = BYT_IOSF_OCP_NETCTRL0 & 0xffffff00;
	pci->write_pci_config(0, 0, 0, MBI_MCRX, 4, high);
	pci->write_pci_config(0, 0, 0, MBI_MCR, 4,
		mbi_command(MBI_CR_READ, BYT_IOSF_SCCEP, BYT_IOSF_OCP_NETCTRL0));
	uint32 value = pci->read_pci_config(0, 0, 0, MBI_MDR, 4);
	bool changed = false;
	if ((value & BYT_IOSF_OCP_TIMEOUT_BASE) != 0) {
		value &= ~BYT_IOSF_OCP_TIMEOUT_BASE;
		pci->write_pci_config(0, 0, 0, MBI_MDR, 4, value);
		pci->write_pci_config(0, 0, 0, MBI_MCRX, 4, high);
		pci->write_pci_config(0, 0, 0, MBI_MCR, 4,
			mbi_command(MBI_CR_WRITE, BYT_IOSF_SCCEP, BYT_IOSF_OCP_NETCTRL0));
		changed = true;
	}

	release_spinlock(&sLock);
	restore_interrupts(state);
	put_module(B_PCI_MODULE_NAME);

	if (changed)
		TRACE_ALWAYS("Bay Trail: OCP timeout base cleared\n");
}

#else

void
sdhci_intel_byt_ocp_fixup()
{
}

#endif


//	#pragma mark - ACPI power


static bool
has_object(acpi_module_info* acpi, acpi_handle device, const char* name)
{
	acpi_handle handle;
	return acpi->get_handle(device, name, &handle) == B_OK;
}


/*!	acpi_device_fix_up_power(): a device that has _PS0 but neither power
	resources nor _PSC is assumed to be in D0, which on these machines it
	need not be: _PS0 is run.
*/
static void
fix_up_power(acpi_module_info* acpi, acpi_handle device, const char* what)
{
	if (!has_object(acpi, device, "_PS0") || has_object(acpi, device, "_PR0")
		|| has_object(acpi, device, "_PSC"))
		return;
	status_t status = acpi->evaluate_method(device, "_PS0", NULL, NULL);
	TRACE_ALWAYS("%s: _PS0: %s\n", what, strerror(status));
}


void
sdhci_acpi_fix_up_power(const char* path)
{
	acpi_module_info* acpi;
	if (path == NULL
		|| get_module(B_ACPI_MODULE_NAME, (module_info**)&acpi) != B_OK)
		return;

	acpi_handle controller;
	if (acpi->get_handle(NULL, path, &controller) == B_OK) {
		fix_up_power(acpi, controller, path);

		// acpi_device_fix_up_power_extended(): its children too (the Wi-Fi
		// chip's power is often a child's _PS0), if present and enabled
		acpi_handle child = NULL;
		while (acpi->get_next_object(ACPI_TYPE_DEVICE, controller, &child)
				== B_OK) {
			acpi_object_type result;
			if (acpi->evaluate_object(child, "_STA", NULL, &result,
					sizeof(result)) == B_OK
				&& result.object_type == ACPI_TYPE_INTEGER
				&& (result.integer.integer & 0x3) != 0x3)
				continue;
			char name[5] = {};
			acpi->get_name(child, ACPI_SINGLE_NAME, name, sizeof(name));
			char what[64];
			snprintf(what, sizeof(what), "%s.%s", path, name);
			fix_up_power(acpi, child, what);
		}
	}

	put_module(B_ACPI_MODULE_NAME);
}
