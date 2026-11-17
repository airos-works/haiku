/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef BROADCOMFMAC_SDMMC_SHIM_H
#define BROADCOMFMAC_SDMMC_SHIM_H


/*	What if_bwfm_sdio.c uses of OpenBSD's sdmmc(4), on the chip's SDIO card
	(bwfm_host.h: the MMC bus' SDIO function, or the Raspberry Pi 4's port). */

#include <sys/rwlock.h>

#include "bwfm_host.h"


struct sdmmc_softc {
	int				sc_function_count;
	struct rwlock	sc_lock;
};

struct sdmmc_function {
	int					number;
	struct sdmmc_softc*	sc;
};


static inline int
sdmmc_io_set_blocklen(struct sdmmc_function* sf, unsigned int length)
{
	return bwfm_host_set_block_size(sf->number, length) == B_OK ? 0 : EIO;
}


static inline int
sdmmc_io_function_enable(struct sdmmc_function* sf)
{
	return bwfm_host_enable_function(sf->number, true) == B_OK ? 0 : EIO;
}


static inline void
sdmmc_io_function_disable(struct sdmmc_function* sf)
{
	bwfm_host_enable_function(sf->number, false);
}


static inline uint8_t
sdmmc_io_read_1(struct sdmmc_function* sf, int reg)
{
	uint8_t value = 0;
	bwfm_host_rw_byte(false, sf->number, reg, &value);
	return value;
}


static inline void
sdmmc_io_write_1(struct sdmmc_function* sf, int reg, uint8_t value)
{
	bwfm_host_rw_byte(true, sf->number, reg, &value);
}


static inline uint32_t
sdmmc_io_read_4(struct sdmmc_function* sf, int reg)
{
	uint32_t value = 0;
	bwfm_host_rw_extended(false, sf->number, reg, (uint8*)&value, 4, true);
	return le32toh(value);
}


static inline void
sdmmc_io_write_4(struct sdmmc_function* sf, int reg, uint32_t value)
{
	value = htole32(value);
	bwfm_host_rw_extended(true, sf->number, reg, (uint8*)&value, 4, true);
}


static inline int
sdmmc_io_read_multi_1(struct sdmmc_function* sf, int reg, u_char* data,
	int length)
{
	return bwfm_host_rw_extended(false, sf->number, reg, data, length, false)
		== B_OK ? 0 : EIO;
}


static inline int
sdmmc_io_read_region_1(struct sdmmc_function* sf, int reg, u_char* data,
	int length)
{
	return bwfm_host_rw_extended(false, sf->number, reg, data, length, true)
		== B_OK ? 0 : EIO;
}


static inline int
sdmmc_io_write_region_1(struct sdmmc_function* sf, int reg, u_char* data,
	int length)
{
	return bwfm_host_rw_extended(true, sf->number, reg, data, length, true)
		== B_OK ? 0 : EIO;
}


#endif	/* BROADCOMFMAC_SDMMC_SHIM_H */
