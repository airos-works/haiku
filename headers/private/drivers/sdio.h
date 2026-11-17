/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SDIO_H
#define _SDIO_H


/*!	SDIO: the I/O cards of the SD bus (Wi-Fi and Bluetooth chips, mostly).

	The MMC bus manager (bus_managers/mmc) finds an SDIO card on any host
	whose mmc_bus_interface has the SDIO hooks, sets it up (address, four
	data lines and high speed where the card and the host have them) and
	publishes one device node per I/O function, with the attributes below.
	A function driver matches on them (vendor, device and function number,
	as in the card's CIS) and talks to the card through the node's
	sdio_function_interface: get_driver() on the node gives the interface and
	the cookie for it.

	Every call names the function it addresses, so the driver of one function
	reaches the card's others too (function 0: the CCCR, the FBRs and the
	CIS), as Wi-Fi chips with a function for the backplane and one for the
	data need. Calls may block; none may be made from interrupt context.

	Register layout, argument formats and status bits: SDIO Simplified
	Specification Version 3.00, sections 5 (commands), 6.9 (CCCR), 6.10 (FBR)
	and 16 (CIS). */


#include <device_manager.h>


#define SDIO_FUNCTION_MODULE_NAME	"bus_managers/mmc/sdio_function/v1"

#define SDIO_MAX_FUNCTIONS			8	// function 0 and seven I/O functions

// device node attributes of a function
#define SDIO_VENDOR_ATTRIBUTE		"sdio/vendor"		// uint16, CISTPL_MANFID
#define SDIO_DEVICE_ATTRIBUTE		"sdio/device"		// uint16, CISTPL_MANFID
#define SDIO_FUNCTION_ATTRIBUTE		"sdio/function"		// uint8, 1 to 7
#define SDIO_CLASS_ATTRIBUTE		"sdio/class"		// uint8, FBR interface code
#define SDIO_MAX_BLOCK_SIZE_ATTRIBUTE "sdio/max_block_size"	// uint16, CIS
#define SDIO_FUNCTION_COUNT_ATTRIBUTE "sdio/function_count"	// uint8, OCR


// CMD52/CMD53 (IO_RW_DIRECT/IO_RW_EXTENDED) arguments
#define SDIO_ARG_WRITE				(1u << 31)
#define SDIO_ARG_FUNCTION(f)		((uint32)((f) & 7) << 28)
#define SDIO_ARG_RAW				(1u << 27)	// CMD52: read after write
#define SDIO_ARG_BLOCK_MODE			(1u << 27)	// CMD53
#define SDIO_ARG_INCREMENT			(1u << 26)	// CMD53: address counts up
#define SDIO_ARG_ADDRESS(a)			((uint32)((a) & 0x1ffff) << 9)
#define SDIO_MAX_ADDRESS			0x1ffff
#define SDIO_MAX_BLOCK_COUNT		511		// CMD53 count field, block mode
#define SDIO_MAX_BYTE_COUNT			512		// byte mode (0 in the field)

// R5 (CMD52/CMD53 response) flags
#define SDIO_R5_COM_CRC_ERROR		(1u << 15)
#define SDIO_R5_ILLEGAL_COMMAND		(1u << 14)
#define SDIO_R5_ERROR				(1u << 11)
#define SDIO_R5_FUNCTION_NUMBER		(1u << 9)
#define SDIO_R5_OUT_OF_RANGE		(1u << 8)
#define SDIO_R5_ERROR_MASK			0xcb00

// R4 (CMD5 response)
#define SDIO_OCR_READY				(1u << 31)
#define SDIO_OCR_FUNCTION_COUNT(ocr) (((ocr) >> 28) & 7)
#define SDIO_OCR_MEMORY_PRESENT		(1u << 27)
#define SDIO_OCR_S18A				(1u << 24)
#define SDIO_OCR_VOLTAGE_MASK		0x00ffffff

