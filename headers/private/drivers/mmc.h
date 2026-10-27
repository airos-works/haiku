/*
 * Copyright 2019-2020, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Adrien Destugues, pulkomandy@pulkomandy.tk
 */
#ifndef _MMC_H
#define _MMC_H


#include <device_manager.h>


#define MMC_BUS_MODULE_NAME "bus_managers/mmc/driver_v1"


struct IOOperation;

typedef enum card_type {
	CARD_TYPE_UNKNOWN,
	CARD_TYPE_MMC,
	CARD_TYPE_MMC_EXTENDED_CAPACITY,
	CARD_TYPE_SD,
	CARD_TYPE_SDHC,
	CARD_TYPE_UHS1,
	CARD_TYPE_UHS2,
	CARD_TYPE_SDIO
} card_type;


inline bool
is_mmc_card(card_type type)
{
	return type == CARD_TYPE_MMC || type == CARD_TYPE_MMC_EXTENDED_CAPACITY;
}


// SDHCI exposes bits 127..8 of an R2 reply, in low-word-first order.
// Field positions below retain the bit numbers used by the card specification.
inline uint32_t
mmc_response_bits(const uint32_t response[4], unsigned first, unsigned width)
{
	if (first < 8 || first > 127 || width == 0 || width > 32 || first + width > 128)
		return 0;
	unsigned shift = first - 8;
	unsigned word = shift / 32;
	shift %= 32;
	uint64_t value = response[word] >> shift;
	if (shift != 0 && width > 32 - shift)
		value |= (uint64_t)response[word + 1] << (32 - shift);
	return value & (((uint64_t)1 << width) - 1);
}


inline uint32_t
mmc_ext_csd_sector_count(const uint8_t data[512])
{
	return (uint32_t)data[212] | ((uint32_t)data[213] << 8)
		| ((uint32_t)data[214] << 16) | ((uint32_t)data[215] << 24);
}


inline uint32_t
mmc_ext_csd_cache_size(const uint8_t data[512])
{
	return (uint32_t)data[249] | ((uint32_t)data[250] << 8)
		| ((uint32_t)data[251] << 16) | ((uint32_t)data[252] << 24);
}


// Native R1 status: command/data errors, locked/write-protected media and
// erase status, plus the MMC SWITCH_ERROR flag (bit 7).
const uint32_t kMmcR1ErrorMask = 0xfff9a080;


// Commands for SD/eMMC cards defined in:
// SD: Physical Layer Simplified Specification Version 8.00
// eMMC: JEDEC Standard No. 84-B51. Sec 6.10.4
//
// They are in the common .h file for the mmc stack because the SDHCI driver
// currently needs to map them to the corresponding expected response types.
//
// TODO maybe it would be simpler to have the upper layers (the bus manager and mmc_disk) specify
// the expected response type when sending a command. This would avoid hardcoding command types
// and the corresponding response types in the command sending code.
//
// When the commands are specific to SD or MMC types, the constant is named with the corresponding
// prefix.
enum SD_COMMANDS {
	// Basic commands, class 0
	GO_IDLE_STATE = 0,
	MMC_SEND_OP_COND = 1,
	ALL_SEND_CID = 2,
	SD_SEND_RELATIVE_ADDR = 3,
	MMC_SET_RELATIVE_ADDR = 3,
	MMC_SWITCH = 6, // Same command number as SD_SET_BUS_WIDTH, but this one is not an ACMD
	SELECT_DESELECT_CARD = 7,
	SD_SEND_IF_COND = 8,
	MMC_SEND_EXT_CSD = 8,
	SD_SWITCH_FUNCTION = 6, // with a data phase, unlike MMC_SWITCH
	SEND_CSD = 9,
	SEND_CID = 10,
	SD_STOP_TRANSMISSION = 12,
	SEND_STATUS = 13,
	SET_BLOCK_LENGTH = 16,

	// Block oriented read and write commands, class 2
	SD_READ_SINGLE_BLOCK = 17,
	SD_READ_MULTIPLE_BLOCKS = 18,

	SD_WRITE_SINGLE_BLOCK = 24,
	SD_WRITE_MULTIPLE_BLOCKS = 25,

	// Erase commands, class 5
	SD_ERASE_WR_BLK_START = 32,
	SD_ERASE_WR_BLK_END = 33,
	SD_ERASE = 38,

