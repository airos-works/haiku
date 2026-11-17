/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

/*
 * RTL8710B channel and bandwidth switching.
 *
 * TX power: the RTL8188F's per-group scheme (r88f_get_txpower()), which
 * rtl8xxxu also shares between the two chips (rtl8188f_set_tx_power()).
 * The rest is the sequence of rtl8xxxu's rtl8710bu_config_channel() and
 * rtl8710b_revise_cck_tx_psf(); the code is rtwn's own.
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
#include <dev/rtwn/if_rtwn_ridx.h>

#include <dev/rtwn/rtl8192c/r92c.h>
#include <dev/rtwn/rtl8192c/r92c_reg.h>

#include <dev/rtwn/rtl8188f/r88f.h>

#include <dev/rtwn/rtl8710b/r10b.h>
#include <dev/rtwn/rtl8710b/r10b_reg.h>
#include <dev/rtwn/rtl8710b/r10b_var.h>

static void
r10b_set_txpower(struct rtwn_softc *sc, struct ieee80211_channel *c)
{
	uint8_t power[RTWN_RIDX_COUNT];

	memset(power, 0, sizeof(power));
	r88f_get_txpower(sc, 0, c, power);
	r92c_write_txpower(sc, 0, power);
}

/*
 * CCK TX shaping filter: channel 14 (Japan, 802.11b only) needs a
 * narrower one, channel 13 a different first coefficient set; every
 * other channel the BB table's values.
 */
static void
r10b_set_cck_filter(struct rtwn_softc *sc, int chan)
{
	const uint32_t *normal = R10B_SOFTC(sc)->cck_filter;
	uint32_t filter1 = normal[0], filter2 = normal[1], port = normal[2],
	    filter3 = normal[3];

	if (chan == 13)
		filter1 = 0xd1d80001;
	else if (chan == 14) {
		filter2 = 0x0000b81c;
		port = 0x00000000;
		filter3 = 0x00003667;
	}
	rtwn_bb_write(sc, R10B_CCK0_TX_FILTER2, filter2);
	rtwn_bb_write(sc, R10B_CCK0_DEBUG_PORT, port);
	rtwn_bb_write(sc, R10B_CCK0_TX_FILTER3, filter3);
	rtwn_bb_write(sc, R10B_CCK0_TX_FILTER1, filter1);
}

void
r10b_set_chan(struct rtwn_softc *sc, struct ieee80211_channel *c)
{
	uint32_t rf;
	uint8_t chan, subchannel = 0;
	int ht40;

	/* Center channel: the HT40 pair's middle. */
	chan = rtwn_chan2centieee(c);
	ht40 = IEEE80211_IS_CHAN_HT40(c);
	if (IEEE80211_IS_CHAN_HT40U(c))
		subchannel = 2;		/* primary below the center */
	else if (IEEE80211_IS_CHAN_HT40D(c))
		subchannel = 1;		/* primary above the center */

	/* Set Tx power for this new channel. */
	r10b_set_txpower(sc, c);

	/* RF channel. */
	rf = rtwn_rf_read(sc, 0, R92C_RF_CHNLBW);
	rf = (rf & ~R10B_RF_CHNLBW_CHAN_M) | chan;
	rtwn_rf_write(sc, 0, R92C_RF_CHNLBW, rf);

	r10b_set_cck_filter(sc, chan);

	/* MAC bandwidth and the primary channel's place. */
	rtwn_setbits_2(sc, R10B_WMAC_TRXPTCL_CTL, R10B_WMAC_TRXPTCL_CTL_BW_M,
	    ht40 ? R10B_WMAC_TRXPTCL_CTL_BW_40 : 0);
	rtwn_write_1(sc, R10B_DATA_SUBCHANNEL, subchannel);

	/* BB bandwidth. */
	rtwn_bb_setbits(sc, R92C_FPGA0_RFMOD, R92C_RFMOD_40MHZ,
	    ht40 ? R92C_RFMOD_40MHZ : 0);
	rtwn_bb_setbits(sc, R92C_FPGA1_RFMOD, R92C_RFMOD_40MHZ,
	    ht40 ? R92C_RFMOD_40MHZ : 0);
	if (ht40) {
		/* CCK control sideband: set when the primary is above. */
		rtwn_bb_setbits(sc, R92C_CCK0_SYSTEM,
		    R92C_CCK0_SYSTEM_CCK_SIDEBAND,
		    IEEE80211_IS_CHAN_HT40U(c) ? 0 :
		    R92C_CCK0_SYSTEM_CCK_SIDEBAND);
	}

	/* RX ADC clock (bits 10:8), TX DAC clock (14, 12 on; 13 off). */
	rtwn_bb_setbits(sc, R92C_FPGA0_RFMOD, 0, 0x00000700);
	rtwn_bb_setbits(sc, R92C_FPGA0_RFMOD, 0x00002000, 0x00005000);

	/* Small bandwidth off; ADC buffer clock. */
	rtwn_bb_setbits(sc, R10B_OFDM0_TX_PSDO_NOISE_WEIGHT, 0xc0000000, 0);
	rtwn_bb_setbits(sc, R10B_OFDM0_TX_PSDO_NOISE_WEIGHT, 0x20000000,
	    0x10000000);
	rtwn_bb_setbits(sc, R10B_OFDM0_XA_RX_AFE, 0x20000000, 0x10000000);
	rtwn_bb_setbits(sc, R10B_FPGA0_XB_RF_INT_OE, 0x40000000, 0x20000000);

	/* OFDM RX digital filter, and the RF TRX bandwidth. */
	rf = rtwn_rf_read(sc, 0, R92C_RF_CHNLBW) & ~R10B_RF_CHNLBW_BW_M;
	if (ht40) {
		rtwn_bb_setbits(sc, R10B_RX_DFIR, 0x0ff80000, 0);
		rf |= R10B_RF_CHNLBW_BW40;
	} else {
		rtwn_bb_setbits(sc, R10B_RX_DFIR, 0x0ff80000, 0x0a880000);
		rf |= R10B_RF_CHNLBW_BW20;
	}
	rtwn_rf_write(sc, 0, R92C_RF_CHNLBW, rf);
}
