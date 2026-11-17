/*-
 * Copyright (c) 2026 air/OS
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * TX of the SDIO backend.
 *
 * Each frame goes to the FIFO of its queue as one CMD53: the 40-byte TX
 * descriptor (checksummed, "aggregation number" 1) and the frame, padded
 * to whole words or, past a block, whole blocks, with the unpadded length
 * in the FIFO address. Before writing, the frame's 128-byte pages must be
 * free in its queue or the public pool (RTWN_SDIO_FREE_TXPG, kept as a
 * count that is only read back when it runs short) and the output queue
 * table must have room. Frames wait in a short queue for a task that
 * does the writing; while pages run short it enables the AVAL ("pages
 * available") interrupt and sleeps on it, briefly, and drops the frame at
 * the head after RTWN_SDIO_TX_STALL_MS without progress.
 *
 * The page and OQT accounting follows what Linux's drivers do (staging
 * rtl8723bs hal/rtl8723bs_xmit.c rtl8723_dequeue_writeport(),
 * hal/sdio_ops.c HalQueryTxBufferStatus8723BSdio(); rtw88 sdio.c
 * rtw_sdio_check_free_txpg(), rtw_sdio_write_port()); queue mapping as the
 * vendor's three-FIFO setup (sdio_halinit.c
 * _InitNormalChipThreeOutEpPriority()), which is R92C_TRXDMA_CTRL_QMAP_3EP.
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
#include <dev/rtwn/if_rtwn_beacon.h>
#include <dev/rtwn/if_rtwn_debug.h>
#include <dev/rtwn/if_rtwn_tx.h>

#include <dev/rtwn/sdio/rtwn_sdio_var.h>
#include <dev/rtwn/sdio/rtwn_sdio_frame.h>

int
rtwn_sdio_alloc_tx_list(struct rtwn_softc *sc)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	int i;

	STAILQ_INIT(&sd->sd_tx_free);
	STAILQ_INIT(&sd->sd_tx_pending);
	for (i = 0; i < RTWN_SDIO_TX_LIST_COUNT; i++) {
		struct rtwn_sdio_txbuf *tb = &sd->sd_tx[i];

		tb->buf = malloc(RTWN_SDIO_TXBUFSZ, M_DEVBUF, M_NOWAIT | M_ZERO);
		if (tb->buf == NULL) {
			RTWN_SDIO_LOG(sc, "cannot allocate TX buffers\n");
			rtwn_sdio_free_tx_list(sc);
			return (ENOMEM);
		}
		tb->m = NULL;
		tb->ni = NULL;
		STAILQ_INSERT_TAIL(&sd->sd_tx_free, tb, next);
	}
	return (0);
}

void
rtwn_sdio_free_tx_list(struct rtwn_softc *sc)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	int i;

	for (i = 0; i < RTWN_SDIO_TX_LIST_COUNT; i++) {
		struct rtwn_sdio_txbuf *tb = &sd->sd_tx[i];

		if (tb->ni != NULL)
			ieee80211_free_node(tb->ni);
		if (tb->m != NULL)
			m_freem(tb->m);
		tb->ni = NULL;
		tb->m = NULL;
		free(tb->buf, M_DEVBUF);
		tb->buf = NULL;
	}
	STAILQ_INIT(&sd->sd_tx_free);
	STAILQ_INIT(&sd->sd_tx_pending);
}

/* Frames of \a vap (all with NULL) are dropped from the queue. */
void
rtwn_sdio_reset_tx_list(struct rtwn_softc *sc, struct ieee80211vap *vap)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	struct rtwn_vap *uvp = RTWN_VAP(vap);
	struct rtwn_sdio_txbuf *tb, *tmp;
	int id;

	RTWN_ASSERT_LOCKED(sc);

	id = (uvp != NULL ? uvp->id : RTWN_VAP_ID_INVALID);
	STAILQ_FOREACH_SAFE(tb, &sd->sd_tx_pending, next, tmp) {
		if (vap != NULL && !(tb->ni == NULL && tb->id == id) &&
		    !(tb->ni != NULL && tb->ni->ni_vap == vap))
			continue;

		if (tb->ni != NULL) {
			ieee80211_free_node(tb->ni);
			tb->ni = NULL;
		}
		if (tb->m != NULL) {
			m_freem(tb->m);
			tb->m = NULL;
		}
		STAILQ_REMOVE(&sd->sd_tx_pending, tb, rtwn_sdio_txbuf, next);
		STAILQ_INSERT_TAIL(&sd->sd_tx_free, tb, next);
	}
	sc->qfullmsk = 0;
}

