/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _FBSD_COMPAT_DEV_SDIO_SDIO_SUBR_H_
#define _FBSD_COMPAT_DEV_SDIO_SDIO_SUBR_H_


/*
 * SDIO function drivers of the compatibility layer.
 *
 * The cards are found and set up by Haiku's MMC bus manager, which publishes
 * one device node per I/O function (headers/private/drivers/sdio.h). The
 * compatibility layer (sdio.cpp, in freebsd_sdio.a) looks the functions up
 * by the node attributes sdio_find_function() uses, gives each one a device_t
 * on a "sdiob" root (FreeBSD's sdiob(4) is the parent of function drivers)
 * and hands the driver a struct sdio_func as its ivars.
 *
 * The names follow FreeBSD's dev/sdio/sdio_subr.h where FreeBSD has the
 * call; CMD53 and the card interrupt, which FreeBSD's function drivers do
 * not have yet, are Haiku additions. Every call may block; none may be made
 * from a filter or with a spin lock held. Errors are errno values.
 */


#include <sys/bus.h>


struct sdio_func {
	device_t	dev;
	uint16_t	vendor;		/* CISTPL_MANFID */
	uint16_t	device;
	uint8_t		fn;		/* 1 to 7 */
	uint8_t		fclass;		/* FBR standard interface code (FreeBSD: class) */
	uint16_t	max_blksize;	/* from the function's CIS */
	uint16_t	cur_blksize;	/* as set, 0: byte mode only */

	/* Haiku: the card as the MMC bus set it up */
	uint8_t		bus_width;	/* 1 or 4 */
	uint8_t		high_speed;
	uint8_t		capabilities;	/* CCCR card capability */
	uint32_t	clock;		/* kHz */

	/* Haiku: private to the compatibility layer */
	void		*haiku_node;
	void		*haiku_interface;
	void		*haiku_cookie;
};


static inline struct sdio_func *
sdio_get_function(device_t dev)
{
	return ((struct sdio_func *)device_get_ivars(dev));
}

static inline uint16_t
sdio_get_vendor(device_t dev)
{
	return (sdio_get_function(dev)->vendor);
}

static inline uint16_t
sdio_get_device(device_t dev)
{
	return (sdio_get_function(dev)->device);
}

static inline uint8_t
sdio_get_funcnum(device_t dev)
{
	return (sdio_get_function(dev)->fn);
}


#ifdef __cplusplus
extern "C" {
#endif

int	sdio_enable_func(struct sdio_func *);
int	sdio_disable_func(struct sdio_func *);
int	sdio_set_block_size(struct sdio_func *, uint16_t);

/* CMD52 on the function, and on function 0 (CCCR, FBR, CIS) */
uint8_t	sdio_read_1(struct sdio_func *, uint32_t, int *);
void	sdio_write_1(struct sdio_func *, uint32_t, uint8_t, int *);
uint8_t	sdio_f0_read_1(struct sdio_func *, uint32_t, int *);
void	sdio_f0_write_1(struct sdio_func *, uint32_t, uint8_t, int *);

/*
 * Haiku: CMD53. The bus sends what is at least a block in block mode and
 * the rest in byte mode, one command per block run or byte chunk; a caller
 * that needs the transfer in one command (a FIFO with the length in its
 * address) pads it to a multiple of the block size, or keeps it within a
 * block. incaddr: the address counts up (memory) or stays (a FIFO port).
 */
int	sdio_read_extended(struct sdio_func *, uint32_t, bool, void *,
	    size_t);
int	sdio_write_extended(struct sdio_func *, uint32_t, bool,
	    const void *, size_t);

/*
 * Haiku: the function's card interrupt. The handler runs in a thread of
 * the MMC bus, may do I/O and take sleepable locks; the card interrupt stays
 * masked until it returns, so it must serve its function before returning.
 * sdio_teardown_intr() returns once the handler is no longer running: do not
 * call it with a lock held that the handler takes.
 */
int	sdio_setup_intr(struct sdio_func *, void (*)(void *), void *);
int	sdio_teardown_intr(struct sdio_func *);

#ifdef __cplusplus
}
#endif


#endif	/* _FBSD_COMPAT_DEV_SDIO_SDIO_SUBR_H_ */
