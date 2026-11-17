/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

/*
 * RTL8710B register tables, BB/RF/MAC initialisation.
 *
 * The sequence is the one Linux's rtl8xxxu runs for this chip
 * (core.c rtl8xxxu_init_device(), rtl8xxxu_init_phy_rf(),
 * rtl8xxxu_init_burst(); 8710b.c rtl8710bu_init_phy_bb(),
 * rtl8710b_enable_rf(), rtl8710bu_init_statistics(),
 * rtl8710b_set_crystal_cap()), mapped onto rtwn's init hooks the way
 * FreeBSD's rtl8188f code does it for the RTL8188F; the code is rtwn's
 * own. The register values come from the table file.
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
#include <sys/firmware.h>

#include <net/if.h>
#include <net/ethernet.h>
#include <net/if_media.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>
#include <dev/rtwn/if_rtwn_debug.h>

#include <dev/rtwn/rtl8192c/r92c.h>
#include <dev/rtwn/rtl8192c/r92c_reg.h>

#include <dev/rtwn/rtl8812a/r12a_reg.h>

#include <dev/rtwn/rtl8710b/r10b.h>
#include <dev/rtwn/rtl8710b/r10b_reg.h>
#include <dev/rtwn/rtl8710b/r10b_tables.h>
#include <dev/rtwn/rtl8710b/r10b_var.h>

void
r10b_tables_free(struct rtwn_softc *sc)
{
	struct r10b_softc *rs = R10B_SOFTC(sc);

	free(rs->mac, M_RTWN_PRIV);
	free(rs->bb, M_RTWN_PRIV);
	free(rs->agc, M_RTWN_PRIV);
	free(rs->rf, M_RTWN_PRIV);
	rs->mac = NULL;
	rs->bb = rs->agc = rs->rf = NULL;
	rs->mac_count = rs->bb_count = rs->agc_count = rs->rf_count = 0;
	sc->mac_prog = NULL;
	sc->mac_size = 0;
}

static struct r10b_reg_val *
r10b_tables_copy(const struct r10b_tbl_ref *ref)
{
	struct r10b_reg_val *table;
	uint32_t i, reg, val;

	table = malloc(ref->count * sizeof(*table), M_RTWN_PRIV, M_WAITOK);
	for (i = 0; i < ref->count; i++) {
		r10b_tbl_entry(ref, i, &reg, &val);
		table[i].reg = reg;
		table[i].val = val;
	}
	return (table);
}

/*
 * Loads, checks and keeps the register tables for this chip's package
 * (called with the softc locked, from r10b_efuse_preread()).
 */
