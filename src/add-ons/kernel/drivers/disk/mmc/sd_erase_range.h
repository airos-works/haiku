/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SD_ERASE_RANGE_H
#define SD_ERASE_RANGE_H


#include <stdint.h>


/*!	The arguments of the SD erase sequence for the bytes from \a offset to
	\a offset + \a length of a card, both multiples of \a blockSize.

	ERASE_WR_BLK_START (CMD32) takes the address of the first block to erase
	and ERASE_WR_BLK_END (CMD33) the address of the last one: of the last
	block of the range, not of the block that follows it. Cards addressed in
	blocks (SDHC, SDXC) take block numbers, the others (SDSC) the byte
	address at which the block starts.

	Returns false when the range is empty or cannot be told in 32 bits.
	Nothing but arithmetic, so that it can be tested without a card
	(src/tests/add-ons/kernel/drivers/mmc).
*/
static inline bool
sd_erase_range(uint64_t offset, uint64_t length, uint32_t blockSize,
	bool blockAddressed, uint32_t* _first, uint32_t* _last)
{
	if (length < blockSize || offset % blockSize != 0
		|| length % blockSize != 0 || offset + length < offset) {
		return false;
	}

	uint64_t first = offset;
	uint64_t last = offset + length - blockSize;
	if (blockAddressed) {
		first /= blockSize;
		last /= blockSize;
	}
	if (last > UINT32_MAX)
		return false;

	*_first = (uint32_t)first;
	*_last = (uint32_t)last;
	return true;
}


#endif	// SD_ERASE_RANGE_H
