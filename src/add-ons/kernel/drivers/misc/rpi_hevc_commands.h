/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RPI_HEVC_COMMANDS_H
#define RPI_HEVC_COMMANDS_H


/*	The check of phase one's command list (#202). The block runs the list
	as register writes of its own, and some of them steer its memory
	accesses: the bit reader is given a physical address (BIT_BASE), a byte
	count and a start, and the positions of the coding tree blocks decide
	how many rows of prediction unit and coefficient data the block writes
	(one stride each, at the bases the driver sets). Other registers of the
	block, written from the list, would move those bases or start another
	list. So the driver runs a copy of the list in memory of its own, which
	the program cannot change while the block reads it, and lets through
	only:
	- the registers the Linux drivers (rpivid, hevc_dec) and the rpi_hevc
	  media add-on write, with the values they write where a value has a
	  meaning not known here (TRANSFER, MODE, STATUS, BIT_CONTROL);
	- a bit reader that is given its address and count in this list, and
	  whose bytes, from the address and offset it is started with, lie in
	  the bytes of the bitstream buffer in use;
	- a start of decoding (CONTROL) after the tile's start and end are set
	  in this list: registers left by another program say nothing.
	It reports the rows the list names (the largest row of any coding tree
	block position, plus one); the driver compares that with the buffers.

	What the hardware does was not seen, only read from the Linux drivers:
	the check assumes that the bit reader takes its address and count when
	it is started (both drivers write them while it is still going from the
	slice before, then stop and start it), and that the block decodes no
	coding tree block below the rows the list names (it stops at a tile's
	end; with wavefronts each row is its own tile).

	Header only, so that a host test can run it
	(src/tests/add-ons/kernel/drivers/rpi_hevc). Whoever includes it has
	<rpi_hevc.h>. */


// the registers of phase one, as the command list names them
enum {
	HEVC_P1_SPS0				= 0,
	HEVC_P1_SPS1				= 4,
	HEVC_P1_PPS					= 8,
	HEVC_P1_SLICE				= 12,
	HEVC_P1_TILE_START			= 16,
	HEVC_P1_TILE_END			= 20,
	HEVC_P1_SLICE_START			= 24,
	HEVC_P1_MODE				= 28,
	HEVC_P1_QP					= 48,
	HEVC_P1_CONTROL				= 52,
	HEVC_P1_STATUS				= 56,
	HEVC_P1_BIT_BASE			= 64,
	HEVC_P1_BIT_COUNT			= 68,
	HEVC_P1_BIT_CONTROL			= 72,
	HEVC_P1_SLICE_COMMANDS		= 96,
	HEVC_P1_BEGIN_TILE_END		= 100,
	HEVC_P1_TRANSFER			= 104,
	HEVC_P1_PROBABILITIES		= 0x1000,
	HEVC_P1_SCALING_FACTORS		= 0x2000,
	HEVC_P1_SLICE_MESSAGES		= 0x4000
};

// sizes of the tables, in bytes (messages: in words)
#define HEVC_P1_PROBABILITY_BYTES		156
#define HEVC_P1_SCALING_FACTOR_BYTES	4064
#define HEVC_P1_SLICE_MESSAGE_COUNT		(2 * 16 * 8 + 3)

// the two values TRANSFER is written with: keep the CABAC state aside, and
// take it back
#define HEVC_P1_TRANSFER_SAVE			((20 << 12) | (20 << 6))
#define HEVC_P1_TRANSFER_RESTORE		((20 << 12) | 20)

#define HEVC_P1_BIT_STOP				(1 << 7)


struct hevc_phase1_check {
	uint32		rows;		// of coding tree blocks the list names
	uint32		entry;		// the first entry refused
	const char*	reason;		// and why
};


static inline status_t
hevc_refuse_command(hevc_phase1_check& check, uint32 entry,
	const char* reason)
{
	check.entry = entry;
	check.reason = reason;
	return B_BAD_VALUE;
}


