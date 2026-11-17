/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	A minimal SDIO host for the Raspberry Pi 4's Wi-Fi chip, see sdio_host.h.

	The controller is an SDHCI with the BCM2835 family's manners: registers
	are 32 bits wide and must be accessed that way (transfer mode and
	command are written together), and two writes need more than two card
	clock cycles between them. Commands and data are polled; data goes
	through the data register a word at a time. Only the card's own
	interrupt (its functions asking for service) uses the interrupt line. */


#include "sdio_host.h"

#include <stdio.h>
#include <string.h>

#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>

#include <lock.h>
#include <util/AutoLock.h>

#include <rpi_firmware.h>


//#define TRACE_SDIO
#ifdef TRACE_SDIO
#	define TRACE(x...) dprintf("broadcomfmac: sdio: " x)
#else
#	define TRACE(x...) ;
#endif
#define ERROR(x...)	dprintf("broadcomfmac: sdio: " x)

// where the BCM2711 has things (ARM physical addresses)
#define SDHCI_BASE				0xfe300000
#define SDHCI_INTERRUPT			(126 + 32)
	// shared with the other SD controller (the card slot's)
#define GPIO_BASE				0xfe200000
#define GPIO_FSEL3				0x0c	// pins 30 to 39, three bits each
#define GPIO_PULL2				0xec	// pins 32 to 47, two bits each
#define GPIO_ALT3				7
#define GPIO_PULL_UP			1
#define EXPANDER_GPIO_BASE		128
#define EXPANDER_WL_ON			1

#define REG_BLOCK				0x04
#define REG_ARGUMENT			0x08
#define REG_COMMAND				0x0c
#define REG_RESPONSE			0x10
#define REG_DATA				0x20
#define REG_PRESENT_STATE		0x24
#define REG_CONTROL0			0x28
#define REG_CONTROL1			0x2c
#define REG_INTERRUPT			0x30
#define REG_INTERRUPT_MASK		0x34
#define REG_INTERRUPT_ENABLE	0x38

#define TM_BLOCK_COUNT			(1 << 1)
#define TM_READ					(1 << 4)
#define TM_MULTI_BLOCK			(1 << 5)
#define CMD_RESPONSE_48			(2 << 16)
#define CMD_CRC_CHECK			(1 << 19)
#define CMD_INDEX_CHECK			(1 << 20)
#define CMD_DATA				(1 << 21)
#define CMD_INDEX(x)			((uint32)(x) << 24)
#define CMD_R1		(CMD_RESPONSE_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_R4		CMD_RESPONSE_48

#define STATE_COMMAND_INHIBIT	(1 << 0)
#define STATE_DATA_INHIBIT		(1 << 1)
#define STATE_WRITE_READY		(1 << 10)
#define STATE_READ_READY		(1 << 11)

#define CONTROL0_4BIT			(1 << 1)
#define CONTROL0_HIGH_SPEED		(1 << 2)
#define CONTROL0_POWER_ON		(1 << 8)
#define CONTROL0_POWER_3V3		(7 << 9)

#define CONTROL1_CLOCK_INTERNAL	(1 << 0)
#define CONTROL1_CLOCK_STABLE	(1 << 1)
#define CONTROL1_CLOCK_CARD		(1 << 2)
#define CONTROL1_TIMEOUT_MAX	(0xe << 16)
#define CONTROL1_RESET_ALL		(1 << 24)
#define CONTROL1_RESET_COMMAND	(1 << 25)
#define CONTROL1_RESET_DATA		(1 << 26)

#define INT_COMMAND_COMPLETE	(1 << 0)
#define INT_TRANSFER_COMPLETE	(1 << 1)
#define INT_WRITE_READY			(1 << 4)
#define INT_READ_READY			(1 << 5)
#define INT_CARD				(1 << 8)
#define INT_ERROR_MASK			0xffff0000

// SDIO
#define SD_GO_IDLE				0
#define SD_SEND_RELATIVE_ADDR	3
#define SD_IO_SEND_OP_COND		5
#define SD_SELECT_CARD			7
#define SD_IO_RW_DIRECT			52
#define SD_IO_RW_EXTENDED		53

