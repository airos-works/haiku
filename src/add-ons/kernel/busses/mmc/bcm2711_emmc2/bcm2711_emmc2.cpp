/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The SD card controller of the BCM2711 (Raspberry Pi 4): "EMMC2".

	It follows the SD host controller specification, but the generic sdhci
	driver cannot run it: the block only works with 32-bit register accesses,
	and it loses a write that follows another one within two SD clock cycles.
	So the transfer mode is written in one go with the command, the block
	size with the block count, and every write is followed by a pause while
	the card clock is slow. (Linux: sdhci-iproc.c; FreeBSD: bcm2835_sdhci.c.)

	Data goes through SDMA and a private buffer in memory the controller can
	reach; the reachable range and the bus address offset come from the
	parent bus' "dma-ranges" (the first boards only reach the low gigabyte).
	Card detection is not wired to the controller on the Pi 4, so the slot is
	simply probed when the bus comes up.

	The BCM2712's (Raspberry Pi 5) SD card slot is the same kind of block
	("brcm,bcm2712-sdhci", Linux: sdhci-brcmstb.c) and runs the same way; the
	loader puts its fixed 200 MHz base clock into the device tree. */


#include <algorithm>
#include <new>
#include <stdio.h>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>

#include <arch/atomic.h>
#include <condition_variable.h>
#include <vm/vm.h>

#include "IOSchedulerSimple.h"
#include "mmc.h"


//#define TRACE_EMMC2
#ifdef TRACE_EMMC2
#	define TRACE(x...) dprintf("emmc2: " x)
#else
#	define TRACE(x...) ;
#endif
#define INFO(x...)	dprintf("emmc2: " x)
#define ERROR(x...)	dprintf("emmc2: " x)


#define EMMC2_DRIVER_MODULE_NAME	"busses/mmc/bcm2711_emmc2/driver_v1"
#define EMMC2_BUS_MODULE_NAME		"busses/mmc/bcm2711_emmc2/device/v1"


// registers
#define REG_SDMA_ADDRESS	0x00
#define REG_BLOCK			0x04	// size [11:0], SDMA boundary [14:12], count [31:16]
#define REG_ARGUMENT		0x08
#define REG_COMMAND			0x0c	// transfer mode [15:0], command [31:16]
#define REG_RESPONSE		0x10
#define REG_DATA			0x20
#define REG_PRESENT_STATE	0x24
#define REG_CONTROL0		0x28	// host control, power, block gap, wake-up
#define REG_CONTROL1		0x2c	// clock, timeout, software reset
#define REG_INTERRUPT		0x30
#define REG_INTERRUPT_MASK	0x34	// which events are latched
#define REG_INTERRUPT_ENABLE 0x38	// which of them raise the interrupt
#define REG_CAPABILITIES	0x40

#define BLOCK_SDMA_BOUNDARY_512K	(7 << 12)

#define TM_DMA_ENABLE		(1 << 0)
#define TM_BLOCK_COUNT		(1 << 1)
#define TM_AUTO_CMD12		(1 << 2)
#define TM_READ				(1 << 4)
#define TM_MULTI_BLOCK		(1 << 5)

#define CMD_RESPONSE_NONE	(0 << 16)
#define CMD_RESPONSE_136	(1 << 16)
#define CMD_RESPONSE_48		(2 << 16)
#define CMD_RESPONSE_48_BUSY (3 << 16)
#define CMD_CRC_CHECK		(1 << 19)
#define CMD_INDEX_CHECK		(1 << 20)
#define CMD_DATA			(1 << 21)
#define CMD_INDEX(x)		((uint32)(x) << 24)

#define CMD_R1		(CMD_RESPONSE_48 | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_R1B		(CMD_RESPONSE_48_BUSY | CMD_CRC_CHECK | CMD_INDEX_CHECK)
#define CMD_R2		(CMD_RESPONSE_136 | CMD_CRC_CHECK)
#define CMD_R3		CMD_RESPONSE_48
#define CMD_R6		CMD_R1
#define CMD_R7		CMD_R1

#define STATE_COMMAND_INHIBIT	(1 << 0)
#define STATE_DATA_INHIBIT		(1 << 1)
#define STATE_WRITE_READY		(1 << 10)
#define STATE_READ_READY		(1 << 11)

#define CAPABILITY_SDMA			(1 << 22)

#define CONTROL0_4BIT			(1 << 1)
#define CONTROL0_HIGH_SPEED		(1 << 2)
#define CONTROL0_DMA_MASK		(3 << 3)
#define CONTROL0_POWER_ON		(1 << 8)
#define CONTROL0_POWER_3V3		(7 << 9)

#define CONTROL1_CLOCK_INTERNAL	(1 << 0)
#define CONTROL1_CLOCK_STABLE	(1 << 1)
#define CONTROL1_CLOCK_CARD		(1 << 2)
#define CONTROL1_TIMEOUT_MASK	(0xf << 16)
#define CONTROL1_TIMEOUT_MAX	(0xe << 16)
#define CONTROL1_RESET_ALL		(1 << 24)
#define CONTROL1_RESET_COMMAND	(1 << 25)
#define CONTROL1_RESET_DATA		(1 << 26)

