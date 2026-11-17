/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

/*
 * RTL8710B RX signal strength.
 *
 * The RX descriptor is the RTL8188F's (r88f_get_rx_stats() parses it),
 * but the PHY status after it has the newer layout Realtek calls
 * "Jaguar 2" (the RTL8822B's): byte 0 is the report type (0 CCK, 1 OFDM)
 * and byte 1 the path A power in dB + 110 for both types. Older CCK AGC
 * reports carry an LNA/VGA gain index pair instead, which the chip flags
 * in BB 0xa9c bit 17 (read in r10bu_post_init()). As Linux's rtl8xxxu
 * reads them (core.c jaguar2_rx_parse_phystats(), 8710b.c
 * rtl8710b_cck_rssi()).
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
#include <sys/linker.h>

#include <net/if.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>

#include <dev/rtwn/rtl8710b/r10b.h>
#include <dev/rtwn/rtl8710b/r10b_var.h>

/* PHY status ("Jaguar 2" layout), the bytes used here. */
#define R10B_PHYST_PAGE		0	/* 0: CCK, 1: OFDM/HT */
#define R10B_PHYST_PWDB_A	1	/* dB + 110 */
#define R10B_PHYST_CCK_AGC	13	/* VGA index 4:0, LNA index 2:0 in 7:5 */
#define R10B_PHYST_CCK_LNA_H	14	/* bit 7: LNA index bit 3 */

static int8_t
r10b_get_rssi(struct rtwn_softc *sc, const uint8_t *phy)
{
	int lna, vga;

	/* OFDM/HT reports, and CCK ones from the newer AGC: dB + 110. */
	if (phy[R10B_PHYST_PAGE] != 0 || R10B_SOFTC(sc)->cck_new_agc)
		return ((int)phy[R10B_PHYST_PWDB_A] - 110);

	/* RX power from the CCK AGC gain indices, in dBm. */
	vga = phy[R10B_PHYST_CCK_AGC] & 0x1f;
	lna = ((phy[R10B_PHYST_CCK_LNA_H] & 0x80) >> 4) |
	    (phy[R10B_PHYST_CCK_AGC] >> 5);
	switch (lna) {
	case 7:
		return (-52 - 2 * vga);
	case 6:
		return (-42 - 2 * vga);
	case 5:
		return (-36 - 2 * vga);
	case 3:
		return (-12 - 2 * vga);
	case 2:
		return (-2 * vga);
	default:
		return (0);
	}
}

/*
 * rtwn picks one of these by the frame's rate; the report says which
 * kind it is anyway, so both read it the same way.
 */
int8_t
r10b_get_rssi_cck(struct rtwn_softc *sc, void *physt)
{
	return (r10b_get_rssi(sc, physt));
}

int8_t
r10b_get_rssi_ofdm(struct rtwn_softc *sc, void *physt)
{
	return (r10b_get_rssi(sc, physt));
}
