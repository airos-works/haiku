/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef BROADCOMFMAC_SDIO_HOST_H
#define BROADCOMFMAC_SDIO_HOST_H


#include <SupportDefs.h>


/*	The Raspberry Pi 4's SDIO port: the BCM2711's first SD host controller
	(Arasan, "mmcnr") with the board's Wi-Fi chip as its only card. No driver
	of the MMC bus runs that controller yet (its clock and the chip's power
	line WL_ON are the firmware's, behind its mailbox), so the Wi-Fi driver
	brings its own small host for it, behind an SDIO function interface
	(bwfm_host.cpp). The Pi 5's port is the MMC bus' (bcm2711_emmc2). */

#ifdef __cplusplus
extern "C" {
#endif

/*!	Is this a Raspberry Pi with that controller? Touches no hardware. */
bool		rpi_sdio_present(void);

/*!	Powers the Wi-Fi chip, sets up the controller and the card (4 bit,
	high speed if the card has it). */
status_t	rpi_sdio_init(void);
void		rpi_sdio_uninit(void);

/*!	CMD52: one register of a function. With \a write, \a _value is written
	and the value read back is returned in it. */
status_t	rpi_sdio_rw_byte(bool write, uint32 function, uint32 address,
				uint8* _value);

/*!	CMD53: \a length bytes from or to a function's address, in blocks of the
	function's block size where it pays off. \a increment: the address
	counts up (memory) or stays (a FIFO). */
status_t	rpi_sdio_rw_extended(bool write, uint32 function, uint32 address,
				uint8* buffer, size_t length, bool increment);

status_t	rpi_sdio_set_block_size(uint32 function, uint16 size);
status_t	rpi_sdio_enable_function(uint32 function, bool enable);

/*!	Whether the card raises its interrupt line right now. */
bool		rpi_sdio_card_interrupt(void);

/*!	The card interrupt: \a handler is called in interrupt context when the
	card raises its line, and the interrupt is then off until
	rpi_sdio_enable_card_interrupt() is called again (the line stays raised
	until the card's function has been served). */
status_t	rpi_sdio_set_interrupt_handler(void (*handler)(void*), void* data);
void		rpi_sdio_enable_card_interrupt(void);

#ifdef __cplusplus
}
#endif

#endif	/* BROADCOMFMAC_SDIO_HOST_H */