#define INT_COMMAND_COMPLETE	(1 << 0)
#define INT_TRANSFER_COMPLETE	(1 << 1)
#define INT_DMA					(1 << 3)
#define INT_ERROR				(1 << 15)
#define INT_COMMAND_TIMEOUT		(1 << 16)
#define INT_DATA_TIMEOUT		(1 << 20)
#define INT_ERROR_MASK			0xffff0000
#define INT_WANTED				(INT_COMMAND_COMPLETE | INT_TRANSFER_COMPLETE \
									| INT_DMA | INT_ERROR | INT_ERROR_MASK)

// CMD6 of SD cards (as an application command the number sets the width)
#define SD_SWITCH_FUNCTION		6

// what the firmware clocks the controller with, unless the device tree says
#define DEFAULT_BASE_CLOCK		100000000

static const size_t kDmaBufferSize = 512 * 1024;
static const uint32 kBlockSize = 512;


device_manager_info* gDeviceManager;


struct emmc2_device {
	device_node*	node;
	uint64			registers;
	uint64			registersSize;
	uint32			interrupt;
	uint32			baseClock;		// Hz
	uint32			busWidth;
	// from the parent's dma-ranges
	uint64			dmaHighAddress;	// CPU addresses below this are reachable
	int64			dmaBusOffset;	// bus address = CPU address + this
};


class Emmc2Bus {
public:
								Emmc2Bus(const emmc2_device& device);
								~Emmc2Bus();

			status_t			InitCheck() const { return fStatus; }

			status_t			SetClock(uint32 kilohertz);
			status_t			ExecuteCommand(uint8 command, uint32 argument,
									uint32* response);
			status_t			DoIO(uint8 command, IOOperation* operation,
									bool offsetAsSectors);
			void				SetScanSemaphore(sem_id semaphore);
			void				SetBusWidth(int width);
			void				SetCardType(card_type type)
									{ fCardType = type; }
			void				Terminate();

			int32				HandleInterrupt();

private:
			uint32				_Read(uint32 reg);
			void				_Write(uint32 reg, uint32 value);
			status_t			_Reset(uint32 what);
			void				_Recover();
			status_t			_AllocateBuffer();
			status_t			_TransferPolled(bool isWrite, size_t size,
									size_t blockSize = kBlockSize);
			status_t			_CopyVecs(bool toBuffer,
									const generic_io_vec* vecs, size_t count,
									size_t& index, generic_size_t& vecOffset,
									size_t size);
			status_t			_ReadData(uint8 command, uint32 argument,
									size_t size);
			void				_SwitchToHighSpeed();
			status_t			_SendCommand(uint8 command, uint32 argument,
									uint32 flags, uint32* response);
			status_t			_Wait(uint32 mask, bigtime_t timeout);

private:
			emmc2_device		fDevice;
			area_id				fRegisterArea;
			volatile uint8*		fRegisters;
			status_t			fStatus;
			bool				fInterruptInstalled;
			uint32				fClock;			// Hz, 0 while off
			int32				fEvents;
			ConditionVariable	fEventCondition;
			card_type			fCardType;
			bool				fUseDMA;
			area_id				fBufferArea;
			void*				fBuffer;
			phys_addr_t			fBufferAddress;
};


static int32
emmc2_interrupt(void* data)
{
	return ((Emmc2Bus*)data)->HandleInterrupt();
}


Emmc2Bus::Emmc2Bus(const emmc2_device& device)
	:
	fDevice(device),
	fRegisterArea(-1),
	fRegisters(NULL),
	fStatus(B_NO_INIT),
	fInterruptInstalled(false),
	fClock(0),
	fEvents(0),
	fCardType(CARD_TYPE_UNKNOWN),
	fUseDMA(true),
	fBufferArea(-1),
	fBuffer(NULL),
	fBufferAddress(0)
{
	fEventCondition.Init(this, "emmc2 events");

	fRegisterArea = map_physical_memory("emmc2 registers", fDevice.registers,
		fDevice.registersSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fRegisters);
	if (fRegisterArea < 0) {
		fStatus = fRegisterArea;
		return;
	}

	// The older controller of the family (and QEMU's model of this one) has
	// no SDMA; the data then goes through the data register word by word.
	fUseDMA = (_Read(REG_CAPABILITIES) & CAPABILITY_SDMA) != 0;

	fStatus = _AllocateBuffer();
	if (fStatus != B_OK)
		return;

	fStatus = _Reset(CONTROL1_RESET_ALL);
	if (fStatus != B_OK) {
		ERROR("the controller does not come out of reset\n");
		return;
	}

	_Write(REG_INTERRUPT_ENABLE, 0);
	_Write(REG_INTERRUPT_MASK, INT_WANTED);
	_Write(REG_INTERRUPT, 0xffffffff);

	fStatus = install_io_interrupt_handler(fDevice.interrupt, emmc2_interrupt,
		this, 0);
	if (fStatus != B_OK) {
		ERROR("cannot install the handler for interrupt %" B_PRIu32 "\n",
			fDevice.interrupt);
		return;
	}
	fInterruptInstalled = true;
	_Write(REG_INTERRUPT_ENABLE, INT_WANTED);

	// Card power itself is a regulator that the firmware left on.
	_Write(REG_CONTROL0, CONTROL0_POWER_ON | CONTROL0_POWER_3V3);
	_Write(REG_CONTROL1, (_Read(REG_CONTROL1) & ~CONTROL1_TIMEOUT_MASK)
		| CONTROL1_TIMEOUT_MAX);

	fStatus = SetClock(400);
	if (fStatus != B_OK)
		return;

	INFO("registers %#" B_PRIx64 ", interrupt %" B_PRIu32 ", base clock %"
		B_PRIu32 " Hz, capabilities %#" B_PRIx32 " %#" B_PRIx32 ", %s below %#"
		B_PRIx64 " at bus offset %#" B_PRIx64 "\n", fDevice.registers,
		fDevice.interrupt, fDevice.baseClock, _Read(REG_CAPABILITIES),
		_Read(REG_CAPABILITIES + 4), fUseDMA ? "SDMA" : "no DMA; buffer",
		fDevice.dmaHighAddress, (uint64)fDevice.dmaBusOffset);
}


