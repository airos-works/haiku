/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	The SDIO protocol, see sdio_card.h. Written from the SDIO Simplified
	Specification Version 3.00: CMD5 and R4 (5.1), CMD52 (5.3), CMD53 (5.3),
	R5 (5.4), the CCCR (6.9), the FBR (6.10) and the CIS (16). Where the
	specification leaves room, the choices are Linux's (drivers/mmc/core/
	sdio*.c), as noted. */


#include "sdio_card.h"

#include <string.h>

#ifdef _KERNEL_MODE
#	include <KernelExport.h>
#	define SDIO_LOG(x...)	dprintf("\33[33mmmc_bus:\33[0m sdio: " x)
#else
#	include <stdio.h>
#	define SDIO_LOG(x...)	printf("sdio: " x)
#endif


static const uint32 kCisMaxBytes = 4096;
	// stop at that much CIS: the end tuple may be missing or the pointer bad


status_t
sdio_rw_direct(const sdio_host_ops& host, bool write, uint8 function,
	uint32 address, uint8 value, uint8* _value, bool readAfterWrite)
{
	if (function >= SDIO_MAX_FUNCTIONS || address > SDIO_MAX_ADDRESS)
		return B_BAD_VALUE;

	uint32 response = 0;
	status_t status = host.command(host.cookie, 52,
		sdio_direct_argument(write, function, address, value, readAfterWrite),
		&response);
	if (status != B_OK)
		return status;
	if ((response & SDIO_R5_ERROR_MASK) != 0) {
		if ((response & SDIO_R5_OUT_OF_RANGE) != 0
			|| (response & SDIO_R5_FUNCTION_NUMBER) != 0)
			return B_BAD_VALUE;
		return B_IO_ERROR;
	}
	if (_value != NULL)
		*_value = response & 0xff;
	return B_OK;
}


static status_t
sdio_extended(const sdio_host_ops& host, bool write, uint8 function,
	uint32 address, bool increment, uint8* buffer, uint16 blockSize,
	uint16 blockCount)
{
	uint32 response = 0;
	uint32 argument = sdio_extended_argument(write, function, address,
		blockCount != 0, increment, blockCount != 0 ? blockCount : blockSize);
	status_t status = host.transfer(host.cookie, argument, write, buffer,
		blockSize, blockCount, &response);
	if (status != B_OK)
		return status;
	if ((response & SDIO_R5_ERROR_MASK) != 0)
		return B_IO_ERROR;
	return B_OK;
}


status_t
sdio_rw_extended(const sdio_host_ops& host, bool write, uint8 function,
	uint32 address, bool increment, uint8* buffer, size_t length,
	uint16 blockSize, bool multiBlock, uint16 maxByteSize)
{
	if (function >= SDIO_MAX_FUNCTIONS || buffer == NULL)
		return B_BAD_VALUE;
	if (maxByteSize == 0 || maxByteSize > SDIO_MAX_BYTE_COUNT)
		maxByteSize = SDIO_MAX_BYTE_COUNT;
	if (blockSize > 2048)
		return B_BAD_VALUE;
	// The address field is 17 bits: an incrementing transfer must stay in it.
	if (address > SDIO_MAX_ADDRESS
		|| (increment && length > (size_t)SDIO_MAX_ADDRESS + 1 - address))
		return B_BAD_VALUE;

	// Whole blocks first: what is at least a block goes in block mode, as
	// the Raspberry Pi's Wi-Fi was brought up with. (Linux uses block mode
	// only above the byte mode maximum; both are within the specification.)
	while (multiBlock && blockSize != 0 && length >= blockSize) {
		size_t count = length / blockSize;
		if (count > SDIO_MAX_BLOCK_COUNT)
			count = SDIO_MAX_BLOCK_COUNT;
		status_t status = sdio_extended(host, write, function, address,
			increment, buffer, blockSize, (uint16)count);
		if (status != B_OK)
			return status;
		size_t done = count * blockSize;
		buffer += done;
		length -= done;
		if (increment)
			address += done;
	}

	// The rest in byte mode: at most a block (if one is set) at a time
	uint16 chunkSize = maxByteSize;
	if (blockSize != 0 && blockSize < chunkSize)
		chunkSize = blockSize;
	while (length > 0) {
		uint16 size = length < chunkSize ? (uint16)length : chunkSize;
		status_t status = sdio_extended(host, write, function, address,
			increment, buffer, size, 0);
		if (status != B_OK)
			return status;
		buffer += size;
		length -= size;
		if (increment)
			address += size;
	}
	return B_OK;
}


