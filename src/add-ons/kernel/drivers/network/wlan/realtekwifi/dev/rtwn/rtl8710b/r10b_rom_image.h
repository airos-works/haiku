/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

#ifndef R10B_ROM_IMAGE_H
#define R10B_ROM_IMAGE_H

#include <dev/rtwn/rtl8188e/r88e_rom_defs.h>

/*
 * The RTL8710B logical eFuse map (512 bytes). The fields the driver uses
 * are where the vendor driver keeps them: the TX power block of the
 * RTL8188F layout at 0x20 instead of 0x10, then channel plan, crystal
 * and thermal meter at 0xc8..0xca, the MAC address at 0x11a and the USB
 * IDs at 0x1c0.
 */
struct r10b_rom {
	uint16_t	id;			/* 0x8195 */
	uint8_t		reserved1[30];
	uint8_t		cck_tx_pwr[R88E_GROUP_2G];		/* 0x20 */
	uint8_t		ht40_tx_pwr[R88E_GROUP_2G - 1];	/* 0x26 */
	uint8_t		tx_pwr_diff;		/* 0x2b: OFDM low, HT20 high */
	uint8_t		reserved2[156];
	uint8_t		channel_plan;		/* 0xc8 */
	uint8_t		crystalcap;		/* 0xc9 */
	uint8_t		thermal_meter;		/* 0xca */
	uint8_t		reserved3[79];
	uint8_t		macaddr[IEEE80211_ADDR_LEN];	/* 0x11a */
	uint8_t		reserved4[17];
	uint8_t		rf_board_opt;		/* 0x131 */
	uint8_t		reserved5[2];
	uint8_t		version;		/* 0x134 */
	uint8_t		customer_id;		/* 0x135 */
	uint8_t		reserved6[5];
	uint8_t		country_code;		/* 0x13b */
	uint8_t		reserved7[132];
	uint16_t	vid;			/* 0x1c0 */
	uint16_t	pid;			/* 0x1c2 */
	uint8_t		reserved8[60];
} __packed;

_Static_assert(sizeof(struct r10b_rom) == 512,
    "struct r10b_rom must be the 512-byte logical eFuse map");

#define R10B_ROM_ASSERT_OFFSET(field, offset)				\
	_Static_assert(__builtin_offsetof(struct r10b_rom, field) == (offset), \
	    "struct r10b_rom: " #field " must be at " #offset)
R10B_ROM_ASSERT_OFFSET(cck_tx_pwr, 0x20);
R10B_ROM_ASSERT_OFFSET(tx_pwr_diff, 0x2b);
R10B_ROM_ASSERT_OFFSET(channel_plan, 0xc8);
R10B_ROM_ASSERT_OFFSET(thermal_meter, 0xca);
R10B_ROM_ASSERT_OFFSET(macaddr, 0x11a);
R10B_ROM_ASSERT_OFFSET(rf_board_opt, 0x131);
R10B_ROM_ASSERT_OFFSET(country_code, 0x13b);
R10B_ROM_ASSERT_OFFSET(vid, 0x1c0);
#undef R10B_ROM_ASSERT_OFFSET

#endif	/* R10B_ROM_IMAGE_H */
