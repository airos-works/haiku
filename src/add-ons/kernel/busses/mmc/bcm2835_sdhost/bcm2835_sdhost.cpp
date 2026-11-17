/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The SD card slot of the BCM2835/BCM2837 (Raspberry Pi 3): the "sdhost"
	controller, Broadcom's own block next to the Arasan SDHCI (which serves
	the Pi 3's Wi-Fi chip). Linux: drivers/mmc/host/bcm2835.c, the reference
	for everything below.

	It is not an SD host controller of the standard kind: a command register
	with a "new" flag that the block clears when the answer is in, a status
	register, a 16-word FIFO behind the data register and a debug register
	(SDEDM) that tells the state of its data state machine and how full the
	FIFO is. There is no automatic CMD12: open-ended multi-block transfers
	are ended with a CMD12 of our own.

	Data goes by PIO, polled: the CPU moves the words through the FIFO as
	they come, in bursts of up to eight (Linux' SDDATA_FIFO_PIO_BURST), and
	waits for the block interrupt status after each block it writes. Long
	waits (the card busy after a write, the first data of a read) poll with
	short sleeps. The block's interrupt is not used: its line follows the
	status bits that the polling code clears itself.

	Quirks taken from Linux:
	- the FIFO thresholds in SDEDM limited to 4 words (a silicon bug);
	- SLOW_CARD always set, so that the clock divider of the identification
	  mode applies in the data mode too (its three bits are too few for a
	  core clock above 250 MHz);
	- the state machine left waiting for more data (READWAIT, WRITESTART1)
	  is kicked back into the data mode with FORCE_DATA_MODE;
	- CRC7 errors of CMD1's answer (R3 has no CRC) are not errors.

	The core clock is the sdhost's input clock; the loader puts the
	firmware's rate into the node's "clock-frequency", and sets the pins
	(GPIO 48 to 53, ALT0) as the tree says, which the firmware has done
	already on the boards but QEMU's raspi3b has not. */


#include <algorithm>
#include <new>
#include <stdio.h>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>

#include <vm/vm.h>

#include "IOSchedulerSimple.h"
#include "mmc.h"


//#define TRACE_SDHOST
#ifdef TRACE_SDHOST
#	define TRACE(x...) dprintf("sdhost: " x)
#else
#	define TRACE(x...) ;
#endif
#define INFO(x...)	dprintf("sdhost: " x)
#define ERROR(x...)	dprintf("sdhost: " x)


#define SDHOST_DRIVER_MODULE_NAME	"busses/mmc/bcm2835_sdhost/driver_v1"
#define SDHOST_BUS_MODULE_NAME		"busses/mmc/bcm2835_sdhost/device/v1"


// registers
#define SDCMD		0x00	// command to the card
#define SDARG		0x04	// its argument
#define SDTOUT		0x08	// timeout, in card clock cycles
#define SDCDIV		0x0c	// clock divider: core clock / (SDCDIV + 2)
#define SDRSP0		0x10	// the answer, bits 31:0 (of 127:0)
#define SDHSTS		0x20	// status, write 1s to clear
#define SDVDD		0x30	// power
#define SDEDM		0x34	// emergency debug mode: state machine, FIFO
#define SDHCFG		0x38	// configuration
#define SDHBCT		0x3c	// block size
#define SDDATA		0x40	// the FIFO
#define SDHBLC		0x50	// block count

#define SDCMD_NEW_FLAG			0x8000
#define SDCMD_FAIL_FLAG			0x4000
#define SDCMD_BUSYWAIT			0x0800
#define SDCMD_NO_RESPONSE		0x0400
#define SDCMD_LONG_RESPONSE		0x0200
#define SDCMD_WRITE_CMD			0x0080
#define SDCMD_READ_CMD			0x0040
#define SDCMD_CMD_MASK			0x003f

#define SDCDIV_MAX_CDIV			0x7ff

#define SDHSTS_BUSY_IRPT		0x400
#define SDHSTS_BLOCK_IRPT		0x200
#define SDHSTS_SDIO_IRPT		0x100
#define SDHSTS_REW_TIME_OUT		0x080
#define SDHSTS_CMD_TIME_OUT		0x040
#define SDHSTS_CRC16_ERROR		0x020
#define SDHSTS_CRC7_ERROR		0x010
#define SDHSTS_FIFO_ERROR		0x008
#define SDHSTS_DATA_FLAG		0x001

#define SDHSTS_TRANSFER_ERROR_MASK	(SDHSTS_CRC7_ERROR | SDHSTS_CRC16_ERROR \
	| SDHSTS_REW_TIME_OUT | SDHSTS_FIFO_ERROR)
#define SDHSTS_ERROR_MASK		(SDHSTS_CMD_TIME_OUT \
	| SDHSTS_TRANSFER_ERROR_MASK)
#define SDHSTS_CLEAR_ALL		(SDHSTS_BUSY_IRPT | SDHSTS_BLOCK_IRPT \
	| SDHSTS_SDIO_IRPT | SDHSTS_ERROR_MASK | SDHSTS_DATA_FLAG)

#define SDHCFG_BUSY_IRPT_EN		(1 << 10)
#define SDHCFG_BLOCK_IRPT_EN	(1 << 8)
#define SDHCFG_SDIO_IRPT_EN		(1 << 5)
#define SDHCFG_DATA_IRPT_EN		(1 << 4)
#define SDHCFG_SLOW_CARD		(1 << 3)
#define SDHCFG_WIDE_EXT_BUS		(1 << 2)
#define SDHCFG_WIDE_INT_BUS		(1 << 1)
#define SDHCFG_REL_CMD_LINE		(1 << 0)