Emmc2Bus::~Emmc2Bus()
{
	if (fRegisters != NULL) {
		_Write(REG_INTERRUPT_ENABLE, 0);
		_Write(REG_INTERRUPT_MASK, 0);
	}
	if (fInterruptInstalled) {
		remove_io_interrupt_handler(fDevice.interrupt, emmc2_interrupt, this);
	}
	if (fBufferArea >= 0)
		delete_area(fBufferArea);
	if (fRegisterArea >= 0)
		delete_area(fRegisterArea);
}


uint32
Emmc2Bus::_Read(uint32 reg)
{
	return *(volatile uint32*)(fRegisters + reg);
}


void
Emmc2Bus::_Write(uint32 reg, uint32 value)
{
	*(volatile uint32*)(fRegisters + reg) = value;
	memory_full_barrier();

	// The next write must be more than two card clock cycles away.
	if (fClock == 0)
		spin(10);
	else if (fClock <= 400000)
		spin((4 * 1000000 + fClock - 1) / fClock);
}


status_t
Emmc2Bus::_Reset(uint32 what)
{
	_Write(REG_CONTROL1, _Read(REG_CONTROL1) | what);

	bigtime_t timeout = system_time() + 100000;
	while ((_Read(REG_CONTROL1) & what) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		snooze(100);
	}
	return B_OK;
}


/*!	After a failed command: reset the command and data lines, as the
	specification asks for, and forget what was signalled.
*/
void
Emmc2Bus::_Recover()
{
	_Reset(CONTROL1_RESET_COMMAND | CONTROL1_RESET_DATA);
	_Write(REG_INTERRUPT, 0xffffffff);
	atomic_set(&fEvents, 0);
}


status_t
Emmc2Bus::_AllocateBuffer()
{
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = fDevice.dmaHighAddress;
	physicalRestrictions.alignment = kDmaBufferSize;
		// a transfer never crosses the SDMA boundary

	fBufferArea = create_area_etc(B_SYSTEM_TEAM, "emmc2 DMA buffer",
		kDmaBufferSize, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		0, 0, &virtualRestrictions, &physicalRestrictions, &fBuffer);
	if (fBufferArea < 0)
		return fBufferArea;

	physical_entry entry;
	status_t status = get_memory_map(fBuffer, kDmaBufferSize, &entry, 1);
	if (status != B_OK)
		return status;
	if (entry.size < kDmaBufferSize
		|| entry.address + kDmaBufferSize > fDevice.dmaHighAddress) {
		return B_BAD_DATA;
	}
	fBufferAddress = entry.address;

	// The controller reads and writes the memory behind the CPU's back:
	// take what the allocation left in the cache out of it, and keep the
	// buffer out of the cache from here on.
	for (addr_t line = (addr_t)fBuffer; line < (addr_t)fBuffer + kDmaBufferSize;
			line += 64) {
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	}
	memory_full_barrier();

	return vm_set_area_memory_type(fBufferArea, fBufferAddress,
		B_WRITE_COMBINING_MEMORY);
}


/*!	Moves \a size bytes between the buffer and the card through the data
	register, a block at a time, after the command has been sent.
*/
status_t
Emmc2Bus::_TransferPolled(bool isWrite, size_t size, size_t blockSize)
{
	uint32* data = (uint32*)fBuffer;
	uint32 ready = isWrite ? STATE_WRITE_READY : STATE_READ_READY;

	for (size_t block = 0; block < size / blockSize; block++) {
		bigtime_t timeout = system_time() + 5000000;
		while ((_Read(REG_PRESENT_STATE) & ready) == 0) {
			if ((atomic_get(&fEvents) & (INT_ERROR | INT_ERROR_MASK)) != 0)
				return B_IO_ERROR;
			if (system_time() > timeout)
				return B_TIMED_OUT;
			snooze(10);
		}

		for (uint32 i = 0; i < blockSize / 4; i++) {
			if (isWrite)
				*(volatile uint32*)(fRegisters + REG_DATA) = *data++;
			else
				*data++ = *(volatile uint32*)(fRegisters + REG_DATA);
		}
		memory_full_barrier();
	}

	return B_OK;
}


