/*
 * Copyright (c) 2026 Mir Saheb Ali <mirsahebali@pm.me>
 * Copyright 2026, Haiku, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * RTL8710BU (RTL8188GU) attachment: FreeBSD's r88fu_attach() method
 * table, with the RTL8710B's own hooks where the chip differs from the
 * RTL8188F.
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
#include <dev/rtwn/if_rtwn_nop.h>

#include <dev/rtwn/usb/rtwn_usb_var.h>

#include <dev/rtwn/rtl8188e/r88e.h>

#include <dev/rtwn/rtl8192c/r92c.h>
#include <dev/rtwn/rtl8192c/r92c_reg.h>

#include <dev/rtwn/rtl8192e/r92e.h>

#include <dev/rtwn/rtl8812a/r12a.h>
#include <dev/rtwn/rtl8812a/usb/r12au.h>
#include <dev/rtwn/rtl8812a/usb/r12au_tx_desc.h>

#include <dev/rtwn/rtl8188f/r88f.h>

#include <dev/rtwn/rtl8710b/r10b.h>
#include <dev/rtwn/rtl8710b/r10b_reg.h>
#include <dev/rtwn/rtl8710b/r10b_var.h>
#include <dev/rtwn/rtl8710b/usb/r10bu.h>

void	r10bu_attach(struct rtwn_usb_softc *);

static void
r10b_set_led(struct rtwn_softc *sc, int led, int on)
{
	/* The RTL8188GU dongles drive no LED from the Wi-Fi side. */
}

static void
r10bu_attach_private(struct rtwn_softc *sc)
{
	struct r10b_softc *rs;

	rs = malloc(sizeof(struct r10b_softc), M_RTWN_PRIV, M_WAITOK | M_ZERO);

	rs->rs.rs_txpwr			= &rs->txpwr;
	rs->rs.rs_txagc			= NULL;

	rs->rs.rs_set_bw20		= r88e_set_bw20;
	rs->rs.rs_get_txpower		= r88f_get_txpower;
	rs->rs.rs_set_gain		= r88e_set_gain;
	rs->rs.rs_tx_enable_ampdu	= r88e_tx_enable_ampdu;
	rs->rs.rs_tx_setup_hwseq	= r88e_tx_setup_hwseq;
	rs->rs.rs_tx_setup_macid	= r88e_tx_setup_macid;
	rs->rs.rs_set_rom_opts		= NULL;	/* r10b_parse_rom() does it */

	rs->rs.rf_read_delay[0]		= 10;
	rs->rs.rf_read_delay[1]		= 100;
	rs->rs.rf_read_delay[2]		= 10;

	rs->package			= R10B_PACKAGE_QFN48M_U;
	rs->xtal_k			= 0xff;

	sc->sc_priv = rs;
}

static void
r10bu_adj_devcaps(struct rtwn_softc *sc)
{
	/* XXX TODO? (as for the RTL8188F) */
}

