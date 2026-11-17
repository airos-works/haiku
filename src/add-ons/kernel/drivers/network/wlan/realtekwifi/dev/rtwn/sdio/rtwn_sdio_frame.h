/*-
 * Copyright (c) 2026 air/OS
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RTWN_SDIO_FRAME_H
#define RTWN_SDIO_FRAME_H

/*
 * Address and framing arithmetic of the SDIO backend, without any kernel
 * dependency, so the host tests (src/tests/add-ons/kernel/drivers/network/
 * realtekwifi) check the same code the driver runs. The includer provides
 * uint8_t/uint16_t/uint32_t and size_t.
 *
 * See rtwn_sdio_reg.h for the address domains and the Linux sources these
 * were learned from.
 */

#include <dev/rtwn/sdio/rtwn_sdio_reg.h>

/* I/O address of an SDIO local register. */
static inline uint32_t
rtwn_sdio_local_addr(uint32_t reg)
{
	return ((RTWN_SDIO_DOM_LOCAL << RTWN_SDIO_DOM_SHIFT) |
	    (reg & RTWN_SDIO_LOCAL_MASK));
}

/* I/O address of a MAC register. */
static inline uint32_t
rtwn_sdio_mac_addr(uint32_t reg)
{
	return ((RTWN_SDIO_DOM_MAC << RTWN_SDIO_DOM_SHIFT) |
	    (reg & RTWN_SDIO_MAC_MASK));
}

/* The TX FIFO domain of a queue (RTWN_SDIO_QUEUE_*). */
static inline int
rtwn_sdio_tx_domain(int queue)
{
	switch (queue) {
	case RTWN_SDIO_QUEUE_HIGH:
		return (RTWN_SDIO_DOM_TX_HIQ);
	case RTWN_SDIO_QUEUE_NORMAL:
		return (RTWN_SDIO_DOM_TX_MIQ);
	default:
		return (RTWN_SDIO_DOM_TX_LOQ);
	}
}

/*
 * I/O address for writing \a len bytes (the descriptor and the frame, not
 * the padding) into a queue's TX FIFO: the length goes in the address, in
 * 32-bit words.
 */
static inline uint32_t
rtwn_sdio_tx_addr(int queue, size_t len)
{
	return (((uint32_t)rtwn_sdio_tx_domain(queue) << RTWN_SDIO_DOM_SHIFT) |
	    (uint32_t)(((len + 3) / 4) & RTWN_SDIO_FIFO_LEN_MASK));
}

/* I/O address of the RX FIFO for the read numbered \a seq. */
static inline uint32_t
rtwn_sdio_rx_addr(unsigned seq)
{
	return ((RTWN_SDIO_DOM_RX0FF << RTWN_SDIO_DOM_SHIFT) |
	    (seq & RTWN_SDIO_RX_SEQ_MASK));
}

/*
 * How much to transfer for \a len bytes of FIFO data so that it goes in one
 * CMD53: whole 32-bit words, and whole blocks once it is more than a block
 * (the bus sends a block run as one command, but splits a block run plus a
 * tail into two).
 */
static inline size_t
rtwn_sdio_xfer_len(size_t len, unsigned blksize)
{
	len = (len + 3) & ~(size_t)3;
	if (blksize != 0 && len > blksize)
		len = (len + blksize - 1) / blksize * blksize;
	return (len);
}

/* TX pages (128 bytes) a frame takes, descriptor included. */
static inline int
rtwn_sdio_tx_pages(size_t len)
{
	return ((int)((len + RTWN_SDIO_TX_PAGE_SIZE - 1) /
	    RTWN_SDIO_TX_PAGE_SIZE));
}

/*
 * The free page register: a byte per queue (high, normal, low) and one
 * for the public pool any queue draws from.
 */
static inline void
rtwn_sdio_parse_free_pages(uint32_t reg, uint8_t free_pages[4])
{
	free_pages[RTWN_SDIO_QUEUE_HIGH] = reg & 0xff;
	free_pages[RTWN_SDIO_QUEUE_NORMAL] = (reg >> 8) & 0xff;
	free_pages[RTWN_SDIO_QUEUE_LOW] = (reg >> 16) & 0xff;
	free_pages[RTWN_SDIO_QUEUE_PUBLIC] = (reg >> 24) & 0xff;
}