status_t
Emmc2Bus::SetClock(uint32 kilohertz)
{
	if (kilohertz == 0)
		return B_BAD_VALUE;

	uint32 control = _Read(REG_CONTROL1);
	control &= ~(CONTROL1_CLOCK_CARD | CONTROL1_CLOCK_INTERNAL | 0xffc0);
	_Write(REG_CONTROL1, control);
	fClock = 0;

	// 10-bit divided clock: base / (2 * divider), or the base clock itself
	uint64 target = (uint64)kilohertz * 1000;
	uint32 divider = 0;
	if (fDevice.baseClock > target) {
		divider = (fDevice.baseClock + 2 * target - 1) / (2 * target);
		if (divider > 0x3ff)
			divider = 0x3ff;
	}

	control |= (divider & 0xff) << 8 | ((divider >> 8) & 0x3) << 6
		| CONTROL1_CLOCK_INTERNAL;
	_Write(REG_CONTROL1, control);

	bigtime_t timeout = system_time() + 100000;
	while ((_Read(REG_CONTROL1) & CONTROL1_CLOCK_STABLE) == 0) {
		if (system_time() > timeout) {
			ERROR("the clock does not become stable\n");
			return B_TIMED_OUT;
		}
		snooze(100);
	}

	_Write(REG_CONTROL1, control | CONTROL1_CLOCK_CARD);
	fClock = divider == 0 ? fDevice.baseClock : fDevice.baseClock / (2 * divider);

	uint32 control0 = _Read(REG_CONTROL0);
	if (fClock > 25000000)
		control0 |= CONTROL0_HIGH_SPEED;
	else
		control0 &= ~CONTROL0_HIGH_SPEED;
	_Write(REG_CONTROL0, control0);

	TRACE("clock: %" B_PRIu32 " kHz asked for, %" B_PRIu32 " Hz set\n",
		kilohertz, fClock);
	return B_OK;
}


status_t
Emmc2Bus::_Wait(uint32 mask, bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;

	while (true) {
		ConditionVariableEntry entry;
		fEventCondition.Add(&entry);

		uint32 events = atomic_get(&fEvents);
		if ((events & (INT_ERROR | INT_ERROR_MASK)) != 0) {
			return (events & (INT_COMMAND_TIMEOUT | INT_DATA_TIMEOUT)) != 0
				? B_TIMED_OUT : B_IO_ERROR;
		}
		if ((events & mask) != 0) {
			atomic_and(&fEvents, ~mask);
			return B_OK;
		}

		status_t status = entry.Wait(B_ABSOLUTE_TIMEOUT, deadline);
		if (status != B_OK && status != B_INTERRUPTED)
			return status;
	}
}


status_t
Emmc2Bus::_SendCommand(uint8 command, uint32 argument, uint32 flags,
	uint32* response)
{
	uint32 inhibit = STATE_COMMAND_INHIBIT;
	if ((flags & CMD_DATA) != 0
		|| (flags & CMD_RESPONSE_48_BUSY) == CMD_RESPONSE_48_BUSY) {
		inhibit |= STATE_DATA_INHIBIT;
	}

	bigtime_t timeout = system_time() + 1000000;
	while ((_Read(REG_PRESENT_STATE) & inhibit) != 0) {
		if (system_time() > timeout) {
			ERROR("command %u: the bus stays busy\n", command);
			_Recover();
			return B_BUSY;
		}
		snooze(100);
	}

	atomic_set(&fEvents, 0);
	_Write(REG_ARGUMENT, argument);
	_Write(REG_COMMAND, CMD_INDEX(command) | flags);

	status_t status = _Wait(INT_COMMAND_COMPLETE, 1000000);
	if (status != B_OK) {
		// Cards that do not know a command do not answer; the bus manager
		// probes that way, so it is not worth a line in the log.
		TRACE("command %u (%#" B_PRIx32 ") failed: %s, events %#" B_PRIx32 "\n",
			command, argument, strerror(status), (uint32)atomic_get(&fEvents));
		_Recover();
		return status;
	}

	if (response != NULL) {
		if ((flags & (3 << 16)) == CMD_RESPONSE_136) {
			for (int i = 0; i < 4; i++)
				response[i] = _Read(REG_RESPONSE + 4 * i);
		} else if ((flags & (3 << 16)) != CMD_RESPONSE_NONE)
			response[0] = _Read(REG_RESPONSE);
	}

	if ((flags & CMD_DATA) == 0
		&& (flags & (3 << 16)) == CMD_RESPONSE_48_BUSY) {
		// the card releases DAT0 when it is done
		status = _Wait(INT_TRANSFER_COMPLETE, 5000000);
		if (status != B_OK) {
			_Recover();
			return status;
		}
	}

	return B_OK;
}


status_t
Emmc2Bus::ExecuteCommand(uint8 command, uint32 argument, uint32* response)
{
	if (fStatus != B_OK)
		return fStatus;

	uint32 flags;
	switch (command) {
		case GO_IDLE_STATE:
			flags = CMD_RESPONSE_NONE;
			break;

		case SD_APP_CMD:
		case SEND_STATUS:
		case SET_BLOCK_LENGTH:
		case SD_ERASE_WR_BLK_START:
		case SD_ERASE_WR_BLK_END:
		case SD_SET_BUS_WIDTH:
			// also MMC_SWITCH, which has R1b
			flags = is_mmc_card(fCardType) ? CMD_R1B : CMD_R1;
			if (command != SD_SET_BUS_WIDTH)
				flags = CMD_R1;
			break;

		case SELECT_DESELECT_CARD:
			flags = argument == 0 ? CMD_RESPONSE_NONE : CMD_R1B;
			break;

		case SD_ERASE:
			flags = CMD_R1B;
			break;

		case ALL_SEND_CID:
		case SEND_CSD:
			flags = CMD_R2;
			break;

		case MMC_SEND_OP_COND:
		case SD_SEND_OP_COND:
			flags = CMD_R3;
			break;

		case SD_SEND_RELATIVE_ADDR:
			flags = is_mmc_card(fCardType) ? CMD_R1 : CMD_R6;
			break;

		case SD_SEND_IF_COND:
			if (is_mmc_card(fCardType))
				return B_NOT_SUPPORTED;
					// MMC_SEND_EXT_CSD: there is no eMMC behind this controller
			flags = CMD_R7;
			break;

		default:
			ERROR("unknown command %u\n", command);
			return B_BAD_DATA;
	}

	if (response == NULL && flags != CMD_RESPONSE_NONE)
		return B_BAD_VALUE;

	// An application command to a card with an address: the disk driver has
	// selected the card and is about to set its width. That is the moment
	// for the card's timing, and it comes first, as in other systems.
	if (command == SD_APP_CMD && argument != 0 && !is_mmc_card(fCardType))
		_SwitchToHighSpeed();

	return _SendCommand(command, argument, flags, response);
}