#define CCCR_IO_ENABLE			0x02
#define CCCR_IO_READY			0x03
#define CCCR_BUS_INTERFACE		0x07
#define CCCR_SPEED				0x13
#define FBR_BLOCK_SIZE(f)		((f) * 0x100 + 0x10)

#define MAX_FUNCTIONS			8


static volatile uint8* sRegisters;
static area_id sRegisterArea = -1;
static volatile uint8* sGpio;
static area_id sGpioArea = -1;
static rpi_firmware_module_info* sFirmware;
static mutex sLock = MUTEX_INITIALIZER("broadcomfmac sdio");
static uint32 sBaseClock;
static uint32 sClock;
static uint16 sBlockSize[MAX_FUNCTIONS];
static void (*sCardHandler)(void*);
static void* sCardHandlerData;
static bool sInterruptInstalled;
static spinlock sInterruptLock = B_SPINLOCK_INITIALIZER;

static int32 card_interrupt(void* data);


static inline uint32
read_reg(uint32 reg)
{
	return *(volatile uint32*)(sRegisters + reg);
}


static void
write_reg(uint32 reg, uint32 value)
{
	*(volatile uint32*)(sRegisters + reg) = value;
	memory_full_barrier();

	// the next write must be more than two card clock cycles away
	if (sClock == 0)
		spin(10);
	else if (sClock <= 400000)
		spin((4 * 1000000 + sClock - 1) / sClock);
	else
		spin(1);
}


static status_t
reset(uint32 what)
{
	write_reg(REG_CONTROL1, read_reg(REG_CONTROL1) | what);

	bigtime_t timeout = system_time() + 100000;
	while ((read_reg(REG_CONTROL1) & what) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		snooze(100);
	}
	return B_OK;
}


static status_t
set_clock(uint32 frequency)
{
	uint32 control = read_reg(REG_CONTROL1) & ~0xffffu;
	write_reg(REG_CONTROL1, control);
	sClock = 0;

	// the 10 bit divided clock of SDHCI 3: base / (2 * divider)
	uint32 divider = 0;
	if (frequency < sBaseClock) {
		divider = (sBaseClock + 2 * frequency - 1) / (2 * frequency);
		if (divider > 0x3ff)
			divider = 0x3ff;
	}

	control |= ((divider & 0xff) << 8) | ((divider >> 8) << 6)
		| CONTROL1_CLOCK_INTERNAL | CONTROL1_TIMEOUT_MAX;
	write_reg(REG_CONTROL1, control);

	bigtime_t timeout = system_time() + 100000;
	while ((read_reg(REG_CONTROL1) & CONTROL1_CLOCK_STABLE) == 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		snooze(100);
	}

	write_reg(REG_CONTROL1, control | CONTROL1_CLOCK_CARD);
	sClock = divider == 0 ? sBaseClock : sBaseClock / (2 * divider);
	snooze(2000);
	return B_OK;
}


/*!	Waits for one of \a events; an error bit ends the wait. */
static status_t
wait_for(uint32 events, bigtime_t timeout)
{
	bigtime_t end = system_time() + timeout;
	uint32 spins = 0;
	while (true) {
		uint32 status = read_reg(REG_INTERRUPT);
		if ((status & INT_ERROR_MASK) != 0) {
			write_reg(REG_INTERRUPT, status & ~INT_CARD);
			return (status & (1 << 16 | 1 << 20)) != 0 ? B_TIMED_OUT
				: B_IO_ERROR;
		}
		if ((status & events) != 0) {
			write_reg(REG_INTERRUPT, status & events);
			return B_OK;
		}
		if (system_time() > end)
			return B_TIMED_OUT;
		if (++spins > 2000)
			snooze(50);
	}
}


static void
recover()
{
	reset(CONTROL1_RESET_COMMAND | CONTROL1_RESET_DATA);
	write_reg(REG_INTERRUPT, 0xffffffff & ~INT_CARD);
}


