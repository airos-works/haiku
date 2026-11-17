/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

/*
 * RTL8710BU power sequences and the USB-side init hooks.
 *
 * The register sequences are the ones Linux's rtl8xxxu runs for this chip
 * (8710b.c rtl8710bu_power_on(), rtl8710b_emu_to_active(),
 * rtl8710bu_power_off(), rtl8710bu_active_to_lps(),
 * rtl8710bu_active_to_emu(), rtl8710bu_init_aggregation(),
 * rtl8710b_usb_quirks(); core.c rtl8xxxu_flush_fifo() and the RTL8710B
 * parts of rtl8xxxu_init_device()); the code is rtwn's own, arranged as
 * FreeBSD's rtl8188f/usb/r88fu_init.c arranges the RTL8188F's.
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
#include <dev/rtwn/rtl8710b/r10b_var.h>
#include <dev/rtwn/rtl8710b/usb/r10bu.h>

#define R10BU_POWER_POLL	500

static int
r10bu_wait_pwr_fsm(struct rtwn_softc *sc, uint8_t bit)
{
	int ntries;

	uint8_t reg;

	for (ntries = 0; ntries < R10BU_POWER_POLL; ntries++) {
		reg = rtwn_read_1(sc, R10B_PWR_FSM);
		if (reg == 0xff)	/* USB read failed: device gone */
			return (EIO);
		if (!(reg & bit))
			return (0);
		rtwn_delay(sc, 10);
	}
	return (ETIMEDOUT);
}

int
r10bu_power_on(struct rtwn_softc *sc)
{
#define RTWN_CHK(res) do {	\
	if (res != 0)		\
		return (EIO);	\
} while (0)
	uint32_t reg;

	/* Card disabled -> card emulation. */
	RTWN_CHK(rtwn_write_1(sc, R10B_USB_ACCESS_TIMEOUT, 0x80));
	RTWN_CHK(rtwn_setbits_1(sc, R92C_SYS_ISO_CTRL, 0x20, 0));
	RTWN_CHK(rtwn_setbits_1(sc, R10B_SYS_FUNC, 0, R10B_SYS_FUNC_BIT0));
	RTWN_CHK(rtwn_setbits_1(sc, R10B_PWR_FSM, 0, R10B_PWR_FSM_ON));
	RTWN_CHK(rtwn_write_1(sc, R10B_AFE_CTRL, 0));
	RTWN_CHK(rtwn_setbits_1(sc, R10B_WL_STATUS, 0, R10B_WL_STATUS_BIT1));

	/* Card emulation -> active; AFE in LDO mode. */
	RTWN_CHK(rtwn_setbits_1(sc, R10B_AFE_PWR_MODE, 0x01, 0));
	RTWN_CHK(rtwn_setbits_1(sc, R10B_SYS_FUNC, 0, R10B_SYS_FUNC_BIT0));
	RTWN_CHK(rtwn_write_1(sc, R10B_PWR_056, 0x0e));
	RTWN_CHK(rtwn_setbits_1(sc, R10B_PWR_FSM, 0, R10B_PWR_FSM_ON));
	if (r10bu_wait_pwr_fsm(sc, R10B_PWR_FSM_ON) != 0) {
		device_printf(sc->sc_dev,
		    "timeout waiting for chip power up\n");
		return (ETIMEDOUT);
	}

	/* Enable MAC DMA/WMAC/SCHEDULE/SEC blocks. */
	RTWN_CHK(rtwn_write_2(sc, R92C_CR, 0));
	RTWN_CHK(rtwn_setbits_2(sc, R92C_CR, 0,
	    R92C_CR_HCI_TXDMA_EN | R92C_CR_TXDMA_EN |
	    R92C_CR_HCI_RXDMA_EN | R92C_CR_RXDMA_EN |
	    R92C_CR_PROTOCOL_EN | R92C_CR_SCHEDULE_EN |
	    R92C_CR_ENSEC | R92C_CR_CALTMR_EN));

	/* Hardware sequence numbers. */
	RTWN_CHK(rtwn_setbits_1(sc, R92C_HWSEQ_CTRL, 0, 0x7f));
	rtwn_delay(sc, 2);

	/*
	 * What the vendor driver does at the start of its HAL init: a
	 * host interface bit, and keep the Cortex-M4 from suspending while
	 * the Wi-Fi side runs.
	 */
	RTWN_CHK(rtwn_setbits_1(sc, R10B_HCI_PWR_FEF9, 0x01, 0));
	reg = r10b_syson_read(sc, R10B_SYSON_SUSPEND_CTRL);
	if (reg == 0xffffffff)
		return (EIO);
	RTWN_CHK(r10b_syson_write(sc, R10B_SYSON_SUSPEND_CTRL,
	    reg & ~R10B_SYSON_SUSPEND_CM4));

	return (0);