status_t
Emmc2Bus::DoIO(uint8 command, IOOperation* operation, bool offsetAsSectors)
{
	if (fStatus != B_OK)
		return fStatus;
	if (operation == NULL || operation->Offset() < 0)
		return B_BAD_VALUE;

	bool isWrite = operation->IsWrite();
	if ((isWrite && command != SD_WRITE_MULTIPLE_BLOCKS
			&& command != SD_WRITE_SINGLE_BLOCK)
		|| (!isWrite && command != SD_READ_MULTIPLE_BLOCKS
			&& command != SD_READ_SINGLE_BLOCK)) {
		return B_BAD_VALUE;
	}

	uint64 offset = operation->Offset();
	generic_size_t length = operation->Length();
	if (offset % kBlockSize != 0 || length % kBlockSize != 0)
		return B_BAD_VALUE;

	bool multiple = command == SD_READ_MULTIPLE_BLOCKS
		|| command == SD_WRITE_MULTIPLE_BLOCKS;
	if (!multiple && length != kBlockSize)
		return B_BAD_VALUE;

	const generic_io_vec* vecs = operation->Vecs();
	size_t count = operation->VecCount();
	uint64 unit = offsetAsSectors ? kBlockSize : 1;

	// The pieces of memory are gathered in the buffer, so that the card gets
	// commands as long as the buffer allows: a command per page costs most
	// of a card's speed, when writing even more than when reading.
	size_t vecIndex = 0;
	generic_size_t vecOffset = 0;

	while (length != 0) {
		size_t size = std::min(length, (generic_size_t)kDmaBufferSize);
		if (!multiple)
			size = kBlockSize;

		if (isWrite) {
			status_t status = _CopyVecs(true, vecs, count, vecIndex,
				vecOffset, size);
			if (status != B_OK)
				return status;
			memory_full_barrier();
		}

		// SDMA, and no interrupt at the buffer boundary: the buffer is
		// aligned to it and never larger.
		_Write(REG_CONTROL0, _Read(REG_CONTROL0) & ~CONTROL0_DMA_MASK);
		if (fUseDMA) {
			_Write(REG_SDMA_ADDRESS,
				(uint32)(fBufferAddress + fDevice.dmaBusOffset));
		}
		_Write(REG_BLOCK, kBlockSize | BLOCK_SDMA_BOUNDARY_512K
			| (uint32)(size / kBlockSize) << 16);

		uint32 flags = CMD_R1 | CMD_DATA;
		if (fUseDMA)
			flags |= TM_DMA_ENABLE;
		if (!isWrite)
			flags |= TM_READ;
		if (multiple)
			flags |= TM_MULTI_BLOCK | TM_BLOCK_COUNT | TM_AUTO_CMD12;

		uint32 response = 0;
		status_t status = _SendCommand(command, offset / unit, flags,
			&response);
		if (status == B_OK && (response & kMmcR1ErrorMask) != 0)
			status = B_IO_ERROR;
		if (status == B_OK && !fUseDMA)
			status = _TransferPolled(isWrite, size);
		if (status == B_OK)
			status = _Wait(INT_TRANSFER_COMPLETE, 5000000);
		if (status != B_OK) {
			ERROR("%s of %" B_PRIuSIZE " bytes at %" B_PRIu64 " failed: "
				"%s, response %#" B_PRIx32 ", events %#" B_PRIx32 "\n",
				isWrite ? "write" : "read", size, offset, strerror(status),
				response, (uint32)atomic_get(&fEvents));
			_Recover();
			return status;
		}

		if (!isWrite) {
			memory_full_barrier();
			status = _CopyVecs(false, vecs, count, vecIndex, vecOffset,
				size);
			if (status != B_OK)
				return status;
		}

		offset += size;
		length -= size;
	}

	return B_OK;
}


/*!	Copies the next \a size bytes of the operation's memory to the buffer
	or back, and moves \a index and \a vecOffset past them.
*/
status_t
Emmc2Bus::_CopyVecs(bool toBuffer, const generic_io_vec* vecs, size_t count,
	size_t& index, generic_size_t& vecOffset, size_t size)
{
	uint8* buffer = (uint8*)fBuffer;

	while (size != 0) {
		if (index >= count)
			return B_BAD_VALUE;
		if (vecOffset == vecs[index].length) {
			index++;
			vecOffset = 0;
			continue;
		}

		size_t toCopy = std::min((generic_size_t)size,
			vecs[index].length - vecOffset);
		status_t status;
		if (toBuffer) {
			status = vm_memcpy_from_physical(buffer,
				vecs[index].base + vecOffset, toCopy, false);
		} else {
			status = vm_memcpy_to_physical(vecs[index].base + vecOffset,
				buffer, toCopy, false);
		}
		if (status != B_OK)
			return status;

		buffer += toCopy;
		vecOffset += toCopy;
		size -= toCopy;
	}

	return B_OK;
}


