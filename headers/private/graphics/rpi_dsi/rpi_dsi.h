/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RPI_DSI_H
#define _RPI_DSI_H


#include <auxdisplay.h>


/*	The Raspberry Pi 4's DSI display connector (DSI1), driven by the
	rpi_dsi driver: /dev/auxdisplay/rpi_dsi/0 speaks the auxdisplay
	protocol; what follows is for the lab tool (tools/rpi4/dsi.cpp). */

#define RPI_DSI_DEVICE			"auxdisplay/rpi_dsi/0"

enum {
	// rpi_dsi_state
	RPI_DSI_GET_STATE = AUX_DISPLAY_GET_INFO + 0x80,
	// uint32: run one step of the enable sequence (RPI_DSI_STEP_*)
	RPI_DSI_RUN_STEP,
	// rpi_dsi_register: read a register of one of the blocks
	RPI_DSI_READ_REGISTER,
	// rpi_dsi_register: write one (lab use)
	RPI_DSI_WRITE_REGISTER,
	// uint32[4096]: the HVS display list memory
	RPI_DSI_READ_DISPLAY_LIST,
	// rpi_dsi_touch_probe: read the touch controller's id over the
	// connector's I2C (a sign the panel is powered and connected)
	RPI_DSI_TOUCH_PROBE
};

struct rpi_dsi_touch_probe {
	uint32	address;		// in: 7-bit I2C address
	uint32	status;			// out: B_OK, B_TIMED_OUT (no transfer), B_IO_ERROR (no ack)
	uint32	controller_status;	// the BSC status register at the end
	uint8	id[6];			// Goodix: product id (4 ASCII) + firmware version (2)
};

// enable steps, in order
enum {
	RPI_DSI_STEP_CLOCKS = 1,	// PLLD_DSI1, the escape clock
	RPI_DSI_STEP_PHY,			// the DSI block and its PHY, pixel clock
	RPI_DSI_STEP_HVS,			// the compositor channel and display list
	RPI_DSI_STEP_PIXEL_VALVE,	// the pixel valve's timings, running
	RPI_DSI_STEP_VIDEO,			// the DSI video output
	RPI_DSI_STEP_OFF = 100		// everything off again
};

// register blocks
enum {
	RPI_DSI_BLOCK_DSI = 0,
	RPI_DSI_BLOCK_PIXEL_VALVE,
	RPI_DSI_BLOCK_HVS,
	RPI_DSI_BLOCK_CLOCKS,
	RPI_DSI_BLOCK_GPIO,
	RPI_DSI_BLOCK_I2C
};

struct rpi_dsi_register {
	uint32	block;
	uint32	offset;
	uint32	value;
};

struct rpi_dsi_timing {
	uint32	width;
	uint32	height;
	uint32	hfront;
	uint32	hsync;
	uint32	hback;
	uint32	vfront;
	uint32	vsync;
	uint32	vback;
	uint32	clock;			// kHz
	uint32	lanes;
};

struct rpi_dsi_state {
	uint32	steps_done;		// the highest step completed
	rpi_dsi_timing panel;	// as configured
	rpi_dsi_timing mode;	// as driven (the integer divider stretches it)
	uint32	plld_rate;		// Hz
	uint32	plld_per_rate;
	uint32	hs_clock;		// the DSI bit clock's PLL output
	uint32	escape_clock;
	uint32	pll_divider;
	uint32	display_list;	// word offset in the HVS display list memory
	uint32	channel;		// HVS channel
	uint64	buffer_address;	// physical
	uint32	buffer_size;
	uint32	buffer_count;
	uint32	shown;			// buffer index
	// the registers as the driver found them at init
	uint32	initial_hvs[32];	// 0x00 .. 0x7c
	uint32	initial_pv[16];		// 0x00 .. 0x3c
	uint32	initial_dsi[36];	// 0x00 .. 0x8c
};


#endif	/* _RPI_DSI_H */
