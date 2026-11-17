/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef BROADCOMFMAC_BWFM_HOST_H
#define BROADCOMFMAC_BWFM_HOST_H


#include <SupportDefs.h>


/*	The Wi-Fi chip's SDIO card, for the driver. Behind it is an SDIO function
	interface (headers/private/drivers/sdio.h): the MMC bus' function node of
	the card, wherever the MMC bus has found one (the Raspberry Pi 5's Wi-Fi
	port, any SD host controller), or the driver's own host for the
	Raspberry Pi 4's port (sdio_host.h), which the MMC bus does not drive. */

#ifdef __cplusplus
extern "C" {
#endif

/*!	Is there a chip: a Broadcom or Cypress SDIO function 1 on the MMC bus,
	or the Raspberry Pi 4's port. On a board whose port the MMC bus drives,
	waits a few seconds for the bus to have found the card. */
bool		bwfm_host_present(void);

status_t	bwfm_host_init(void);
void		bwfm_host_uninit(void);

/*!	CMD52. A write reads the register back into \a _value. */
status_t	bwfm_host_rw_byte(bool write, uint32 function, uint32 address,
				uint8* _value);

/*!	CMD53: \a length bytes, \a increment: the address counts up. */
status_t	bwfm_host_rw_extended(bool write, uint32 function, uint32 address,
				uint8* buffer, size_t length, bool increment);

status_t	bwfm_host_set_block_size(uint32 function, uint16 size);
status_t	bwfm_host_enable_function(uint32 function, bool enable);

/*!	\a handler is called in a thread when the card raises its interrupt
	for function 1 or 2, and serves the card before it returns; the
	interrupt is off until then. NULL takes it away, and returns once it
	is no longer running. */
status_t	bwfm_host_set_interrupt_handler(void (*handler)(void*),
				void* data);

#ifdef __cplusplus
}
#endif

#endif	/* BROADCOMFMAC_BWFM_HOST_H */