#define SDVDD_POWER_OFF			0
#define SDVDD_POWER_ON			1

#define SDEDM_FORCE_DATA_MODE	(1 << 19)
#define SDEDM_WRITE_THRESHOLD_SHIFT	9
#define SDEDM_READ_THRESHOLD_SHIFT	14
#define SDEDM_THRESHOLD_MASK	0x1f
#define SDEDM_FIFO_FILL(edm)	(((edm) >> 4) & 0x1f)

#define SDEDM_FSM_MASK			0xf
#define SDEDM_FSM_IDENTMODE		0x0
#define SDEDM_FSM_DATAMODE		0x1
#define SDEDM_FSM_READDATA		0x2
#define SDEDM_FSM_WRITEDATA		0x3
#define SDEDM_FSM_READWAIT		0x4
#define SDEDM_FSM_READCRC		0x5
#define SDEDM_FSM_WRITECRC		0x6
#define SDEDM_FSM_WRITEWAIT1	0x7
#define SDEDM_FSM_POWERDOWN		0x8
#define SDEDM_FSM_POWERUP		0x9
#define SDEDM_FSM_WRITESTART1	0xa
#define SDEDM_FSM_WRITESTART2	0xb
#define SDEDM_FSM_GENPULSES		0xc
#define SDEDM_FSM_WRITEWAIT2	0xd
#define SDEDM_FSM_STARTPOWDOWN	0xf

#define SDDATA_FIFO_WORDS		16
#define FIFO_READ_THRESHOLD		4
#define FIFO_WRITE_THRESHOLD	4
#define SDDATA_FIFO_PIO_BURST	8

// how a command is answered
enum {
	RESPONSE_NONE	= SDCMD_NO_RESPONSE,
	RESPONSE_48		= 0,
	RESPONSE_48_BUSY = SDCMD_BUSYWAIT,
	RESPONSE_136	= SDCMD_LONG_RESPONSE,
};

// the bits of R1 that a CMD12 after a write reports the write's failure
// with: write protection, ECC, controller and generic errors
static const uint32 kWriteStopErrors = (1u << 26) | (1u << 21) | (1u << 20)
	| (1u << 19);

// what the firmware clocks the controller with, unless the tree says
#define DEFAULT_BASE_CLOCK		250000000

// Each transfer takes at most this many blocks: the block count register
// has nine bits.
static const uint32 kBlockSize = 512;
static const size_t kBufferSize = 128 * 1024;
static const uint32 kMaxBlocks = kBufferSize / kBlockSize;


device_manager_info* gDeviceManager;


struct sdhost_device {
	device_node*	node;
	uint64			registers;
	uint64			registersSize;
	uint32			baseClock;		// Hz
	uint32			busWidth;
};


class SdhostBus {
public:
								SdhostBus(const sdhost_device& device);
								~SdhostBus();

			status_t			InitCheck() const { return fStatus; }

			status_t			SetClock(uint32 kilohertz);
			status_t			ExecuteCommand(uint8 command, uint32 argument,
									uint32* response);
			status_t			DoIO(uint8 command, IOOperation* operation,
									bool offsetAsSectors);
			status_t			ReadExtendedCsd(uint8 data[512]);
			status_t			SwitchSdFunction(uint32 argument,
									uint8 data[64]);
			void				SetScanSemaphore(sem_id semaphore);
			void				SetBusWidth(int width);
			void				SetCardType(card_type type)
									{ fCardType = type; }
			void				Terminate();

private:
			uint32				_Read(uint32 reg)
									{ return *(volatile uint32*)
										(fRegisters + reg); }
			void				_Write(uint32 reg, uint32 value)
									{ *(volatile uint32*)(fRegisters + reg)
										= value; }

			void				_ResetController();
			void				_KickStateMachine();
			status_t			_WaitIdle(bool checkState);
			status_t			_SendCommand(uint8 command, uint32 argument,
									uint32 flags, uint32* response,
									uint32 blockSize = 0, uint32 blocks = 0,
									bool checkState = true);
			void				_DrainFifo();
			status_t			_WaitBusy(uint8 command);
			status_t			_ReadFifo(uint32* buffer, size_t words);
			status_t			_WriteFifo(const uint32* buffer,
									size_t words);
			status_t			_WaitBlockDone();
			status_t			_WaitTransferComplete(bool isWrite);
			status_t			_StopTransmission(bool isWrite);
			status_t			_Transfer(uint8 command, uint32 argument,
									bool isWrite, uint32 blockSize,
									uint32 blocks, bool stop);
			status_t			_CopyVecs(bool toBuffer,
									const generic_io_vec* vecs, size_t count,
									size_t& index, generic_size_t& vecOffset,
									size_t size);
			status_t			_DataError(uint32 status);

private:
			sdhost_device		fDevice;
			area_id				fRegisterArea;
			volatile uint8*		fRegisters;
			status_t			fStatus;
			uint32				fConfig;		// SDHCFG
			uint32				fDivider;		// SDCDIV
			uint32				fClock;			// Hz
			card_type			fCardType;
			area_id				fBufferArea;
			uint32*				fBuffer;
};


