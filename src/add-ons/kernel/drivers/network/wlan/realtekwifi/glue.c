/*
 * Copyright 2018, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */


#include <sys/bus.h>
#include <sys/kernel.h>

#include <driver_settings.h>

#include <dev/pci/pcivar.h>

#include <machine/bus.h>

#include <net/if.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_amrr.h>

#include <dev/rtwn/if_rtwnvar.h>
#include <dev/rtwn/pci/rtwn_pci_var.h>


extern driver_t* DRIVER_MODULE_NAME(rtwn_pci, pci);
extern driver_t* DRIVER_MODULE_NAME(rtwn_usb, uhub);
extern driver_t* DRIVER_MODULE_NAME(rtwn_sdio, sdiob);

HAIKU_FBSD_WLAN_DRIVERS_GLUE(realtekwifi)
HAIKU_DRIVER_REQUIREMENTS(FBSD_WLAN);
HAIKU_FIRMWARE_VERSION(0);
HAIKU_FIRMWARE_NAME_MAP({
	{"rtwn-rtl8188eefw", "rtwn-rtl8188eefw.ucode"},
	{"rtwn-rtl8188eufw", "rtwn-rtl8188eufw.ucode"},
	{"rtwn-rtl8192cfwE", "rtwn-rtl8192cfwE.ucode"},
	{"rtwn-rtl8192cfwE_B", "rtwn-rtl8192cfwE_B.ucode"},
	{"rtwn-rtl8192cfwU", "rtwn-rtl8192cfwU.ucode"},
	{"rtwn-rtl8192cfwT", "rtwn-rtl8192cfwT.ucode"},
	{"rtwn-rtl8192eufw", "rtwn-rtl8192eufw.ucode"},
	{"rtwn-rtl8723bufw", "rtwn-rtl8723bufw.ucode"},
	{"rtwn-rtl8812aufw", "rtwn-rtl8812aufw.ucode"},
	{"rtwn-rtl8821aufw", "rtwn-rtl8821aufw.ucode"},
});

NO_HAIKU_FBSD_MII_DRIVER();
NO_HAIKU_REENABLE_INTERRUPTS();


/*
 * The RTL8723BS on SDIO (airos-works/haiku #133) is new and untested on
 * real hardware, so it attaches only when the driver settings ask for it:
 *	~/config/settings/kernel/drivers/realtekwifi:	sdio 1
 * Without that line nothing of it runs: if such a card is there, the
 * driver only says so in the syslog. PCI and USB devices are not affected
 * either way.
 */
static bool
sdio_attach_enabled(void)
{
	void* settings = load_driver_settings("realtekwifi");
	bool enabled = false;

	if (settings != NULL) {
		enabled = get_driver_boolean_parameter(settings, "sdio", false,
			true);
		unload_driver_settings(settings);
	}
	return enabled;
}


static void
init_hardware_sdio(void)
{
	static const uint16 kVendors[] = { 0x024c };
	static const uint16 kRtl8723bs[] = {
		0x0523, 0x0525, 0x0623, 0x0626, 0x0627, 0xb723
	};
	driver_t *sdio_drivers[] = {
		DRIVER_MODULE_NAME(rtwn_sdio, sdiob),
		NULL
	};
	uint16 device;
	uint8 function;

	if (!sdio_attach_enabled()) {
		if (_fbsd_sdio_find(kVendors[0], kRtl8723bs, B_COUNT_OF(kRtl8723bs),
				&device, &function) == B_OK) {
			dprintf("realtekwifi: RTL8723BS SDIO card %04x:%04x found; its "
				"driver is off (experimental): add the line \"sdio 1\" to "
				"~/config/settings/kernel/drivers/realtekwifi to try it\n",
				kVendors[0], device);
		}
		return;
	}

	dprintf("realtekwifi: SDIO attach enabled by the driver settings, "
		"looking for RTL8723BS cards\n");
	// The MMC bus finds SDIO cards in the background at boot: give it a
	// few seconds when there is none of Realtek's yet.
	if (_fbsd_init_hardware_sdio(sdio_drivers, kVendors, B_COUNT_OF(kVendors),
			10000000) != B_OK) {
		dprintf("realtekwifi: no RTL8723BS SDIO card on the MMC bus (is the "
			"SD host controller supported? see the mmc lines of the "
			"syslog)\n");
	}
}


void
__haiku_init_hardware()
{
	driver_t *pci_drivers[] = {
		DRIVER_MODULE_NAME(rtwn_pci, pci),
		NULL
	};
	driver_t *usb_drivers[] = {
		DRIVER_MODULE_NAME(rtwn_usb, uhub),
		NULL
	};
	_fbsd_init_hardware_pci(pci_drivers);
	_fbsd_init_hardware_uhub(usb_drivers);
	init_hardware_sdio();
}


int
HAIKU_CHECK_DISABLE_INTERRUPTS(device_t dev)
{
	struct rtwn_pci_softc* pc = (struct rtwn_pci_softc*)device_get_softc(dev);
	int32_t status, tx_rings;

	status = rtwn_pci_get_intr_status(pc, &tx_rings);
	if (status == 0 && tx_rings == 0)
		return 0;

	atomic_set(&pc->pc_intr_status, status);
	atomic_set(&pc->pc_intr_tx_rings, tx_rings);

	return 1;
}
