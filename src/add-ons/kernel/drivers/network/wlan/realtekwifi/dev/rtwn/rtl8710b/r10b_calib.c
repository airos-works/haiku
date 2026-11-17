/*
 * Copyright (c) 2026 Mir Saheb Ali <mirsahebali@pm.me>
 * Copyright 2026, Haiku, Inc. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * RTL8710B IQ calibration (path A, the only one).
 *
 * The structure is FreeBSD's r88f_calib.c (save the ADDA/MAC/BB state,
 * up to three runs of a TX then an RX IQK, compare runs, write the IQ
 * matrix), with the RTL8710B's sequence, which differs from the RTL8188F's
 * in most constants, in switching the S0/S1 path selection around each
 * one-shot, in its BB setup and in stricter RX result checks. The
 * sequence and constants are the ones Linux's rtl8xxxu uses for this chip
 * (8710b.c rtl8710bu_iqk_path_a(), rtl8710bu_rx_iqk_path_a(),
 * rtl8710bu_phy_iqcalibrate(), rtl8710bu_phy_iq_calibrate(); core.c
 * rtl8xxxu_gen2_simularity_compare(), rtl8xxxu_fill_iqk_matrix_a());
 * no code is taken from there.
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
#include <dev/rtwn/if_rtwn_debug.h>

#include <dev/rtwn/rtl8192c/r92c_reg.h>

#include <dev/rtwn/rtl8710b/r10b.h>
#include <dev/rtwn/rtl8710b/r10b_reg.h>

#define R10B_ADDA_REGS		16
#define R10B_MAC_REGS		4
#define R10B_BB_REGS		9
#define R10B_IQK_RETRY		2
#define R10B_IQK_TOLERANCE	5

struct r10b_iqk_backup {
	uint32_t	adda[R10B_ADDA_REGS];
	uint32_t	mac[R10B_MAC_REGS];
	uint32_t	bb[R10B_BB_REGS];
	int		pi_mode;
};

/* One run: TX before/after (0xe94/0xe9c), RX before/after (0xea4/0xeac). */
enum { IQK_TX_X, IQK_TX_Y, IQK_RX_X, IQK_RX_Y, IQK_VALUES };
struct r10b_iqk_result {
	uint32_t	v[IQK_VALUES];
};

static const uint16_t iqk_adda_regs[R10B_ADDA_REGS] = {
	R10B_FPGA0_XCD_SWITCH_CTRL, R10B_BLUETOOTH,
	R10B_RX_WAIT_CCA, R10B_TX_CCK_RFON,
	R10B_TX_CCK_BBON, R10B_TX_OFDM_RFON,
	R10B_TX_OFDM_BBON, R10B_TX_TO_RX,
	R10B_TX_TO_TX, R10B_RX_CCK,
	R10B_RX_OFDM, R10B_RX_WAIT_RIFS,
	R10B_RX_TO_RX, R10B_STANDBY,
	R10B_SLEEP, R10B_PMPD_ANAEN
};
static const uint16_t iqk_mac_regs[R10B_MAC_REGS] = {
	R92C_TXPAUSE, R92C_BCN_CTRL(0),
	R92C_BCN_CTRL(1), R92C_GPIO_MUXCFG
};
/* As the RTL8188F's, with CCK0_AFESETTING in place of FPGA0_RFMOD. */
static const uint16_t iqk_bb_regs[R10B_BB_REGS] = {
	R92C_OFDM0_TRXPATHENA, R92C_OFDM0_TRMUXPAR,
	R10B_FPGA0_XCD_RF_SW_CTRL, R92C_CONFIG_ANT(0), R92C_CONFIG_ANT(1),
	R92C_FPGA0_RFIFACESW(0), R92C_FPGA0_RFIFACEOE(0),
	R92C_FPGA0_RFIFACEOE(1), R92C_CCK0_AFESETTING
};

static void
r10b_iqk_save_regs(struct rtwn_softc *sc, struct r10b_iqk_backup *backup)
{
	int i;

	for (i = 0; i < R10B_ADDA_REGS; i++)
		backup->adda[i] = rtwn_bb_read(sc, iqk_adda_regs[i]);
	for (i = 0; i < R10B_MAC_REGS - 1; i++)
		backup->mac[i] = rtwn_read_1(sc, iqk_mac_regs[i]);
	backup->mac[i] = rtwn_read_4(sc, iqk_mac_regs[i]);
	for (i = 0; i < R10B_BB_REGS; i++)
		backup->bb[i] = rtwn_bb_read(sc, iqk_bb_regs[i]);
}