status_t
sdio_negotiate_ocr(const sdio_host_ops& host, uint32 hostVoltages,
	uint32* _ocr, bigtime_t timeout)
{
	// CMD5 with no voltage window asks for the card's (5.1); a memory card
	// does not answer.
	uint32 ocr = 0;
	status_t status = host.command(host.cookie, 5, 0, &ocr);
	if (status != B_OK)
		return B_DEVICE_NOT_FOUND;
	if (SDIO_OCR_FUNCTION_COUNT(ocr) == 0) {
		// a memory-only card that knows CMD5
		return B_DEVICE_NOT_FOUND;
	}

	uint32 voltages = ocr & hostVoltages & SDIO_OCR_VOLTAGE_MASK;
	if (voltages == 0) {
		SDIO_LOG("no common voltage: card %#" B_PRIx32 ", host %#" B_PRIx32
			"\n", ocr & SDIO_OCR_VOLTAGE_MASK, hostVoltages);
		return B_NOT_SUPPORTED;
	}

	// Again with the window until the card is ready; Linux gives it a
	// second, in steps of 10 ms.
	bigtime_t deadline = host.time(host.cookie) + timeout;
	while (true) {
		status = host.command(host.cookie, 5, voltages, &ocr);
		if (status != B_OK)
			return status;
		if ((ocr & SDIO_OCR_READY) != 0)
			break;
		if (host.time(host.cookie) >= deadline)
			return B_TIMED_OUT;
		host.sleep(host.cookie, 10000);
	}

	*_ocr = ocr;
	return B_OK;
}


uint32
sdio_tran_speed(uint8 value)
{
	// TRAN_SPEED: a time value (tenths) times a unit, as in the SD CSD
	static const uint8 kValues[16] = {
		0, 10, 12, 13, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 70, 80
	};
	static const uint32 kUnits[8] = {
		100, 1000, 10000, 100000, 0, 0, 0, 0
	};	// kbit/s
	return kValues[(value >> 3) & 0xf] * kUnits[value & 7] / 10;
}


static status_t
read_pointer(const sdio_host_ops& host, uint32 address, uint32* _pointer)
{
	uint32 pointer = 0;
	for (uint32 i = 0; i < 3; i++) {
		uint8 byte;
		status_t status = sdio_rw_direct(host, false, 0, address + i, 0, &byte);
		if (status != B_OK)
			return status;
		pointer |= (uint32)byte << (8 * i);
	}
	*_pointer = pointer;
	return B_OK;
}


/*!	Reads the CIS of \a function (0: the common CIS) and takes what the bus
	needs from it: the manufacturer's tuple and the function extension.
*/
static status_t
read_cis(const sdio_host_ops& host, sdio_card* card, uint8 function)
{
	uint32 pointer;
	status_t status = read_pointer(host, SDIO_FBR_BASE(function)
		+ SDIO_FBR_CIS_POINTER, &pointer);
		// the CCCR's CIS pointer is at the same place as the FBRs'
	if (status != B_OK)
		return status;
	if (pointer < SDIO_CIS_START || pointer > SDIO_CIS_END) {
		SDIO_LOG("function %u: CIS pointer %#" B_PRIx32 " out of range\n",
			function, pointer);
		return B_BAD_DATA;
	}

	uint32 bytesRead = 0;
	while (bytesRead < kCisMaxBytes && pointer <= SDIO_CIS_END) {
		uint8 code;
		status = sdio_rw_direct(host, false, 0, pointer++, 0, &code);
		if (status != B_OK)
			return status;
		bytesRead++;
		if (code == SDIO_CISTPL_END)
			break;
		if (code == SDIO_CISTPL_NULL)
			continue;

		uint8 link;
		status = sdio_rw_direct(host, false, 0, pointer++, 0, &link);
		if (status != B_OK)
			return status;
		bytesRead++;
		if (link == 0xff)
			break;

		uint8 data[255];
		for (uint32 i = 0; i < link; i++) {
			status = sdio_rw_direct(host, false, 0, pointer + i, 0, &data[i]);
			if (status != B_OK)
				return status;
		}
		pointer += link;
		bytesRead += link;

		switch (code) {
			case SDIO_CISTPL_MANFID:
				// manufacturer code and manufacturer information, 16 bits
				// each, low byte first
				if (link < 4)
					break;
				card->function_vendor[function] = data[0] | (data[1] << 8);
				card->function_device[function] = data[2] | (data[3] << 8);
				break;

			case SDIO_CISTPL_FUNCE:
				if (link < 1)
					break;
				if (function == 0 && data[0] == 0x00 && link >= 4) {
					// function 0: TPLFE_FN0_BLK_SIZE, TPLFE_MAX_TRAN_SPEED
					card->info.max_block_size[0] = data[1] | (data[2] << 8);
					card->max_clock = sdio_tran_speed(data[3]);
				} else if (function != 0 && data[0] == 0x01 && link >= 14) {
					// functions 1-7: TPLFE_MAX_BLK_SIZE at 12, and since
					// SDIO 1.10 TPLFE_ENABLE_TIMEOUT_VAL (10 ms units) at
					// 28. A tuple of 1.00's length is read as 1.00 whatever
					// the CCCR says (Linux does the same).
					card->info.max_block_size[function]
						= data[12] | (data[13] << 8);
					if (link >= 30 && card->info.sdio_revision >= 1) {
						uint32 timeout = (data[28] | (data[29] << 8)) * 10;
						if (timeout != 0)
							card->enable_timeout[function] = timeout;
					}
				}
				break;

			default:
				break;
		}
	}
	return B_OK;
}


