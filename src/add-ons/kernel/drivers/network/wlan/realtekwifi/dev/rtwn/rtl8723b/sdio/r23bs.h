/*-
 * Copyright (c) 2026 air/OS
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef RTL8723BS_H
#define RTL8723BS_H

#include <dev/rtwn/rtl8723b/r23b.h>

/* r23bs_init.c */
int	r23bs_power_on(struct rtwn_softc *);
void	r23bs_power_off(struct rtwn_softc *);
int	r23bs_set_page_size(struct rtwn_softc *);
void	r23bs_init_intr(struct rtwn_softc *);
void	r23bs_init_rx_agg(struct rtwn_softc *);
void	r23bs_init_ampdu(struct rtwn_softc *);
int	r23bs_efuse_preread(struct rtwn_softc *);
int	r23bs_ant_local_write(struct rtwn_softc *, uint8_t);

/* r23bs_rom.c */
void	r23bs_parse_rom(struct rtwn_softc *, uint8_t *);

#endif	/* RTL8723BS_H */