static void
r10b_iqk_restore_regs(struct rtwn_softc *sc,
    const struct r10b_iqk_backup *backup)
{
	int i;

	for (i = 0; i < R10B_ADDA_REGS; i++)
		rtwn_bb_write(sc, iqk_adda_regs[i], backup->adda[i]);
	for (i = 0; i < R10B_MAC_REGS - 1; i++)
		rtwn_write_1(sc, iqk_mac_regs[i], backup->mac[i]);
	rtwn_write_4(sc, iqk_mac_regs[i], backup->mac[i]);
	for (i = 0; i < R10B_BB_REGS; i++)
		rtwn_bb_write(sc, iqk_bb_regs[i], backup->bb[i]);
}

static void
r10b_iqk_mode(struct rtwn_softc *sc, int on)
{
	rtwn_bb_setbits(sc, R92C_FPGA0_IQK, 0xffffff00, on ? 0x80800000 : 0);
}

/* PA/PAD gain for the IQK tone; `pad' replaces the bits of `pad_mask'. */
static void
r10b_iqk_rf_setup(struct rtwn_softc *sc, uint32_t rck_os, uint32_t txpa_g2,
    uint32_t pad_mask, uint32_t pad)
{
	rtwn_rf_setbits(sc, 0, R10B_RF_WE_LUT, 0, 0x80000);
	rtwn_rf_write(sc, 0, R92C_RF_RCK_OS, rck_os);
	rtwn_rf_write(sc, 0, R92C_RF_TXPA_G(0), 0x0000f);
	rtwn_rf_write(sc, 0, R92C_RF_TXPA_G(1), txpa_g2);
	rtwn_rf_setbits(sc, 0, R10B_RF_GAIN_CCA, 0, 0x00800);
	rtwn_rf_setbits(sc, 0, R10B_RF_PAD_TXG, pad_mask, pad);
}

static void
r10b_iqk_one_shot(struct rtwn_softc *sc, uint32_t first)
{
	rtwn_bb_write(sc, R92C_IQK_AGC_PTS, first);
	rtwn_bb_write(sc, R92C_IQK_AGC_PTS, 0xf8000000);
	rtwn_delay(sc, 10000);
}

/* The TX IQK of the last one-shot, as 10-bit values. */
static int
r10b_iqk_tx_ok(struct rtwn_softc *sc, uint32_t *x, uint32_t *y)
{
	uint32_t reg_eac = rtwn_bb_read(sc, R92C_RX_POWER_IQK_AFTER(0));

	*x = (rtwn_bb_read(sc, R92C_TX_POWER_IQK_BEFORE(0)) >> 16) & 0x3ff;
	*y = (rtwn_bb_read(sc, R92C_TX_POWER_IQK_AFTER(0)) >> 16) & 0x3ff;

	return (!(reg_eac & 0x10000000) && *x != 0x142 && *y != 0x042);
}

/* TX IQK and LO calibration; the LO result is kept in *lok. */
static int
r10b_iqk_tx(struct rtwn_softc *sc, uint32_t *lok,
    struct r10b_iqk_result *res)
{
	uint32_t path_sel;

	path_sel = rtwn_bb_read(sc, R10B_S0S1_PATH_SWITCH);
	rtwn_bb_write(sc, R10B_S0S1_PATH_SWITCH, 0x99000000);

	r10b_iqk_mode(sc, 0);
	r10b_iqk_rf_setup(sc, 0x20000, 0x07ff7, 0x00fff, 0x1ed);
	r10b_iqk_mode(sc, 1);

	rtwn_bb_write(sc, R92C_TX_IQK_TONE(0), 0x18008c1c);
	rtwn_bb_write(sc, R92C_RX_IQK_TONE(0), 0x38008c1c);
	rtwn_bb_write(sc, R92C_TX_IQK_PI(0), 0x821403ff);
	rtwn_bb_write(sc, R92C_RX_IQK_PI(0), 0x28160c06);
	rtwn_bb_write(sc, R92C_IQK_AGC_RSP, 0x02002911);
	r10b_iqk_one_shot(sc, 0xfa000000);

