/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _QCOM_DISPLAY_H
#define _QCOM_DISPLAY_H


#include <Drivers.h>
#include <OS.h>


/*	The Qualcomm SC8280XP's display outputs (the Radxa Dragon Q8B: HDMI
	through a DisplayPort to HDMI bridge, DisplayPort over its two USB-C
	ports), between the qcom_display driver and its accelerant. The protocol
	is rpi_display's and sunxi_display's: the driver owns one frame buffer and
	shows a region of it on each output. So far the driver takes over the
	path the firmware lit, so an output's video mode is the one it found. */

#define QCOM_DISPLAY_ACCELERANT		"qcom_display.accelerant"
#define QCOM_DISPLAY_DEVICE			"graphics/qcom_display/0"
#define QCOM_DISPLAY_VERSION			1
#define QCOM_DISPLAY_MAX_OUTPUTS		3

// DisplayPort controller 2 of MDSS 0, behind the board's HDMI bridge
#define QCOM_DISPLAY_OUTPUT_HDMI0		1
// DisplayPort controllers 0 and 1, the USB-C ports
#define QCOM_DISPLAY_OUTPUT_DP0		2
#define QCOM_DISPLAY_OUTPUT_DP1		3

enum {
	// area_id of the shared info (struct qcom_display_shared_info)
	QCOM_DISPLAY_GET_SHARED_AREA = B_DEVICE_OP_CODES_END + 1,
	// struct qcom_display_layout; the caller becomes the frame buffer's owner
	QCOM_DISPLAY_SET_LAYOUT,
	// maps the frame buffer into the caller's team; returns an area_info
	QCOM_DISPLAY_CLONE_FRAME_BUFFER,
	// consistent snapshot of qcom_display_shared_info, including hotplug state
	QCOM_DISPLAY_GET_STATE,
	// struct qcom_display_change_port; negative port unregisters notifications
	QCOM_DISPLAY_SET_CHANGE_PORT,
	// struct qcom_display_register: reads or writes a display register;
	// only when the driver settings say "debug_registers true" (the lab)
	QCOM_DISPLAY_DEBUG_REGISTER,
	// sem_id: the calling team's semaphore, released at every vertical
	// blank of the first output (B_ACCELERANT_RETRACE_SEMAPHORE)
	QCOM_DISPLAY_GET_RETRACE_SEMAPHORE
};

#define QCOM_DISPLAY_OUTPUT_CONNECTED	0x1
#define QCOM_DISPLAY_OUTPUT_ENABLED	0x2
#define QCOM_DISPLAY_OUTPUT_MIRROR		0x4
#define QCOM_DISPLAY_OUTPUT_VIRTUAL	0x8
	// nothing is attached: the output stands in for a display so that the
	// desktop runs headless

struct qcom_display_timing {
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

struct qcom_display_output {
	uint32	id;					// QCOM_DISPLAY_OUTPUT_DP0, ...
	uint32	flags;
	uint16	native_width;		// the display's preferred mode
	uint16	native_height;
	qcom_display_timing native_timing;
	qcom_display_timing timing;
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

struct qcom_display_shared_info {
	uint32	version;
	uint32	output_count;
	qcom_display_output outputs[QCOM_DISPLAY_MAX_OUTPUTS];
	// the frame buffer, B_RGB32; generation counts its replacements
	uint32	width;
	uint32	height;
	uint32	bytes_per_row;
	uint32	generation;
	uint64	physical_address;
};

struct qcom_display_layout {
	uint32	version;
	uint32	width;				// frame buffer size
	uint32	height;
	qcom_display_output outputs[QCOM_DISPLAY_MAX_OUTPUTS];
		// by index as in the shared info; id, flags and the region and
		// mode fields count
};

struct qcom_display_register {
	uint64	address;			// physical, in a block the driver maps
	uint32	value;
	uint32	write;				// 0: read into value, 1: write value
};

struct qcom_display_change_port {
	port_id port;
	int32 code;
};


#endif	/* _QCOM_DISPLAY_H */
