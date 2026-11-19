/*-
 * Copyright (c) 2026 air/OS
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * The card interrupt and RX of the SDIO backend.
 *
 * The chip raises the SDIO card interrupt for the causes enabled in the
 * SDIO local HIMR register; HISR tells which. RX_REQUEST: the RX FIFO
 * holds RX0_REQ_LEN bytes, read in one CMD53 from the RX FIFO domain (its
 * address carries a 2-bit sequence number). With RX aggregation one read
 * holds several packets, each an RX descriptor (the same as on USB),
 * driver info, a shift and the frame, the next one 8-byte aligned. Frames
 * the firmware sends (C2H: TX reports and other events) come the same way,
 * flagged in their descriptor; rtwn_classify_intr() tells them apart.
 * AVAL: TX pages came free; the TX task waits for that.
 *
 * Followed: Linux staging rtl8723bs hal/sdio_ops.c sd_int_hdl(),
 * sd_int_dpc(), sd_recv_rxfifo() and hal/rtl8723bs_recv.c
 * rtl8723bs_recv_tasklet() (GPL-2.0); rtw88 sdio.c
 * rtw_sdio_handle_interrupt(), rtw_sdio_rx_isr() and rtw_sdio_rxfifo_recv()
 * (GPL-2.0 OR BSD-3-Clause), whose note that an 11n chip may report a
 * length before the data is complete is why this reads HISR again before
 * each further read.
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
#include <dev/rtwn/if_rtwn_debug.h>
#include <dev/rtwn/if_rtwn_rx.h>
#include <dev/rtwn/if_rtwn_tx.h>

#include <dev/rtwn/sdio/rtwn_sdio_var.h>
#include <dev/rtwn/sdio/rtwn_sdio_frame.h>

/* A data frame: checked and copied into an mbuf, descriptor included. */
static struct mbuf *
rtwn_sdio_rx_copy(struct rtwn_softc *sc, const uint8_t *buf,
    const struct rtwn_sdio_rx_pkt *pkt)
{
	struct ieee80211com *ic = &sc->sc_ic;
	const struct rtwn_rx_stat_common *stat =
	    (const struct rtwn_rx_stat_common *)buf;
	uint32_t rxdw0 = le32toh(stat->rxdw0);
	struct mbuf *m;

	if (!(sc->sc_flags & RTWN_RUNNING))
		return (NULL);

	/* The RX filter keeps these out; count them if they come anyway. */
	if ((rxdw0 & (RTWN_RXDW0_CRCERR | RTWN_RXDW0_ICVERR)) != 0 ||
	    pkt->pktlen < sizeof(struct ieee80211_frame_ack)) {
		counter_u64_add(ic->ic_ierrors, 1);
		return (NULL);
	}

	m = m_get3(pkt->totlen, M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL) {
		counter_u64_add(ic->ic_ierrors, 1);
		return (NULL);
	}
	memcpy(mtod(m, uint8_t *), buf, pkt->totlen);
	m->m_pkthdr.len = m->m_len = pkt->totlen;

	if (rtwn_check_frame(sc, m) != 0) {
		m_freem(m);
		counter_u64_add(ic->ic_ierrors, 1);
		return (NULL);
	}
	return (m);
}

/*
 * The packets of one FIFO read: data frames are appended to *\a tail,
 * firmware reports handled here.
 */
static void
rtwn_sdio_rx_parse(struct rtwn_softc *sc, uint8_t *buf, uint32_t len,
    struct mbuf ***tail)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	struct rtwn_sdio_rx_pkt pkt;
	uint32_t off = 0;
	int status;

	while ((status = rtwn_sdio_rx_next_pkt(buf, len, off, &pkt)) ==
	    RTWN_SDIO_RX_PKT_OK) {
		uint8_t *p = buf + pkt.off;
		struct mbuf *m;

		sd->sd_rx_pkts++;
		switch (rtwn_classify_intr(sc, p, pkt.totlen)) {
		case RTWN_RX_DATA:
			m = rtwn_sdio_rx_copy(sc, p, &pkt);
			if (m != NULL) {
				**tail = m;
				*tail = &m->m_nextpkt;
			}
			break;
		case RTWN_RX_TX_REPORT:
			if (sc->sc_ratectl == RTWN_RATECTL_NET80211) {
				RTWN_NT_LOCK(sc);
				rtwn_handle_tx_report(sc, p, pkt.totlen);
				RTWN_NT_UNLOCK(sc);
			}
			break;
		case RTWN_RX_TX_REPORT2:
			if (sc->sc_ratectl == RTWN_RATECTL_NET80211) {
				RTWN_NT_LOCK(sc);
				rtwn_handle_tx_report2(sc, p, pkt.totlen);
				RTWN_NT_UNLOCK(sc);
			}
			break;
		case RTWN_RX_OTHER:
			rtwn_handle_c2h_report(sc, p, pkt.totlen);
			break;
		}
		off = pkt.next;
	}

	if (status == RTWN_SDIO_RX_PKT_TRUNCATED) {
		counter_u64_add(sc->sc_ic.ic_ierrors, 1);
		if (sd->sd_logged_rx < 8) {
			sd->sd_logged_rx++;
			RTWN_SDIO_LOG(sc, "RX: packet at %u of %u claims %u "
			    "bytes, rest of the read dropped\n", pkt.off, len,
			    pkt.totlen);
		}
	}
}

