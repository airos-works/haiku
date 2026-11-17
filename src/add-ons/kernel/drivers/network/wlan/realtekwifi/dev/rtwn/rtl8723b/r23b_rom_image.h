/*
 * Copyright (c) 2026 Ahmad Khalifa <vexeduxr@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef R23B_ROM_IMAGE_H
#define R23B_ROM_IMAGE_H

#include <dev/rtwn/rtl8723b/r23b_rom_defs.h>

#define R23B_DEF_TX_PWR_2G	0x2d
#define R23B_DEF_TX_HT20_DIFF	0x02
#define R23B_DEF_TX_OFDM_DIFF	0x04
#define R23B_DEF_TX_DIFF	0xfe

struct r23b_tx_pwr_2g {
	uint8_t		cck[R23B_GROUP_2G];
	uint8_t		ht40[R23B_GROUP_2G - 1];
} __packed;

struct r23b_tx_pwr_diff123_2g {
	uint8_t	ht40_ht20;
	uint8_t	ofdm_cck;
} __packed;

struct r23b_tx_pwr_diff_2g {
	uint8_t		ht20_ofdm;
	struct r23b_tx_pwr_diff123_2g diff123[R23B_MAX_TX_COUNT - 1];
} __packed;

struct r23b_tx_pwr {
	struct r23b_tx_pwr_2g		pwr_2g;
	struct r23b_tx_pwr_diff_2g	pwr_diff_2g;
	uint8_t				reserved[24];
} __packed;

struct r23b_rom {
	uint16_t		id;
	uint8_t			reserved1[14];
	struct r23b_tx_pwr 	tx_pwr[R23B_MAX_RF_PATH];
	uint8_t			channel_plan;
	uint8_t			crystal_cap;
#define R23B_ROM_CRYSTALCAP_DEF		0x20

	uint8_t			thermal_meter;
#define R23B_ROM_THERMALMETER_DEF	0x18

	uint8_t			iqk_lck;
	uint8_t			pa_type;
	uint8_t			lna_type_2g;
	uint8_t			reserved2;
	uint8_t			lna_type_5g;
	uint8_t			reserved3;
	uint8_t			rf_board_opt;
	uint8_t			rf_feature_opt;
	uint8_t			rf_bt_opt;
	uint8_t			version;
	uint8_t			customer_id;
	uint8_t			tx_bswing_2g;
	uint8_t			reserved4;
	uint8_t			tx_pwr_calib_rate;
	uint8_t			rf_antenna_opt;
	uint8_t			rfe_opt;
	uint8_t			country_code;

	/*
	 * Haiku: the rest of the map depends on the bus. FreeBSD has only
	 * the USB layout; the SDIO one (RTL8723BS: MAC address at 0x11a) is
	 * for the SDIO attachment.
	 */
	union {
		struct {
			uint8_t reserved1[52];
			uint16_t vid;
			uint16_t pid;
			uint8_t reserved2[3];
			uint8_t macaddr[IEEE80211_ADDR_LEN];
			uint8_t reserved3[243];
		} __packed usb;
		struct {
			uint8_t reserved1[78];
			uint8_t macaddr[IEEE80211_ADDR_LEN];
			uint8_t reserved2[224];
		} __packed sdio;
	};
} __packed;

_Static_assert(sizeof(struct r23b_rom) == R23B_EFUSE_MAP_LEN,
    "R23B_EFUSE_MAP_LEN must be equal to sizeof(struct r23b_rom)!");

/*
 * Haiku: the offsets the vendor's drivers use for the RTL8723B logical
 * eFuse map, so that a change to the structure above cannot move a field.
 */
#define R23B_ROM_ASSERT_OFFSET(field, offset)				\
	_Static_assert(__builtin_offsetof(struct r23b_rom, field) == (offset), \
	    "struct r23b_rom: " #field " must be at " #offset)
R23B_ROM_ASSERT_OFFSET(tx_pwr, 0x10);
R23B_ROM_ASSERT_OFFSET(channel_plan, 0xb8);
R23B_ROM_ASSERT_OFFSET(crystal_cap, 0xb9);
R23B_ROM_ASSERT_OFFSET(thermal_meter, 0xba);
R23B_ROM_ASSERT_OFFSET(rf_board_opt, 0xc1);
R23B_ROM_ASSERT_OFFSET(rf_bt_opt, 0xc3);
R23B_ROM_ASSERT_OFFSET(usb.vid, 0x100);
R23B_ROM_ASSERT_OFFSET(usb.pid, 0x102);
R23B_ROM_ASSERT_OFFSET(usb.macaddr, 0x107);
R23B_ROM_ASSERT_OFFSET(sdio.macaddr, 0x11a);
#undef R23B_ROM_ASSERT_OFFSET

#endif /* R23B_ROM_IMAGE_H */