static status_t
command(uint32 index, uint32 argument, uint32 flags, uint32* _response)
{
	bigtime_t timeout = system_time() + 1000000;
	while ((read_reg(REG_PRESENT_STATE) & (STATE_COMMAND_INHIBIT
			| ((flags & CMD_DATA) != 0 ? STATE_DATA_INHIBIT : 0))) != 0) {
		if (system_time() > timeout) {
			recover();
			return B_BUSY;
		}
		spin(10);
	}

	write_reg(REG_INTERRUPT, 0xffffffff & ~INT_CARD);
	write_reg(REG_ARGUMENT, argument);
	write_reg(REG_COMMAND, CMD_INDEX(index) | flags);

	status_t status = wait_for(INT_COMMAND_COMPLETE, 1000000);
	if (status != B_OK) {
		recover();
		return status;
	}
	if (_response != NULL)
		*_response = read_reg(REG_RESPONSE);
	return B_OK;
}


static status_t
rw_direct(bool write, uint32 function, uint32 address, uint8* _value)
{
	uint32 argument = (write ? 1u << 31 : 0) | (function << 28)
		| (write ? 1 << 27 : 0) | ((address & 0x1ffff) << 9)
		| (write ? *_value : 0);
	uint32 response;
	status_t status = command(SD_IO_RW_DIRECT, argument, CMD_R1, &response);
	if (status != B_OK)
		return status;
	if ((response & 0xcb00) != 0)
		return B_IO_ERROR;
	*_value = response & 0xff;
	return B_OK;
}


/*!	One CMD53: \a count blocks of \a blockSize bytes, or with \a count 0 a
	byte transfer of \a blockSize bytes.
*/
static status_t
rw_extended(bool write, uint32 function, uint32 address, uint8* buffer,
	uint32 blockSize, uint32 count, bool increment)
{
	bool blockMode = count > 0;
	if (!blockMode)
		count = 1;

	write_reg(REG_BLOCK, blockSize | (count << 16));

	uint32 argument = (write ? 1u << 31 : 0) | (function << 28)
		| (blockMode ? 1 << 27 : 0) | (increment ? 1 << 26 : 0)
		| ((address & 0x1ffff) << 9)
		| (blockMode ? count : (blockSize == 512 ? 0 : blockSize));
	uint32 flags = CMD_R1 | CMD_DATA | (write ? 0 : TM_READ)
		| (count > 1 ? TM_MULTI_BLOCK | TM_BLOCK_COUNT : 0);

	uint32 response;
	status_t status = command(SD_IO_RW_EXTENDED, argument, flags, &response);
	if (status != B_OK)
		return status;
	if ((response & 0xcb00) != 0) {
		recover();
		return B_IO_ERROR;
	}

	for (uint32 block = 0; block < count; block++) {
		status = wait_for(write ? INT_WRITE_READY : INT_READ_READY, 1000000);
		if (status != B_OK) {
			recover();
			return status;
		}
		for (uint32 i = 0; i < blockSize; i += 4) {
			uint32 word = 0;
			uint32 bytes = blockSize - i < 4 ? blockSize - i : 4;
			if (write) {
				memcpy(&word, buffer + i, bytes);
				*(volatile uint32*)(sRegisters + REG_DATA) = word;
			} else {
				word = *(volatile uint32*)(sRegisters + REG_DATA);
				memcpy(buffer + i, &word, bytes);
			}
		}
		buffer += blockSize;
	}

	status = wait_for(INT_TRANSFER_COMPLETE, 1000000);
	if (status != B_OK)
		recover();
	return status;
}


