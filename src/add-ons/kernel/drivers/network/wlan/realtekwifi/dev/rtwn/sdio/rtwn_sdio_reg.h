/*-
 * Copyright (c) 2026 air/OS
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RTWN_SDIO_REG_H
#define RTWN_SDIO_REG_H

/*
 * The SDIO side of Realtek's 802.11n SDIO chips (RTL8723BS, RTL8188ES,
 * RTL8821AS ...), as far as this driver uses it.
 *
 * Everything the host reaches goes through I/O function 1 with a 17-bit
 * CMD52/CMD53 address whose bits 16:13 pick a "domain":
 *   0   SDIO local registers (the bus interface itself), offset in 11:0
 *   4   TX FIFO of the high priority queue  \  bits 12:0: the length of
 *   5   TX FIFO of the normal queue          > what is written, in 32-bit
 *   6   TX FIFO of the low priority queue   /  words (the "agg length")
 *   7   RX FIFO 0, bits 1:0 a sequence number that changes every read
 *   8   MAC registers, offset in 15:0 (bit 16 set, so 15:13 are offset)
 * Only CMD52 works while the MAC is powered down (card disabled or in
 * firmware power save); CMD53 needs it on.
 *
 * Learned from (not copied from) Linux: drivers/staging/rtl8723bs
 * hal/sdio_ops.c (_cvrt2ftaddr(), hal_sdio_get_cmd_addr_8723b(),
 * sdio_read32(), sd_int_hdl(), sd_int_dpc()) and include/hal_com_reg.h,
 * GPL-2.0; and drivers/net/wireless/realtek/rtw88 sdio.c and sdio.h,
 * GPL-2.0 OR BSD-3-Clause (rtw_sdio_to_bus_offset(), rtw_sdio_get_tx_addr(),
 * rtw_sdio_check_free_txpg(), rtw_sdio_rx_isr(), rtw_sdio_rxfifo_recv()).
 * The register offsets and bits are facts of the hardware; the code here
 * is written for this driver.
 */

/* Address domains, bits 16:13 of the I/O address. */
#define RTWN_SDIO_DOM_SHIFT		13
#define RTWN_SDIO_DOM_LOCAL		0
#define RTWN_SDIO_DOM_TX_HIQ		4
#define RTWN_SDIO_DOM_TX_MIQ		5
#define RTWN_SDIO_DOM_TX_LOQ		6
#define RTWN_SDIO_DOM_RX0FF		7
#define RTWN_SDIO_DOM_MAC		8

#define RTWN_SDIO_LOCAL_MASK		0x0fff
#define RTWN_SDIO_MAC_MASK		0xffff
#define RTWN_SDIO_FIFO_LEN_MASK		0x1fff	/* in 32-bit words */
#define RTWN_SDIO_RX_SEQ_MASK		0x0003

/* SDIO local registers (domain 0). */
#define RTWN_SDIO_TX_CTRL		0x000
#define RTWN_SDIO_HIMR			0x014	/* host interrupt mask */
#define RTWN_SDIO_HISR			0x018	/* host interrupt status */
#define RTWN_SDIO_RX0_REQ_LEN		0x01c	/* 16 bits on 11n chips */
#define RTWN_SDIO_OQT_FREE_PG		0x01e	/* output queue table space */
#define RTWN_SDIO_FREE_TXPG		0x020	/* free TX pages, per queue */
#define RTWN_SDIO_HCPWM1		0x025
#define RTWN_SDIO_BTCOEX_ANT		0x060	/* RTL8723BS: antenna setup */
#define RTWN_SDIO_HRPWM1		0x080
#define RTWN_SDIO_HSUS_CTRL		0x086	/* HCI suspend control */
#define RTWN_SDIO_HSUS_SUS_REQ		0x01	/* request suspend */
#define RTWN_SDIO_HSUS_RESUME_RDY	0x02	/* out of suspend */

/* Bits of RTWN_SDIO_HIMR and RTWN_SDIO_HISR. */
#define RTWN_SDIO_INT_RX_REQUEST	0x00000001	/* RX FIFO has data */
#define RTWN_SDIO_INT_AVAL		0x00000002	/* TX pages freed */
#define RTWN_SDIO_INT_TXERR		0x00000004
#define RTWN_SDIO_INT_RXERR		0x00000008
#define RTWN_SDIO_INT_TXFOVW		0x00000010
#define RTWN_SDIO_INT_RXFOVW		0x00000020
#define RTWN_SDIO_INT_TXBCNOK		0x00000040
#define RTWN_SDIO_INT_TXBCNERR		0x00000080
#define RTWN_SDIO_INT_BCNERLY		0x00010000
#define RTWN_SDIO_INT_C2HCMD		0x00020000
#define RTWN_SDIO_INT_CPWM1		0x00040000
#define RTWN_SDIO_INT_CPWM2		0x00080000
#define RTWN_SDIO_INT_HSISR_IND		0x00100000
#define RTWN_SDIO_INT_GTINT3		0x00200000
#define RTWN_SDIO_INT_GTINT4		0x00400000
#define RTWN_SDIO_INT_PSTIMEOUT		0x00800000
#define RTWN_SDIO_INT_OCPINT		0x01000000
/*
 * Status bits the host acknowledges by writing ones. RX_REQUEST and AVAL
 * are levels: they stay while the FIFO has data or pages are free.
 */
#define RTWN_SDIO_INT_W1C						\
	(RTWN_SDIO_INT_TXERR | RTWN_SDIO_INT_RXERR |			\
	 RTWN_SDIO_INT_TXFOVW | RTWN_SDIO_INT_RXFOVW |			\
	 RTWN_SDIO_INT_TXBCNOK | RTWN_SDIO_INT_TXBCNERR |		\
	 RTWN_SDIO_INT_C2HCMD | RTWN_SDIO_INT_CPWM1 |			\
	 RTWN_SDIO_INT_CPWM2 | RTWN_SDIO_INT_HSISR_IND |		\
	 RTWN_SDIO_INT_GTINT3 | RTWN_SDIO_INT_GTINT4 |			\
	 RTWN_SDIO_INT_PSTIMEOUT | RTWN_SDIO_INT_OCPINT)

/* Queues as RTWN_SDIO_FREE_TXPG counts them, one byte each. */
#define RTWN_SDIO_QUEUE_HIGH		0
#define RTWN_SDIO_QUEUE_NORMAL		1
#define RTWN_SDIO_QUEUE_LOW		2
#define RTWN_SDIO_QUEUE_PUBLIC		3
#define RTWN_SDIO_QUEUE_COUNT		3	/* the ones with a FIFO */

#define RTWN_SDIO_TX_PAGE_SIZE		128
#define RTWN_SDIO_RX_DESC_SIZE		24
#define RTWN_SDIO_RX_ALIGN		8	/* packets in an RX aggregate */
#define RTWN_SDIO_BLOCK_SIZE		512

#endif	/* RTWN_SDIO_REG_H */