SdhostBus::SdhostBus(const sdhost_device& device)
	:
	fDevice(device),
	fRegisterArea(-1),
	fRegisters(NULL),
	fStatus(B_NO_INIT),
	fConfig(SDHCFG_BUSY_IRPT_EN | SDHCFG_WIDE_INT_BUS | SDHCFG_SLOW_CARD),
	fDivider(SDCDIV_MAX_CDIV),
	fClock(0),
	fCardType(CARD_TYPE_UNKNOWN),
	fBufferArea(-1),
	fBuffer(NULL)
{
	fRegisterArea = map_physical_memory("sdhost registers", fDevice.registers,
		std::max(fDevice.registersSize, (uint64)B_PAGE_SIZE),
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&fRegisters);
	if (fRegisterArea < 0) {
		fStatus = fRegisterArea;
		return;
	}

	// whole blocks go through it, gathered from and spread to the
	// operation's memory
	fBufferArea = create_area("sdhost buffer", (void**)&fBuffer,
		B_ANY_KERNEL_ADDRESS, kBufferSize, B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (fBufferArea < 0) {
		fStatus = fBufferArea;
		return;
	}

	_ResetController();

	fStatus = SetClock(400);
	if (fStatus != B_OK)
		return;

	INFO("registers %#" B_PRIx64 ", core clock %" B_PRIu32 " Hz, %" B_PRIu32
		"-bit bus, PIO\n", fDevice.registers, fDevice.baseClock,
		fDevice.busWidth);
}


SdhostBus::~SdhostBus()
{
	if (fRegisters != NULL) {
		_Write(SDHCFG, 0);
		_Write(SDVDD, SDVDD_POWER_OFF);
	}
	if (fBufferArea >= 0)
		delete_area(fBufferArea);
	if (fRegisterArea >= 0)
		delete_area(fRegisterArea);
}


/*!	Linux' bcm2835_reset_internal(): every register to its initial value,
	the FIFO thresholds limited, and the power off and on again.
*/
void
SdhostBus::_ResetController()
{
	_Write(SDVDD, SDVDD_POWER_OFF);
	_Write(SDCMD, 0);
	_Write(SDARG, 0);
	_Write(SDTOUT, 0xf00000);
	_Write(SDCDIV, 0);
	_Write(SDHSTS, SDHSTS_CLEAR_ALL);
	_Write(SDHCFG, 0);
	_Write(SDHBCT, 0);
	_Write(SDHBLC, 0);

	// Limit the FIFO's use, a silicon bug. Writing the state back as it
	// was takes the state machine out of its power down state.
	uint32 edm = _Read(SDEDM);
	edm &= ~((SDEDM_THRESHOLD_MASK << SDEDM_READ_THRESHOLD_SHIFT)
		| (SDEDM_THRESHOLD_MASK << SDEDM_WRITE_THRESHOLD_SHIFT));
	edm |= (FIFO_READ_THRESHOLD << SDEDM_READ_THRESHOLD_SHIFT)
		| (FIFO_WRITE_THRESHOLD << SDEDM_WRITE_THRESHOLD_SHIFT);
	_Write(SDEDM, edm);
	snooze(20000);
	_Write(SDVDD, SDVDD_POWER_ON);
	snooze(20000);

	_Write(SDHCFG, fConfig);
	_Write(SDCDIV, fDivider);
	if (fClock != 0)
		_Write(SDTOUT, fClock / 2);
}


/*!	A state machine left waiting for more data, after an open-ended
	transfer or a failed command, goes back to the data mode.
*/
void
SdhostBus::_KickStateMachine()
{
	uint32 edm = _Read(SDEDM);
	uint32 state = edm & SDEDM_FSM_MASK;
	if (state == SDEDM_FSM_READWAIT || state == SDEDM_FSM_WRITESTART1)
		_Write(SDEDM, edm | SDEDM_FORCE_DATA_MODE);
}


/*!	Waits until the previous command is through and, with \a checkState,
	the state machine is back in the identification or the data mode. If it
	does not get there, the controller is reset. A CMD12 goes out while the
	state machine is still in its data transfer.
*/
status_t
SdhostBus::_WaitIdle(bool checkState)
{
	bigtime_t start = system_time();
	while (true) {
		uint32 state = _Read(SDEDM) & SDEDM_FSM_MASK;
		if ((_Read(SDCMD) & SDCMD_NEW_FLAG) == 0
			&& (!checkState || state == SDEDM_FSM_IDENTMODE
				|| state == SDEDM_FSM_DATAMODE))
			return B_OK;

		bigtime_t elapsed = system_time() - start;
		if (elapsed > 50000)
			_KickStateMachine();
		if (elapsed > 500000) {
			ERROR("the controller stays busy (command %#" B_PRIx32 ", EDM %#"
				B_PRIx32 "): reset\n", _Read(SDCMD), _Read(SDEDM));
			_ResetController();
			return B_OK;
		}
		if (elapsed > 50)
			snooze(50);
	}
}


/*!	Sends a command, and with \a blocks of \a blockSize bytes a data phase
	(SDCMD_READ_CMD or SDCMD_WRITE_CMD in \a flags), and waits for the
	answer: until the block clears the new flag. Not for the end of the
	data phase or of the card's busy signal.
*/
status_t
SdhostBus::_SendCommand(uint8 command, uint32 argument, uint32 flags,
	uint32* response, uint32 blockSize, uint32 blocks, bool checkState)
{
	status_t status = _WaitIdle(checkState);
	if (status != B_OK)
		return status;

	_Write(SDHSTS, SDHSTS_CLEAR_ALL);

	uint32 config = fConfig;
	if ((flags & SDCMD_WRITE_CMD) != 0)
		config |= SDHCFG_BLOCK_IRPT_EN;
	_Write(SDHCFG, config);
	if ((flags & (SDCMD_READ_CMD | SDCMD_WRITE_CMD)) != 0) {
		_Write(SDHBCT, blockSize);
		_Write(SDHBLC, blocks);
	}

	_Write(SDARG, argument);
	_Write(SDCMD, (command & SDCMD_CMD_MASK) | flags | SDCMD_NEW_FLAG);

	// The answer takes 48 or 136 clock cycles and some: up to half a
	// millisecond at 400 kHz, microseconds after that.
	bigtime_t start = system_time();
	uint32 value;
	while (((value = _Read(SDCMD)) & SDCMD_NEW_FLAG) != 0) {
		bigtime_t elapsed = system_time() - start;
		if (elapsed > 100000) {
			ERROR("command %u (%#" B_PRIx32 ") never completed: status %#"
				B_PRIx32 ", EDM %#" B_PRIx32 "\n", command, argument,
				_Read(SDHSTS), _Read(SDEDM));
			_ResetController();
			return B_TIMED_OUT;
		}
		if (elapsed > 20)
			snooze(elapsed > 1000 ? 100 : 10);
	}

	if ((value & SDCMD_FAIL_FLAG) != 0) {
		uint32 hostStatus = _Read(SDHSTS);
		_Write(SDHSTS, SDHSTS_ERROR_MASK);

		// R3 (CMD1, ACMD41) carries no CRC: an error of it means nothing
		bool ignore = (hostStatus & SDHSTS_ERROR_MASK) == SDHSTS_CRC7_ERROR
			&& (command == MMC_SEND_OP_COND || command == SD_SEND_OP_COND);
		if (!ignore) {
			_KickStateMachine();
			// Cards that do not know a command do not answer; the bus
			// manager probes that way, so it is not worth a line.
			TRACE("command %u (%#" B_PRIx32 ") failed: status %#" B_PRIx32
				"\n", command, argument, hostStatus);
			return (hostStatus & SDHSTS_CMD_TIME_OUT) != 0
				? B_TIMED_OUT : B_IO_ERROR;
		}
	}

	if (response != NULL) {
		if ((flags & SDCMD_LONG_RESPONSE) != 0) {
			// The block has all 128 bits, CRC and end bit in bits 7:0. The
			// bus manager wants them as SDHCI has them: bits 127:8, in
			// four words from the lowest on.
			uint32 raw[4];
			for (int i = 0; i < 4; i++)
				raw[i] = _Read(SDRSP0 + 4 * i);
			for (int i = 0; i < 4; i++)
				response[i] = raw[i] >> 8 | (i < 3 ? raw[i + 1] << 24 : 0);
		} else if ((flags & SDCMD_NO_RESPONSE) == 0)
			response[0] = _Read(SDRSP0);
	}

	return B_OK;
}


/*!	After a command with SDCMD_BUSYWAIT: waits until the card lets go of
	DAT0 (the busy interrupt status).
*/
status_t
SdhostBus::_WaitBusy(uint8 command)
{
	bigtime_t start = system_time();
	while (true) {
		uint32 status = _Read(SDHSTS);
		if ((status & SDHSTS_ERROR_MASK) != 0) {
			_Write(SDHSTS, SDHSTS_ERROR_MASK | SDHSTS_BUSY_IRPT);
			ERROR("command %u: error %#" B_PRIx32 " while the card is busy\n",
				command, status);
			_KickStateMachine();
			return (status & (SDHSTS_CMD_TIME_OUT | SDHSTS_REW_TIME_OUT)) != 0
				? B_TIMED_OUT : B_IO_ERROR;
		}
		if ((status & SDHSTS_BUSY_IRPT) != 0) {
			_Write(SDHSTS, SDHSTS_BUSY_IRPT);
			return B_OK;
		}

		// erasing and writing can take long; the spec allows 250 ms for a
		// write, more for an erase
		bigtime_t elapsed = system_time() - start;
		if (elapsed > 5000000) {
			ERROR("command %u: the card stays busy\n", command);
			return B_TIMED_OUT;
		}
		if (elapsed > 20)
			snooze(elapsed > 1000 ? 100 : 10);
	}
}


status_t
SdhostBus::_DataError(uint32 status)
{
	if ((status & (SDHSTS_REW_TIME_OUT | SDHSTS_CMD_TIME_OUT)) != 0)
		return B_TIMED_OUT;
	return B_IO_ERROR;
}


/*!	Takes \a words words out of the FIFO, in bursts of up to eight words
	that are there.
*/
status_t
SdhostBus::_ReadFifo(uint32* buffer, size_t words)
{
	bigtime_t lastProgress = system_time();
	while (words > 0) {
		uint32 edm = _Read(SDEDM);
		uint32 available = SDEDM_FIFO_FILL(edm);
		uint32 burst = std::min((size_t)SDDATA_FIFO_PIO_BURST, words);
		if (available < burst) {
			uint32 status = _Read(SDHSTS);
			if ((status & SDHSTS_ERROR_MASK) != 0) {
				ERROR("read: error %#" B_PRIx32 ", EDM %#" B_PRIx32 ", %"
					B_PRIuSIZE " words to go\n", status, edm, words);
				return _DataError(status);
			}
			bigtime_t elapsed = system_time() - lastProgress;
			if (elapsed > 1000000) {
				ERROR("read: no data (EDM %#" B_PRIx32 ", status %#" B_PRIx32
					"), %" B_PRIuSIZE " words to go\n", edm, status, words);
				return B_TIMED_OUT;
			}
			// the first data of a read can take a millisecond or two
			if (elapsed > 50)
				snooze(20);
			continue;
		}

		uint32 count = std::min((size_t)available, words);
		words -= count;
		while (count-- > 0)
			*buffer++ = _Read(SDDATA);
		lastProgress = system_time();
	}

	return B_OK;
}


/*!	Puts \a words words into the FIFO while it has room for them, in
	bursts of up to eight words.
*/
status_t
SdhostBus::_WriteFifo(const uint32* buffer, size_t words)
{
	bigtime_t lastProgress = system_time();
	while (words > 0) {
		uint32 edm = _Read(SDEDM);
		uint32 space = SDDATA_FIFO_WORDS - std::min(SDEDM_FIFO_FILL(edm),
			(uint32)SDDATA_FIFO_WORDS);
		uint32 burst = std::min((size_t)SDDATA_FIFO_PIO_BURST, words);
		if (space < burst) {
			uint32 status = _Read(SDHSTS);
			if ((status & SDHSTS_ERROR_MASK) != 0) {
				ERROR("write: error %#" B_PRIx32 ", EDM %#" B_PRIx32 ", %"
					B_PRIuSIZE " words to go\n", status, edm, words);
				return _DataError(status);
			}
			bigtime_t elapsed = system_time() - lastProgress;
			if (elapsed > 1000000) {
				ERROR("write: no room (EDM %#" B_PRIx32 ", status %#" B_PRIx32
					"), %" B_PRIuSIZE " words to go\n", edm, status, words);
				return B_TIMED_OUT;
			}
			if (elapsed > 50)
				snooze(20);
			continue;
		}

		uint32 count = std::min((size_t)space, words);
		words -= count;
		while (count-- > 0)
			_Write(SDDATA, *buffer++);
		lastProgress = system_time();
	}

	return B_OK;
}


/*!	After a block has been written into the FIFO: waits until it is on the
	card, the CRC status included (the block interrupt status).
*/
status_t
SdhostBus::_WaitBlockDone()
{
	bigtime_t start = system_time();
	while (true) {
		uint32 status = _Read(SDHSTS);
		if ((status & SDHSTS_ERROR_MASK) != 0) {
			ERROR("write: error %#" B_PRIx32 " at the end of a block, EDM %#"
				B_PRIx32 "\n", status, _Read(SDEDM));
			return _DataError(status);
		}
		if ((status & SDHSTS_BLOCK_IRPT) != 0) {
			_Write(SDHSTS, SDHSTS_BLOCK_IRPT);
			return B_OK;
		}

		bigtime_t elapsed = system_time() - start;
		if (elapsed > 1000000) {
			ERROR("write: the block does not get done (EDM %#" B_PRIx32 ")\n",
				_Read(SDEDM));
			return B_TIMED_OUT;
		}
		if (elapsed > 50)
			snooze(20);
	}
}


/*!	Linux' bcm2835_wait_transfer_complete(): after a transfer without a
	CMD12, until the state machine is back in the data mode, or waits for
	more data that is not to come.
*/
status_t
SdhostBus::_WaitTransferComplete(bool isWrite)
{
	uint32 alternateIdle = isWrite
		? SDEDM_FSM_WRITESTART1 : SDEDM_FSM_READWAIT;
	bigtime_t start = system_time();
	while (true) {
		uint32 edm = _Read(SDEDM);
		uint32 state = edm & SDEDM_FSM_MASK;
		if (state == SDEDM_FSM_IDENTMODE || state == SDEDM_FSM_DATAMODE)
			return B_OK;
		if (state == alternateIdle) {
			_Write(SDEDM, edm | SDEDM_FORCE_DATA_MODE);
			return B_OK;
		}

		bigtime_t elapsed = system_time() - start;
		if (elapsed > 500000) {
			ERROR("the transfer does not complete (EDM %#" B_PRIx32 ")\n",
				edm);
			return B_TIMED_OUT;
		}
		if (elapsed > 20)
			snooze(10);
	}
}


/*!	CMD12, the end of an open-ended multi-block transfer, and the card's
	busy signal after it.
*/
status_t
SdhostBus::_StopTransmission(bool isWrite)
{
	uint32 response = 0;
	status_t status = _SendCommand(SD_STOP_TRANSMISSION, 0,
		RESPONSE_48_BUSY, &response, 0, 0, false);
	if (status == B_OK)
		status = _WaitBusy(SD_STOP_TRANSMISSION);
	if (status == B_OK && isWrite && (response & kWriteStopErrors) != 0) {
		ERROR("write failed: card status %#" B_PRIx32 "\n", response);
		status = B_IO_ERROR;
	}
	return status;
}


/*!	Takes what a read left in the FIFO out of it: the words of an aborted
	transfer, or what the block read ahead of an open-ended one before the
	CMD12, which would otherwise start the next read.
*/
void
SdhostBus::_DrainFifo()
{
	uint32 drained = 0;
	while (SDEDM_FIFO_FILL(_Read(SDEDM)) > 0 && drained < 4 * SDDATA_FIFO_WORDS) {
		_Read(SDDATA);
		drained++;
	}
	if (drained > 0)
		TRACE("%" B_PRIu32 " words left in the FIFO after a read\n", drained);
}


/*!	A command with a data phase of \a blocks blocks between the card and
	the buffer, and CMD12 after it if \a stop.
*/
status_t
SdhostBus::_Transfer(uint8 command, uint32 argument, bool isWrite,
	uint32 blockSize, uint32 blocks, bool stop)
{
	uint32 response = 0;
	status_t status = _SendCommand(command, argument,
		RESPONSE_48 | (isWrite ? SDCMD_WRITE_CMD : SDCMD_READ_CMD), &response,
		blockSize, blocks);
	if (status != B_OK)
		return status;
	if ((response & kMmcR1ErrorMask) != 0) {
		ERROR("command %u (%#" B_PRIx32 "): card status %#" B_PRIx32 "\n",
			command, argument, response);
		status = B_IO_ERROR;
	}

	if (status == B_OK) {
		size_t blockWords = blockSize / 4;
		if (isWrite) {
			// whatever the block count write let through before the command
			_Write(SDHSTS, SDHSTS_BLOCK_IRPT);
			for (uint32 block = 0; block < blocks && status == B_OK;
					block++) {
				status = _WriteFifo(fBuffer + block * blockWords, blockWords);
				if (status == B_OK)
					status = _WaitBlockDone();
			}
		} else
			status = _ReadFifo(fBuffer, blocks * blockWords);

		if (status == B_OK) {
			uint32 hostStatus = _Read(SDHSTS);
			if ((hostStatus & SDHSTS_ERROR_MASK) != 0) {
				ERROR("%s: error %#" B_PRIx32 " after the data\n",
					isWrite ? "write" : "read", hostStatus);
				status = _DataError(hostStatus);
			}
		}
	}

	// the card goes back to the transfer state, after an error too
	if (stop) {
		_Write(SDHCFG, fConfig);
		status_t stopStatus = _StopTransmission(isWrite);
		if (status == B_OK)
			status = stopStatus;
	} else if (status == B_OK)
		status = _WaitTransferComplete(isWrite);

	_Write(SDHCFG, fConfig);
	if (!isWrite)
		_DrainFifo();
	if (status != B_OK) {
		_KickStateMachine();
		_Write(SDHSTS, SDHSTS_CLEAR_ALL);
	}
	return status;
}


status_t
SdhostBus::SetClock(uint32 kilohertz)
{
	if (kilohertz == 0)
		return B_BAD_VALUE;

	// SDCDIV holds the divider minus two, eleven bits (Linux'
	// bcm2835_set_clock()); below 100 kHz it is just as slow as it goes.
	uint32 target = kilohertz * 1000;
	uint32 divider;
	if (target < 100000)
		divider = SDCDIV_MAX_CDIV + 2;
	else {
		divider = fDevice.baseClock / target;
		if (divider < 2)
			divider = 2;
		if (fDevice.baseClock / divider > target)
			divider++;
		if (divider > SDCDIV_MAX_CDIV + 2)
			divider = SDCDIV_MAX_CDIV + 2;
	}

	fDivider = divider - 2;
	fClock = fDevice.baseClock / divider;
	_Write(SDCDIV, fDivider);
	// half a second for the card to answer or to send data
	_Write(SDTOUT, fClock / 2);

	TRACE("clock: %" B_PRIu32 " kHz asked for, %" B_PRIu32 " Hz set\n",
		kilohertz, fClock);
	return B_OK;
}


status_t
SdhostBus::ExecuteCommand(uint8 command, uint32 argument, uint32* response)
{
	if (fStatus != B_OK)
		return fStatus;

	uint32 flags;
	switch (command) {
		case GO_IDLE_STATE:
			flags = RESPONSE_NONE;
			break;

		case SD_APP_CMD:
		case SEND_STATUS:
		case SET_BLOCK_LENGTH:
		case SD_ERASE_WR_BLK_START:
		case SD_ERASE_WR_BLK_END:
			flags = RESPONSE_48;
			break;

		case SD_SET_BUS_WIDTH:
			// also MMC_SWITCH, which has R1b
			flags = is_mmc_card(fCardType) ? RESPONSE_48_BUSY : RESPONSE_48;
			break;

		case SELECT_DESELECT_CARD:
			flags = argument == 0 ? RESPONSE_NONE : RESPONSE_48_BUSY;
			break;

		case SD_ERASE:
		case SD_STOP_TRANSMISSION:
			flags = RESPONSE_48_BUSY;
			break;

		case ALL_SEND_CID:
		case SEND_CSD:
		case SEND_CID:
			flags = RESPONSE_136;
			break;

		case MMC_SEND_OP_COND:
		case SD_SEND_OP_COND:
		case SD_SEND_RELATIVE_ADDR:
			flags = RESPONSE_48;
			break;

		case SD_SEND_IF_COND:
			if (is_mmc_card(fCardType))
				return B_NOT_SUPPORTED;
					// MMC_SEND_EXT_CSD, which has data: ReadExtendedCsd()
			flags = RESPONSE_48;
			break;

		default:
			ERROR("unknown command %u\n", command);
			return B_BAD_DATA;
	}

	if (response == NULL && flags != RESPONSE_NONE)
		return B_BAD_VALUE;

	status_t status = _SendCommand(command, argument, flags, response);
	if (status == B_OK && (flags & SDCMD_BUSYWAIT) != 0)
		status = _WaitBusy(command);
	return status;
}


status_t
SdhostBus::DoIO(uint8 command, IOOperation* operation, bool offsetAsSectors)
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
	size_t vecIndex = 0;
	generic_size_t vecOffset = 0;

	while (length != 0) {
		size_t size = std::min(length, (generic_size_t)kBufferSize);

		if (isWrite) {
			status_t status = _CopyVecs(true, vecs, count, vecIndex,
				vecOffset, size);
			if (status != B_OK)
				return status;
		}

		status_t status = _Transfer(command, offset / unit, isWrite,
			kBlockSize, size / kBlockSize, multiple);
		if (status != B_OK) {
			ERROR("%s of %" B_PRIuSIZE " bytes at %" B_PRIu64 " failed: %s\n",
				isWrite ? "write" : "read", size, offset, strerror(status));
			return status;
		}

		if (!isWrite) {
			status = _CopyVecs(false, vecs, count, vecIndex, vecOffset, size);
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
SdhostBus::_CopyVecs(bool toBuffer, const generic_io_vec* vecs, size_t count,
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


status_t
SdhostBus::ReadExtendedCsd(uint8 data[512])
{
	if (fStatus != B_OK)
		return fStatus;
	status_t status = _Transfer(MMC_SEND_EXT_CSD, 0, false, 512, 1, false);
	if (status == B_OK)
		memcpy(data, fBuffer, 512);
	return status;
}


status_t
SdhostBus::SwitchSdFunction(uint32 argument, uint8 data[64])
{
	if (fStatus != B_OK)
		return fStatus;
	status_t status = _Transfer(SD_SWITCH_FUNCTION, argument, false, 64, 1,
		false);
	if (status == B_OK)
		memcpy(data, fBuffer, 64);
	return status;
}


void
SdhostBus::SetScanSemaphore(sem_id semaphore)
{
	// The Pi 3's slot has no card detection: have the bus look for a card
	// right away.
	if (semaphore >= 0)
		release_sem(semaphore);
}


void
SdhostBus::SetBusWidth(int width)
{
	if (width == 4)
		fConfig |= SDHCFG_WIDE_EXT_BUS;
	else
		fConfig &= ~SDHCFG_WIDE_EXT_BUS;
	_Write(SDHCFG, fConfig);
}


void
SdhostBus::Terminate()
{
	// The clock cannot be stopped; as slow as it goes then.
	fDivider = SDCDIV_MAX_CDIV;
	_Write(SDCDIV, fDivider);
}


//	#pragma mark - driver


static bool
is_enabled(fdt_device_module_info* fdt, fdt_device* device)
{
	int length;
	const char* status = (const char*)fdt->get_prop(device, "status",
		&length);
	return status == NULL || strcmp(status, "okay") == 0
		|| strcmp(status, "ok") == 0;
}


static float
sdhost_supports_device(device_node* parent)
{
	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK
		|| strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	const char* compatible;
	if (gDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK
		|| strcmp(compatible, "brcm,bcm2835-sdhost") != 0) {
		return 0.0f;
	}

	// The Pi 4's tree has the block too, switched off: its pins are the
	// EMMC2's there.
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (gDeviceManager->get_driver(parent, (driver_module_info**)&fdt,
			(void**)&device) != B_OK
		|| !is_enabled(fdt, device)) {
		return 0.0f;
	}

	return 1.0f;
}


static status_t
sdhost_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{.string = "BCM2835 SD host controller"}},
		{}
	};

	return gDeviceManager->register_node(parent, SDHOST_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static status_t
sdhost_init_driver(device_node* node, void** _cookie)
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

	sdhost_device* device = new(std::nothrow) sdhost_device;
	if (device == NULL) {
		gDeviceManager->put_node(parent);
		return B_NO_MEMORY;
	}
	device->node = node;

	if (!fdt->get_reg(fdtDevice, 0, &device->registers,
			&device->registersSize)) {
		ERROR("no registers in the device tree\n");
		gDeviceManager->put_node(parent);
		delete device;
		return B_BAD_DATA;
	}

	int length;
	const uint32* property = (const uint32*)fdt->get_prop(fdtDevice,
		"clock-frequency", &length);
	device->baseClock = property != NULL && length == 4
		? B_BENDIAN_TO_HOST_INT32(*property) : DEFAULT_BASE_CLOCK;
	if (device->baseClock == 0)
		device->baseClock = DEFAULT_BASE_CLOCK;

	property = (const uint32*)fdt->get_prop(fdtDevice, "bus-width", &length);
	device->busWidth = property != NULL && length == 4
		? B_BENDIAN_TO_HOST_INT32(*property) : 4;
	if (device->busWidth != 1)
		device->busWidth = 4;

	gDeviceManager->put_node(parent);

	*_cookie = device;
	return B_OK;
}


static void
sdhost_uninit_driver(void* cookie)
{
	delete (sdhost_device*)cookie;
}


static status_t
sdhost_register_child_devices(void* cookie)
{
	sdhost_device* device = (sdhost_device*)cookie;

	// SD high speed: 50 MHz needs a core clock of at least 100 MHz (the
	// divider is at least two).
	uint8 highSpeed = device->baseClock >= 100000000 ? 1 : 0;

	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "SD card slot"}},
		{B_DEVICE_BUS, B_STRING_TYPE, {.string = "mmc"}},
		{B_DEVICE_FIXED_CHILD, B_STRING_TYPE, {.string = MMC_BUS_MODULE_NAME}},
		{kMmcReadOnlyAttribute, B_UINT8_TYPE, {.ui8 = 0}},
		{kMmcNonRemovableAttribute, B_UINT8_TYPE, {.ui8 = 0}},
		{kMmcSdHighSpeedAttribute, B_UINT8_TYPE, {.ui8 = highSpeed}},
		{kMmcMaxBusWidthAttribute, B_UINT8_TYPE,
			{.ui8 = (uint8)device->busWidth}},
		{kMmcEnableCacheAttribute, B_UINT8_TYPE, {.ui8 = 0}},
		// whole blocks in pieces of memory that are copied through the
		// driver's own buffer, at most the buffer's size per transfer
		{B_DMA_ALIGNMENT, B_UINT32_TYPE, {.ui32 = kBlockSize - 1}},
		{B_DMA_MAX_SEGMENT_COUNT, B_UINT32_TYPE,
			{.ui32 = (uint32)(kBufferSize / B_PAGE_SIZE)}},
		{B_DMA_MAX_SEGMENT_BLOCKS, B_UINT32_TYPE, {.ui32 = kMaxBlocks}},
		{B_DMA_MAX_TRANSFER_BLOCKS, B_UINT32_TYPE, {.ui32 = kMaxBlocks}},
		{}
	};

	return gDeviceManager->register_node(device->node, SDHOST_BUS_MODULE_NAME,
		attrs, NULL, NULL);
}


