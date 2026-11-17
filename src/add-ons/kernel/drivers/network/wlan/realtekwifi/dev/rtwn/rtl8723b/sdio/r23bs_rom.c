/*-
 * Copyright (c) 2026 air/OS
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * RTL8723BS efuse map: the RTL8723B's common part (r23b_parse_rom()), the
 * MAC address at 0x11a where USB has its IDs and address (struct r23b_rom,
 * sdio), and the antenna wiring from 0xc3 (bit 0 clear: two antennas,
 * bit 6 clear: the antenna at the main port, S1), which the USB
 * attachment does not look at. An unprogrammed map (0xc3 = 0xff) means one
 * antenna at the main port, and a map without a MAC address gets the
 * vendor's default, as Linux's staging driver does
 * (hal/rtl8723b_hal_init.c Hal_EfuseParseBTCoexistInfo_8723B(),
 * hal/sdio_halinit.c Hal_EfuseParseMACAddr_8723BS()).
 */

#include <sys/param.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/mbuf.h>
#include <sys/kernel.h>
#include <sys/socket.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>
#include <sys/bus.h>
#include <sys/endian.h>

#include <net/if.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>

#include <dev/rtwn/sdio/rtwn_sdio_var.h>

#include <dev/rtwn/rtl8192e/r92e_var.h>

#include <dev/rtwn/rtl8723b/r23b_var.h>
#include <dev/rtwn/rtl8723b/r23b_rom_image.h>

#include <dev/rtwn/rtl8723b/sdio/r23bs.h>

#define R23BS_ROM_ANT_AUX	0x40	/* rf_bt_opt (0xc3) bit 6 */

static const uint8_t r23bs_default_macaddr[IEEE80211_ADDR_LEN] =
	{ 0x00, 0xe0, 0x4c, 0xb7, 0x23, 0x00 };

void
r23bs_parse_rom(struct rtwn_softc *sc, uint8_t *buf)
{
	struct r23b_rom *rom = (struct r23b_rom *)buf;
	struct r23b_softc *rs = sc->sc_priv;
	const uint8_t *mac = rom->sdio.macaddr;
	int i, blank = 1, ones = 1;

	r23b_parse_rom(sc, buf);

	for (i = 0; i < IEEE80211_ADDR_LEN; i++) {
		if (mac[i] != 0)
			blank = 0;
		if (mac[i] != 0xff)
			ones = 0;
	}
	if (blank || ones || ETHER_IS_MULTICAST(mac)) {
		RTWN_SDIO_LOG(sc, "efuse: no MAC address (%s), using the "
		    "vendor default\n", ether_sprintf(mac));
		mac = r23bs_default_macaddr;
	}
	IEEE80211_ADDR_COPY(sc->sc_ic.ic_macaddr, mac);

	if (rom->rf_bt_opt == 0xff) {
		rs->ant_main = 1;
		rs->bt_antnum = 1;
	} else
		rs->ant_main = (rom->rf_bt_opt & R23BS_ROM_ANT_AUX) == 0;

	RTWN_SDIO_LOG(sc, "efuse: id %#06x, MAC %s, 0xc3 %#04x: %s antenna%s "
	    "at the %s port (S%d), channel plan %#04x, crystal %#04x, thermal "
	    "%#04x\n", le16toh(rom->id), ether_sprintf(sc->sc_ic.ic_macaddr),
	    rom->rf_bt_opt, rs->bt_antnum == 0 ? "two" : "one",
	    rs->bt_antnum == 0 ? "s" : "", rs->ant_main ? "main" : "auxiliary",
	    rs->ant_main ? 1 : 0, rom->channel_plan, rs->super.crystalcap,
	    sc->thermal_meter);
}