	rtwn_bb_write(sc, R10B_S0S1_PATH_SWITCH, path_sel);
	r10b_iqk_mode(sc, 0);
	rtwn_rf_setbits(sc, 0, R10B_RF_GAIN_CCA, 0x00800, 0);

	*lok = rtwn_rf_read(sc, 0, R92C_RF_TXM_IDAC);

	return (r10b_iqk_tx_ok(sc, &res->v[IQK_TX_X], &res->v[IQK_TX_Y]));
}

/*
 * RX IQK: a TX IQK under RX gain settings first, whose result feeds the
 * RX one-shot. Returns true when both passed.
 */
static int
r10b_iqk_rx(struct rtwn_softc *sc, uint32_t lok, struct r10b_iqk_result *res)
{
	uint32_t path_sel, x, y, reg_ea4, reg_eac, rx_x, rx_y, mag;

	path_sel = rtwn_bb_read(sc, R10B_S0S1_PATH_SWITCH);
	rtwn_bb_write(sc, R10B_S0S1_PATH_SWITCH, 0x99000000);

	/* TX IQK with the RX IQK mode table. */
	r10b_iqk_mode(sc, 0);
	r10b_iqk_rf_setup(sc, 0x30000, 0xf1173, 0x003e0, 0xf << 5);
	r10b_iqk_mode(sc, 1);

	rtwn_bb_write(sc, R92C_TX_IQK_TONE(0), 0x18008c1c);
	rtwn_bb_write(sc, R92C_RX_IQK_TONE(0), 0x38008c1c);
	rtwn_bb_write(sc, R92C_TX_IQK_PI(0), 0x8216129f);
	rtwn_bb_write(sc, R92C_RX_IQK_PI(0), 0x28160c00);
	rtwn_bb_write(sc, R92C_TX_IQK, 0x01007c00);
	rtwn_bb_write(sc, R92C_RX_IQK, 0x01004800);
	rtwn_bb_write(sc, R92C_IQK_AGC_RSP, 0x0046a911);
	r10b_iqk_one_shot(sc, 0xf9000000);

	if (!r10b_iqk_tx_ok(sc, &x, &y)) {
		/* TX failed: RX would be meaningless. */
		rtwn_bb_write(sc, R10B_S0S1_PATH_SWITCH, path_sel);
		r10b_iqk_mode(sc, 0);
		rtwn_rf_setbits(sc, 0, R10B_RF_GAIN_CCA, 0x00800, 0);
		return (0);
	}
	rtwn_bb_write(sc, R92C_TX_IQK, 0x80007c00 | (x << 16) | y);

	/* RX IQK. */
	r10b_iqk_mode(sc, 0);
	r10b_iqk_rf_setup(sc, 0x30000, 0xf7ff2, 0x00fff, 0x02a);
	r10b_iqk_mode(sc, 1);

	rtwn_bb_write(sc, R92C_RX_IQK, 0x01004800);
	rtwn_bb_write(sc, R92C_TX_IQK_TONE(0), 0x38008c1c);
	rtwn_bb_write(sc, R92C_RX_IQK_TONE(0), 0x18008c1c);
	rtwn_bb_write(sc, R92C_RX_IQK_PI(0), 0x2816169f);
	rtwn_bb_write(sc, R92C_IQK_AGC_RSP, 0x0046a911);
	r10b_iqk_one_shot(sc, 0xf9000000);

	rtwn_bb_write(sc, R10B_S0S1_PATH_SWITCH, path_sel);
	r10b_iqk_mode(sc, 0);
	rtwn_rf_setbits(sc, 0, R10B_RF_GAIN_CCA, 0x00800, 0);

	/* Reload the LO calibration of the TX IQK. */
	rtwn_rf_write(sc, 0, R92C_RF_TXM_IDAC, lok);

	reg_eac = rtwn_bb_read(sc, R92C_RX_POWER_IQK_AFTER(0));
	reg_ea4 = rtwn_bb_read(sc, R92C_RX_POWER_IQK_BEFORE(0));
	rx_x = (reg_ea4 >> 16) & 0x3ff;
	rx_y = (reg_eac >> 16) & 0x3ff;
	/* |Y| of the 10-bit two's complement value. */
	mag = (rx_y & 0x200) ? 0x400 - rx_y : rx_y;

	res->v[IQK_RX_X] = rx_x;
	res->v[IQK_RX_Y] = rx_y;

	return (!(reg_eac & 0x08000000) && rx_x != 0x132 && rx_y != 0x036 &&
	    rx_x > 0xe6 && rx_x < 0x11a && mag < 0x1a);
}