static void
rtwn_sdio_txeof(struct rtwn_sdio_softc *sd, struct rtwn_sdio_txbuf *tb,
    int status)
{
	struct rtwn_softc *sc = &sd->sd_sc;

	RTWN_ASSERT_LOCKED(sc);

	if (tb->ni != NULL)	/* not a beacon frame */
		ieee80211_tx_complete(tb->ni, tb->m, status);
	tb->ni = NULL;
	tb->m = NULL;

	if (sc->sc_ratectl != RTWN_RATECTL_NET80211 && sc->sc_tx_n_active > 0)
		sc->sc_tx_n_active--;

	STAILQ_INSERT_TAIL(&sd->sd_tx_free, tb, next);
	sc->qfullmsk = 0;
#ifndef D4054
	sc->sc_tx_timer = STAILQ_EMPTY(&sd->sd_tx_pending) ? 0 : 5;
#endif
}

void
rtwn_sdio_update_free_pages(struct rtwn_softc *sc)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	uint32_t reg;

	reg = rtwn_sdio_read_local_4(sc, RTWN_SDIO_FREE_TXPG);
	if (reg == 0xffffffff)
		return;
	rtwn_sdio_parse_free_pages(reg, sd->sd_free_pages);
}

static void
rtwn_sdio_set_himr(struct rtwn_sdio_softc *sd, uint32_t himr)
{
	if (sd->sd_himr == himr)
		return;
	sd->sd_himr = himr;
	rtwn_sdio_write_local_4(&sd->sd_sc, RTWN_SDIO_HIMR, himr);
}

void
rtwn_sdio_tx_kick(struct rtwn_softc *sc)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);

	if (sd->sd_tq != NULL)
		taskqueue_enqueue(sd->sd_tq, &sd->sd_tx_task);
}

int
rtwn_sdio_tx_start(struct rtwn_softc *sc, struct ieee80211_node *ni,
    struct mbuf *m, uint8_t *tx_desc, uint8_t type, int id)
{
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	struct rtwn_tx_desc_common *txd;
	struct rtwn_sdio_txbuf *tb;
	int len, ac;

	RTWN_ASSERT_LOCKED(sc);

	if (sd->sd_gone)
		return (ENXIO);

	len = m->m_pkthdr.len + sc->txdesc_len;
	if (len > RTWN_SDIO_TXBUFSZ ||
	    rtwn_sdio_xfer_len(len, sd->sd_blksize) > RTWN_SDIO_TXBUFSZ)
		return (EINVAL);

	tb = STAILQ_FIRST(&sd->sd_tx_free);
	if (tb == NULL) {
		sc->qfullmsk = 1;
		return (ENOBUFS);
	}
	STAILQ_REMOVE_HEAD(&sd->sd_tx_free, next);

	/* The FIFO, as R92C_TRXDMA_CTRL_QMAP_3EP maps the queues. */
	ac = M_WME_GETAC(m);
	if (ni == NULL || type == IEEE80211_FC0_TYPE_CTL ||
	    type == IEEE80211_FC0_TYPE_MGT || ac == WME_AC_VO)
		tb->queue = RTWN_SDIO_QUEUE_HIGH;
	else if (ac == WME_AC_VI)
		tb->queue = RTWN_SDIO_QUEUE_NORMAL;
	else
		tb->queue = RTWN_SDIO_QUEUE_LOW;

	txd = (struct rtwn_tx_desc_common *)tx_desc;
	txd->pktlen = htole16(m->m_pkthdr.len);
	txd->offset = sc->txdesc_len;
	rtwn_sdio_tx_set_agg_num(tx_desc, 1);
	rtwn_sdio_tx_checksum(tx_desc);

	rtwn_dump_tx_desc(sc, tx_desc);

	memcpy(tb->buf, tx_desc, sc->txdesc_len);
	m_copydata(m, 0, m->m_pkthdr.len, (caddr_t)(tb->buf + sc->txdesc_len));
	tb->len = len;
	tb->id = id;
	tb->ni = ni;
	tb->m = (ni != NULL) ? m : NULL;
#ifndef D4054
	if (ni != NULL)
		sc->sc_tx_timer = 5;
#endif

	STAILQ_INSERT_TAIL(&sd->sd_tx_pending, tb, next);
	if (STAILQ_EMPTY(&sd->sd_tx_free))
		sc->qfullmsk = 1;
	if (sc->sc_ratectl != RTWN_RATECTL_NET80211)
		sc->sc_tx_n_active++;

