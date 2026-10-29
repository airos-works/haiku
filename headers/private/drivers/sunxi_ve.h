/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SUNXI_VE_H
#define _SUNXI_VE_H


#include <Drivers.h>
#include <OS.h>


/*	The video decoder (Cedar "VE") of the Allwinner A733, for the program that
	parses the stream: /dev/misc/sunxi_ve.

	The program does everything codec specific. It parses the parameter
	sets and slice headers, keeps the reference pictures, and describes one
	slice at a time as a list of register writes (SUNXI_VE_RUN). The last
	write starts the engine; the driver waits for its interrupt and returns
	the engine's status. Everything the engine reads and writes is
	physically contiguous memory below 4 GiB that this driver hands out: a
	buffer is an area the program clones, it goes away with the file
	descriptor, and its bus address is what the program writes into the
	registers. The CPU cache is not coherent with the engine: the program
	cleans what it wrote and invalidates what it is about to read
	(SUNXI_VE_SYNC).

	The driver powers the engine (PCK-600 domain, clocks, resets, IOMMU
	bypass) while the device is open, keeps the registers that control that
	to itself, and lets one slice run at a time. */

#define SUNXI_VE_DEVICE_PATH		"/dev/misc/sunxi_ve"

#define SUNXI_VE_MAX_BUFFERS		128
#define SUNXI_VE_MAX_OPS			8192
#define SUNXI_VE_REGISTER_WINDOW	0x2000

enum {
	SUNXI_VE_GET_INFO = B_DEVICE_OP_CODES_END + 0x5645,
	SUNXI_VE_ALLOCATE,
	SUNXI_VE_FREE,
	SUNXI_VE_SYNC,
	SUNXI_VE_RUN,
	SUNXI_VE_END_PICTURE,	// no argument: see SUNXI_VE_RUN_PICTURE
	SUNXI_VE_ENCODE			// struct sunxi_ve_encode
};

struct sunxi_ve_info {
	uint32	decoder_ip;		// the engine's VE+0xe0, VE+0xe4 and VE+0xf0
	uint32	encoder_ip;
	uint32	version;
	uint32	clock;			// Hz
	uint32	address_offset;	// bus address = physical address - this
};

struct sunxi_ve_allocate {
	uint64	size;			// in: bytes; out: rounded up to pages
	uint32	buffer;			// out: its number
	area_id	area;			// out: to clone
	uint32	address;		// out: the engine's (bus) address of it
};

#define SUNXI_VE_SYNC_FOR_DEVICE	1	// the CPU wrote it: clean the cache
#define SUNXI_VE_SYNC_FOR_CPU		2	// the CPU is to read it: invalidate

struct sunxi_ve_sync {
	uint32	buffer;
	uint32	offset;
	uint32	size;
	uint32	direction;
};

enum {
	SUNXI_VE_OP_WRITE = 1,			// register = value
	SUNXI_VE_OP_POLL_CLEAR = 2,		// wait until (register & value) == 0
	SUNXI_VE_OP_WRITE_BACK = 3		// register = register: clears what is
									// set of its write-one-to-clear bits
};

struct sunxi_ve_op {
	uint16	type;
	uint16	reg;			// byte offset in the decoder's register window
	uint32	value;
};

/*	A slice: the writes, then trigger_value to trigger_register, which starts
	the engine. On its interrupt the driver reads status_register, clears it
	(writing 7, the engines' write-one-to-clear done/error/request bits) and
	returns it.

	The engine keeps state from one slice to the next. The driver resets it
	when another file descriptor's slice comes, and with
	SUNXI_VE_RUN_PICTURE a program keeps the engine to itself until it says
	SUNXI_VE_END_PICTURE (or for a tenth of a second at most), so that the
	slices of one picture are not interleaved with another program's. */
#define SUNXI_VE_RUN_PICTURE	1
/*	A VP9 frame: the HEVC engine's trigger and status, with the registers of
	its VP9 decoder (a driver from before VP9 refuses it). */
#define SUNXI_VE_RUN_VP9		2

struct sunxi_ve_run {
	const sunxi_ve_op* ops;
	uint32	count;
	uint16	trigger_register;
	uint16	status_register;
	uint32	trigger_value;
	uint32	flags;
	uint32	status;			// out
	uint32	engine_time;	// out: microseconds from trigger to interrupt
};