static void
r10b_iq_calib_run(struct rtwn_softc *sc, struct r10b_iqk_backup *backup,
    struct r10b_iqk_result *res, int t)
{
	uint32_t rx_initial_gain, lok = 0, path_sel_bb, path_sel_rf;
	int i, ok;

	rx_initial_gain = rtwn_bb_read(sc, R92C_OFDM0_AGCCORE1(0));

	if (t == 0)
		r10b_iqk_save_regs(sc, backup);

	/* Path A ADDA on. */
	for (i = 0; i < R10B_ADDA_REGS; i++)
		rtwn_bb_write(sc, iqk_adda_regs[i], R10B_ADDA_1T);

	if (t == 0) {
		backup->pi_mode = !!(rtwn_bb_read(sc, R92C_HSSI_PARAM1(0)) &
		    R92C_HSSI_PARAM1_PI);
	}
	if (!backup->pi_mode) {
		/*
		 * IQK needs the BB in PI mode. Like the vendor driver, it is
		 * not switched back to SI afterwards.
		 */
		rtwn_bb_write(sc, R92C_HSSI_PARAM1(0), 0x01000100);
		rtwn_bb_write(sc, R92C_HSSI_PARAM1(1), 0x01000100);
	}

	/* MAC: no TX during the calibration. */
	rtwn_bb_setbits(sc, R92C_TX_PTCL_CTRL, 0, 0x00ff0000);

	path_sel_bb = rtwn_bb_read(sc, R10B_S0S1_PATH_SWITCH);
	path_sel_rf = rtwn_rf_read(sc, 0, R10B_RF_S0S1);

	/* BB setting. */
	rtwn_bb_setbits(sc, R92C_CCK0_AFESETTING, 0, 0x0f000000);
	rtwn_bb_write(sc, R10B_RX_WAIT_CCA, 0x03c00010);
	rtwn_bb_write(sc, R92C_OFDM0_TRXPATHENA, 0x03a05601);
	rtwn_bb_write(sc, R92C_OFDM0_TRMUXPAR, 0x000800e4);
	rtwn_bb_write(sc, R10B_FPGA0_XCD_RF_SW_CTRL, 0x25204000);

	r10b_iqk_mode(sc, 1);
	rtwn_bb_write(sc, R92C_TX_IQK, 0x01007c00);
	rtwn_bb_write(sc, R92C_RX_IQK, 0x01004800);

	/* A failed step reads as X = 0x100, Y = 0 (unity, no correction). */
	ok = 0;
	for (i = 0; i < R10B_IQK_RETRY && !ok; i++)
		ok = r10b_iqk_tx(sc, &lok, res);
	if (!ok) {
		res->v[IQK_TX_X] = 0x100;
		res->v[IQK_TX_Y] = 0;
	}

	ok = 0;
	for (i = 0; i < R10B_IQK_RETRY && !ok; i++)
		ok = r10b_iqk_rx(sc, lok, res);
	if (!ok) {
		res->v[IQK_RX_X] = 0x100;
		res->v[IQK_RX_Y] = 0;
		RTWN_DPRINTF(sc, RTWN_DEBUG_CALIB, "%s: path A RX IQK failed\n",
		    __func__);
	}

	r10b_iqk_mode(sc, 0);

	if (t == 0)
		return;

	/* Reload ADDA power saving, MAC and BB parameters, RF path. */
	r10b_iqk_restore_regs(sc, backup);
	rtwn_bb_write(sc, R10B_S0S1_PATH_SWITCH, path_sel_bb);
	rtwn_rf_write(sc, 0, R10B_RF_S0S1, path_sel_rf);

	/* Restore RX initial gain (through 0x50 to latch it). */
	rtwn_bb_setbits(sc, R92C_OFDM0_AGCCORE1(0), 0xff, 0x50);
	rtwn_bb_setbits(sc, R92C_OFDM0_AGCCORE1(0), 0xff,
	    rx_initial_gain & 0xff);