/*!	Checks \a count entries of a command list for phase one, which the
	block will read from memory nobody else can change. \a bitstream and
	\a bitstreamSize: where the bitstream buffer starts (its physical
	address) and how many bytes of it are in use.
*/
static status_t
check_phase1_commands(const rpi_hevc_command* commands, uint32 count,
	uint64 bitstream, uint64 bitstreamSize, hevc_phase1_check& check)
{
	check.rows = 0;
	check.entry = 0;
	check.reason = NULL;
	if (count == 0 || count > RPI_HEVC_MAX_COMMANDS)
		return hevc_refuse_command(check, 0, "number of commands");

	uint64 bitBase = 0;
	uint64 bitCount = 0;
	bool haveBitBase = false;
	bool haveBitCount = false;
	bool haveTileStart = false;
	bool haveTileEnd = false;
	uint32 lastRow = 0;
	bool haveRow = false;

	for (uint32 i = 0; i < count; i++) {
		uint32 address = commands[i].address;
		uint32 data = commands[i].data;
		// the row of a coding tree block position, if this names one
		int64 row = -1;

		switch (address) {
			case HEVC_P1_SPS0:
			case HEVC_P1_SPS1:
			case HEVC_P1_PPS:
			case HEVC_P1_SLICE:
			case HEVC_P1_QP:
			case HEVC_P1_SLICE_COMMANDS:
				// what is decoded, not where
				break;

			case HEVC_P1_TILE_START:
				haveTileStart = true;
				row = data >> 16;
				break;
			case HEVC_P1_TILE_END:
				haveTileEnd = true;
				row = data >> 16;
				break;
			case HEVC_P1_BEGIN_TILE_END:
			case HEVC_P1_SLICE_START:
				row = data >> 16;
				break;

			case HEVC_P1_CONTROL:
				// starts decoding at a coding tree block
				if (!haveTileStart || !haveTileEnd) {
					return hevc_refuse_command(check, i,
						"decoding started before the tile was set");
				}
				row = data >> 16;
				break;

			case HEVC_P1_STATUS:
			{
				// the end of a slice (1), of a tile or row (2), a pause
				// after a row's second block (5)
				uint32 code = data & 31;
				if (code != 1 && code != 2 && code != 5)
					return hevc_refuse_command(check, i, "status code");
				row = data >> 18;
				break;
			}

			case HEVC_P1_MODE:
			{
				// pause at a tile's end, at a row's end, or not; the last
				// column and row; bit 16 as the wavefront pause sets it
				uint32 pause = data & 0xffff;
				if ((data >> 19) != 0
					|| (pause != 0 && pause != 1 && pause != 0xffff)) {
					return hevc_refuse_command(check, i, "mode");
				}
				break;
			}

			case HEVC_P1_TRANSFER:
				if (data != HEVC_P1_TRANSFER_SAVE
					&& data != HEVC_P1_TRANSFER_RESTORE) {
					return hevc_refuse_command(check, i, "transfer");
				}
				break;

			case HEVC_P1_BIT_BASE:
				// in units of 64 bytes
				bitBase = (uint64)data << 6;
				if (bitBase < bitstream
					|| bitBase >= bitstream + bitstreamSize) {
					return hevc_refuse_command(check, i,
						"bitstream address outside the bitstream");
				}
				haveBitBase = true;
				break;

			case HEVC_P1_BIT_COUNT:
				if (data > bitstreamSize) {
					return hevc_refuse_command(check, i,
						"bitstream longer than the bitstream");
				}
				bitCount = data;
				haveBitCount = true;
				break;

			case HEVC_P1_BIT_CONTROL:
			{
				// the offset into the first 64 bytes, without emulation
				// prevention bytes (bit 6), stop (bit 7)
				if ((data & ~(uint32)0xff) != 0)
					return hevc_refuse_command(check, i, "bit control");
				if ((data & HEVC_P1_BIT_STOP) != 0)
					break;

				// a start: the bit reader reads from here on
				if (!haveBitBase || !haveBitCount) {
					return hevc_refuse_command(check, i,
						"bit reader started without address and count");
				}
				uint64 start = bitBase + (data & 63);
				if (start < bitstream
					|| start + bitCount > bitstream + bitstreamSize) {
					return hevc_refuse_command(check, i,
						"bit reader started outside the bitstream");
				}
				break;
			}

			default:
				if ((address & 3) != 0)
					return hevc_refuse_command(check, i, "register");
				if (address >= HEVC_P1_PROBABILITIES
					&& address < HEVC_P1_PROBABILITIES
						+ HEVC_P1_PROBABILITY_BYTES) {
					break;
				}
				if (address >= HEVC_P1_SCALING_FACTORS
					&& address < HEVC_P1_SCALING_FACTORS
						+ HEVC_P1_SCALING_FACTOR_BYTES) {
					break;
				}
				if (address >= HEVC_P1_SLICE_MESSAGES
					&& address < HEVC_P1_SLICE_MESSAGES
						+ 4 * HEVC_P1_SLICE_MESSAGE_COUNT) {
					if (data > 0xffff) {
						return hevc_refuse_command(check, i,
							"slice message");
					}
					break;
				}
				// the bases and strides the driver sets, another list,
				// phase two, anything else
				return hevc_refuse_command(check, i, "register");
		}

		if (row >= 0 && (!haveRow || (uint32)row > lastRow)) {
			lastRow = (uint32)row;
			haveRow = true;
		}
	}

	check.rows = haveRow ? lastRow + 1 : 0;
	return B_OK;
}


#endif	// RPI_HEVC_COMMANDS_H
