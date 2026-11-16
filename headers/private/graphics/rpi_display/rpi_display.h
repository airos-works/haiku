/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RPI_DISPLAY_H
#define _RPI_DISPLAY_H


#include <Drivers.h>
#include <OS.h>


/*	The Raspberry Pi 4's two HDMI outputs, between the rpi_display driver and
	its accelerant. The firmware keeps the outputs' video modes; the driver
	owns one frame buffer and has the firmware's compositor show a region of
	it on each output, scaled to the output's mode. */

#define RPI_DISPLAY_ACCELERANT		"rpi_display.accelerant"
#define RPI_DISPLAY_DEVICE			"graphics/rpi_display/0"
#define RPI_DISPLAY_VERSION			3
#define RPI_DISPLAY_MAX_OUTPUTS		2

enum {
	// area_id of the shared info (struct rpi_display_shared_info)
	RPI_DISPLAY_GET_SHARED_AREA = B_DEVICE_OP_CODES_END + 1,
	// struct rpi_display_layout; the caller becomes the frame buffer's owner
	RPI_DISPLAY_SET_LAYOUT,
	// maps the frame buffer into the caller's team; returns an area_info
	RPI_DISPLAY_CLONE_FRAME_BUFFER,
	// consistent snapshot of rpi_display_shared_info, including hotplug state
	RPI_DISPLAY_GET_STATE,
	// struct rpi_display_change_port; negative port unregisters notifications
	RPI_DISPLAY_SET_CHANGE_PORT,
	// sem_id: the calling team's semaphore, released at every vertical
	// blank of the first output shown (B_ACCELERANT_RETRACE_SEMAPHORE)
	RPI_DISPLAY_GET_RETRACE_SEMAPHORE,
	// maps the hardware cursor's buffer into the caller's team; area_info
	RPI_DISPLAY_CLONE_CURSOR_BUFFER,
	// struct rpi_display_cursor; the caller filled the cursor buffer first
	RPI_DISPLAY_DEFINE_CURSOR,
	// struct rpi_display_cursor_position; the hotspot, in frame buffer pixels
	RPI_DISPLAY_MOVE_CURSOR,
	// bool; shows or hides the cursor
	RPI_DISPLAY_SHOW_CURSOR,
	// struct rpi_display_cursor_config; experiments with the cursor's plane
	RPI_DISPLAY_CURSOR_CONFIG,
	// struct rpi_display_source: memory the DMA engine copies from, mapped
	// into the caller, or a shadow only the engine reaches
	// (RPI_DISPLAY_DEVICE_COPIES)
	RPI_DISPLAY_CREATE_SOURCE,
	// uint32: a source's id
	RPI_DISPLAY_DELETE_SOURCE,
	// struct rpi_display_copy; returns once the rectangles are copied
	RPI_DISPLAY_COPY
};

// sources and copies
#define RPI_DISPLAY_SOURCE_SHADOW		0x1
	// only the engine reaches it: not mapped into the caller
#define RPI_DISPLAY_FRAME_BUFFER_ID		0
	// as a copy's destination: the frame buffer
#define RPI_DISPLAY_MAX_COPY_RECTS		128

struct rpi_display_source {
	uint32	flags;			// RPI_DISPLAY_SOURCE_*
	uint16	width;
	uint16	height;
	// filled in: B_RGB32 pixels, bytes_per_row apart; area and address are
	// the caller's mapping (-1 and NULL for a shadow)
	uint32	id;
	uint32	bytes_per_row;
	area_id	area;
	void*	address;
};

struct rpi_display_copy {
	uint32	from;			// a source's id
	uint32	to;				// a source's id or RPI_DISPLAY_FRAME_BUFFER_ID
	uint32	count;
	// left, top, right, bottom each (inclusive): the same place in both
	const uint16* rects;
};

// the hardware cursor's buffer is this many pixels a side
#define RPI_DISPLAY_CURSOR_MAX	64

struct rpi_display_cursor_config {
	uint8	plane_offset;	// added to the display's plane base
	int8	layer;
	uint8	image_type;	// a VC_IMAGE_* value
	uint8	canvas;			// 1: the plane covers the whole 64x64 buffer
};

struct rpi_display_cursor {
	uint16	width;
	uint16	height;
	int16	hot_x;
	int16	hot_y;
};

struct rpi_display_cursor_position {
	int32	x;
	int32	y;
};

#define RPI_DISPLAY_OUTPUT_CONNECTED	0x1
#define RPI_DISPLAY_OUTPUT_ENABLED		0x2
#define RPI_DISPLAY_OUTPUT_MIRROR		0x4

struct rpi_display_output {
	uint32	id;					// the firmware's display id (2 and 7: HDMI0/1)
	uint32	flags;
	uint16	native_width;		// the output's video mode
	uint16	native_height;
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

struct rpi_display_shared_info {
	uint32	version;
	uint32	output_count;
	rpi_display_output outputs[RPI_DISPLAY_MAX_OUTPUTS];
	// the frame buffer, B_RGB32; generation counts its replacements
	uint32	width;
	uint32	height;
	uint32	bytes_per_row;
	uint32	generation;
	uint64	physical_address;
	uint32	flags;				// RPI_DISPLAY_FIXED_MODES
};

// The outputs keep the modes the firmware set at boot and show their
// regions unscaled (the Raspberry Pi 5, whose firmware has no planes for
// the OS): no other sizes, no scaling.
#define RPI_DISPLAY_FIXED_MODES		0x1
// The SoC's DMA engine copies rectangles between sources and into the frame
// buffer (RPI_DISPLAY_CREATE_SOURCE, RPI_DISPLAY_COPY).
#define RPI_DISPLAY_DEVICE_COPIES	0x2

struct rpi_display_layout {
	uint32	version;
	uint32	width;				// frame buffer size
	uint32	height;
	rpi_display_output outputs[RPI_DISPLAY_MAX_OUTPUTS];
		// by index as in the shared info; id, flags and the region and
		// mode fields count
};

struct rpi_display_change_port {
	port_id port;
	int32 code;
};


#endif	/* _RPI_DISPLAY_H */
