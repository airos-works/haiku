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
#define RPI_DISPLAY_VERSION			2
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
	RPI_DISPLAY_GET_RETRACE_SEMAPHORE
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
};

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