int
r10b_tables_load(struct rtwn_softc *sc)
{
	static const uint16_t cck_filter_regs[4] = {
		R10B_CCK0_TX_FILTER1, R10B_CCK0_TX_FILTER2,
		R10B_CCK0_DEBUG_PORT, R10B_CCK0_TX_FILTER3
	};
	struct r10b_softc *rs = R10B_SOFTC(sc);
	const struct firmware *fw;
	const struct r10b_tbl_ref *mac;
	struct r10b_tbl_set set;
	const char *why;
	uint32_t i, j, found = 0;
	int error = 0;

	RTWN_UNLOCK(sc);
	fw = firmware_get(R10B_TABLES_NAME);
	if (fw == NULL) {
		device_printf(sc->sc_dev,
		    "cannot load %s: the RTL8710B's register tables are not "
		    "part of this driver (GPL-2.0 data, made from Linux's "
		    "rtl8xxxu); install the Realtek Wi-Fi firmware package "
		    "that has firmware/realtekwifi/rtwn-rtl8710b-tables.bin\n",
		    R10B_TABLES_NAME);
		error = ENOENT;
		goto out;
	}
	if (r10b_tbl_parse(fw->data, fw->datasize, rs->package, &set,
	    &why) != 0) {
		device_printf(sc->sc_dev, "cannot use %s (%zu bytes): %s\n",
		    R10B_TABLES_NAME, (size_t)fw->datasize, why);
		error = EINVAL;
		goto put;
	}

	r10b_tables_free(sc);

	mac = &set.tables[R10B_TBL_KIND_MAC - 1];
	rs->mac = malloc(mac->count * sizeof(*rs->mac), M_RTWN_PRIV, M_WAITOK);
	for (i = 0; i < mac->count; i++) {
		uint32_t reg, val;

		r10b_tbl_entry(mac, i, &reg, &val);
		rs->mac[i].reg = reg;
		rs->mac[i].val = val;
	}
	rs->mac_count = mac->count;
	rs->bb = r10b_tables_copy(&set.tables[R10B_TBL_KIND_BB - 1]);
	rs->bb_count = set.tables[R10B_TBL_KIND_BB - 1].count;
	rs->agc = r10b_tables_copy(&set.tables[R10B_TBL_KIND_AGC - 1]);
	rs->agc_count = set.tables[R10B_TBL_KIND_AGC - 1].count;
	rs->rf = r10b_tables_copy(&set.tables[R10B_TBL_KIND_RF - 1]);
	rs->rf_count = set.tables[R10B_TBL_KIND_RF - 1].count;
	strlcpy(rs->tables_source, set.source, sizeof(rs->tables_source));

	/* rtwn_mac_init() writes these. */
	sc->mac_prog = rs->mac;
	sc->mac_size = rs->mac_count;

	/* The CCK TX filter of the normal channels, for r10b_set_chan(). */
	for (i = 0; i < (uint32_t)rs->bb_count; i++) {
		for (j = 0; j < nitems(cck_filter_regs); j++) {
			if (rs->bb[i].reg == cck_filter_regs[j]) {
				rs->cck_filter[j] = rs->bb[i].val;
				found |= 1 << j;
			}
		}
	}
	if (found != (1 << nitems(cck_filter_regs)) - 1) {
		device_printf(sc->sc_dev,
		    "%s: BB table lacks the CCK TX filter registers\n",
		    R10B_TABLES_NAME);
		r10b_tables_free(sc);
		error = EINVAL;
		goto put;
	}

	device_printf(sc->sc_dev,
	    "register tables: MAC %d, BB %d, AGC %d, RF %d (%s)\n",
	    rs->mac_count, rs->bb_count, rs->agc_count, rs->rf_count,
	    rs->tables_source);

put:
	firmware_put(fw, FIRMWARE_UNLOAD);
out:
	RTWN_LOCK(sc);
	return (error);
}

void
r10b_init_bb(struct rtwn_softc *sc)
{
	struct r10b_softc *rs = R10B_SOFTC(sc);
	int i;

	/* Enable BB and RF (rtl8710bu_init_phy_bb()). */
	rtwn_setbits_4(sc, R10B_SYS_FUNC, 0,
	    R10B_SYS_FUNC_BB | R10B_SYS_FUNC_RF);

	for (i = 0; i < rs->bb_count; i++) {
		RTWN_DPRINTF(sc, RTWN_DEBUG_RESET, "BB: reg 0x%03x, val 0x%08x\n",
		    rs->bb[i].reg, rs->bb[i].val);
		rtwn_bb_write(sc, rs->bb[i].reg, rs->bb[i].val);
		rtwn_delay(sc, 1);
	}
	for (i = 0; i < rs->agc_count; i++) {
		RTWN_DPRINTF(sc, RTWN_DEBUG_RESET, "AGC: reg 0x%03x, val 0x%08x\n",
		    rs->agc[i].reg, rs->agc[i].val);
		rtwn_bb_write(sc, rs->agc[i].reg, rs->agc[i].val);
		rtwn_delay(sc, 1);
	}
}

