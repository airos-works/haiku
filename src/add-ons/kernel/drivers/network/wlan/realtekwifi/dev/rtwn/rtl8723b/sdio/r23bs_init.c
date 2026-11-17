/*-
 * Copyright (c) 2026 air/OS
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * RTL8723BS: what differs from the RTL8723BU in bringing the chip up.
 *
 * Power. The vendor's power tables (Linux staging rtl8723bs
 * include/hal_pwr_seq.h, also in rtw88's rtw8723d.c for the next chip of
 * the family) mark a few steps of the "card disabled <-> card emulation"
 * transitions as SDIO-only: the HCI suspend request in SDIO local register
 * 0x86 (cleared, then wait for "resume ready"; set, then wait for it to
 * go) and the 12H LDO's sleep mode, bit 4 of 0x23. r23bs_power_on() does
 * those around r23b_power_on() (card emulation -> active, shared with the
 * USB attachment), r23bs_power_off() after r23b_power_off(). The vendor's
 * other SDIO power-on extras (sdio_halinit.c CardEnable() and
 * _InitPowerOn_8723BS()): unlock the power registers (0x1c) first, enable
 * HW power down (APS_FSMCO PDN_EN), clear early mode (0x4d0); and when
 * disabling (CardDisableRTL8723BSdio()) the MCU I/O wrapper reset and the
 * SOP option (0x07).
 *
 * RX aggregation and DMA bursts as the vendor sets them for SDIO
 * (sdio_halinit.c HalRxAggr8723BSdio(), sdio_AggSettingRxUpdate()): size
 * and timeout limits of 6 per aggregate, 4 DMA bursts of 1 KiB. 128-byte pages
 * (_InitTransferPageSize()), which the backend's page counting assumes.
 * The MAC's own interrupt registers are cleared (_InitInterrupt()): on SDIO
 * everything goes through the SDIO local HIMR/HISR.
 *
 * The register numbers and values are facts of the chip; the code and the
 * words are this driver's.
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

#include <dev/rtwn/rtl8192c/r92c_reg.h>

#include <dev/rtwn/rtl8192e/r92e_var.h>

#include <dev/rtwn/rtl8723b/r23b_reg.h>
#include <dev/rtwn/rtl8723b/r23b_var.h>

#include <dev/rtwn/rtl8723b/sdio/r23bs.h>

#include <dev/rtwn/rtl8812a/r12a_reg.h>

#define R23BS_EARLY_MODE_CONTROL	0x4d0
#define R23BS_LDO_SLEEP			0x10	/* R92C_LPLDO_CTRL (0x23) */
#define R23BS_SOP_OPTION		0x20	/* APS_FSMCO byte 3 */
#define R23BS_RSV_CTRL_MIO_EN		0x01	/* RSV_CTRL byte 1: MCU I/O */
#define R23BS_HSUS_TIMEOUT_US		50000	/* the vendor: 5000 * 10 us */

int
r23bs_power_on(struct rtwn_softc *sc)
{
#define RTWN_CHK(res, what) do {					\
	if ((res) != 0) {						\
		RTWN_SDIO_LOG(sc, "power on: %s failed\n", what);	\
		return (EIO);						\
	}								\
} while (0)
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	int error;

	if (sd->sd_gone)
		return (ENXIO);

	sd->sd_mac_on = 0;
	RTWN_SDIO_LOG(sc, "power on: APS_FSMCO %#010x, HSUS_CTRL %#04x\n",
	    rtwn_read_4(sc, R92C_APS_FSMCO),
	    rtwn_sdio_read_local_1(sc, RTWN_SDIO_HSUS_CTRL));

	/* Unlock the isolation, clock and power control registers. */
	RTWN_CHK(rtwn_write_1(sc, R92C_RSV_CTRL, 0), "unlocking 0x1c");

	/* Card disabled -> card emulation. */
	RTWN_CHK(rtwn_setbits_1_shift(sc, R92C_APS_FSMCO,
	    R92C_APS_FSMCO_AFSM_HSUS | R92C_APS_FSMCO_APDM_HPDN, 0, 1),
	    "clearing suspend and power down");
	RTWN_CHK(rtwn_sdio_write_local_1(sc, RTWN_SDIO_HSUS_CTRL,
	    rtwn_sdio_read_local_1(sc, RTWN_SDIO_HSUS_CTRL) &
	    ~RTWN_SDIO_HSUS_SUS_REQ), "HCI resume request");
	error = rtwn_sdio_poll_local_1(sc, RTWN_SDIO_HSUS_CTRL,
	    RTWN_SDIO_HSUS_RESUME_RDY, RTWN_SDIO_HSUS_RESUME_RDY,
	    R23BS_HSUS_TIMEOUT_US);
	if (error != 0) {
		RTWN_SDIO_LOG(sc, "power on: the SDIO interface does not "
		    "leave suspend (error %d)\n", error);
		return (error);
	}
	RTWN_CHK(rtwn_setbits_1_shift(sc, R92C_APS_FSMCO,
	    R92C_APS_FSMCO_AFSM_HSUS | R92C_APS_FSMCO_AFSM_PCIE, 0, 1),
	    "clearing WL suspend");
	RTWN_CHK(rtwn_setbits_1(sc, R92C_LPLDO_CTRL, R23BS_LDO_SLEEP, 0),
	    "12H LDO normal mode");
	RTWN_SDIO_LOG(sc, "power on: card emulation\n");

	/*
	 * Card emulation -> active, as on USB. The MAC is on once its
	 * ONMAC request completes, about in the middle; the vendor driver
	 * counts it as on for the whole sequence, which the backend's rule
	 * (CMD53 only for dwords at 0x100 and up) allows: everything before
	 * ONMAC is below 0x100, and the BB antenna register written after it
	 * has to arrive in one piece.
	 */
	sd->sd_mac_on = 1;
	error = r23b_power_on(sc);
	if (error != 0) {
		sd->sd_mac_on = 0;
		RTWN_SDIO_LOG(sc, "power on: card emulation -> active failed, "
		    "error %d (APS_FSMCO %#010x)\n", error,
		    rtwn_read_4(sc, R92C_APS_FSMCO));
		return (error);
	}

	/* The vendor's SDIO extras. */
	RTWN_CHK(rtwn_setbits_2(sc, R92C_APS_FSMCO, 0, R92C_APS_FSMCO_PDN_EN),
	    "enabling HW power down");
	RTWN_CHK(rtwn_write_1(sc, R23BS_EARLY_MODE_CONTROL, 0),
	    "clearing early mode");

	RTWN_SDIO_LOG(sc, "power on: active, APS_FSMCO %#010x, CR %#06x\n",
	    rtwn_read_4(sc, R92C_APS_FSMCO), rtwn_read_2(sc, R92C_CR));
	return (0);