status_t
sdio_read_card(const sdio_host_ops& host, uint32 ocr, sdio_card* card)
{
	memset(card, 0, sizeof(*card));
	card->ocr = ocr;
	card->info.function_count = SDIO_OCR_FUNCTION_COUNT(ocr);

	uint8 value;
	status_t status = sdio_rw_direct(host, false, 0, SDIO_CCCR_REVISION, 0,
		&value);
	if (status != B_OK)
		return status;
	card->info.cccr_revision = value & 0xf;
	card->info.sdio_revision = value >> 4;
	if (card->info.cccr_revision > 3) {
		SDIO_LOG("unknown CCCR revision %u\n", card->info.cccr_revision);
		return B_NOT_SUPPORTED;
	}

	status = sdio_rw_direct(host, false, 0, SDIO_CCCR_CAPABILITY, 0,
		&card->info.capabilities);
	if (status != B_OK)
		return status;

	for (uint32 i = 0; i < SDIO_MAX_FUNCTIONS; i++)
		card->enable_timeout[i] = 1000;
	card->max_clock = 25000;

	status = read_cis(host, card, 0);
	if (status != B_OK) {
		SDIO_LOG("common CIS: %s\n", strerror(status));
		return status;
	}
	card->info.vendor = card->function_vendor[0];
	card->info.device = card->function_device[0];
	if (card->max_clock == 0)
		card->max_clock = 25000;

	for (uint8 function = 1; function <= card->info.function_count;
			function++) {
		status = sdio_rw_direct(host, false, 0,
			SDIO_FBR_BASE(function) + SDIO_FBR_INTERFACE, 0, &value);
		if (status != B_OK)
			return status;
		value &= 0xf;
		if (value == 0xf) {
			status = sdio_rw_direct(host, false, 0,
				SDIO_FBR_BASE(function) + SDIO_FBR_INTERFACE_EXT, 0, &value);
			if (status != B_OK)
				return status;
		}
		card->function_class[function] = value;

		// A function's CIS need not repeat the manufacturer's tuple.
		card->function_vendor[function] = card->info.vendor;
		card->function_device[function] = card->info.device;
		status = read_cis(host, card, function);
		if (status != B_OK) {
			SDIO_LOG("function %u CIS: %s\n", function, strerror(status));
			return status;
		}
	}
	return B_OK;
}


uint8
sdio_interrupts_to_serve(uint8 handlers, uint8 pending, bool pendingRead)
{
	handlers &= 0xfe;
	if (!pendingRead)
		return handlers;
	return handlers & pending & 0xfe;
}


uint8
sdio_interrupt_enable(uint8 current, uint8 function, bool enable)
{
	if (function == 0 || function >= SDIO_MAX_FUNCTIONS)
		return current;
	if (enable)
		current |= (1 << function) | 1;
	else {
		current &= ~(1 << function);
		if ((current & 0xfe) == 0)
			current = 0;
	}
	return current;
}
