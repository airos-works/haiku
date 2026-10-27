/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _RPI_GPIO_H
#define _RPI_GPIO_H


#include <Drivers.h>
#include <OS.h>


/*	The general purpose I/O pins of the BCM2711 (Raspberry Pi 4), for
	programs: /dev/misc/rpi_gpio.

	Everyone may read the state of all 58 pins. A program that wants to
	change a pin claims it: as an input (with or without a pull resistor) or
	as an output. Only the pins of the 40-pin header (GPIO 0 to 27) can be
	claimed; the others belong to the board (Wi-Fi, Bluetooth, the SD card,
	Ethernet). A claimed pin belongs to the file descriptor that claimed it
	until it is released or the descriptor is closed; then it gets back the
	function, pull and level it had before. A claim with
	RPI_GPIO_CLAIM_DETACH configures the pin and keeps nothing: the setting
	stays when the program ends (a command line tool).

	Level changes of claimed pins are events, timestamped with system_time()
	in the interrupt handler (inputs) or when the level was written
	(outputs). RPI_GPIO_WAIT_EVENTS returns those of the caller's own pins.
	A pulse shorter than the interrupt's latency shows up as two events with
	the same time. An input that changes faster than the driver can report
	(tens of thousands of edges a second) is sampled instead for a while; the
	events then carry RPI_GPIO_EVENT_SAMPLED.

	Other boards' pin headers have the same interface under another name;
	rpi_gpio_info.board says which board and so what the pin numbers and
	function values mean:

	RPI_GPIO_BOARD_RPI4: /dev/misc/rpi_gpio, the BCM2711's GPIO numbers and
	function select values, as described above. Drivers from before the
	board field existed left it 0, which is this board.

	RPI_GPIO_BOARD_CUBIE_A7S: /dev/misc/sunxi_gpio (Radxa Cubie A7S,
	Allwinner A733). Its 30-pin header J11 and 15-pin header J6 carry pins
	of the A733's PIO and R_PIO ports (PB0, PD14, PL5...). Pin n is pin n of
	J11 (1 to 30) and pin n - 30 of J6 (31 to 45); the numbers of power and
	ground pins are never claimable and read as low inputs. function[] holds
	the A733's pin multiplexer values: 0 input, 1 output, 2 to 13 the
	peripheral functions, 14 the interrupt input, 15 switched off (the state
	after reset). Pull values are the same as the Pi's. */

#define RPI_GPIO_DEVICE_PATH	"/dev/misc/rpi_gpio"
#define SUNXI_GPIO_DEVICE_PATH	"/dev/misc/sunxi_gpio"
#define RPI_GPIO_API_VERSION	1

// rpi_gpio_info.board
#define RPI_GPIO_BOARD_RPI4			0
#define RPI_GPIO_BOARD_CUBIE_A7S	1

#define RPI_GPIO_MAX_PINS		64

// pin functions: the BCM2711's function select values
#define RPI_GPIO_INPUT			0
#define RPI_GPIO_OUTPUT			1
#define RPI_GPIO_ALT5			2
#define RPI_GPIO_ALT4			3
#define RPI_GPIO_ALT0			4
#define RPI_GPIO_ALT1			5
#define RPI_GPIO_ALT2			6
#define RPI_GPIO_ALT3			7

// pull resistors: the BCM2711's GPIO_PUP_PDN_CNTRL values
#define RPI_GPIO_PULL_NONE		0
#define RPI_GPIO_PULL_UP		1
#define RPI_GPIO_PULL_DOWN		2

enum {
	RPI_GPIO_GET_INFO = B_DEVICE_OP_CODES_END + 0x4750,
	RPI_GPIO_GET_STATE,
	RPI_GPIO_CLAIM,
	RPI_GPIO_RELEASE,
	RPI_GPIO_WRITE,
	RPI_GPIO_WAIT_EVENTS
};

// rpi_gpio_info.flags
#define RPI_GPIO_INFO_EDGE_INTERRUPTS	0x01	// else inputs are sampled

struct rpi_gpio_info {
	uint32		api_version;		// RPI_GPIO_API_VERSION
	uint32		pin_count;			// 58 (Pi 4), 46 (Cubie A7S)
	uint64		claimable;			// bit n: pin n can be claimed
	uint32		board_revision;		// the Pi firmware's revision code, or 0
	uint32		flags;
	uint64		board_serial;		// or 0
	uint32		board;				// RPI_GPIO_BOARD_*
	uint32		_reserved[7];
};

struct rpi_gpio_state {
	bigtime_t	time;				// when the levels were read
	uint64		levels;				// bit n: pin n is high
	uint64		claimed;			// by this file descriptor
	uint64		claimed_elsewhere;	// by other file descriptors
	uint8		function[RPI_GPIO_MAX_PINS];	// RPI_GPIO_INPUT...
	uint8		pull[RPI_GPIO_MAX_PINS];		// RPI_GPIO_PULL_*
};

// rpi_gpio_claim.flags
#define RPI_GPIO_CLAIM_DETACH	0x01

struct rpi_gpio_claim {
	uint32		pin;
	uint8		function;			// RPI_GPIO_INPUT or RPI_GPIO_OUTPUT
	uint8		pull;				// RPI_GPIO_PULL_*
	int8		level;				// outputs: 0, 1, or -1 for as it is
	uint8		flags;
};

// RPI_GPIO_RELEASE takes the pin number as a uint32.

// sets the claimed outputs in mask to the bits in levels
struct rpi_gpio_write {
	uint64		mask;
	uint64		levels;
};

// rpi_gpio_event.flags
#define RPI_GPIO_EVENT_CLAIMED	0x01	// the level when the pin was claimed
#define RPI_GPIO_EVENT_WRITTEN	0x02	// a program set an output
#define RPI_GPIO_EVENT_SAMPLED	0x04	// found while the edges were too fast

struct rpi_gpio_event {
	bigtime_t	time;				// system_time()
	uint8		pin;
	uint8		level;
	uint8		flags;
	uint8		_reserved[5];
};

struct rpi_gpio_wait {
	bigtime_t	timeout;			// in: relative; B_INFINITE_TIMEOUT blocks
	struct rpi_gpio_event* events;	// in: room for capacity events
	uint32		capacity;			// in
	uint32		count;				// out: events returned
	uint32		lost;				// out: events dropped since the last call
	uint32		_reserved;
};


#endif	/* _RPI_GPIO_H */