// Card Common Control Registers (function 0)
#define SDIO_CCCR_REVISION			0x00	// CCCR [3:0], SDIO [7:4]
#define SDIO_CCCR_SD_REVISION		0x01
#define SDIO_CCCR_IO_ENABLE			0x02	// IOEx
#define SDIO_CCCR_IO_READY			0x03	// IORx
#define SDIO_CCCR_INT_ENABLE		0x04	// IENx; bit 0: master (IENM)
#define SDIO_CCCR_INT_PENDING		0x05	// INTx
#define SDIO_CCCR_ABORT				0x06
#define  SDIO_ABORT_RESET			0x08	// RES: I/O reset
#define SDIO_CCCR_BUS_INTERFACE		0x07
#define  SDIO_BUS_WIDTH_MASK		0x03
#define  SDIO_BUS_WIDTH_1BIT		0x00
#define  SDIO_BUS_WIDTH_4BIT		0x02
#define  SDIO_BUS_CD_DISABLE		0x80
#define SDIO_CCCR_CAPABILITY		0x08
#define  SDIO_CAP_SDC				0x01	// CMD52 during data transfer
#define  SDIO_CAP_SMB				0x02	// multi-block CMD53
#define  SDIO_CAP_SRW				0x04
#define  SDIO_CAP_SBS				0x08
#define  SDIO_CAP_S4MI				0x10
#define  SDIO_CAP_E4MI				0x20
#define  SDIO_CAP_LSC				0x40	// low speed card (400 kHz)
#define  SDIO_CAP_4BLS				0x80	// four bit low speed card
#define SDIO_CCCR_CIS_POINTER		0x09	// three bytes, low byte first
#define SDIO_CCCR_BLOCK_SIZE		0x10	// function 0's, two bytes
#define SDIO_CCCR_POWER				0x12
#define SDIO_CCCR_SPEED				0x13
#define  SDIO_SPEED_SHS				0x01	// supports high speed
#define  SDIO_SPEED_EHS				0x02	// enable high speed (BSS0)

// Function Basic Registers, at 0x100 * function
#define SDIO_FBR_BASE(f)			((uint32)(f) * 0x100)
#define SDIO_FBR_INTERFACE			0x00	// standard interface code [3:0]
#define SDIO_FBR_INTERFACE_EXT		0x01	// when the code is 0xf
#define SDIO_FBR_CIS_POINTER		0x09
#define SDIO_FBR_BLOCK_SIZE			0x10

// CIS tuples
#define SDIO_CISTPL_NULL			0x00
#define SDIO_CISTPL_VERS_1			0x15
#define SDIO_CISTPL_MANFID			0x20
#define SDIO_CISTPL_FUNCID			0x21
#define SDIO_CISTPL_FUNCE			0x22
#define SDIO_CISTPL_END				0xff
#define SDIO_CIS_START				0x1000	// the CIS area, 0x1000-0x17fff
#define SDIO_CIS_END				0x17fff


static inline uint32
sdio_direct_argument(bool write, uint8 function, uint32 address, uint8 data,
	bool readAfterWrite)
{
	return (write ? SDIO_ARG_WRITE : 0) | SDIO_ARG_FUNCTION(function)
		| (write && readAfterWrite ? SDIO_ARG_RAW : 0)
		| SDIO_ARG_ADDRESS(address) | (write ? data : 0);
}


/*!	\a count: blocks in block mode, bytes (1 to 512) otherwise. */
static inline uint32
sdio_extended_argument(bool write, uint8 function, uint32 address,
	bool blockMode, bool increment, uint32 count)
{
	return (write ? SDIO_ARG_WRITE : 0) | SDIO_ARG_FUNCTION(function)
		| (blockMode ? SDIO_ARG_BLOCK_MODE : 0)
		| (increment ? SDIO_ARG_INCREMENT : 0)
		| SDIO_ARG_ADDRESS(address) | (count & 0x1ff);
			// 512 bytes, the most in byte mode, is 0
}


typedef struct sdio_card_info {
	uint16	vendor;				// CISTPL_MANFID of the common CIS
	uint16	device;
	uint8	function_count;		// I/O functions, without function 0
	uint8	cccr_revision;
	uint8	sdio_revision;
	uint8	capabilities;		// SDIO_CCCR_CAPABILITY
	uint8	bus_width;			// 1 or 4, as set up by the bus
	bool	high_speed;
	uint32	clock;				// kHz
	uint16	max_block_size[SDIO_MAX_FUNCTIONS];
		// per function from its CIS (function 0: the common CIS)
	uint16	block_size[SDIO_MAX_FUNCTIONS];
		// as set with set_block_size(), 0 until then: byte mode only
} sdio_card_info;