	// Application specific commands, class 8
	SD_APP_CMD = 55,

	// I/O mode commands, class 9
	SD_IO_ABORT = 52,
};


// Application commands are sent with a prefix CMD55 (SD_APP_CMD)
enum SDHCI_APPLICATION_COMMANDS {
	SD_SET_BUS_WIDTH = 6,
	SD_SEND_OP_COND = 41,
};


// Cache enable and flush can outlast the SDHCI hardware busy counter. Collect
// R1 and let the bus manager poll status while retaining its bus lock. Use a
// bounded 30-second budget for these two operations only.
inline bigtime_t
mmc_cache_busy_timeout(card_type type, uint8_t command, uint32_t argument)
{
	if (is_mmc_card(type) && command == MMC_SWITCH
		&& (argument == 0x03200100 || argument == 0x03210100))
		return 30000000;
	return 0;
}


// Interface between mmc_bus and underlying implementation (sdhci_pci or any
// other thing that can execute mmc commands)
typedef struct mmc_bus_interface {
	driver_module_info info;

	status_t (*set_clock)(void* controller, uint32_t kilohertz);
		// Configure the bus clock. The bus is initialized with a slow clock
		// that allows device enumeration in all cases, but after enumeration
		// the mmc_bus knows how fast each card can go, and configures the bus
		// accordingly.
	status_t (*execute_command)(void* controller, uint8_t command,
		uint32_t argument, uint32_t* result);
		// Execute a command with no I/O phase
	status_t (*do_io)(void* controller, uint8_t command,
		IOOperation* operation, bool offsetAsSectors);
		// Execute a command that involves a data transfer.
	void (*set_scan_semaphore)(void* controller, sem_id sem);
		// Pass the semaphore used for device rescan to the bus controller
	void (*set_bus_width)(void* controller, int width);
		// Set the data bus width to 1, 4 or 8 bit mode.
	void (*terminate_bus)(void* controller);
	void (*set_card_type)(void* controller, card_type type);
	status_t (*read_extended_csd)(void* controller, uint8_t data[512]);
		// Read the selected MMC card's 512-byte EXT_CSD data phase.
	status_t (*switch_sd_function)(void* controller, uint32_t argument,
		uint8_t data[64]);
		// SD CMD6 (SWITCH_FUNC) to the selected card, with its 64-byte status
		// data phase. Optional: without it, SD cards stay at default speed.
} mmc_bus_interface;


// Interface between mmc device driver (mmc_disk, sdio drivers, ...) and mmc_bus
// This interface is rather generic as it allows implementation of drivers for
// different type of cards, which will use different commands. The bus
// provides a generic interface for all of them, and is not specific to any
// type of card.
typedef struct mmc_device_interface {
	driver_module_info info;
	status_t (*execute_command)(device_node* node, void* cookie, uint16_t rca,
		uint8_t command, uint32_t argument, uint32_t* result);
		// Execute a command with no I/O phase
	status_t (*do_io)(device_node* controller, void* cookie, uint16_t rca,
		uint8_t command, IOOperation* operation, bool offsetAsSectors);
		// Execute a command that involves a data transfer.
	void (*set_bus_width)(device_node* controller, void* cookie, int width);
		// Set the data bus width to 1, 4 or 8 bit mode.
} mmc_device_interface;


// Device attribute paths for the MMC device
const char* const kMmcRcaAttribute = "mmc/rca";
const char* const kMmcTypeAttribute = "mmc/type";
const char* const kMmcSectorCountAttribute = "mmc/sector_count";
const char* const kMmcCacheEnabledAttribute = "mmc/cache_enabled";
const char* const kMmcEnableCacheAttribute = "mmc/enable_cache";
const char* const kMmcReadOnlyAttribute = "mmc/read_only";
const char* const kMmcNonRemovableAttribute = "mmc/non_removable";
const char* const kMmcMaxBusWidthAttribute = "mmc/max_bus_width";
const char* const kMmcSdHighSpeedAttribute = "mmc/sd_high_speed";
	// set to 1 by a host whose SD high speed mode (50 MHz) was verified
const char* const kMmcBusWidthAttribute = "mmc/bus_width";


#endif /* _MMC_H */
