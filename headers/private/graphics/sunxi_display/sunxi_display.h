/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SUNXI_DISPLAY_H
#define _SUNXI_DISPLAY_H


#include <Drivers.h>
#include <OS.h>


/*	The Allwinner A733's display outputs (the Cubie A7S: DisplayPort over
	USB-C), between the sunxi_display driver and its accelerant. The
	protocol is rpi_display's: the driver owns one frame buffer and shows a
	region of it on each output. The display engine does not scale on the
	A733 so far, so an output's video mode is a real one: the accelerant
	lists the modes the display's EDID offers and the driver sets the one a
	layout asks for. */

#define SUNXI_DISPLAY_ACCELERANT		"sunxi_display.accelerant"
#define SUNXI_DISPLAY_DEVICE			"graphics/sunxi_display/0"
#define SUNXI_DISPLAY_VERSION			3
#define SUNXI_DISPLAY_MAX_OUTPUTS		2

#define SUNXI_DISPLAY_OUTPUT_DP0		1

enum {
	// area_id of the shared info (struct sunxi_display_shared_info)
	SUNXI_DISPLAY_GET_SHARED_AREA = B_DEVICE_OP_CODES_END + 1,
	// struct sunxi_display_layout; the caller becomes the frame buffer's owner
	SUNXI_DISPLAY_SET_LAYOUT,
	// maps the frame buffer into the caller's team; returns an area_info
	SUNXI_DISPLAY_CLONE_FRAME_BUFFER,
	// consistent snapshot of sunxi_display_shared_info, including hotplug state
	SUNXI_DISPLAY_GET_STATE,
	// struct sunxi_display_change_port; negative port unregisters notifications
	SUNXI_DISPLAY_SET_CHANGE_PORT,
	// struct sunxi_display_register: reads or writes a display register;
	// only when the driver settings say "debug_registers true" (the lab)
	SUNXI_DISPLAY_DEBUG_REGISTER,
	// sem_id: the calling team's semaphore, released at every vertical
	// blank of the DisplayPort output (B_ACCELERANT_RETRACE_SEMAPHORE)
	SUNXI_DISPLAY_GET_RETRACE_SEMAPHORE
};

#define SUNXI_DISPLAY_OUTPUT_CONNECTED	0x1
#define SUNXI_DISPLAY_OUTPUT_ENABLED	0x2
#define SUNXI_DISPLAY_OUTPUT_MIRROR		0x4
#define SUNXI_DISPLAY_OUTPUT_VIRTUAL	0x8
	// nothing is attached: the output stands in for a display so that the
	// desktop runs headless

struct sunxi_display_timing {
	uint32	pixel_clock;		// kHz
	uint16	h_display;
	uint16	h_sync_start;
	uint16	h_sync_end;
	uint16	h_total;
	uint16	v_display;
	uint16	v_sync_start;
	uint16	v_sync_end;
	uint16	v_total;
	uint32	flags;				// B_POSITIVE_HSYNC, B_POSITIVE_VSYNC
};

struct sunxi_display_output {
	uint32	id;					// SUNXI_DISPLAY_OUTPUT_DP0, ...
	uint32	flags;
	uint16	native_width;		// the display's preferred mode
	uint16	native_height;
	sunxi_display_timing native_timing;
	sunxi_display_timing timing;
		// the mode the output shows; in a layout, the one it should show
		// (h_display 0: the preferred one)
	uint32	max_pixel_clock;	// kHz, what the link carries
	// the region of the frame buffer the output shows
	int32	x;
	int32	y;
	uint16	width;
	uint16	height;
	// the resolution and scales (percent) the region stands for
	uint16	mode_width;
	uint16	mode_height;
	uint16	scale;
	uint16	render_scale;
	uint32	edid_length;
	uint8	edid[256];
};

struct sunxi_display_shared_info {
	uint32	version;
	uint32	output_count;
	sunxi_display_output outputs[SUNXI_DISPLAY_MAX_OUTPUTS];
	// the frame buffer, B_RGB32; generation counts its replacements
	uint32	width;
	uint32	height;
	uint32	bytes_per_row;
	uint32	generation;
	uint64	physical_address;
};

struct sunxi_display_layout {
	uint32	version;
	uint32	width;				// frame buffer size
	uint32	height;
	sunxi_display_output outputs[SUNXI_DISPLAY_MAX_OUTPUTS];
		// by index as in the shared info; id, flags and the region and
		// mode fields count
};

struct sunxi_display_register {
	uint64	address;			// physical, in a block the driver maps
	uint32	value;
	uint32	write;				// 0: read into value, 1: write value
};

struct sunxi_display_change_port {
	port_id port;
	int32 code;
};


#endif	/* _SUNXI_DISPLAY_H */
