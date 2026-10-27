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
	SUNXI_VE_END_PICTURE	// no argument: see SUNXI_VE_RUN_PICTURE
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


#endif	/* _SUNXI_VE_H */