/*!	Sends a command that the card answers with \a size bytes of data, a
	single block, and leaves them in the buffer.
*/
status_t
Emmc2Bus::_ReadData(uint8 command, uint32 argument, size_t size)
{
	_Write(REG_CONTROL0, _Read(REG_CONTROL0) & ~CONTROL0_DMA_MASK);
	// Writing SDMA_ADDRESS can resume a suspended DMA transfer. Leave it
	// alone for PIO, especially after a short single-block command (CMD6)
	// whose block-count register need not have reached zero.
	if (fUseDMA)
		_Write(REG_SDMA_ADDRESS, (uint32)(fBufferAddress + fDevice.dmaBusOffset));
	_Write(REG_BLOCK, (uint32)size | BLOCK_SDMA_BOUNDARY_512K | 1 << 16);

	uint32 flags = CMD_R1 | CMD_DATA | TM_READ;
	if (fUseDMA)
		flags |= TM_DMA_ENABLE;

	uint32 response = 0;
	status_t status = _SendCommand(command, argument, flags, &response);
	if (status == B_OK && (response & kMmcR1ErrorMask) != 0)
		status = B_IO_ERROR;
	if (status == B_OK && !fUseDMA)
		status = _TransferPolled(false, size, size);
	if (status == B_OK)
		status = _Wait(INT_TRANSFER_COMPLETE, 1000000);
	if (status != B_OK) {
		_Recover();
		return status;
	}

	memory_full_barrier();
	return B_OK;
}


/*!	Has an SD card that knows the high speed timing use it, and clocks it
	with 50 MHz then (SD physical layer specification, 4.3.10: CMD6). The
	card is selected and in the transfer state. A card that does not answer,
	or does not switch, stays at 25 MHz.
*/
void
Emmc2Bus::_SwitchToHighSpeed()
{
	if (fClock > 25000000 || fDevice.baseClock < 50000000)
		return;

	// Group 1 (access mode) to function 1 (high speed), the other groups as
	// they are; asked first what would become of it, as other systems do.
	status_t status = _ReadData(SD_SWITCH_FUNCTION, 0x00fffff1, 64);
	const uint8* switchStatus = (const uint8*)fBuffer;
	if (status != B_OK || (switchStatus[13] & 0x02) == 0
		|| (switchStatus[16] & 0x0f) != 1) {
		INFO("no high speed timing on this card: 25 MHz\n");
		return;
	}

	status = _ReadData(SD_SWITCH_FUNCTION, 0x80fffff1, 64);
	if (status != B_OK) {
		INFO("the card does not answer the switch command (%s): 25 MHz\n",
			strerror(status));
		return;
	}

	// The lab's card answers the switch with function 0 and has switched
	// all the same: what counts is the function it names as its current
	// one when asked without a change.
	uint8 answered = switchStatus[16] & 0x0f;
	snooze(1000);
	status = _ReadData(SD_SWITCH_FUNCTION, 0x00ffffff, 64);
	if (status != B_OK || (switchStatus[16] & 0x0f) != 1) {
		INFO("the card stays at default speed (answered %u, now %u): "
			"25 MHz\n", answered, switchStatus[16] & 0x0f);
		return;
	}

	// the card changes its timing within eight clock cycles
	snooze(1000);
	if (SetClock(50000) == B_OK)
		INFO("high speed: %" B_PRIu32 " Hz\n", fClock);
}


void
Emmc2Bus::SetScanSemaphore(sem_id semaphore)
{
	// There is no card detection to wait for: have the bus look for a card
	// right away.
	if (semaphore >= 0)
		release_sem(semaphore);
}


void
Emmc2Bus::SetBusWidth(int width)
{
	uint32 control = _Read(REG_CONTROL0);
	if (width == 4)
		control |= CONTROL0_4BIT;
	else
		control &= ~CONTROL0_4BIT;
	_Write(REG_CONTROL0, control);
}


void
Emmc2Bus::Terminate()
{
	_Write(REG_INTERRUPT_ENABLE, 0);
	_Write(REG_CONTROL1, _Read(REG_CONTROL1) & ~CONTROL1_CLOCK_CARD);
}


int32
Emmc2Bus::HandleInterrupt()
{
	uint32 events = _Read(REG_INTERRUPT);
	if (events == 0 || events == 0xffffffff)
		return B_UNHANDLED_INTERRUPT;

	// acknowledge
	*(volatile uint32*)(fRegisters + REG_INTERRUPT) = events;
	memory_full_barrier();

	if ((events & INT_DMA) != 0) {
		// stopped at the buffer boundary: writing the address resumes
		*(volatile uint32*)(fRegisters + REG_SDMA_ADDRESS)
			= *(volatile uint32*)(fRegisters + REG_SDMA_ADDRESS);
		memory_full_barrier();
	}

	events &= INT_COMMAND_COMPLETE | INT_TRANSFER_COMPLETE | INT_ERROR
		| INT_ERROR_MASK;
	if (events != 0) {
		atomic_or(&fEvents, events);
		fEventCondition.NotifyAll();
	}

	return B_HANDLED_INTERRUPT;
}


