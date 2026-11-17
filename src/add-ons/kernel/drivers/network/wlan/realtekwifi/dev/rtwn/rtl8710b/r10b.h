/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

#ifndef RTL8710B_H
#define RTL8710B_H

/*
 * RTL8710B, sold as the RTL8188GU USB Wi-Fi dongle chip (Linux calls it
 * RTL8710BU, rtl8xxxu/8710b.c). A one-stream 2.4 GHz 802.11n MAC/BB/RF of
 * the RTL8188F generation, behind an Ameba Cortex-M4 SoC that also boots
 * the 8051 Wi-Fi firmware. What differs from the RTL8188F for the driver:
 * - over USB the MAC registers 0x00..0xff sit at 0x8000 + address, and
 *   the 8051 firmware control register is at 0x90 (usb/r10bu_reg.c);
 * - SYSON registers and the eFuse are read through an indirect window
 *   (r10b_rom.c);
 * - power on/off are its own sequences (usb/r10bu_init.c);
 * - firmware per foundry: rtl8710bufw_SMIC.bin or _UMC.bin;
 * - two package variants (QFN48M-U, -S) with their own BB and RF tables;
 * - the PHY status is the newer ("Jaguar 2") layout (r10b_rx.c);
 * - TX descriptor checksums are inverted.
 * The register tables come from a GPL data file (r10b_tables.h).
 */

#include <dev/rtwn/if_rtwn_ridx.h>

/* Global definitions. */
#define R10B_TX_PAGE_COUNT	247
#define R10B_TXPKTBUF_COUNT	177
#define R10B_HQ_NPAGES		12
#define R10B_LQ_NPAGES		2
#define R10B_NQ_NPAGES		2
#define R10B_PUBQ_NPAGES	142
#define R10B_RX_DMA_BUFFER_SIZE	0x3f80
#define R10B_MAX_FW_SIZE	0x8000
#define R10B_MACID_MAX		15
#define R10B_CAM_ENTRY_COUNT	32
#define R10B_EFUSE_MAX_LEN	512
#define R10B_EFUSE_MAP_LEN	512
#define R10B_EFUSE_ID		0x8195
#define R10B_FW_SIGNATURE	0x10b	/* 0x10b1 SMIC, 0x10b2 UMC */
#define R10B_AMPDU_MAX_TIME	0x5e
#define R10B_ADDA_1T		0x03c00016

/* Firmware and table file names (glue.c maps them to files). */
#define R10B_FWNAME_SMIC	"rtwn-rtl8710bufw_SMIC"
#define R10B_FWNAME_UMC		"rtwn-rtl8710bufw_UMC"
#define R10B_TABLES_NAME	"rtwn-rtl8710b-tables"

struct rtwn_softc;
struct ieee80211_channel;
struct ieee80211_rx_stats;

/* r10b_calib.c */
void	r10b_lc_calib(struct rtwn_softc *);
void	r10b_iq_calib(struct rtwn_softc *);

/* r10b_chan.c */
void	r10b_set_chan(struct rtwn_softc *, struct ieee80211_channel *);

/* r10b_fw.c */
void	r10b_fw_reset(struct rtwn_softc *, int);
void	r10b_fw_download_enable(struct rtwn_softc *, int);

/* r10b_init.c */
int	r10b_tables_load(struct rtwn_softc *);
void	r10b_tables_free(struct rtwn_softc *);
void	r10b_init_bb(struct rtwn_softc *);
void	r10b_init_rf(struct rtwn_softc *);
void	r10b_init_ampdu(struct rtwn_softc *);
void	r10b_init_stats(struct rtwn_softc *);
void	r10b_set_crystal_cap(struct rtwn_softc *);
void	r10b_detach_private(struct rtwn_softc *);
void	r10b_postattach(struct rtwn_softc *);
void	r10b_temp_measure(struct rtwn_softc *);

/* r10b_rom.c */
uint32_t r10b_syson_read(struct rtwn_softc *, uint32_t);
int	r10b_syson_write(struct rtwn_softc *, uint32_t, uint32_t);
int	r10b_efuse_read_1(struct rtwn_softc *, uint16_t, uint8_t *);
int	r10b_efuse_preread(struct rtwn_softc *);
int	r10b_efuse_read(struct rtwn_softc *, uint8_t *, uint16_t);
void	r10b_parse_rom(struct rtwn_softc *, uint8_t *);

/* r10b_rx.c */
int8_t	r10b_get_rssi_cck(struct rtwn_softc *, void *);
int8_t	r10b_get_rssi_ofdm(struct rtwn_softc *, void *);

#endif	/* RTL8710B_H */
