/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RPI_HEVC_H
#define _RPI_HEVC_H


#include <Drivers.h>
#include <OS.h>


/*	The HEVC decoder block of the BCM2711 (Raspberry Pi 4) and the BCM2712
	(Raspberry Pi 5), for the program that parses the stream:
	/dev/misc/rpi_hevc. The two blocks are programmed alike; nothing here
	differs between them.

	The block decodes a picture in two phases. The first reads the slice
	data (CABAC) as a list of register writes tells it to, and leaves
	prediction units and coefficients in two buffers; the second makes the
	picture from those and the reference pictures. Each phase ends with an
	interrupt. Everything the block reads and writes is physically
	contiguous memory, which this driver hands out: a buffer is an area the
	program clones, and it goes away with the file descriptor.

	The driver knows nothing of HEVC. It checks that the buffers named are
	the caller's own, keeps the CPU cache out of the way, and waits for the
	interrupts. Phase one of a picture may run while phase two of the one
	before does. */

#define RPI_HEVC_DEVICE_PATH	"/dev/misc/rpi_hevc"

#define RPI_HEVC_MAX_BUFFERS	96
#define RPI_HEVC_NO_BUFFER		0xffffffffu

enum {
	RPI_HEVC_ALLOCATE = B_DEVICE_OP_CODES_END + 0x4845,
	RPI_HEVC_FREE,
	RPI_HEVC_PHASE1,
	RPI_HEVC_PHASE2
};

struct rpi_hevc_allocate {
	uint64	size;			// in: bytes; out: rounded up to pages
	uint32	buffer;			// out: its number
	area_id	area;			// out: to clone
	uint64	physical;		// out: where the decoder finds it (the
							// physical address on both SoCs)
};

// one write to a register of phase one
struct rpi_hevc_command {
	uint32	address;
	uint32	data;
};

// results of phase one
#define RPI_HEVC_PHASE1_OK				0
#define RPI_HEVC_PHASE1_ERROR			1	// the stream did not decode
#define RPI_HEVC_PHASE1_COEFF_FULL		8	// a larger buffer is needed
#define RPI_HEVC_PHASE1_PU_FULL			16

struct rpi_hevc_phase1 {
	uint32	commands;		// buffer of rpi_hevc_command
	uint32	command_count;
	uint32	bitstream;		// buffer the commands point into
	uint32	bitstream_size;	// bytes of it in use
	uint32	pu;				// buffers the phase writes
	uint32	pu_stride;		// bytes per row of coding tree blocks
	uint32	coeff;
	uint32	coeff_stride;
	uint32	result;			// out: RPI_HEVC_PHASE1_...
	uint32	status;			// out: the block's status register
};

struct rpi_hevc_phase2 {
	uint32	pu;
	uint32	pu_stride;
	uint32	coeff;
	uint32	coeff_stride;
	uint32	frame;			// buffer of the picture to make
	uint32	chroma_offset;	// of the chroma plane in a picture's buffer
	uint32	frame_stride;	// bytes from one 128 byte column to the next
	uint32	references[16];	// buffers, RPI_HEVC_NO_BUFFER where unused
	uint32	config;
	uint32	frame_size;
	uint32	current_poc;
	uint32	rows;			// of coding tree blocks
	uint32	mv_stride;
	uint32	mv;				// buffer for this picture's motion vectors,
	uint32	collocated;		// and of the collocated picture's; or none
};


#endif	/* _RPI_HEVC_H */