	/* Load 0xe30 IQC default value. */
	rtwn_bb_write(sc, R92C_TX_IQK_TONE(0), 0x01008c00);
	rtwn_bb_write(sc, R92C_RX_IQK_TONE(0), 0x01008c00);
}

static int32_t
r10b_iqk_sext(uint32_t v)
{
	return ((v & 0x200) ? (int32_t)(v | 0xfffffc00) : (int32_t)v);
}

/*
 * Compares runs c1 and c2 within the vendor's tolerance. Returns true if
 * they agree. Otherwise the parts that agree (TX, RX), or the TX part of
 * a run when the other's RX part is missing, are collected in `best'.
 */
static int
r10b_iqk_similar(const struct r10b_iqk_result *r1,
    const struct r10b_iqk_result *r2, struct r10b_iqk_result *best)
{
	const struct r10b_iqk_result *keep = NULL;
	uint32_t differ = 0;
	int32_t a, b;
	int i;

	for (i = 0; i < IQK_VALUES; i++) {
		if (i & 1) {
			a = r10b_iqk_sext(r1->v[i]);
			b = r10b_iqk_sext(r2->v[i]);
		} else {
			a = r1->v[i];
			b = r2->v[i];
		}
		if (abs(a - b) <= R10B_IQK_TOLERANCE)
			continue;
		if (i == IQK_RX_X && differ == 0) {
			/* One run without RX results: keep the other's TX. */
			if (r1->v[IQK_RX_X] + r1->v[IQK_RX_Y] == 0) {
				keep = r2;
				continue;
			}
			if (r2->v[IQK_RX_X] + r2->v[IQK_RX_Y] == 0) {
				keep = r1;
				continue;
			}
		}
		differ |= 1 << i;
	}

	if (differ == 0) {
		if (keep == NULL)
			return (1);
		best->v[IQK_TX_X] = keep->v[IQK_TX_X];
		best->v[IQK_TX_Y] = keep->v[IQK_TX_Y];
		return (0);
	}

	if (!(differ & ((1 << IQK_TX_X) | (1 << IQK_TX_Y)))) {
		best->v[IQK_TX_X] = r1->v[IQK_TX_X];
		best->v[IQK_TX_Y] = r1->v[IQK_TX_Y];
	}
	if (!(differ & ((1 << IQK_RX_X) | (1 << IQK_RX_Y)))) {
		best->v[IQK_RX_X] = r1->v[IQK_RX_X];
		best->v[IQK_RX_Y] = r1->v[IQK_RX_Y];
	}
	return (0);
}

/* The IQ correction matrix for path A (as r88e_calib.c writes it). */
static void
r10b_iqk_write_matrix(struct rtwn_softc *sc,
    const struct r10b_iqk_result *res, int tx_only)
{
	uint32_t reg, oldval, x, tx_a;
	int32_t y, tx_c;

	reg = rtwn_bb_read(sc, R92C_OFDM0_TXIQIMBALANCE(0));
	oldval = reg >> 22;

	x = r10b_iqk_sext(res->v[IQK_TX_X]);
	tx_a = (x * oldval) >> 8;
	rtwn_bb_setbits(sc, R92C_OFDM0_TXIQIMBALANCE(0), 0x3ff, tx_a & 0x3ff);
	rtwn_bb_setbits(sc, R92C_OFDM0_ECCATHRESHOLD, 0x80000000,
	    ((x * oldval >> 7) & 0x1) << 31);

	y = r10b_iqk_sext(res->v[IQK_TX_Y]);
	tx_c = (y * (int32_t)oldval) >> 8;
	rtwn_bb_setbits(sc, R92C_OFDM0_TXAFE(0), 0xf0000000,
	    ((tx_c & 0x3c0) >> 6) << 28);
	rtwn_bb_setbits(sc, R92C_OFDM0_TXIQIMBALANCE(0), 0x003f0000,
	    (tx_c & 0x3f) << 16);
	rtwn_bb_setbits(sc, R92C_OFDM0_ECCATHRESHOLD, 0x20000000,
	    ((y * (int32_t)oldval >> 7) & 0x1) << 29);

	if (tx_only) {
		RTWN_DPRINTF(sc, RTWN_DEBUG_CALIB, "%s: only TX\n", __func__);
		return;
	}

	rtwn_bb_setbits(sc, R92C_OFDM0_RXIQIMBALANCE(0), 0x3ff,
	    res->v[IQK_RX_X] & 0x3ff);
	rtwn_bb_setbits(sc, R92C_OFDM0_RXIQIMBALANCE(0), 0xfc00,
	    (res->v[IQK_RX_Y] & 0x3f) << 10);
	rtwn_bb_setbits(sc, R92C_OFDM0_RXIQEXTANTA, 0xf0000000,
	    ((res->v[IQK_RX_Y] >> 6) & 0xf) << 28);
}

