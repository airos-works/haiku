/*
 * Copyright (c) 2026 Ahmad Khalifa <vexeduxr@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef R23B_VAR_H
#define R23B_VAR_H

#include <dev/rtwn/rtl8723b/r23b_rom_defs.h>

struct r23b_softc {
	struct r92e_softc super;

	uint8_t bt_antnum;

	/*
	 * Haiku: how the antenna is wired, for the SDIO attachment. FreeBSD
	 * knew only the RTL8723BU, fixed at S0 (the auxiliary port) with the
	 * antenna "inverted" and its setting in USB local register 0xfe08;
	 * that is what all zeros here mean. The RTL8723BS reads the port from
	 * efuse 0xc3 bit 6 and keeps the setting in an SDIO local register
	 * (Linux staging rtl8723bs hal/rtl8723b_hal_init.c
	 * Hal_EfuseParseBTCoexistInfo_8723B(), hal/HalBtc8723b1Ant.c
	 * EXhalbtc8723b1ant_PowerOnSetting(), halbtc8723b1ant_SetAntPath()).
	 */
	uint8_t ant_main;	/* antenna at the main port (S1), not inverted */
	int (*rs_ant_local_write)(struct rtwn_softc *, uint8_t);
				/* NULL: the USB register */
};

#endif