/*	The video encoder: an engine of its own (0x01c10000, PCK-600 domain 3,
	its own clocks, reset and interrupt), reached through the same device
	and the same buffers. It encodes H.264 (High, CABAC) and H.265 (Main)
	pictures from NV12, one slice a picture; the program writes the
	parameter sets and the slice headers, the engine the slice data.

	SUNXI_VE_ENCODE is one picture: the driver turns the encoder's input
	stage and encoder on, runs the writes (registers of the input stage at
	0xa00, of the encoder at 0xb00 and of its extension at 0xc00; only
	writes), puts header[] through the encoder's bit writer (the start code
	and emulation prevention bytes must be in it: the bit writer adds
	none), starts the encoder and waits for it. It returns the encoder's
	status (bit 0: done; bit 1: the bit stream buffer was full, bit 2: the
	picture took too long; the driver then resets the encoder) and the
	number of bits the picture's slice takes in the bit stream buffer from
	its start, header included.

	Every address the encoder reads or writes must be set by each picture's
	writes (the encoder keeps what an earlier picture set) and lie in the
	program's buffers, with room for the picture size the writes set. The
	engine takes bus addresses: the encoder's buffers in units of 256
	bytes (address >> 8), the input planes in bytes.

	The driver powers the encoder at the first picture and until the device
	is closed. Another program's picture resets the encoder first. */
#define SUNXI_VE_ENCODER_WINDOW		0x1000
#define SUNXI_VE_MAX_HEADER			256

enum {
	SUNXI_VE_ENCODE_H264 = 0,
	SUNXI_VE_ENCODE_HEVC = 1
};

#define SUNXI_VE_ENCODE_DONE		0x1
#define SUNXI_VE_ENCODE_FULL		0x2		// the bit stream buffer
#define SUNXI_VE_ENCODE_OVERTIME	0x4

/*	The buffers an encoder picture's addresses point to, in bytes, for a
	picture size (what Allwinner's own encoder allocates, with room to
	spare): the driver refuses smaller ones. The reference pictures are
	the encoder's own (compressed) format. */
struct sunxi_ve_encoder_sizes {
	uint32	luma;			// each reference picture's (0xba0, 0xbb0)
	uint32	chroma;			// (0xba4, 0xbb4)
	uint32	subsampled;		// (0xbb8, 0xbbc)
	uint32	mb_info;		// H.264: 0xb60; H.265: 0xb60 and 0xb64 each
	uint32	block_info;		// H.265: 0xba8
	uint32	colocated;		// H.265: 0xb3c
	uint32	statistics;		// H.264: 0xa9c
	uint32	row_table;		// 0xb9c
	uint32	lines;			// 0xbc0 and 0xbc4 each
};

static inline void
sunxi_ve_get_encoder_sizes(uint32 codec, uint32 width, uint32 height,
	struct sunxi_ve_encoder_sizes* sizes)
{
	uint32 width64 = (width + 63) & ~63u;
	uint32 height16 = (height + 15) & ~15u;
	uint32 height64 = (height + 63) & ~63u;
	uint32 ctbHeight = (height + 31) / 32;
	if (codec == SUNXI_VE_ENCODE_HEVC) {
		sizes->luma = width64 * (height16 + 64);
		sizes->chroma = width64 * height64 / 2;
		sizes->mb_info = (512 + width64) * (ctbHeight + 1);
		sizes->block_info = width64 / 16 * (height64 / 16) * 8;
		sizes->colocated = width64 / 32 * (height64 / 32) * 24;
		sizes->statistics = 0;
	} else {
		sizes->luma = width64 * (((height16 + 32) + 63) & ~63u);
		sizes->chroma = sizes->luma / 2;
		sizes->mb_info = width64 / 16 * (height16 / 16) * 8;
		sizes->block_info = 0;
		sizes->colocated = 0;
		sizes->statistics = width64 * height64 / 32;
	}
	sizes->subsampled = width64 * height64 / 8;
	sizes->row_table = 512 + (height16 / 16 + 2) * 12;
	sizes->lines = width64 * 8;
}

struct sunxi_ve_encode {
	const sunxi_ve_op* ops;		// SUNXI_VE_OP_WRITE only
	uint32	count;
	uint32	codec;				// SUNXI_VE_ENCODE_*
	const uint8* header;		// the slice's NAL header and slice header
	uint32	header_size;		// bytes, at most SUNXI_VE_MAX_HEADER
	uint32	status;				// out
	uint32	bits;				// out: the slice's size
	uint32	engine_time;		// out: microseconds
	uint32	reserved[4];
};


#endif	/* _SUNXI_VE_H */