/*
 * LC calibration: the RTL8188F's (r88f_lc_calib(), rtl8xxxu's
 * rtl8188f_phy_lc_calibrate(), which rtl8xxxu uses for this chip too),
 * polling every 10 ms as rtl8xxxu does rather than every 10 us: over USB
 * a hundred quick polls can end before the calibration does, and writing
 * the RF mode back then cuts it short.
 */
void
r10b_lc_calib(struct rtwn_softc *sc)
{
	uint32_t lstf, rf_amode;
	int i;

	/* Stop continuous TX, or block all queues for packet TX. */
	lstf = rtwn_bb_read(sc, R92C_OFDM1_LSTF);
	if (lstf & 0x70000000)
		rtwn_bb_write(sc, R92C_OFDM1_LSTF, lstf & ~0x70000000);
	else
		rtwn_write_1(sc, R92C_TXPAUSE, R92C_TX_QUEUE_ALL);

	rf_amode = rtwn_rf_read(sc, 0, R92C_RF_CHNLBW);
	rtwn_rf_write(sc, 0, R92C_RF_CHNLBW, rf_amode | 0x08000);
	for (i = 0; i < 100; i++) {
		if ((rtwn_rf_read(sc, 0, R92C_RF_CHNLBW) & 0x08000) == 0)
			break;
		rtwn_delay(sc, 10000);
	}
	if (i == 100)
		device_printf(sc->sc_dev, "LC calibration timed out\n");
	rtwn_rf_write(sc, 0, R92C_RF_CHNLBW, rf_amode);

	if (lstf & 0x70000000)
		rtwn_bb_write(sc, R92C_OFDM1_LSTF, lstf);
	else
		rtwn_write_1(sc, R92C_TXPAUSE, 0);
}

void
r10b_iq_calib(struct rtwn_softc *sc)
{
	struct r10b_iqk_backup backup;
	/* Runs 0..2 and the best parts collected from them. */
	struct r10b_iqk_result res[4];
	const struct r10b_iqk_result *final = NULL;
	uint32_t path_sel_bb;
	int i, j, sum;

	path_sel_bb = rtwn_bb_read(sc, R10B_S0S1_PATH_SWITCH);
	memset(res, 0, sizeof(res));
	memset(&backup, 0, sizeof(backup));

	for (i = 0; i < 3; i++) {
		r10b_iq_calib_run(sc, &backup, &res[i], i);
		RTWN_DPRINTF(sc, RTWN_DEBUG_CALIB,
		    "IQK run %d: TX %#x/%#x RX %#x/%#x\n", i,
		    res[i].v[IQK_TX_X], res[i].v[IQK_TX_Y],
		    res[i].v[IQK_RX_X], res[i].v[IQK_RX_Y]);

		if (i == 1 && r10b_iqk_similar(&res[0], &res[1], &res[3])) {
			final = &res[0];
			break;
		}
		if (i == 2) {
			if (r10b_iqk_similar(&res[0], &res[2], &res[3]))
				final = &res[0];
			else if (r10b_iqk_similar(&res[1], &res[2], &res[3]))
				final = &res[1];
			else {
				for (j = 0, sum = 0; j < IQK_VALUES; j++)
					sum += res[3].v[j];
				if (sum != 0)
					final = &res[3];
			}
		}
	}

	if (final == NULL)
		device_printf(sc->sc_dev, "IQ calibration failed\n");
	else if (final->v[IQK_TX_X] != 0)
		r10b_iqk_write_matrix(sc, final, final->v[IQK_RX_X] == 0);

	rtwn_bb_write(sc, R10B_S0S1_PATH_SWITCH, path_sel_bb);
}