#undef RTWN_CHK
}

#define R10B_RXPKT_NUM_RW_RELEASE_EN	0x00040000
#define R10B_RXPKT_NUM_RXDMA_IDLE	0x00020000

static void
r10bu_flush_fifo(struct rtwn_softc *sc)
{
	int ntries;

	rtwn_write_1(sc, R92C_TXPAUSE, R92C_TX_QUEUE_ALL);
	rtwn_setbits_4(sc, R92C_RXPKT_NUM, 0, R10B_RXPKT_NUM_RW_RELEASE_EN);
	for (ntries = 0; ntries < 100; ntries++) {
		/* All ones (a failed read) also has the idle bit. */
		if (rtwn_read_4(sc, R92C_RXPKT_NUM) & R10B_RXPKT_NUM_RXDMA_IDLE)
			break;
	}
	if (ntries == 100)
		device_printf(sc->sc_dev, "%s: RX DMA does not go idle\n",
		    __func__);

	rtwn_write_2(sc, R92C_RQPN_NPQ, 0);
	rtwn_write_4(sc, R92C_RQPN, R92C_RQPN_LD);
	rtwn_delay(sc, 2000);
}

void
r10bu_power_off(struct rtwn_softc *sc)
{
	uint32_t reg;
	int error, ntries;

	/* Block TX, let RX DMA drain. */
	error = rtwn_write_1(sc, R92C_TXPAUSE, R92C_TX_QUEUE_ALL);
	if (error == ENXIO)	/* hardware gone */
		return;
	r10bu_flush_fifo(sc);

	/* Mask and clear interrupts. */
	rtwn_write_4(sc, R10B_HISR0, 0xffffffff);
	r10bu_write_4_direct(sc, R10B_HIMR0, 0);

	/* Let the Cortex-M4 suspend again. */
	reg = r10b_syson_read(sc, R10B_SYSON_SUSPEND_CTRL);
	if (reg != 0xffffffff) {
		r10b_syson_write(sc, R10B_SYSON_SUSPEND_CTRL,
		    reg | R10B_SYSON_SUSPEND_CM4);
	}

	/* Stop RX. */
	rtwn_write_1(sc, R92C_CR, 0);

	/* Active -> low power: wait for the scheduler to go idle. */
	rtwn_write_1(sc, R92C_TXPAUSE, R92C_TX_QUEUE_ALL);
	for (ntries = 0; ntries < 100; ntries++) {
		reg = rtwn_read_4(sc, R10B_SCH_TX_CMD);
		if (reg == 0 || reg == 0xffffffff)
			break;
		rtwn_delay(sc, 10);
	}
	if (ntries == 100 || reg == 0xffffffff) {
		device_printf(sc->sc_dev, "%s: failed to flush TX queue\n",
		    __func__);
	} else {
		/* CCK and OFDM off, clock gated, then the whole BB reset. */
		rtwn_setbits_1(sc, R92C_SYS_FUNC_EN,
		    R92C_SYS_FUNC_EN_BBRSTB, 0);
		rtwn_delay(sc, 2);
		rtwn_setbits_1(sc, R92C_SYS_FUNC_EN,
		    R92C_SYS_FUNC_EN_BB_GLB_RST, 0);

		/* Reset MAC TRX, security off. */
		rtwn_setbits_2(sc, R92C_CR, 0x00ff | R92C_CR_ENSEC,
		    R92C_CR_HCI_TXDMA_EN | R92C_CR_HCI_RXDMA_EN);

		/* Respond TxOK to the scheduler. */
		rtwn_setbits_1(sc, R92C_DUAL_TSF_RST, 0, 0x20);
	}

	/* Hold the MCU in reset, clear its ready state. */
	rtwn_setbits_1(sc, R10B_8051FW_CTRL + 3,
	    R10B_8051FW_CTRL_MCU_EN >> 24, 0);
	rtwn_write_1(sc, R10B_8051FW_CTRL, 0);

	/* Active -> card emulation: RF off, BB reset, MAC off. */
	rtwn_setbits_4(sc, R10B_SYS_FUNC, R10B_SYS_FUNC_RF, 0);
	rtwn_setbits_4(sc, R10B_SYS_FUNC, R10B_SYS_FUNC_BB, 0);
	rtwn_setbits_1(sc, R10B_PWR_FSM, 0, R10B_PWR_FSM_OFF);
	if (r10bu_wait_pwr_fsm(sc, R10B_PWR_FSM_OFF) != 0)
		device_printf(sc->sc_dev, "%s: could not turn off MAC\n",
		    __func__);
}