static void
setup_pins()
{
	// GPIO 34 to 39 to the SDIO controller (ALT3), pull-ups on all but the
	// clock
	uint32 select = *(volatile uint32*)(sGpio + GPIO_FSEL3);
	uint32 pull = *(volatile uint32*)(sGpio + GPIO_PULL2);
	for (uint32 pin = 34; pin <= 39; pin++) {
		select = (select & ~(7u << ((pin - 30) * 3)))
			| (GPIO_ALT3 << ((pin - 30) * 3));
		pull &= ~(3u << ((pin - 32) * 2));
		if (pin != 34)
			pull |= GPIO_PULL_UP << ((pin - 32) * 2);
	}
	*(volatile uint32*)(sGpio + GPIO_FSEL3) = select;
	*(volatile uint32*)(sGpio + GPIO_PULL2) = pull;
	memory_full_barrier();
}


static status_t
set_power(bool on)
{
	uint32 request[2] = { EXPANDER_GPIO_BASE + EXPANDER_WL_ON, on ? 1u : 0u };
	return sFirmware->property(RPI_FIRMWARE_SET_GPIO_STATE, request,
		sizeof(request));
}


//	#pragma mark -


bool
rpi_sdio_present(void)
{
	// The device tree says which board this is: look for the BCM2711's
	// second SD controller, which only this family has.
	device_manager_info* manager;
	if (get_module(B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&manager)
			!= B_OK) {
		return false;
	}

	device_attr attributes[] = {
		{ "fdt/compatible", B_STRING_TYPE,
			{ .string = "brcm,bcm2711-emmc2" } },
		{}
	};
	device_node* root = manager->get_root_node();
	device_node* node = NULL;
	bool found = manager->find_child_node(root, attributes, &node) == B_OK;
	if (found) {
		// The lab's QEMU tree redirects emmc2 to the only emulated SDHCI.
		// It then occupies our SDIO registers: resetting them would destroy
		// the boot card's transfer state. Never claim a shared controller.
		fdt_device_module_info* module;
		fdt_device* device;
		uint64 address;
		uint64 length;
		found = manager->get_driver(node, (driver_module_info**)&module,
			(void**)&device) == B_OK
			&& module->get_reg(device, 0, &address, &length)
			&& address != SDHCI_BASE;
		manager->put_node(node);
	}
	manager->put_node(root);

	put_module(B_DEVICE_MANAGER_MODULE_NAME);
	return found;
}


