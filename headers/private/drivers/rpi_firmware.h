/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RPI_FIRMWARE_H
#define _RPI_FIRMWARE_H


#include <module.h>


/*	The Raspberry Pi's VideoCore firmware keeps hold of the clocks, some
	power domains, the board's GPIO expander and more. Drivers ask it through
	the "property" channel of the mailbox. */

#define RPI_FIRMWARE_MODULE_NAME	"generic/rpi_firmware/v1"

// tags
#define RPI_FIRMWARE_GET_BOARD_REVISION		0x00010002
#define RPI_FIRMWARE_GET_BOARD_SERIAL		0x00010004
#define RPI_FIRMWARE_GET_CLOCK_STATE		0x00030001
#define RPI_FIRMWARE_GET_CLOCK_RATE			0x00030002
#define RPI_FIRMWARE_GET_MAX_CLOCK_RATE		0x00030004
#define RPI_FIRMWARE_GET_TEMPERATURE		0x00030006
#define RPI_FIRMWARE_GET_DOMAIN_STATE		0x00030030
#define RPI_FIRMWARE_GET_GPIO_STATE			0x00030041
#define RPI_FIRMWARE_NOTIFY_XHCI_RESET		0x00030058
#define RPI_FIRMWARE_SET_CLOCK_STATE		0x00038001
#define RPI_FIRMWARE_SET_CLOCK_RATE			0x00038002
#define RPI_FIRMWARE_SET_DOMAIN_STATE		0x00038030
#define RPI_FIRMWARE_SET_GPIO_STATE			0x00038041

// clocks
#define RPI_FIRMWARE_CLOCK_EMMC				1
#define RPI_FIRMWARE_CLOCK_UART				2
#define RPI_FIRMWARE_CLOCK_ARM				3
#define RPI_FIRMWARE_CLOCK_CORE				4
#define RPI_FIRMWARE_CLOCK_V3D				5
#define RPI_FIRMWARE_CLOCK_H264				6
#define RPI_FIRMWARE_CLOCK_ISP				7
#define RPI_FIRMWARE_CLOCK_PIXEL			9
#define RPI_FIRMWARE_CLOCK_HEVC				11
#define RPI_FIRMWARE_CLOCK_EMMC2			12
#define RPI_FIRMWARE_CLOCK_M2MC				13
#define RPI_FIRMWARE_CLOCK_PIXEL_BVB		14

// power domains of the DOMAIN_STATE tags: {domain, state}; the device
// tree's index (dt-bindings/power/raspberrypi-power.h) plus one
#define RPI_FIRMWARE_DOMAIN_V3D				11


typedef struct rpi_firmware_module_info {
	module_info	info;

	/*!	Sends one tag. \a data holds the request values and receives the
		response, \a size bytes of it (a multiple of 4, at most 1024).
	*/
	status_t	(*property)(uint32 tag, void* data, size_t size);

	/*! The convenience forms of the clock tags; rates in Hz. */
	status_t	(*get_clock_rate)(uint32 clock, bool maximum, uint32* _rate);
	status_t	(*set_clock_rate)(uint32 clock, uint32 rate);
	status_t	(*set_clock_state)(uint32 clock, bool on);
} rpi_firmware_module_info;


#endif	/* _RPI_FIRMWARE_H */
