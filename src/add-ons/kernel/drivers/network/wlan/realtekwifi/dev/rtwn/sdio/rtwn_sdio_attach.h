/*-
 * Copyright (c) 2026 air/OS
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RTWN_SDIO_ATTACH_H
#define RTWN_SDIO_ATTACH_H

void	r23bs_attach(struct rtwn_sdio_softc *);

enum {
	RTWN_CHIP_RTL8723BS,
	RTWN_CHIP_SDIO_MAX
};

struct rtwn_sdio_ident {
	uint16_t	vendor;
	uint16_t	device;
	int		chip;
};

/*
 * Realtek (0x024c) RTL8723BS modules, the IDs Linux's staging driver
 * claims (os_dep/sdio_intf.c sdio_ids[]). 0xb723 is the common one.
 */
#define RTWN_SDIO_VENDOR_REALTEK	0x024c

static const struct rtwn_sdio_ident rtwn_sdio_ident_table[] = {
	{ RTWN_SDIO_VENDOR_REALTEK, 0x0523, RTWN_CHIP_RTL8723BS },
	{ RTWN_SDIO_VENDOR_REALTEK, 0x0525, RTWN_CHIP_RTL8723BS },
	{ RTWN_SDIO_VENDOR_REALTEK, 0x0623, RTWN_CHIP_RTL8723BS },
	{ RTWN_SDIO_VENDOR_REALTEK, 0x0626, RTWN_CHIP_RTL8723BS },
	{ RTWN_SDIO_VENDOR_REALTEK, 0x0627, RTWN_CHIP_RTL8723BS },
	{ RTWN_SDIO_VENDOR_REALTEK, 0xb723, RTWN_CHIP_RTL8723BS },
};

typedef void (*chip_sdio_attach)(struct rtwn_sdio_softc *);

static const chip_sdio_attach rtwn_sdio_chip_attach[RTWN_CHIP_SDIO_MAX] = {
	[RTWN_CHIP_RTL8723BS]	= r23bs_attach,
};

static __inline void
rtwn_sdio_attach_private(struct rtwn_sdio_softc *sd, int chip)
{
	rtwn_sdio_chip_attach[chip](sd);
}

#endif	/* RTWN_SDIO_ATTACH_H */