status_t
rpi_sdio_init(void)
{
	status_t status = get_module(RPI_FIRMWARE_MODULE_NAME,
		(module_info**)&sFirmware);
	if (status != B_OK)
		return status;

	sRegisterArea = map_physical_memory("broadcomfmac sdio", SDHCI_BASE,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&sRegisters);
	sGpioArea = map_physical_memory("broadcomfmac gpio", GPIO_BASE,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&sGpio);
	if (sRegisterArea < 0 || sGpioArea < 0) {
		rpi_sdio_uninit();
		return B_NO_MEMORY;
	}

	MutexLocker locker(sLock);

	if (sFirmware->get_clock_rate(RPI_FIRMWARE_CLOCK_EMMC, false, &sBaseClock)
			!= B_OK || sBaseClock == 0) {
		sBaseClock = 100000000;
	}

	// the chip out of reset, then the controller from scratch
	setup_pins();
	set_power(false);
	snooze(20000);
	set_power(true);
	snooze(200000);

	sClock = 0;
	status = reset(CONTROL1_RESET_ALL);
	if (status != B_OK) {
		ERROR("the controller does not come out of reset\n");
		return status;
	}
	write_reg(REG_CONTROL0, CONTROL0_POWER_ON | CONTROL0_POWER_3V3);
	write_reg(REG_INTERRUPT_ENABLE, 0);
	write_reg(REG_INTERRUPT_MASK, 0xffffffff);
	write_reg(REG_INTERRUPT, 0xffffffff & ~INT_CARD);
	status = set_clock(400000);
	if (status != B_OK)
		return status;
	snooze(10000);

	// identification: idle, the operating conditions, an address
	command(SD_GO_IDLE, 0, 0, NULL);
	uint32 ocr = 0;
	status = command(SD_IO_SEND_OP_COND, 0, CMD_R4, &ocr);
	if (status != B_OK) {
		ERROR("no SDIO card answers (%s)\n", strerror(status));
		return B_DEVICE_NOT_FOUND;
	}
	bigtime_t timeout = system_time() + 1000000;
	do {
		status = command(SD_IO_SEND_OP_COND, ocr & 0x00ff8000, CMD_R4, &ocr);
		if (status != B_OK)
			return status;
		if ((ocr & 0x80000000) != 0)
			break;
		snooze(10000);
	} while (system_time() < timeout);
	if ((ocr & 0x80000000) == 0) {
		ERROR("the card does not get ready (OCR %#" B_PRIx32 ")\n", ocr);
		return B_TIMED_OUT;
	}

	uint32 response;
	status = command(SD_SEND_RELATIVE_ADDR, 0, CMD_R1, &response);
	if (status != B_OK)
		return status;
	uint32 rca = response >> 16;
	status = command(SD_SELECT_CARD, rca << 16, CMD_R1, NULL);
	if (status != B_OK)
		return status;

	// four data lines, and 50 MHz if the card does high speed
	uint8 value = 0;
	status = rw_direct(false, 0, CCCR_BUS_INTERFACE, &value);
	if (status != B_OK)
		return status;
	value = (value & ~3) | 2;
	status = rw_direct(true, 0, CCCR_BUS_INTERFACE, &value);
	if (status != B_OK)
		return status;
	write_reg(REG_CONTROL0, read_reg(REG_CONTROL0) | CONTROL0_4BIT);

	uint32 frequency = 25000000;
	value = 0;
	if (rw_direct(false, 0, CCCR_SPEED, &value) == B_OK && (value & 1) != 0) {
		value |= 2;
		if (rw_direct(true, 0, CCCR_SPEED, &value) == B_OK) {
			write_reg(REG_CONTROL0,
				read_reg(REG_CONTROL0) | CONTROL0_HIGH_SPEED);
			frequency = 50000000;
		}
	}
	status = set_clock(frequency);
	if (status != B_OK)
		return status;

	memset(sBlockSize, 0, sizeof(sBlockSize));
	TRACE("card at address %#" B_PRIx32 ", OCR %#" B_PRIx32 ", %" B_PRIu32
		" functions, clock %" B_PRIu32 " Hz (base %" B_PRIu32 ")\n", rca, ocr,
		(ocr >> 28) & 7, sClock, sBaseClock);
	return B_OK;
}


void
rpi_sdio_uninit(void)
{
	if (sInterruptInstalled) {
		write_reg(REG_INTERRUPT_ENABLE, 0);
		remove_io_interrupt_handler(SDHCI_INTERRUPT, card_interrupt, NULL);
		sInterruptInstalled = false;
	}
	sCardHandler = NULL;
	if (sRegisters != NULL && sFirmware != NULL) {
		write_reg(REG_INTERRUPT_MASK, 0);
		set_power(false);
	}
	if (sRegisterArea >= 0)
		delete_area(sRegisterArea);
	if (sGpioArea >= 0)
		delete_area(sGpioArea);
	sRegisterArea = sGpioArea = -1;
	sRegisters = sGpio = NULL;
	if (sFirmware != NULL)
		put_module(RPI_FIRMWARE_MODULE_NAME);
	sFirmware = NULL;
}


status_t
rpi_sdio_rw_byte(bool write, uint32 function, uint32 address, uint8* _value)
{
	MutexLocker locker(sLock);
	return rw_direct(write, function, address, _value);
}


status_t
rpi_sdio_rw_extended(bool write, uint32 function, uint32 address,
	uint8* buffer, size_t length, bool increment)
{
	if (function >= MAX_FUNCTIONS)
		return B_BAD_VALUE;

	MutexLocker locker(sLock);

	uint32 blockSize = sBlockSize[function];
	while (length > 0) {
		status_t status;
		size_t done;
		if (blockSize != 0 && length >= blockSize) {
			uint32 count = length / blockSize;
			if (count > 256)
				count = 256;
			status = rw_extended(write, function, address, buffer, blockSize,
				count, increment);
			done = (size_t)count * blockSize;
		} else {
			done = length > 512 ? 512 : length;
			if (blockSize != 0 && done > blockSize)
				done = blockSize;
			status = rw_extended(write, function, address, buffer, done, 0,
				increment);
		}
		if (status != B_OK)
			return status;

		buffer += done;
		length -= done;
		if (increment)
			address += done;
	}
	return B_OK;
}


