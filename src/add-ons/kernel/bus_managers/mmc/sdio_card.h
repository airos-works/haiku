/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SDIO_CARD_H
#define SDIO_CARD_H


/*!	The SDIO protocol, apart from any host: identification (CMD5), the CCCR,
	FBRs and CIS, and the framing of CMD52 and CMD53. The MMC bus runs it on
	its host; the unit test (test/) on a simulated card. */


#include <SupportDefs.h>

#include "sdio.h"


struct sdio_host_ops {
	void*		cookie;
	// a command without data: CMD5 (R4), CMD52 (R5), CMD3, CMD7
	status_t	(*command)(void* cookie, uint8 command, uint32 argument,
					uint32* response);
	// CMD53, see mmc_bus_interface::sdio_transfer
	status_t	(*transfer)(void* cookie, uint32 argument, bool write,
					void* buffer, uint16 blockSize, uint16 blockCount,
					uint32* response);
	void		(*sleep)(void* cookie, bigtime_t duration);
	bigtime_t	(*time)(void* cookie);
};


struct sdio_card {
	sdio_card_info	info;
	uint32			ocr;
	uint8			function_class[SDIO_MAX_FUNCTIONS];
	uint16			function_vendor[SDIO_MAX_FUNCTIONS];
	uint16			function_device[SDIO_MAX_FUNCTIONS];
	uint32			enable_timeout[SDIO_MAX_FUNCTIONS];	// ms
	uint32			max_clock;			// kHz, from the common CIS
};


status_t	sdio_rw_direct(const sdio_host_ops& host, bool write, uint8 function,
				uint32 address, uint8 value, uint8* _value,
				bool readAfterWrite = false);

/*!	\a blockSize: the function's block size, 0 if none was set (byte mode
	only). Whole blocks go in block mode if the card has multi-block
	transfers (\a multiBlock), the rest in byte mode transfers of at most
	\a maxByteSize (1 to 512) bytes. */
status_t	sdio_rw_extended(const sdio_host_ops& host, bool write,
				uint8 function, uint32 address, bool increment, uint8* buffer,
				size_t length, uint16 blockSize, bool multiBlock,
				uint16 maxByteSize);

/*!	CMD5 until the card is ready. \a hostVoltages: the OCR window the host
	supplies. Returns B_DEVICE_NOT_FOUND if no I/O card answers. */
status_t	sdio_negotiate_ocr(const sdio_host_ops& host, uint32 hostVoltages,
				uint32* _ocr, bigtime_t timeout = 1000000);

/*!	Reads the selected card's CCCR, common CIS, and every function's FBR and
	CIS into \a card. */
status_t	sdio_read_card(const sdio_host_ops& host, uint32 ocr,
				sdio_card* card);

/*!	The kHz of a CIS TRAN_SPEED byte, 0 if it is invalid. */
uint32		sdio_tran_speed(uint8 value);

/*!	The handlers to call for the card's pending register \a pending, of the
	functions with a handler (bit n: function n). With one handler, the
	pending register is not read (\a pendingRead false) and that one is
	called. */
uint8		sdio_interrupts_to_serve(uint8 handlers, uint8 pending,
				bool pendingRead);

/*!	IENx after function \a function's handler came or went. */
uint8		sdio_interrupt_enable(uint8 current, uint8 function, bool enable);


#endif	// SDIO_CARD_H
