/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

#ifndef RTL8710BU_H
#define RTL8710BU_H

#include <dev/rtwn/rtl8710b/r10b.h>

/* r10bu_init.c */
int	r10bu_power_on(struct rtwn_softc *);
void	r10bu_power_off(struct rtwn_softc *);
void	r10bu_init_intr(struct rtwn_softc *);
void	r10bu_init_rx_agg(struct rtwn_softc *);
void	r10bu_enable_mac_txrx(struct rtwn_softc *);
void	r10bu_post_init(struct rtwn_softc *);

/* r10bu_reg.c */
void	r10bu_attach_reg_methods(struct rtwn_softc *);
int	r10bu_write_4_direct(struct rtwn_softc *, uint16_t, uint32_t);

#endif	/* RTL8710BU_H */