status_t
rpi_sdio_set_block_size(uint32 function, uint16 size)
{
	if (function >= MAX_FUNCTIONS)
		return B_BAD_VALUE;

	MutexLocker locker(sLock);
	uint8 low = size & 0xff, high = size >> 8;
	status_t status = rw_direct(true, 0, FBR_BLOCK_SIZE(function), &low);
	if (status == B_OK)
		status = rw_direct(true, 0, FBR_BLOCK_SIZE(function) + 1, &high);
	if (status == B_OK)
		sBlockSize[function] = size;
	return status;
}


status_t
rpi_sdio_enable_function(uint32 function, bool enable)
{
	MutexLocker locker(sLock);

	uint8 value = 0;
	status_t status = rw_direct(false, 0, CCCR_IO_ENABLE, &value);
	if (status != B_OK)
		return status;
	if (enable)
		value |= 1 << function;
	else
		value &= ~(1 << function);
	status = rw_direct(true, 0, CCCR_IO_ENABLE, &value);
	if (status != B_OK || !enable)
		return status;

	bigtime_t timeout = system_time() + 1000000;
	while (system_time() < timeout) {
		value = 0;
		status = rw_direct(false, 0, CCCR_IO_READY, &value);
		if (status != B_OK)
			return status;
		if ((value & (1 << function)) != 0)
			return B_OK;
		snooze(5000);
	}
	return B_TIMED_OUT;
}


bool
rpi_sdio_card_interrupt(void)
{
	return sRegisters != NULL && (read_reg(REG_INTERRUPT) & INT_CARD) != 0;
}


static int32
card_interrupt(void* data)
{
	// only the card interrupt is ever let through to the line
	if ((read_reg(REG_INTERRUPT_ENABLE) & INT_CARD) == 0
		|| (read_reg(REG_INTERRUPT) & INT_CARD) == 0) {
		return B_UNHANDLED_INTERRUPT;
	}

	// The line stays raised until the function is served: off until then.
	// The controller holds on to the status bit until the bit is masked,
	// whatever the card does; left alone it interrupts again at once, for
	// ever.
	acquire_spinlock(&sInterruptLock);
	*(volatile uint32*)(sRegisters + REG_INTERRUPT_ENABLE) = 0;
	memory_full_barrier();
	spin(1);
	*(volatile uint32*)(sRegisters + REG_INTERRUPT_MASK) = ~(uint32)INT_CARD;
	memory_full_barrier();
	release_spinlock(&sInterruptLock);

	if (sCardHandler != NULL)
		sCardHandler(sCardHandlerData);
	return B_INVOKE_SCHEDULER;
}


status_t
rpi_sdio_set_interrupt_handler(void (*handler)(void*), void* data)
{
	if (sRegisters == NULL)
		return B_NO_INIT;

	sCardHandlerData = data;
	sCardHandler = handler;
	if (handler == NULL || sInterruptInstalled)
		return B_OK;

	status_t status = install_io_interrupt_handler(SDHCI_INTERRUPT,
		card_interrupt, NULL, 0);
	if (status == B_OK)
		sInterruptInstalled = true;
	return status;
}


void
rpi_sdio_enable_card_interrupt(void)
{
	if (sRegisters == NULL || !sInterruptInstalled)
		return;

	cpu_status state = disable_interrupts();
	acquire_spinlock(&sInterruptLock);
	*(volatile uint32*)(sRegisters + REG_INTERRUPT_MASK) = 0xffffffff;
	memory_full_barrier();
	spin(1);
	*(volatile uint32*)(sRegisters + REG_INTERRUPT_ENABLE) = INT_CARD;
	memory_full_barrier();
	release_spinlock(&sInterruptLock);
	restore_interrupts(state);
}