//	#pragma mark - driver


/*!	Returns a property of the FDT node \a level steps (at least one) above
	\a node.
*/
static const void*
get_ancestor_property(device_node* node, int level, const char* name,
	int* _length)
{
	if (level < 1)
		return NULL;

	node = gDeviceManager->get_parent_node(node);
	for (int i = 1; i < level && node != NULL; i++) {
		device_node* parent = gDeviceManager->get_parent_node(node);
		gDeviceManager->put_node(node);
		node = parent;
	}
	if (node == NULL)
		return NULL;

	const void* property = NULL;
	const char* bus;
	fdt_device_module_info* module;
	fdt_device* device;
	if (gDeviceManager->get_attr_string(node, B_DEVICE_BUS, &bus, false) == B_OK
		&& strcmp(bus, "fdt") == 0
		&& gDeviceManager->get_driver(node, (driver_module_info**)&module,
			(void**)&device) == B_OK) {
		property = module->get_prop(device, name, _length);
	}
	gDeviceManager->put_node(node);
	return property;
}


static uint32
get_ancestor_cells(device_node* node, int level, const char* name,
	uint32 defaultValue)
{
	int length;
	const uint32* property = (const uint32*)get_ancestor_property(node, level,
		name, &length);
	if (property == NULL || length != 4)
		return defaultValue;
	return B_BENDIAN_TO_HOST_INT32(*property);
}


static uint64
read_cells(const uint32*& cells, uint32 count)
{
	uint64 value = 0;
	for (uint32 i = 0; i < count; i++)
		value = (value << 32) | B_BENDIAN_TO_HOST_INT32(*cells++);
	return value;
}


static float
emmc2_supports_device(device_node* parent)
{
	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK
		|| strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	const char* compatible;
	if (gDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK) {
		return 0.0f;
	}

	if (strcmp(compatible, "brcm,bcm2711-emmc2") == 0)
		return 1.0f;

	// The Pi 5's (BCM2712) SD card slot. Its other controller of the kind
	// is "non-removable": the Wi-Fi chip's SDIO.
	if (strcmp(compatible, "brcm,bcm2712-sdhci") == 0) {
		fdt_device_module_info* fdt;
		fdt_device* fdtDevice;
		if (gDeviceManager->get_driver(parent, (driver_module_info**)&fdt,
				(void**)&fdtDevice) != B_OK
			|| fdt->get_prop(fdtDevice, "non-removable", NULL) != NULL) {
			return 0.0f;
		}
		return 1.0f;
	}

	return 0.0f;
}


static status_t
emmc2_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{.string = "Raspberry Pi SD controller"}},
		{}
	};

	return gDeviceManager->register_node(parent, EMMC2_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
emmc2_init_driver(device_node* node, void** _cookie)
{
	device_node* parent = gDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* fdtDevice;
	status_t status = gDeviceManager->get_driver(parent,
		(driver_module_info**)&fdt, (void**)&fdtDevice);
	if (status != B_OK) {
		gDeviceManager->put_node(parent);
		return status;
	}

	emmc2_device* device = new(std::nothrow) emmc2_device;
	if (device == NULL) {
		gDeviceManager->put_node(parent);
		return B_NO_MEMORY;
	}
	device->node = node;

	uint64 interrupt = 0;
	if (!fdt->get_reg(fdtDevice, 0, &device->registers, &device->registersSize)
		|| !fdt->get_interrupt(fdtDevice, 0, NULL, &interrupt)) {
		ERROR("no registers or no interrupt in the device tree\n");
		gDeviceManager->put_node(parent);
		delete device;
		return B_BAD_DATA;
	}
	device->interrupt = interrupt;

	int length;
	const uint32* property = (const uint32*)fdt->get_prop(fdtDevice,
		"clock-frequency", &length);
	device->baseClock = property != NULL && length == 4
		? B_BENDIAN_TO_HOST_INT32(*property) : DEFAULT_BASE_CLOCK;

	property = (const uint32*)fdt->get_prop(fdtDevice, "bus-width", &length);
	device->busWidth = property != NULL && length == 4
		? B_BENDIAN_TO_HOST_INT32(*property) : 4;

	// The bus' dma-ranges say which memory the controller reaches, and at
	// which bus address: <bus address, CPU address, size>. Without them,
	// stay in the first gigabyte, which every board revision can reach.
	device->dmaHighAddress = 1ull << 30;
	device->dmaBusOffset = 0;
	property = (const uint32*)get_ancestor_property(parent, 1, "dma-ranges",
		&length);
	uint32 busCells = get_ancestor_cells(parent, 1, "#address-cells", 2);
	uint32 sizeCells = get_ancestor_cells(parent, 1, "#size-cells", 1);
	uint32 cpuCells = get_ancestor_cells(parent, 2, "#address-cells", 2);
	if (property != NULL && busCells <= 2 && sizeCells <= 2 && cpuCells <= 2
		&& (uint32)length >= (busCells + cpuCells + sizeCells) * 4) {
		uint64 busBase = read_cells(property, busCells);
		uint64 cpuBase = read_cells(property, cpuCells);
		uint64 size = read_cells(property, sizeCells);
		if (cpuBase == 0 && size >= kDmaBufferSize) {
			device->dmaHighAddress = std::min(size, (uint64)1 << 32);
			device->dmaBusOffset = (int64)busBase;
		}
	}

	gDeviceManager->put_node(parent);

	*_cookie = device;
	return B_OK;
}


static void
emmc2_uninit_driver(void* cookie)
{
	delete (emmc2_device*)cookie;
}


static status_t
emmc2_register_child_devices(void* cookie)
{
	emmc2_device* device = (emmc2_device*)cookie;

	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "SD card slot"}},
		{B_DEVICE_BUS, B_STRING_TYPE, {.string = "mmc"}},
		{B_DEVICE_FIXED_CHILD, B_STRING_TYPE, {.string = MMC_BUS_MODULE_NAME}},
		{kMmcReadOnlyAttribute, B_UINT8_TYPE, {.ui8 = 0}},
		{kMmcNonRemovableAttribute, B_UINT8_TYPE, {.ui8 = 0}},
		{kMmcMaxBusWidthAttribute, B_UINT8_TYPE,
			{.ui8 = (uint8)device->busWidth}},
		{kMmcEnableCacheAttribute, B_UINT8_TYPE, {.ui8 = 0}},
		// whole blocks in pieces of memory that are copied through the
		// driver's own buffer, at most the buffer's size per transfer
		{B_DMA_ALIGNMENT, B_UINT32_TYPE, {.ui32 = kBlockSize - 1}},
		{B_DMA_MAX_SEGMENT_COUNT, B_UINT32_TYPE,
			{.ui32 = (uint32)(kDmaBufferSize / B_PAGE_SIZE)}},
		{B_DMA_MAX_SEGMENT_BLOCKS, B_UINT32_TYPE,
			{.ui32 = (uint32)(kDmaBufferSize / kBlockSize)}},
		{B_DMA_MAX_TRANSFER_BLOCKS, B_UINT32_TYPE,
			{.ui32 = (uint32)(kDmaBufferSize / kBlockSize)}},
		{}
	};

	return gDeviceManager->register_node(device->node, EMMC2_BUS_MODULE_NAME,
		attrs, NULL, NULL);
}