void
r10b_init_rf(struct rtwn_softc *sc)
{
	struct r10b_softc *rs = R10B_SOFTC(sc);
	uint32_t delay;
	uint16_t type;
	int i;

	/* Save RF_ENV control type. */
	type = rtwn_read_2(sc, R92C_FPGA0_RFIFACESW(0)) & 0x10;

	/* Set RF_ENV enable and output high. */
	rtwn_bb_setbits(sc, R92C_FPGA0_RFIFACEOE(0), 0, 0x100000);
	rtwn_delay(sc, 1);
	rtwn_bb_setbits(sc, R92C_FPGA0_RFIFACEOE(0), 0, 0x10);
	rtwn_delay(sc, 1);

	/* Set address and data lengths of RF registers. */
	rtwn_bb_setbits(sc, R92C_HSSI_PARAM2(0),
	    R92C_HSSI_PARAM2_ADDR_LENGTH, 0);
	rtwn_delay(sc, 1);
	rtwn_bb_setbits(sc, R92C_HSSI_PARAM2(0),
	    R92C_HSSI_PARAM2_DATA_LENGTH, 0);
	rtwn_delay(sc, 1);

	/* RF path A initialisation values, with the vendor's delay codes. */
	for (i = 0; i < rs->rf_count; i++) {
		delay = r10b_rf_delay_us(rs->rf[i].reg);
		if (delay != 0) {
			rtwn_delay(sc, delay);
			continue;
		}
		RTWN_DPRINTF(sc, RTWN_DEBUG_RESET, "RF: reg 0x%02x, val 0x%05x\n",
		    rs->rf[i].reg, rs->rf[i].val);
		rtwn_rf_write(sc, 0, rs->rf[i].reg, rs->rf[i].val);
		rtwn_delay(sc, 1);
	}

	/* Restore RF_ENV control type. */
	rtwn_setbits_2(sc, R92C_FPGA0_RFIFACESW(0), 0x10, type);

	/* Cache RF register CHNLBW. */
	rs->rs.rf_chnlbw[0] = rtwn_rf_read(sc, 0, R92C_RF_CHNLBW);

	/*
	 * RF switch control: TR and antenna switches and PAPE, for the
	 * board and the BD control half (rtl8xxxu_init_device(); the
	 * RTL8710B has no PAPE quirk).
	 */
	rtwn_bb_write(sc, R92C_FPGA0_RFIFACESW(0),
	    R10B_RF_SW_CTRL | (R10B_RF_SW_CTRL_BD << 16));

	/* Turn CCK and OFDM blocks on. */
	rtwn_bb_setbits(sc, R92C_FPGA0_RFMOD, 0,
	    R92C_RFMOD_CCK_EN | R92C_RFMOD_OFDM_EN);

	/* Enable RF and path A for RX and TX (rtl8710b_enable_rf()). */
	rtwn_write_1(sc, R92C_RF_CTRL,
	    R92C_RF_CTRL_EN | R92C_RF_CTRL_RSTB | R92C_RF_CTRL_SDMRSTB);
	rtwn_bb_setbits(sc, R92C_OFDM0_TRXPATHENA, 0xff, 0x11);
	rtwn_write_1(sc, R92C_TXPAUSE, 0);
}

/* rtl8xxxu_init_burst() with the RTL8710B's values. */
void
r10b_init_ampdu(struct rtwn_softc *sc)
{
	/* RX DMA: 512-byte bursts (USB high speed), 3 per transfer. */
	rtwn_setbits_1(sc, R10B_RXDMA_PRO,
	    R10B_RXDMA_PRO_BURST_SIZE_M | R10B_RXDMA_PRO_BURST_CNT_M,
	    (1 << R10B_RXDMA_PRO_BURST_SIZE_S) |
	    (3 << R10B_RXDMA_PRO_BURST_CNT_S) | R10B_RXDMA_PRO_DMA_MODE);

	/* Setup AMPDU aggregation. */
	rtwn_setbits_1(sc, R12A_HT_SINGLE_AMPDU, 0,
	    R12A_HT_SINGLE_AMPDU_PKT_ENA);
	rtwn_write_2(sc, R92C_MAX_AGGR_NUM, 0x0c14);
	rtwn_write_1(sc, R12A_AMPDU_MAX_TIME, R10B_AMPDU_MAX_TIME);
	rtwn_write_4(sc, R92C_AGGLEN_LMT, 0xffffffff);
	rtwn_write_1(sc, R92C_RX_PKT_LIMIT, 0x18);
	rtwn_write_1(sc, R92C_PIFS, 0);
	rtwn_write_1(sc, R92C_FWHW_TXQ_CTRL, R92C_FWHW_TXQ_CTRL_AMPDU_RTY_NEW);
	rtwn_write_4(sc, R92C_FAST_EDCA_CTRL, 0x03086666);
	/*
	 * rtl8xxxu uses 0x28; the vendor driver 0x50, which rtl8xxxu found
	 * to slow uploads down.
	 */
	rtwn_write_1(sc, R92C_USTIME_TSF, 0x28);
	rtwn_write_1(sc, R92C_USTIME_EDCA, 0x28);

	/* To prevent bus resetting the mac. */
	rtwn_setbits_1(sc, R92C_RSV_CTRL, 0,
	    R92C_RSV_CTRL_R_DIS_PRST_0 | R92C_RSV_CTRL_R_DIS_PRST_1);
}