typedef void (*sdio_interrupt_handler)(void* data);


typedef struct sdio_function_interface {
	driver_module_info info;

	/*!	CMD52: reads one register of \a function. */
	status_t (*read_byte)(void* cookie, uint8 function, uint32 address,
		uint8* _value);
	/*!	CMD52: writes one register. With \a _readBack, the card reads the
		register after the write (RAW) and the value is returned there. */
	status_t (*write_byte)(void* cookie, uint8 function, uint32 address,
		uint8 value, uint8* _readBack);

	/*!	CMD53: \a length bytes from or to \a function at \a address. The
		bus uses blocks of the function's block size (set_block_size())
		where the card can do multi-block transfers, and byte mode for the
		rest. \a increment: the address counts up (memory), or stays (a
		FIFO). Any kernel buffer will do. */
	status_t (*read)(void* cookie, uint8 function, uint32 address,
		bool increment, void* buffer, size_t length);
	status_t (*write)(void* cookie, uint8 function, uint32 address,
		bool increment, const void* buffer, size_t length);

	/*!	Writes the function's block size to its FBR (function 0: the
		CCCR), at most what its CIS says. */
	status_t (*set_block_size)(void* cookie, uint8 function, uint16 size);

	/*!	Sets the function's IOEx bit; enabling waits until the card reports
		the function ready (IORx), for as long as the CIS allows. */
	status_t (*enable_function)(void* cookie, uint8 function, bool enable);

	/*!	The function's interrupt: \a handler is called in a thread of the
		bus (it may do I/O) when the card raises its interrupt and the
		function has one pending, or every time if it is the card's only
		handler (the card's pending register is then not read). The card
		interrupt is masked from the host's interrupt until all handlers
		have returned: a handler serves its function before it returns, or
		the interrupt comes back at once. Sets the function's and the
		master's bit in IENx; a NULL \a handler takes them away again. */
	status_t (*set_interrupt_handler)(void* cookie, uint8 function,
		sdio_interrupt_handler handler, void* data);

	status_t (*get_card_info)(void* cookie, sdio_card_info* info);
} sdio_function_interface;


#ifdef _KERNEL_MODE

/*!	For drivers that do not live in the device tree (the network drivers of
	the FreeBSD compatibility layer): finds the published function \a function
	of a card from \a vendor whose device ID is one of the \a deviceCount
	\a devices (\a deviceCount 0: any). The node is acquired; give it back with
	put_node(). The interface and cookie stay valid while the node is
	acquired: function nodes keep their driver loaded. */
static inline status_t
sdio_find_function(device_manager_info* manager, uint16 vendor,
	const uint16* devices, uint32 deviceCount, uint8 function,
	device_node** _node, sdio_function_interface** _interface, void** _cookie)
{
	device_attr attributes[] = {
		{ SDIO_VENDOR_ATTRIBUTE, B_UINT16_TYPE, { .ui16 = vendor } },
		{ SDIO_FUNCTION_ATTRIBUTE, B_UINT8_TYPE, { .ui8 = function } },
		{}
	};

	device_node* root = manager->get_root_node();
	device_node* node = NULL;
	status_t status = B_ENTRY_NOT_FOUND;
	// recursive, going on after the node found last (and releasing it)
	while (manager->find_child_node(root, attributes, &node) == B_OK) {
		uint16 device;
		if (manager->get_attr_uint16(node, SDIO_DEVICE_ATTRIBUTE, &device,
				false) != B_OK) {
			continue;
		}
		bool match = deviceCount == 0;
		for (uint32 i = 0; i < deviceCount && !match; i++)
			match = devices[i] == device;
		if (!match)
			continue;

		status = manager->get_driver(node, (driver_module_info**)_interface,
			_cookie);
		if (status != B_OK || *_cookie == NULL) {
			if (status == B_OK)
				status = B_NO_INIT;
			continue;
		}
		*_node = node;
		manager->put_node(root);
		return B_OK;
	}
	manager->put_node(root);
	return status;
}

#endif	// _KERNEL_MODE


#endif	/* _SDIO_H */