void
r10bu_attach(struct rtwn_usb_softc *uc)
{
	struct rtwn_softc *sc		= &uc->uc_sc;

	/* USB part. */
	uc->uc_align_rx			= r12au_align_rx;
	uc->tx_agg_desc_num		= 1;
	uc->tx_csum_invert		= 1;

	/* Common part. */
	sc->sc_flags			= RTWN_FLAG_EXT_HDR;

	sc->sc_set_chan			= r10b_set_chan;
	sc->sc_fill_tx_desc		= r12a_fill_tx_desc;
	sc->sc_fill_tx_desc_raw		= r12a_fill_tx_desc_raw;
	sc->sc_fill_tx_desc_null	= r12a_fill_tx_desc_null;
	sc->sc_dump_tx_desc		= r12au_dump_tx_desc;
	sc->sc_tx_radiotap_flags	= r12a_tx_radiotap_flags;
	sc->sc_rx_radiotap_flags	= r12a_rx_radiotap_flags;
	sc->sc_get_rx_stats		= r88f_get_rx_stats;
	sc->sc_get_rssi_cck		= r10b_get_rssi_cck;
	sc->sc_get_rssi_ofdm		= r10b_get_rssi_ofdm;
	sc->sc_classify_intr		= r12au_classify_intr;
	sc->sc_handle_tx_report		= r12a_ratectl_tx_complete;
	sc->sc_handle_tx_report2	= rtwn_nop_softc_uint8_int;
	sc->sc_handle_c2h_report	= r92e_handle_c2h_report;
	sc->sc_check_frame		= rtwn_nop_int_softc_mbuf;
	sc->sc_rf_read			= r92c_rf_read;
	sc->sc_rf_write			= r88e_rf_write;
	sc->sc_check_condition		= r92c_check_condition;
	sc->sc_efuse_postread		= rtwn_nop_softc;
	sc->sc_efuse_preread		= r10b_efuse_preread;
	sc->sc_efuse_read		= r10b_efuse_read;
	sc->sc_parse_rom		= r10b_parse_rom;
	sc->sc_set_led			= r10b_set_led;
	sc->sc_power_on			= r10bu_power_on;
	sc->sc_power_off		= r10bu_power_off;
#ifndef RTWN_WITHOUT_UCODE
	sc->sc_fw_reset			= r10b_fw_reset;
	sc->sc_fw_download_enable	= r10b_fw_download_enable;
#endif
	sc->sc_llt_init			= r88f_llt_init;
	sc->sc_enable_mac_txrx		= r10bu_enable_mac_txrx;
	sc->sc_set_page_size		= r88f_set_page_size;
	sc->sc_lc_calib			= r10b_lc_calib;
	sc->sc_iq_calib			= r10b_iq_calib;
	sc->sc_read_chipid_vendor	= rtwn_nop_softc_uint32;
	sc->sc_adj_devcaps		= r10bu_adj_devcaps;
	sc->sc_vap_preattach		= rtwn_nop_softc_vap;
	sc->sc_postattach		= r10b_postattach;
	sc->sc_detach_private		= r10b_detach_private;
	sc->sc_set_media_status		= r88e_set_media_status;
#ifndef RTWN_WITHOUT_UCODE
	sc->sc_set_rsvd_page		= r88e_set_rsvd_page;
	sc->sc_set_pwrmode		= r88e_set_pwrmode;
	sc->sc_set_rssi			= rtwn_nop_softc;	/* XXX TODO? */
#endif
	sc->sc_beacon_init		= r92c_beacon_init;
	sc->sc_beacon_enable		= r88e_beacon_enable;
	sc->sc_sta_beacon_enable	= r88e_sta_beacon_enable;
	sc->sc_beacon_set_rate		= rtwn_nop_void_int;
	sc->sc_beacon_select		= rtwn_nop_softc_int;
	sc->sc_temp_measure		= r88e_temp_measure;
	sc->sc_temp_read		= r88e_temp_read;
	sc->sc_init_tx_agg		= rtwn_nop_softc;
	sc->sc_init_rx_agg		= r10bu_init_rx_agg;
	sc->sc_init_beacon_timer	= rtwn_nop_softc;
	sc->sc_init_ampdu		= r10b_init_ampdu;
	sc->sc_init_intr		= r10bu_init_intr;
	sc->sc_init_edca		= r92c_init_edca;
	sc->sc_init_bb			= r10b_init_bb;
	sc->sc_init_rf			= r10b_init_rf;
	sc->sc_init_antsel		= rtwn_nop_softc;
	sc->sc_post_init		= r10bu_post_init;
	sc->sc_init_bcnq1_boundary	= rtwn_nop_int_softc;
	sc->sc_set_tx_power		= r92c_set_tx_power;

	/* From the table file, in r10b_efuse_preread(). */
	sc->mac_prog			= NULL;
	sc->mac_size			= 0;
	sc->bb_prog			= NULL;
	sc->bb_size			= 0;
	sc->agc_prog			= NULL;
	sc->agc_size			= 0;
	sc->rf_prog			= NULL;

	sc->name			= "RTL8710B";
	sc->fwname			= R10B_FWNAME_UMC;	/* per die */
	sc->fwsig			= R10B_FW_SIGNATURE;

	sc->ackto			= 0x40;
	sc->npubqpages			= R10B_PUBQ_NPAGES;
	sc->nhqpages			= R10B_HQ_NPAGES;
	sc->nlqpages			= R10B_LQ_NPAGES;
	sc->nnqpages			= R10B_NQ_NPAGES;
	sc->page_size			= R92C_TX_PAGE_SIZE;

	sc->page_count			= R10B_TX_PAGE_COUNT;
	sc->pktbuf_count		= R10B_TXPKTBUF_COUNT;

	sc->txdesc_len			= sizeof(struct r12au_tx_desc);
	sc->efuse_maxlen		= R10B_EFUSE_MAX_LEN;
	sc->efuse_maplen		= R10B_EFUSE_MAP_LEN;
	sc->rx_dma_size			= R10B_RX_DMA_BUFFER_SIZE;

	sc->macid_limit			= R10B_MACID_MAX + 1;
	sc->cam_entry_limit		= R10B_CAM_ENTRY_COUNT;
	sc->fwsize_limit		= R10B_MAX_FW_SIZE;
	sc->temp_delta			= R88E_CALIB_THRESHOLD;

	sc->bcn_status_reg[0]		= R92C_TDECTRL;
	sc->bcn_status_reg[1]		= R92C_TDECTRL;
	sc->rcr				= 0;

	sc->ntxchains			= 1;
	sc->nrxchains			= 1;

	r10bu_attach_private(sc);

	/* Under all register access: the RTL8710B's address map. */
	r10bu_attach_reg_methods(sc);
}