/* Noise histogram and false alarm statistics (as r88fu_init_stats()). */
void
r10b_init_stats(struct rtwn_softc *sc)
{
	rtwn_write_2(sc, R10B_NHM_TIMER + 2, 0xc350);
	rtwn_write_2(sc, R10B_NHM_TH9_TH10 + 2, 0xffff);
	rtwn_write_4(sc, R10B_NHM_TH3_TO_TH0, 0xffffff50);
	rtwn_write_4(sc, R10B_NHM_TH7_TO_TH4, 0xffffffff);
	rtwn_setbits_4(sc, R92C_FPGA0_IQK, 0, 0xff);
	rtwn_setbits_4(sc, R10B_NHM_TH9_TH10, 0x00000700, 0x00000100);
	rtwn_setbits_4(sc, R10B_OFDM0_FA_RSTC, 0, 0x00000080);
}

/*
 * The crystal load capacitance from the eFuse, into both halves of the
 * SYSON crystal control register. rtl8xxxu only writes it from its CFO
 * tracking, starting from this value; with no tracking here, set it once.
 */
void
r10b_set_crystal_cap(struct rtwn_softc *sc)
{
	struct r10b_softc *rs = R10B_SOFTC(sc);
	uint32_t reg;
	uint8_t cap;

	if (rs->xtal_k == 0xff)
		return;		/* not calibrated: keep the chip's value */

	cap = rs->xtal_k & 0x3f;
	reg = r10b_syson_read(sc, R10B_SYSON_XTAL_CTRL0);
	if (reg == 0xffffffff)
		return;
	reg &= ~(R10B_SYSON_XTAL_CTRL0_XTAL1 | R10B_SYSON_XTAL_CTRL0_XTAL0);
	reg |= ((uint32_t)cap << 24) | ((uint32_t)cap << 18);
	r10b_syson_write(sc, R10B_SYSON_XTAL_CTRL0, reg);
}

/*
 * Start a thermal meter reading: set the trigger bits of RF 0x42 and keep
 * the rest (rtl8xxxu's gen2 thermal meter for the RTL8188F and RTL8710B),
 * where r88e_temp_measure() overwrites the register.
 */
void
r10b_temp_measure(struct rtwn_softc *sc)
{
	rtwn_rf_setbits(sc, 0, R10B_RF_T_METER, 0, 0x30000);
}

void
r10b_postattach(struct rtwn_softc *sc)
{
	struct r92c_softc *rs = sc->sc_priv;
	struct ieee80211com *ic = &sc->sc_ic;

	rs->rs_scan_start = ic->ic_scan_start;
	ic->ic_scan_start = r92c_scan_start;
	rs->rs_scan_end = ic->ic_scan_end;
	ic->ic_scan_end = r92c_scan_end;
}

void
r10b_detach_private(struct rtwn_softc *sc)
{
	r10b_tables_free(sc);
	r92c_detach_private(sc);
}
