/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2016 Andriy Voskoboinyk <avos@FreeBSD.org>
 * Copyright (c) 2026 Ahmad Khalifa <vexeduxr@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Haiku: only the USB-specific parts of FreeBSD's r23bu_init.c are left
 * here. The power sequences' common part, r23b_init_bb() and the
 * bus-independent A-MPDU setup moved to the chip directory (r23b_pwr.c,
 * r23b_init.c) so that the SDIO attachment can share them unchanged.
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

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include <dev/rtwn/if_rtwnreg.h>
#include <dev/rtwn/if_rtwnvar.h>

#include <dev/rtwn/usb/rtwn_usb_var.h>

#include <dev/rtwn/rtl8192c/r92c_reg.h>

#include <dev/rtwn/rtl8192e/r92e_var.h>

#include <dev/rtwn/rtl8723b/r23b_reg.h>
#include <dev/rtwn/rtl8723b/r23b_var.h>

#include <dev/rtwn/rtl8723b/usb/r23bu.h>

#include <dev/rtwn/rtl8812a/r12a_reg.h>

/*
 * Card disabled -> card emulation as the vendor's tables have it for USB
 * (GPIO9 is the USB wake-up pin), then the shared sequence.
 */
int
r23bu_power_on(struct rtwn_softc *sc)
{
#define RTWN_CHK(res) do {	\
	if (res != 0)		\
		return (EIO);	\
} while(0)

	/* Clear suspend and power down bits.*/
	RTWN_CHK(rtwn_setbits_1_shift(sc, R92C_APS_FSMCO,
	    R92C_APS_FSMCO_AFSM_HSUS | R92C_APS_FSMCO_APDM_HPDN, 0, 1));

	/* Disable GPIO9 as EXT WAKEUP. */
	RTWN_CHK(rtwn_setbits_1(sc, R92C_GPIO_INTM + 2, 0x01, 0));

	/* Enable WL suspend. */
	RTWN_CHK(rtwn_setbits_1_shift(sc, R92C_APS_FSMCO,
	    R92C_APS_FSMCO_AFSM_HSUS | R92C_APS_FSMCO_AFSM_PCIE, 0, 1));

	return (r23b_power_on(sc));
#undef RTWN_CHK
}

void
r23bu_power_off(struct rtwn_softc *sc)
{

	if (r23b_power_off(sc) != 0)
		return;

	/* Enable GPIO9 as EXT WAKEUP. */
	rtwn_setbits_1(sc, R92C_GPIO_INTM + 2, 0, 0x01);
}

void
r23bu_init_rx_agg(struct rtwn_softc *sc)
{
	struct r23b_softc *rs = sc->sc_priv;
	uint32_t val;

	rtwn_setbits_1(sc, R92C_TRXDMA_CTRL, 0, R92C_TRXDMA_CTRL_RXDMA_AGG_EN);

	val = rtwn_read_4(sc, R92C_RXDMA_AGG_PG_TH);
	val = RW(val, R23B_RXDMA_AGG_PG_TH_SIZE, rs->super.ac_usb_dma_size);
	val = RW(val, R23B_RXDMA_AGG_PG_TH_TIME, rs->super.ac_usb_dma_time);
	val |= R23B_RXDMA_AGG_PG_TH_EN;
	rtwn_write_4(sc, R92C_RXDMA_AGG_PG_TH, val);
}

void
r23bu_init_ampdu(struct rtwn_softc *sc)
{
	struct rtwn_usb_softc *uc = RTWN_USB_SOFTC(sc);
	uint8_t val;

	val = rtwn_read_1(sc, R12A_RXDMA_PRO);
	val = RW(val, R12A_BURST_SZ,
	    usbd_get_speed(uc->uc_udev) == USB_SPEED_HIGH ?
	    R12A_BURST_SZ_USB2 : R12A_BURST_SZ_USB1);
	val |= R12A_DMA_MODE | SM(R12A_BURST_CNT, 3);
	rtwn_write_1(sc, R12A_RXDMA_PRO, val);

	r23b_init_ampdu(sc);
}
