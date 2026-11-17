/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

#ifndef R10B_VAR_H
#define R10B_VAR_H

#include <dev/rtwn/rtl8188e/r88e_priv.h>	/* struct rtwn_r88e_txpwr */
#include <dev/rtwn/rtl8192c/r92c_var.h>

/* A BB, AGC or RF table entry, from the table file. */
struct r10b_reg_val {
	uint16_t	reg;
	uint32_t	val;
};

struct r10b_softc {
	/*
	 * First: the RTL8192C/RTL8188E/RTL8188F code this chip shares casts
	 * sc_priv to struct r92c_softc.
	 */
	struct r92c_softc	rs;

	struct rtwn_r88e_txpwr	txpwr;

	/*
	 * The bus register methods under the address translation of
	 * usb/r10bu_reg.c (physical addresses).
	 */
	int		(*raw_write_1)(struct rtwn_softc *, uint16_t, uint8_t);
	int		(*raw_write_2)(struct rtwn_softc *, uint16_t, uint16_t);
	int		(*raw_write_4)(struct rtwn_softc *, uint16_t, uint32_t);
	uint8_t		(*raw_read_1)(struct rtwn_softc *, uint16_t);
	uint16_t	(*raw_read_2)(struct rtwn_softc *, uint16_t);
	uint32_t	(*raw_read_4)(struct rtwn_softc *, uint16_t);
	int		(*raw_fw_write_block)(struct rtwn_softc *,
			    const uint8_t *, uint16_t, int);

	/* Chip identification (SYSON SYSTEM_CFG0, eFuse package byte). */
	uint8_t		chip_cut;
	uint8_t		vendor;
	uint8_t		package;
	uint8_t		rom_rev;

	/* From the logical eFuse map. */
	uint8_t		xtal_k;		/* 0xff: not programmed */

	/* BB 0xa9c bit 17: CCK PHY status carries a power, not LNA/VGA. */
	int		cck_new_agc;

	/* Register tables from the table file, for this package. */
	struct rtwn_mac_prog	*mac;
	int			mac_count;
	struct r10b_reg_val	*bb;
	int			bb_count;
	struct r10b_reg_val	*agc;
	int			agc_count;
	struct r10b_reg_val	*rf;
	int			rf_count;
	char			tables_source[65];

	/*
	 * CCK TX filter values the BB table sets (0xa20, 0xa24, 0xa28,
	 * 0xaac); channels 13 and 14 use others, the rest these.
	 */
	uint32_t	cck_filter[4];
};
#define R10B_SOFTC(_sc)	((struct r10b_softc *)((_sc)->sc_priv))

#endif	/* R10B_VAR_H */