//	#pragma mark - bus


static status_t
sdhost_init_bus(device_node* node, void** _cookie)
{
	device_node* parent = gDeviceManager->get_parent_node(node);
	sdhost_device* device;
	status_t status = gDeviceManager->get_driver(parent, NULL,
		(void**)&device);
	gDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	SdhostBus* bus = new(std::nothrow) SdhostBus(*device);
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
sdhost_uninit_bus(void* cookie)
{
	delete (SdhostBus*)cookie;
}


static status_t
sdhost_set_clock(void* cookie, uint32_t kilohertz)
{
	return ((SdhostBus*)cookie)->SetClock(kilohertz);
}


static status_t
sdhost_execute_command(void* cookie, uint8_t command, uint32_t argument,
	uint32_t* response)
{
	return ((SdhostBus*)cookie)->ExecuteCommand(command, argument, response);
}


static status_t
sdhost_do_io(void* cookie, uint8_t command, IOOperation* operation,
	bool offsetAsSectors)
{
	return ((SdhostBus*)cookie)->DoIO(command, operation, offsetAsSectors);
}


static void
sdhost_set_scan_semaphore(void* cookie, sem_id semaphore)
{
	((SdhostBus*)cookie)->SetScanSemaphore(semaphore);
}


static void
sdhost_set_bus_width(void* cookie, int width)
{
	((SdhostBus*)cookie)->SetBusWidth(width);
}


static void
sdhost_terminate_bus(void* cookie)
{
	((SdhostBus*)cookie)->Terminate();
}


static void
sdhost_set_card_type(void* cookie, card_type type)
{
	((SdhostBus*)cookie)->SetCardType(type);
}


static status_t
sdhost_read_extended_csd(void* cookie, uint8_t data[512])
{
	return ((SdhostBus*)cookie)->ReadExtendedCsd(data);
}


static status_t
sdhost_switch_sd_function(void* cookie, uint32_t argument, uint8_t data[64])
{
	return ((SdhostBus*)cookie)->SwitchSdFunction(argument, data);
}


static driver_module_info sSdhostDriver = {
	{
		SDHOST_DRIVER_MODULE_NAME,
		0,
		NULL
	},
	sdhost_supports_device,
	sdhost_register_device,
	sdhost_init_driver,
	sdhost_uninit_driver,
	sdhost_register_child_devices,
	NULL,	// rescan
	NULL,	// device removed
};

static mmc_bus_interface sSdhostBus = {
	.info = {
		.info = {
			.name = SDHOST_BUS_MODULE_NAME,
		},
		.init_driver = sdhost_init_bus,
		.uninit_driver = sdhost_uninit_bus,
	},
	.set_clock = sdhost_set_clock,
	.execute_command = sdhost_execute_command,
	.do_io = sdhost_do_io,
	.set_scan_semaphore = sdhost_set_scan_semaphore,
	.set_bus_width = sdhost_set_bus_width,
	.terminate_bus = sdhost_terminate_bus,
	.set_card_type = sdhost_set_card_type,
	.read_extended_csd = sdhost_read_extended_csd,
	.switch_sd_function = sdhost_switch_sd_function,
	.sdio_transfer = NULL,
	.set_sdio_interrupt_handler = NULL,
	.enable_sdio_interrupt = NULL
};

module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager},
	{}
};

module_info* modules[] = {
	(module_info*)&sSdhostDriver,
	(module_info*)&sSdhostBus,
	NULL
};