/*
 * Whether \a pages fit in \a queue. One page stays spare, between what
 * the vendor driver keeps (two) and rtw88 (none).
 */
static inline int
rtwn_sdio_pages_fit(const uint8_t free_pages[4], int queue, int pages)
{
	return (free_pages[queue] + free_pages[RTWN_SDIO_QUEUE_PUBLIC] >
	    pages);
}

/* Takes \a pages from the count: the queue's own first, then the pool. */
static inline void
rtwn_sdio_take_pages(uint8_t free_pages[4], int queue, int pages)
{
	int own = free_pages[queue] < pages ? free_pages[queue] : pages;

	free_pages[queue] -= own;
	pages -= own;
	if (pages > free_pages[RTWN_SDIO_QUEUE_PUBLIC])
		pages = free_pages[RTWN_SDIO_QUEUE_PUBLIC];
	free_pages[RTWN_SDIO_QUEUE_PUBLIC] -= pages;
}

/*
 * The TX descriptor checksum: the XOR of the first 32 bytes as 16-bit
 * little endian words, with the checksum field (the low half of the 8th
 * word) taken as zero. Written to that field, little endian.
 */
static inline void
rtwn_sdio_tx_checksum(uint8_t *desc)
{
	uint16_t sum = 0;
	int i;

	desc[28] = desc[29] = 0;
	for (i = 0; i < 32; i += 2)
		sum ^= (uint16_t)(desc[i] | (desc[i + 1] << 8));
	desc[28] = sum & 0xff;
	desc[29] = sum >> 8;
}

/*
 * The 8th descriptor word's top byte: the number of frames in this
 * transfer, set on the first descriptor of it.
 */
static inline void
rtwn_sdio_tx_set_agg_num(uint8_t *desc, uint8_t count)
{
	desc[31] = count;
}

/*
 * One packet of an RX FIFO read: the 24-byte RX descriptor, the PHY status
 * ("driver info", infosz 8-byte units), a shift and the frame; the next
 * packet starts on the next 8-byte boundary.
 */
struct rtwn_sdio_rx_pkt {
	uint32_t	off;		/* of the descriptor in the buffer */
	uint32_t	pktlen;		/* the 802.11 frame, with FCS */
	uint32_t	infosz;		/* bytes */
	uint32_t	shift;
	uint32_t	totlen;		/* descriptor to the end of the frame */
	uint32_t	next;		/* offset of the next packet */
};

enum {
	RTWN_SDIO_RX_PKT_OK = 0,
	RTWN_SDIO_RX_PKT_END,		/* no more packets */
	RTWN_SDIO_RX_PKT_TRUNCATED	/* claims more than was read */
};

static inline int
rtwn_sdio_rx_next_pkt(const uint8_t *buf, uint32_t len, uint32_t off,
    struct rtwn_sdio_rx_pkt *pkt)
{
	uint32_t rxdw0;

	if (off >= len || len - off < RTWN_SDIO_RX_DESC_SIZE)
		return (RTWN_SDIO_RX_PKT_END);

	rxdw0 = (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
	    ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
	pkt->off = off;
	pkt->pktlen = rxdw0 & 0x3fff;
	pkt->infosz = ((rxdw0 >> 16) & 0xf) * 8;
	pkt->shift = (rxdw0 >> 24) & 0x3;
	if (pkt->pktlen == 0)
		return (RTWN_SDIO_RX_PKT_END);

	pkt->totlen = RTWN_SDIO_RX_DESC_SIZE + pkt->infosz + pkt->shift +
	    pkt->pktlen;
	if (pkt->totlen > len - off)
		return (RTWN_SDIO_RX_PKT_TRUNCATED);

	pkt->next = off + ((pkt->totlen + RTWN_SDIO_RX_ALIGN - 1) &
	    ~(uint32_t)(RTWN_SDIO_RX_ALIGN - 1));
	return (RTWN_SDIO_RX_PKT_OK);
}

#endif	/* RTWN_SDIO_FRAME_H */