//	#pragma mark - bus


static status_t
emmc2_init_bus(device_node* node, void** _cookie)
{
	device_node* parent = gDeviceManager->get_parent_node(node);
	emmc2_device* device;
	status_t status = gDeviceManager->get_driver(parent, NULL,
		(void**)&device);
	gDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	Emmc2Bus* bus = new(std::nothrow) Emmc2Bus(*device);
	if (bus == NULL)
		return B_NO_MEMORY;

	status = bus->InitCheck();
	if (status != B_OK) {
		delete bus;
		return status;
	}

	*_cookie = bus;
	return B_OK;
}


static void
emmc2_uninit_bus(void* cookie)
{
	delete (Emmc2Bus*)cookie;
}


static status_t
emmc2_set_clock(void* cookie, uint32_t kilohertz)
{
	return ((Emmc2Bus*)cookie)->SetClock(kilohertz);
}


static status_t
emmc2_execute_command(void* cookie, uint8_t command, uint32_t argument,
	uint32_t* response)
{
	return ((Emmc2Bus*)cookie)->ExecuteCommand(command, argument, response);
}


static status_t
emmc2_do_io(void* cookie, uint8_t command, IOOperation* operation,
	bool offsetAsSectors)
{
	return ((Emmc2Bus*)cookie)->DoIO(command, operation, offsetAsSectors);
}


static void
emmc2_set_scan_semaphore(void* cookie, sem_id semaphore)
{
	((Emmc2Bus*)cookie)->SetScanSemaphore(semaphore);
}


static void
emmc2_set_bus_width(void* cookie, int width)
{
	((Emmc2Bus*)cookie)->SetBusWidth(width);
}


static void
emmc2_terminate_bus(void* cookie)
{
	((Emmc2Bus*)cookie)->Terminate();
}


static void
emmc2_set_card_type(void* cookie, card_type type)
{
	((Emmc2Bus*)cookie)->SetCardType(type);
}


static status_t
emmc2_read_extended_csd(void* cookie, uint8_t data[512])
{
	return B_NOT_SUPPORTED;
}


static driver_module_info sEmmc2Driver = {
	{
		EMMC2_DRIVER_MODULE_NAME,
		0,
		NULL
	},
	emmc2_supports_device,
	emmc2_register_device,
	emmc2_init_driver,
	emmc2_uninit_driver,
	emmc2_register_child_devices,
	NULL,	// rescan
	NULL,	// device removed
};

static mmc_bus_interface sEmmc2Bus = {
	.info = {
		.info = {
			.name = EMMC2_BUS_MODULE_NAME,
		},
		.init_driver = emmc2_init_bus,
		.uninit_driver = emmc2_uninit_bus,
	},
	.set_clock = emmc2_set_clock,
	.execute_command = emmc2_execute_command,
	.do_io = emmc2_do_io,
	.set_scan_semaphore = emmc2_set_scan_semaphore,
	.set_bus_width = emmc2_set_bus_width,
	.terminate_bus = emmc2_terminate_bus,
	.set_card_type = emmc2_set_card_type,
	.read_extended_csd = emmc2_read_extended_csd
};

module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager},
	{}
};

module_info* modules[] = {
	(module_info*)&sEmmc2Driver,
	(module_info*)&sEmmc2Bus,
	NULL
};