	rtwn_sdio_tx_kick(sc);
	return (0);
}

/* Whether the frame at the head fits now; reads the counts back if not. */
static int
rtwn_sdio_tx_room(struct rtwn_sdio_softc *sd, int queue, int pages)
{
	struct rtwn_softc *sc = &sd->sd_sc;

	if (!rtwn_sdio_pages_fit(sd->sd_free_pages, queue, pages))
		rtwn_sdio_update_free_pages(sc);
	if (!rtwn_sdio_pages_fit(sd->sd_free_pages, queue, pages))
		return (0);

	if (sd->sd_oqt_free == 0)
		sd->sd_oqt_free = rtwn_sdio_read_local_1(sc,
		    RTWN_SDIO_OQT_FREE_PG);
	return (sd->sd_oqt_free != 0 && sd->sd_oqt_free != 0xff);
}

void
rtwn_sdio_tx_task(void *arg, int pending)
{
	struct rtwn_softc *sc = arg;
	struct rtwn_sdio_softc *sd = RTWN_SDIO_SOFTC(sc);
	struct rtwn_sdio_txbuf *tb;
	size_t xfer;
	int pages, error;

	RTWN_LOCK(sc);
	while ((tb = STAILQ_FIRST(&sd->sd_tx_pending)) != NULL) {
		if (!sd->sd_xfers || sd->sd_gone)
			break;

		pages = rtwn_sdio_tx_pages(tb->len);
		if (!rtwn_sdio_tx_room(sd, tb->queue, pages)) {
			if (sd->sd_tx_stall_ms >= RTWN_SDIO_TX_STALL_MS) {
				if (sd->sd_tx_dropped++ < 8) {
					RTWN_SDIO_LOG(sc, "TX: no room for %d "
					    "pages in queue %d for %d ms (free "
					    "%u/%u/%u/%u, OQT %u), frame "
					    "dropped\n", pages, tb->queue,
					    sd->sd_tx_stall_ms,
					    sd->sd_free_pages[0],
					    sd->sd_free_pages[1],
					    sd->sd_free_pages[2],
					    sd->sd_free_pages[3],
					    sd->sd_oqt_free);
				}
				sd->sd_tx_stall_ms = 0;
				STAILQ_REMOVE_HEAD(&sd->sd_tx_pending, next);
				rtwn_sdio_txeof(sd, tb, 1);
				continue;
			}

			/*
			 * Wait for the chip to free pages: the AVAL interrupt
			 * wakes us, a 2 ms timeout looks again in any case.
			 * The lock is dropped meanwhile; the queue may have
			 * changed when it is back.
			 */
			rtwn_sdio_set_himr(sd, sd->sd_himr | RTWN_SDIO_INT_AVAL);
			msleep(sd, &sc->sc_mtx, 0, "rtwnsdtx",
			    MAX(1, 2 * hz / 1000));
			sd->sd_tx_stall_ms += 2;
			continue;
		}
		sd->sd_tx_stall_ms = 0;

		/* Beacons go to the beacon queue of their port. */
		if (tb->ni == NULL && RTWN_CHIP_HAS_BCNQ1(sc))
			rtwn_switch_bcnq(sc, tb->id);

		xfer = rtwn_sdio_xfer_len(tb->len, sd->sd_blksize);
		memset(tb->buf + tb->len, 0, xfer - tb->len);
		error = rtwn_sdio_fifo_write(sc,
		    rtwn_sdio_tx_addr(tb->queue, tb->len), tb->buf, xfer);

		rtwn_sdio_take_pages(sd->sd_free_pages, tb->queue, pages);
		sd->sd_oqt_free--;

		STAILQ_REMOVE_HEAD(&sd->sd_tx_pending, next);
		if (error == 0 && sd->sd_tx_frames++ == 0) {
			RTWN_SDIO_LOG(sc, "TX: first frame written (%d bytes, "
			    "queue %d)\n", tb->len, tb->queue);
		}
		rtwn_sdio_txeof(sd, tb, error != 0);
	}

	if (STAILQ_EMPTY(&sd->sd_tx_pending) && sd->sd_xfers && !sd->sd_gone)
		rtwn_sdio_set_himr(sd, sd->sd_himr & ~RTWN_SDIO_INT_AVAL);

	rtwn_start(sc);
	RTWN_UNLOCK(sc);
}