/* Empties the RX FIFO, at most RTWN_SDIO_RX_MAX_PER_INTR bytes. */
static void
rtwn_sdio_rx(struct rtwn_softc *sc, struct mbuf ***tail)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	uint32_t total = 0, len, hisr;
	size_t xfer;

	for (;;) {
		len = rtwn_sdio_read_local_2(sc, RTWN_SDIO_RX0_REQ_LEN);
		if (len == 0 || len == 0xffff)
			break;

		xfer = rtwn_sdio_xfer_len(len, sd->sd_blksize);
		if (xfer > RTWN_SDIO_RXBUFSZ) {
			/* larger than the chip's RX buffer: not a length */
			if (sd->sd_logged_rx < 8) {
				sd->sd_logged_rx++;
				RTWN_SDIO_LOG(sc, "RX: implausible request "
				    "length %u\n", len);
			}
			break;
		}

		if (rtwn_sdio_fifo_read(sc, rtwn_sdio_rx_addr(sd->sd_rx_seq++),
		    sd->sd_rx_buf, xfer) != 0)
			break;
		if (sd->sd_rx_reads++ == 0)
			RTWN_SDIO_LOG(sc, "RX: first read, %u bytes\n", len);

		rtwn_sdio_rx_parse(sc, sd->sd_rx_buf, len, tail);

		total += len;
		if (total >= RTWN_SDIO_RX_MAX_PER_INTR)
			break;
		hisr = rtwn_sdio_read_local_4(sc, RTWN_SDIO_HISR);
		if (hisr == 0xffffffff || !(hisr & RTWN_SDIO_INT_RX_REQUEST))
			break;
	}
}

/* rtwn_rx_common() wants the descriptor apart, as PCIe has it. */
static struct ieee80211_node *
rtwn_sdio_rx_frame(struct rtwn_softc *sc, struct mbuf *m)
{
	struct rtwn_rx_stat_common stat;

	m_copydata(m, 0, sizeof(stat), (caddr_t)&stat);
	m_adj(m, sizeof(stat));
	return (rtwn_rx_common(sc, m, &stat));
}

/*
 * Serves what HISR shows: from the card interrupt, or from the status task
 * when card interrupts do not come (\a polled). Returns whether there was
 * anything to do.
 */
static int
rtwn_sdio_serve(struct rtwn_softc *sc, int polled)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	struct ieee80211com *ic = &sc->sc_ic;
	struct mbuf *m0 = NULL, **tail = &m0, *m, *next;
	struct ieee80211_node *ni;
	uint32_t hisr, ack;

	RTWN_LOCK(sc);
	if (!sd->sd_xfers || sd->sd_gone) {
		/*
		 * Not ours to serve now (down, or a mask left over from before,
		 * a warm boot from another OS say): mask it at the chip, or the
		 * bus calls us again as soon as we return.
		 */
		if (!polled && !sd->sd_gone) {
			sd->sd_himr = 0;
			rtwn_sdio_write_local_4(sc, RTWN_SDIO_HIMR, 0);
		}
		RTWN_UNLOCK(sc);
		return (0);
	}
	if (!polled)
		sd->sd_intrs++;

	hisr = rtwn_sdio_read_local_4(sc, RTWN_SDIO_HISR);
	if (hisr == 0xffffffff) {
		RTWN_UNLOCK(sc);
		return (0);
	}
	hisr &= sd->sd_himr | RTWN_SDIO_INT_W1C;
	if (hisr == 0) {
		RTWN_UNLOCK(sc);
		return (0);
	}
	if (polled)
		sd->sd_polls++;

	ack = hisr & RTWN_SDIO_INT_W1C;
	if (ack != 0)
		rtwn_sdio_write_local_4(sc, RTWN_SDIO_HISR, ack);

	if (hisr & RTWN_SDIO_INT_AVAL) {
		/*
		 * Reading the counts acknowledges it (the vendor driver does
		 * the same); the TX task turns it on again when it waits.
		 */
		rtwn_sdio_update_free_pages(sc);
		sd->sd_himr &= ~RTWN_SDIO_INT_AVAL;
		rtwn_sdio_write_local_4(sc, RTWN_SDIO_HIMR, sd->sd_himr);
		wakeup(sd);
	}

	if (hisr & RTWN_SDIO_INT_RX_REQUEST)
		rtwn_sdio_rx(sc, &tail);

	/* rtwn_rx_common() runs locked, net80211 gets the frames unlocked. */
	for (m = m0; m != NULL; m = m->m_nextpkt) {
		M_ASSERTPKTHDR(m);
		m->m_pkthdr.PH_loc.ptr = rtwn_sdio_rx_frame(sc, m);
	}
	RTWN_UNLOCK(sc);

	for (m = m0; m != NULL; m = next) {
		next = m->m_nextpkt;
		m->m_nextpkt = NULL;

		ni = m->m_pkthdr.PH_loc.ptr;
		m->m_pkthdr.PH_loc.ptr = NULL;
		if (ni != NULL) {
			(void)ieee80211_input_mimo(ni, m);
			ieee80211_free_node(ni);
		} else
			(void)ieee80211_input_mimo_all(ic, m);
	}

	if (m0 != NULL) {
		RTWN_LOCK(sc);
		rtwn_start(sc);
		RTWN_UNLOCK(sc);
	}
	return (1);
}

/*
 * The card interrupt, in a thread of the MMC bus: the card's interrupt
 * stays masked until this returns.
 */
void
rtwn_sdio_intr(void *arg)
{
	(void)rtwn_sdio_serve(arg, 0);
}

/* The polling fallback (rtwn_sdio_status_task()). */
int
rtwn_sdio_poll(struct rtwn_softc *sc)
{
	return (rtwn_sdio_serve(sc, 1));
}