#undef RTWN_CHK
}

void
r23bs_power_off(struct rtwn_softc *sc)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	int error;

	if (sd->sd_gone)
		return;

	/* From here on only CMD52: the MAC goes off on the way. */
	sd->sd_mac_on = 0;

	error = r23b_power_off(sc);
	if (error != 0) {
		RTWN_SDIO_LOG(sc, "power off: active -> card emulation failed, "
		    "error %d; leaving the chip as it is\n", error);
		return;
	}

	/* Reset the MCU I/O wrapper. */
	rtwn_setbits_1(sc, R92C_RSV_CTRL + 1, R23BS_RSV_CTRL_MIO_EN, 0);
	rtwn_setbits_1(sc, R92C_RSV_CTRL + 1, 0, R23BS_RSV_CTRL_MIO_EN);

	/* Card emulation -> card disabled. */
	rtwn_write_1(sc, R92C_APS_FSMCO + 3, R23BS_SOP_OPTION);
	rtwn_setbits_1(sc, R92C_LPLDO_CTRL, 0, R23BS_LDO_SLEEP);
	rtwn_sdio_write_local_1(sc, RTWN_SDIO_HSUS_CTRL,
	    rtwn_sdio_read_local_1(sc, RTWN_SDIO_HSUS_CTRL) |
	    RTWN_SDIO_HSUS_SUS_REQ);
	error = rtwn_sdio_poll_local_1(sc, RTWN_SDIO_HSUS_CTRL,
	    RTWN_SDIO_HSUS_RESUME_RDY, 0, R23BS_HSUS_TIMEOUT_US);
	if (error != 0) {
		RTWN_SDIO_LOG(sc, "power off: the SDIO interface does not "
		    "suspend (error %d)\n", error);
		return;
	}
	RTWN_SDIO_LOG(sc, "power off: card disabled\n");
}

int
r23bs_set_page_size(struct rtwn_softc *sc)
{
	return (rtwn_write_1(sc, R92C_PBP, SM(R92C_PBP_PSRX, R92C_PBP_128) |
	    SM(R92C_PBP_PSTX, R92C_PBP_128)));
}

void
r23bs_init_intr(struct rtwn_softc *sc)
{
	rtwn_write_4(sc, R92C_HISR, 0);
	rtwn_write_4(sc, R92C_HIMR, 0);
}

void
r23bs_init_rx_agg(struct rtwn_softc *sc)
{
	rtwn_setbits_1(sc, R92C_TRXDMA_CTRL, 0, R92C_TRXDMA_CTRL_RXDMA_AGG_EN);

	/* byte 0: the size limit, byte 1: the timeout; the vendor's values */
	rtwn_write_1(sc, R92C_RXDMA_AGG_PG_TH + 1, 0x06);
	rtwn_write_1(sc, R92C_RXDMA_AGG_PG_TH, 0x06);
}

void
r23bs_init_ampdu(struct rtwn_softc *sc)
{
	/* RX DMA: aggregation mode, 4 bursts of 1 KiB. */
	rtwn_write_1(sc, R12A_RXDMA_PRO, R12A_DMA_MODE |
	    SM(R12A_BURST_CNT, 3) | SM(R12A_BURST_SZ, 0));

	r23b_init_ampdu(sc);
}

/*
 * Before the efuse is read: the vendor driver sets bit 6 of 0x4e
 * (LEDCFG2) on SDIO first (sdio_halinit.c _ReadAdapterInfo8723BS()).
 */
int
r23bs_efuse_preread(struct rtwn_softc *sc)
{
	int error;

	error = rtwn_setbits_1(sc, R92C_LEDCFG2, 0, 0x40);
	if (error != 0)
		return (error);
	return (r23b_efuse_preread(sc));
}

/*
 * The antenna setting the firmware reads (bit 0: inverted, bit 1: external
 * switch, bit 2: two antennas): SDIO local register 0x60 on SDIO, where
 * USB has 0xfe08 (Linux staging hal/HalBtc8723b1Ant.c
 * EXhalbtc8723b1ant_PowerOnSetting()).
 */
int
r23bs_ant_local_write(struct rtwn_softc *sc, uint8_t val)
{
	return (rtwn_sdio_write_local_1(sc, RTWN_SDIO_BTCOEX_ANT, val));
}
