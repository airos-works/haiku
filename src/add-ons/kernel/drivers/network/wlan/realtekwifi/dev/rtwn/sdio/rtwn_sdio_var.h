/*-
 * Copyright (c) 2026 air/OS
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RTWN_SDIO_VAR_H
#define RTWN_SDIO_VAR_H

#include <dev/rtwn/sdio/rtwn_sdio_reg.h>

/*
 * TX: tx_start() copies the descriptor and the frame into one of these and
 * queues it; a task writes them to the FIFOs as pages come free (the AVAL
 * interrupt, or a short poll) and completes them. One frame per transfer.
 */
#define RTWN_SDIO_TX_LIST_COUNT		16
#define RTWN_SDIO_TXBUFSZ		(5 * RTWN_SDIO_BLOCK_SIZE)
	/* a 3839-byte A-MSDU and a descriptor, padded to blocks */
#define RTWN_SDIO_TX_STALL_MS		500
	/* no pages for that long: the frame is dropped */

/* RX: one FIFO read at a time, at most the chip's RX buffer (16 KiB). */
#define RTWN_SDIO_RXBUFSZ		(0x4000 + RTWN_SDIO_BLOCK_SIZE)
#define RTWN_SDIO_RX_MAX_PER_INTR	(64 * 1024)

/* Consecutive failed bus transfers after which the card counts as gone. */
#define RTWN_SDIO_MAX_ERRORS		32

/*
 * The status task: once a second while the interface is up it checks that
 * pending work at the chip (HISR) brought a card interrupt; if not, it
 * serves the chip itself from then on, every RTWN_SDIO_POLL_MS. For the
 * first RTWN_SDIO_STATUS_LOG_S seconds it logs the counters every
 * RTWN_SDIO_STATUS_EVERY_S.
 */
#define RTWN_SDIO_POLL_MS		10
#define RTWN_SDIO_STATUS_LOG_S		30
#define RTWN_SDIO_STATUS_EVERY_S	5

struct rtwn_sdio_txbuf {
	uint8_t				*buf;
	int				len;	/* descriptor + frame */
	int				queue;	/* RTWN_SDIO_QUEUE_* */
	int				id;	/* beacons only */
	struct mbuf			*m;
	struct ieee80211_node		*ni;
	STAILQ_ENTRY(rtwn_sdio_txbuf)	next;
};
typedef STAILQ_HEAD(, rtwn_sdio_txbuf) rtwn_sdio_txhead;

struct sdio_func;
struct taskqueue;

struct rtwn_sdio_softc {
	struct rtwn_softc	sd_sc;		/* must be the first */
	struct sdio_func	*sd_func;
	uint16_t		sd_blksize;	/* as set on function 1 */
	int			sd_chip;

	uint8_t			sd_mac_on;	/* CMD53 may be used */
	uint8_t			sd_gone;	/* too many failed transfers */
	uint8_t			sd_intr;	/* card interrupt handler set */
	uint8_t			sd_xfers;	/* HIMR set, TX/RX running */
	uint8_t			sd_rx_seq;
	int			sd_errors;	/* consecutive failures */
	uint32_t		sd_himr;

	/* TX */
	struct rtwn_sdio_txbuf	sd_tx[RTWN_SDIO_TX_LIST_COUNT];
	rtwn_sdio_txhead	sd_tx_free;
	rtwn_sdio_txhead	sd_tx_pending;
	uint8_t			sd_free_pages[4];
	uint8_t			sd_oqt_free;
	int			sd_tx_stall_ms;
	struct taskqueue	*sd_tq;
	struct task		sd_tx_task;

	/* RX */
	uint8_t			*sd_rx_buf;

	/* status and the polling fallback (rtwn_sdio_status_task()) */
	struct taskqueue	*sd_status_tq;
	struct timeout_task	sd_status_task;
	int			sd_status_start;	/* ticks */
	int			sd_status_next;		/* ticks */
	int			sd_status_check;	/* ticks */
	uint32_t		sd_intrs;	/* card interrupts served */
	uint32_t		sd_check_intrs;
	uint32_t		sd_polls;	/* polls that found work */
	int			sd_stuck_checks; /* work, no interrupt */
	uint8_t			sd_polling;	/* no card interrupts: poll */

	/* counters for the log */
	uint32_t		sd_rx_reads;
	uint32_t		sd_rx_pkts;
	uint32_t		sd_tx_frames;
	uint32_t		sd_tx_dropped;
	uint8_t			sd_logged_tx;
	uint8_t			sd_logged_rx;
};
#define RTWN_SDIO_SOFTC(sc)	((struct rtwn_sdio_softc *)(sc))

/* Log prefix: every line the SDIO attachment writes starts with it. */
#define RTWN_SDIO_LOG(_sc, ...) do {					\
	device_printf((_sc)->sc_dev, "sdio: " __VA_ARGS__);		\
} while (0)

/* rtwn_sdio_reg.c */
int		rtwn_sdio_read_local(struct rtwn_softc *, uint32_t, void *,
		    int);
int		rtwn_sdio_write_local(struct rtwn_softc *, uint32_t,
		    const void *, int);
uint8_t		rtwn_sdio_read_local_1(struct rtwn_softc *, uint32_t);
uint16_t	rtwn_sdio_read_local_2(struct rtwn_softc *, uint32_t);
uint32_t	rtwn_sdio_read_local_4(struct rtwn_softc *, uint32_t);
int		rtwn_sdio_write_local_1(struct rtwn_softc *, uint32_t, uint8_t);
int		rtwn_sdio_write_local_4(struct rtwn_softc *, uint32_t,
		    uint32_t);
int		rtwn_sdio_write_1(struct rtwn_softc *, uint16_t, uint8_t);
int		rtwn_sdio_write_2(struct rtwn_softc *, uint16_t, uint16_t);
int		rtwn_sdio_write_4(struct rtwn_softc *, uint16_t, uint32_t);
uint8_t		rtwn_sdio_read_1(struct rtwn_softc *, uint16_t);
uint16_t	rtwn_sdio_read_2(struct rtwn_softc *, uint16_t);
uint32_t	rtwn_sdio_read_4(struct rtwn_softc *, uint16_t);
void		rtwn_sdio_delay(struct rtwn_softc *, int);
int		rtwn_sdio_fw_write_block(struct rtwn_softc *,
		    const uint8_t *, uint16_t, int);
int		rtwn_sdio_poll_local_1(struct rtwn_softc *, uint32_t,
		    uint8_t, uint8_t, int);
int		rtwn_sdio_fifo_write(struct rtwn_softc *, uint32_t,
		    const void *, size_t);
int		rtwn_sdio_fifo_read(struct rtwn_softc *, uint32_t, void *,
		    size_t);

/* rtwn_sdio_tx.c */
int		rtwn_sdio_alloc_tx_list(struct rtwn_softc *);
void		rtwn_sdio_free_tx_list(struct rtwn_softc *);
void		rtwn_sdio_reset_tx_list(struct rtwn_softc *,
		    struct ieee80211vap *);
int		rtwn_sdio_tx_start(struct rtwn_softc *, struct ieee80211_node *,
		    struct mbuf *, uint8_t *, uint8_t, int);
void		rtwn_sdio_tx_task(void *, int);
void		rtwn_sdio_tx_kick(struct rtwn_softc *);
void		rtwn_sdio_update_free_pages(struct rtwn_softc *);

/* rtwn_sdio_rx.c */
void		rtwn_sdio_intr(void *);
int		rtwn_sdio_poll(struct rtwn_softc *);

#endif	/* RTWN_SDIO_VAR_H */