void
r10bu_init_intr(struct rtwn_softc *sc)
{
	/* Nothing is taken from the interrupt endpoint. */
	r10bu_write_4_direct(sc, R10B_HIMR0, 0);
}

void
r10bu_init_rx_agg(struct rtwn_softc *sc)
{
	/* No RX aggregation: one frame per bulk transfer. */
	rtwn_setbits_1(sc, R92C_TRXDMA_CTRL, R92C_TRXDMA_CTRL_RXDMA_AGG_EN, 0);
	rtwn_setbits_4(sc, R92C_RXDMA_AGG_PG_TH, 0x8000ff0f, 0);
}

void
r10bu_enable_mac_txrx(struct rtwn_softc *sc)
{
	rtwn_setbits_2(sc, R92C_CR, 0, R92C_CR_MACTXEN | R92C_CR_MACRXEN);
}

void
r10bu_post_init(struct rtwn_softc *sc)
{
	struct r10b_softc *rs = R10B_SOFTC(sc);

	/* TX reports (for net80211 rate control), every 0xcdf0 * 32 us. */
	rtwn_setbits_1(sc, R10B_TX_RPT_CTRL, 0, R10B_TX_RPT_CTRL_TIMER_EN);
	rtwn_write_1(sc, R10B_TX_RPT_CTRL + 1, 2);	/* max MACID */
	rtwn_write_2(sc, R10B_TX_RPT_TIME, 0xcdf0);
	rtwn_setbits_1(sc, 0xa3, 0x07, 0);
	rtwn_write_1(sc, R10B_EARLY_MODE_CTRL, 0);

	/* Packet lifetime, 256 ms (unit 256 us). */
	rtwn_write_2(sc, R10B_PKT_VO_VI_LIFE_TIME, 0x0400);
	rtwn_write_2(sc, R10B_PKT_BE_BK_LIFE_TIME, 0x0400);

	r10b_init_stats(sc);

	/* Perform LC and IQ calibrations. */
	r10b_lc_calib(sc);
	r10b_iq_calib(sc);

	/* Enable the thermal meter. */
	rtwn_rf_setbits(sc, 0, R10B_RF_T_METER, 0, 0x30000);

	/* ACK for transmitted management frames. */
	rtwn_setbits_4(sc, R92C_FWHW_TXQ_CTRL, 0,
	    R10B_FWHW_TXQ_CTRL_XMIT_MGMT_ACK);

	/* Port 0 and 1 enable (the vendor's normal driver mode), LDO mode. */
	rtwn_setbits_1(sc, R10B_PORT_CTRL, 0, R10B_PORT_CTRL_PORT01_EN);
	rtwn_setbits_4(sc, R10B_WL_RF_PSS, 0, R10B_WL_RF_PSS_LDO);

	rs->cck_new_agc = (rtwn_bb_read(sc, R10B_CCK0_NEW_AGC) &
	    R10B_CCK0_NEW_AGC_BIT) != 0;

	r10b_set_crystal_cap(sc);

	if (sc->sc_ratectl_sysctl == RTWN_RATECTL_FW) {
		/* No support (yet?) for f/w rate adaptation. */
		sc->sc_ratectl = RTWN_RATECTL_NET80211;
	} else
		sc->sc_ratectl = sc->sc_ratectl_sysctl;
}
