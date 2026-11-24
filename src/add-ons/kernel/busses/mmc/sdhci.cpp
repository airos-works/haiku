/*
 * Copyright 2018-2025 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		B Krishnan Iyer, krishnaniyer97@gmail.com
 *		Adrien Destugues, pulkomandy@pulkomandy.tk
 *		Ron Ben Aroya, sed4906birdie@gmail.com
 */


#include <algorithm>
#include <new>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <bus/PCI.h>
#include <ACPI.h>
#include "acpi.h"

#include <ByteOrder.h>
#include <KernelExport.h>
#include <arch/atomic.h>
#include <vm/vm.h>

#include "IOSchedulerSimple.h"
#include "mmc.h"
#include "sdhci.h"


//#define TRACE_SDHCI
#ifdef TRACE_SDHCI
#	define TRACE(x...) dprintf("\33[33msdhci:\33[0m " x)
#else
#	define TRACE(x...) ;
#endif
#define TRACE_ALWAYS(x...)	dprintf("\33[33msdhci:\33[0m " x)
#define ERROR(x...)			dprintf("\33[33msdhci:\33[0m " x)
#define CALLED(x...)		TRACE("CALLED %s\n", __PRETTY_FUNCTION__)


#define SDHCI_DEVICE_MODULE_NAME "busses/mmc/sdhci/driver_v1"


device_manager_info* gDeviceManager;
device_module_info* gMMCBusController;


static int32
sdhci_generic_interrupt(void* data)
{
	SdhciBus* bus = (SdhciBus*)data;
	int32 result = bus->HandleInterrupt();
	if (result != B_UNHANDLED_INTERRUPT)
		bus->InterruptArrived();
	return result;
}


SdhciBus::SdhciBus(struct registers* registers, uint32_t irq, bool poll,
	const sdhci_platform_info* platform, uint32 slotQuirks)
	:
	fRegisters(registers),
	fCommandResult(0),
	fIrq(irq),
	fInterruptInstalled(false),
	fScanSemaphore(-1),
	fReadingDataBlock(false),
	fStatus(B_OK),
	fWorkerThread(-1),
	fInterruptArrived(0),
	fCardType(CARD_TYPE_UNKNOWN),
	fPlatform(platform != NULL ? *platform : sdhci_platform_info{}),
	fDMAArea(-1),
	fDMABuffer(NULL),
	fDMAAddress(0),
	fAdmaArea(-1),
	fAdmaTable(NULL),
	fAdmaAddress(0),
	fAdmaFromMemory(false),
	fDMAQuarantined(false),
	fReportedHighCpuRead(false),
	fReportedHighCpuWrite(false),
	fSlotQuirks(slotQuirks),
	fSdioInterruptEnabled(false),
	fSdioHandler(NULL),
	fSdioHandlerData(NULL),
	fSdioTransfer(false),
	fSdioTransferMode(0)
{
	B_INITIALIZE_SPINLOCK(&fInterruptLock);

	if (irq == 0 || irq == 0xff) {
		ERROR("IRQ not assigned\n");
		fStatus = B_BAD_DATA;
		return;
	}

	fInterruptNotifier.Init(this, "SDHCI interrupts");

	DisableInterrupts();

	fStatus = install_io_interrupt_handler(fIrq,
		sdhci_generic_interrupt, this, 0);

	if (fStatus != B_OK) {
		ERROR("can't install interrupt handler\n");
		return;
	}
	fInterruptInstalled = true;

	// First of all, we have to make sure we are in a sane state. The easiest
	// way is to reset everything.
	Reset();
	if (fStatus != B_OK)
		return;

	TRACE("Controller spec version: %d, vendor version: %#02x\n",
		fRegisters->host_controller_version.specVersion,
		fRegisters->host_controller_version.vendorVersion);

	TRACE("Capabilities: %s%s%s%s%s%s%s%s%s%s%s%s%s%s\n"
		"    Clock multiplier: %" PRIx8 "\n"
		"    Retuning modes: %" PRIx8 "\n"
		"    Retuning timer count: %" PRIx8 "\n"
		"    Slot type: %" PRIx8 "\n"
		"    Supported voltages: %" PRIx8 "\n"
		"    Max block length: %" PRIx8 "\n"
		"    Base clock frequency: %" PRId8 " MHz\n"
		"    Timeout clock: %" PRId8 " kHz\n",
		fRegisters->capabilities.UseTuningForSDR50() ? "SDR50 needs retuning, " : "",
		fRegisters->capabilities.TypeDSupport() ? "Type-D, " : "",
		fRegisters->capabilities.TypeCSupport() ? "Type-C, " : "",
		fRegisters->capabilities.TypeASupport() ? "Type-A, " : "",
		fRegisters->capabilities.DDR50Support() ? "DDR50, " : "",
		fRegisters->capabilities.SDR104Support() ? "SDR104, " : "",
		fRegisters->capabilities.SDR50Support() ? "SDR50, " : "",
		fRegisters->capabilities.AsynchronousInterrupts() ? "Asynchronous interrupts, " : "",
		fRegisters->capabilities.SystemBus64Bits() ? "64-bit system bus, " : "",
		fRegisters->capabilities.SuspendResume() ? "Suspend/Resume, " : "",
		fRegisters->capabilities.SimpleDMA() ? "Simple DMA, " : "",
		fRegisters->capabilities.HighSpeed() ? "High speed, " : "",
		fRegisters->capabilities.AdvancedDMA() ? "Advanced DMA, " : "",
		fRegisters->capabilities.Embedded8Bit() ? "8-bit Embedded mode, " : "",
		fRegisters->capabilities.ClockMultiplier(),
		fRegisters->capabilities.RetuningModes(),
		fRegisters->capabilities.RetuningTimerCount(),
		fRegisters->capabilities.SlotType(),
		fRegisters->capabilities.SupportedVoltages(),
		fRegisters->capabilities.MaxBlockLength(),
		fRegisters->capabilities.BaseClockFrequency(),
		fRegisters->capabilities.TimeoutClockFrequency());
	TRACE("Initial host control: %x\n", fRegisters->host_control.Bits());
	TRACE("Initial host control 2: %x\n", fRegisters->host_control_2);

	if (fRegisters->host_controller_version.specVersion > 3) {
		// TODO proper class for manipulating host_control_2
		fRegisters->host_control_2 &= ~(1<<12);
		TRACE("Host control 2 after disabling v4 DMA mode: %x\n", fRegisters->host_control_2);
	}

	// Turn on the power supply to the card, if there is a card inserted
	if (PowerOn()) {
		// Then we configure the clock to the frequency needed for
		// initialization
		SetClock(400, false);
		if (fStatus != B_OK)
			return;
	}

	fRegisters->timeout_control.SetDivider(fRegisters->capabilities.TimeoutClockFrequency(), 500);
	if (platform != NULL)
		fRegisters->timeout_control.SetDivider(24000, 500);

#if defined(__aarch64__)
	fStatus = sdhci_allocate_dma(&fDMAArea, &fDMABuffer, &fDMAAddress);
	if (fStatus != B_OK)
		return;
	if (platform != NULL && platform->adma_only) {
		void* table;
		fStatus = sdhci_allocate_dma(&fAdmaArea, &table, &fAdmaAddress,
			B_PAGE_SIZE);
		if (fStatus != B_OK)
			return;
		fAdmaTable = (uint32*)table;
	}
	if (platform != NULL && platform->dma_allocated != NULL) {
		fStatus = platform->dma_allocated(platform->cookie, fDMAAddress,
			kSdhciDmaSize);
		if (fStatus == B_OK && fAdmaTable != NULL) {
			fStatus = platform->dma_allocated(platform->cookie, fAdmaAddress,
				B_PAGE_SIZE);
		}
		if (fStatus != B_OK)
			return;
	}
#else
	// With ADMA2, an operation's pieces of memory (up to 256 of them) go to
	// the card in one command, instead of a command for each piece: 4 KiB
	// pages, mostly. Off by default until a pass on a real reader (#181).
	if (sdhci_read_dma_mode().adma2
		&& fRegisters->host_controller_version.specVersion >= 1
		&& fRegisters->capabilities.AdvancedDMA()) {
		void* table;
		if (sdhci_allocate_dma(&fAdmaArea, &table, &fAdmaAddress, B_PAGE_SIZE)
				== B_OK) {
			fAdmaTable = (uint32*)table;
			fAdmaFromMemory = true;
			TRACE_ALWAYS("ADMA2 descriptor table at %#" B_PRIxPHYSADDR "\n",
				fAdmaAddress);
		}
	}
#endif

	// Finally, configure some useful interrupts. A soldered card is never
	// removed, and its slot's card detection may be wrong (Intel's SDIO
	// slots): no card events for it.
	EnableInterrupts(SDHCI_INT_CMD_CMP | SDHCI_INT_TRANS_CMP | SDHCI_INT_ERROR
		| SDHCI_INT_ERROR_MASK
		| ((fSlotQuirks & SDHCI_SLOT_NON_REMOVABLE) == 0
			? SDHCI_INT_CARD_REM : 0));

	// We want to see the other bits in the status register, but not have an
	// interrupt trigger on buffer readiness: EXT_CSD uses polled PIO. The
	// SDIO card interrupt stays off until a function driver wants it: its
	// status follows the card's line and is cleared only by disabling it.
	fRegisters->interrupt_status_enable |= SDHCI_INT_ERROR_MASK
		| (SDHCI_INT_NORMAL_MASK & ~SDHCI_INT_CARD_INT);

	if (poll) {
		// Spawn a polling thread, as the interrupts may not arrive on ACPI
		// machines. It slows down once one has (_WorkerThread()).
		fWorkerThread = spawn_kernel_thread(_WorkerThread, "SD bus poller",
			B_NORMAL_PRIORITY, this);
		if (fWorkerThread < B_OK)
			fStatus = fWorkerThread;
		else {
			fStatus = resume_thread(fWorkerThread);
			if (fStatus != B_OK) {
				kill_thread(fWorkerThread);
				fWorkerThread = -1;
			}
		}
	}
}


SdhciBus::~SdhciBus()
{
	fStatus = B_SHUTTING_DOWN;
	status_t result;
	if (fWorkerThread >= B_OK)
		wait_for_thread(fWorkerThread, &result);

	TerminateBus();

	if (fInterruptInstalled)
		remove_io_interrupt_handler(fIrq, sdhci_generic_interrupt, this);
	if (fAdmaArea >= B_OK)
		delete_area(fAdmaArea);
	if (fDMAArea >= B_OK) {
		if (!fDMAQuarantined || fRegisters->software_reset.ResetAll())
			delete_area(fDMAArea);
		else
			ERROR("DMA area retained after failed controller reset\n");
	}

	area_id regs_area = area_for(fRegisters);
	delete_area(regs_area);

}


void
SdhciBus::EnableInterrupts(uint32_t mask)
{
	fRegisters->interrupt_status_enable |= mask;
	fRegisters->interrupt_signal_enable |= mask;
}


void
SdhciBus::DisableInterrupts()
{
	fRegisters->interrupt_status_enable = 0;
	fRegisters->interrupt_signal_enable = 0;
}


// #pragma mark -
/*
PartA2, SD Host Controller Simplified Specification, Version 4.20
§3.7.1.1 The sequence to issue an SD Command
*/
status_t
SdhciBus::WaitForCompletion(uint32_t mask, bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	while (true) {
		ConditionVariableEntry waiter;
		fInterruptNotifier.Add(&waiter);
		uint32_t result = atomic_get(&fCommandResult);
		if ((result & (SDHCI_INT_ERROR | SDHCI_INT_ERROR_MASK)) != 0)
			return (result & (SDHCI_INT_COMMAND_TIMEOUT | SDHCI_INT_DATA_TIMEOUT))
				!= 0 ? B_TIMED_OUT : B_IO_ERROR;
		if ((result & mask) != 0)
			return B_OK;
		status_t status = waiter.Wait(B_ABSOLUTE_TIMEOUT, deadline);
		if (status != B_OK && status != B_INTERRUPTED)
			return status;
	}
}


status_t
SdhciBus::ExecuteCommand(uint8_t command, uint32_t argument, uint32_t* response)
{
	TRACE("ExecuteCommand(%d, %x)\n", command, argument);
	if (fStatus != B_OK)
		return fStatus;
	if (response == NULL && command != GO_IDLE_STATE
		&& !(command == SELECT_DESELECT_CARD && argument == 0))
		return B_BAD_VALUE;

	// First of all clear the result
	atomic_set(&fCommandResult, 0);

	// Check if it's possible to send a command right now.
	// It is not possible to send a command as long as the command line is busy.
	// The spec says we should wait, but we can't do that on kernel side, since
	// it leaves no chance for the upper layers to handle the problem. So we
	// just say we're busy and the caller can retry later.
	// Note that this should normally never happen: the command line is busy
	// only during command execution, and we don't leave this function with a
	// command running.
	if (fRegisters->present_state.CommandInhibit()) {
		TRACE_ALWAYS("Command execution impossible, command inhibit\n");
		return B_BUSY;
	}
	if (fRegisters->present_state.DataInhibit()) {
		TRACE_ALWAYS("Command execution unwise, data inhibit\n");
		return B_BUSY;
	}

	uint32_t replyType;
	uint16 transferMode = 0;

	switch (command) {
		// Basic reply types
		case GO_IDLE_STATE:
			replyType = Command::kNoReplyType;
			break;
		case SD_APP_CMD:
		case SEND_STATUS:
		case SET_BLOCK_LENGTH:
		case SD_ERASE_WR_BLK_START:
		case SD_ERASE_WR_BLK_END:
			replyType = Command::kR1Type;
			break;
		case SELECT_DESELECT_CARD:
			replyType = argument == 0 ? Command::kNoReplyType : Command::kR1bType;
			break;
		case SD_ERASE:
			replyType = Command::kR1bType;
			break;
		case ALL_SEND_CID:
		case SEND_CSD:
		case SEND_CID:
			replyType = Command::kR2Type;
			break;
		case MMC_SEND_OP_COND:
		case SD_SEND_OP_COND: // SD Application command
			replyType = Command::kR3Type;
			break;

		// SDIO (SDIO Simplified Specification 3.00, 5)
		case SD_IO_SEND_OP_COND:
			// R4: 48 bits, no CRC and no command index, as R3
			replyType = Command::kR3Type;
			break;
		case SD_IO_RW_DIRECT:
			// R5: 48 bits with CRC and command index, as R1
			replyType = Command::kR1Type;
			break;
		case SD_IO_RW_EXTENDED:
			if (!fSdioTransfer)
				return B_BAD_VALUE;
			replyType = Command::kR1Type | Command::kDataPresent;
			transferMode = fSdioTransferMode;
			break;

		// Commands defined with different reply types in SD and MMC specifications
		case SD_SET_BUS_WIDTH: // SD application command. Also MMC_SWITCH, which is not.
			// and SD_SWITCH_FUNCTION, which has a data phase
			if (fReadingDataBlock && !is_mmc_card(fCardType)) {
				replyType = Command::kR1Type | Command::kDataPresent;
				transferMode = TransferMode::kRead;
			} else if (is_mmc_card(fCardType)
				&& mmc_cache_busy_timeout(fCardType, command, argument) == 0)
				replyType = Command::kR1bType;
			else
				replyType = Command::kR1Type;
			break;
		case SD_SEND_RELATIVE_ADDR: // also MMC_SET_RELATIVE_ADDR
			if (is_mmc_card(fCardType))
				replyType = Command::kR1Type;
			else
				replyType = Command::kR6Type;
			break;
		case SD_SEND_IF_COND: // also MMC_SEND_EXT_CSD
			if (is_mmc_card(fCardType)) {
				replyType = Command::kR1Type | Command::kDataPresent;
				transferMode = TransferMode::kRead;
			} else
				replyType = Command::kR7Type;
			break;

		// Commands with data transfer replies, also set transferMode
		case SD_READ_SINGLE_BLOCK:
			transferMode = TransferMode::kRead | TransferMode::kDmaEnable;
			replyType = Command::kR1Type | Command::kDataPresent;
			break;
		case SD_READ_MULTIPLE_BLOCKS:
			transferMode = TransferMode::kRead | TransferMode::kMulti
				| TransferMode::kAutoCmd12Enable | TransferMode::kBlockCountEnable
				| TransferMode::kDmaEnable;
			replyType = Command::kR1Type | Command::kDataPresent;
			break;
		case SD_WRITE_SINGLE_BLOCK:
			transferMode = TransferMode::kWrite | TransferMode::kDmaEnable;
			replyType = Command::kR1Type | Command::kDataPresent;
			break;
		case SD_WRITE_MULTIPLE_BLOCKS:
			transferMode = TransferMode::kWrite | TransferMode::kMulti
				| TransferMode::kAutoCmd12Enable | TransferMode::kBlockCountEnable
				| TransferMode::kDmaEnable;
			replyType = Command::kR1Type | Command::kDataPresent;
			break;
		default:
			ERROR("Unknown command %x\n", command);
			return B_BAD_DATA;
	}

	// Check if DATA line is available (if needed)
	if ((replyType & Command::k32BitResponseCheckBusy) != 0
		&& command != SD_STOP_TRANSMISSION && command != SD_IO_ABORT) {
		if (fRegisters->present_state.DataInhibit()) {
			ERROR("Execution aborted, data inhibit\n");
			return B_BUSY;
		}
	}

	if (fRegisters->present_state.CommandInhibit())
		panic("Command line busy at start of execute command\n");

	fRegisters->argument = argument;

	if ((replyType == Command::kR1bType)
		|| (replyType == (Command::kR1Type | Command::kDataPresent)))
		fRegisters->transfer_mode = transferMode;

	memory_full_barrier();
	fRegisters->command.SendCommand(command, replyType);
	memory_full_barrier();

	status_t status = WaitForCompletion(SDHCI_INT_CMD_CMP, 1000000);
	if (status != B_OK) {
		if ((atomic_get(&fCommandResult) & SDHCI_INT_ADMA_ERROR) != 0) {
			ERROR("DMA error: host control %#x, 2 %#x, SDMA address %#"
				B_PRIx32 ", ADMA address %#" B_PRIx64 ", ADMA error %#x, "
				"version %#x\n", fRegisters->host_control.Bits(),
				fRegisters->host_control_2, fRegisters->system_address,
				fRegisters->adma_system_address,
				fRegisters->adma_error_status,
				*(volatile uint16_t*)((volatile uint8_t*)fRegisters + 0xfe));
		}
		ERROR("Command %u failed: %s (status %#x)\n", command,
			strerror(status), (uint32_t)atomic_get(&fCommandResult));
		RecoverError();
		return status;
	}
	if (fRegisters->present_state.CommandInhibit()) {
		RecoverError();
		return B_IO_ERROR;
	}
	memory_full_barrier();

	switch (replyType & Command::kReplySizeMask) {
		case Command::k32BitResponse:
		case Command::k32BitResponseCheckBusy:
			*response = fRegisters->response[0];
			break;
		case Command::k128BitResponse:
			response[0] = fRegisters->response[0];
			response[1] = fRegisters->response[1];
			response[2] = fRegisters->response[2];
			response[3] = fRegisters->response[3];
			break;

		default:
			// No response
			break;
	}

	// Even an immediately released DAT0 has a transfer-complete event.
	// Consume it before the next command can mistake it for its data phase.
	if (replyType == Command::kR1bType) {
		status = WaitForCompletion(SDHCI_INT_TRANS_CMP, 1000000);
		if (status != B_OK || fRegisters->present_state.DataInhibit()) {
			RecoverError();
			return status == B_OK ? B_IO_ERROR : status;
		}
	}
	TRACE("Command execution %d complete\n", command);
	return B_OK;
}


status_t
SdhciBus::InitCheck()
{
	return fStatus;
}


void
SdhciBus::Reset()
{
	if (!fRegisters->software_reset.ResetAll()) {
		ERROR("SdhciBus::Reset: SoftwareReset timeout\n");
		fStatus = B_TIMED_OUT;
	}
}


void
SdhciBus::SetClock(int kilohertz, bool allowAuto)
{
	if (fStatus != B_OK)
		return;
	if (kilohertz <= 0) {
		fStatus = B_BAD_VALUE;
		return;
	}
	// Presets depend on a negotiated timing mode. Enumeration currently
	// requests explicit legacy clocks, even on a newer host controller.
	(void)allowAuto;
	if (kilohertz == 400)
		PowerOn();
	bool highSpeed = kilohertz > 25000 && fRegisters->capabilities.HighSpeed();
	uint32 baseClock = fRegisters->capabilities.BaseClockFrequency() * 1000;
	fRegisters->clock_control.DisableSD();
	memory_full_barrier();
	if (fPlatform.set_clock != NULL) {
		fStatus = fPlatform.set_clock(fPlatform.cookie, kilohertz, &baseClock);
		if (fStatus != B_OK)
			return;
		if (kilohertz == 400 && fPlatform.identification_clock != 0)
			kilohertz = fPlatform.identification_clock;
	}
	if (baseClock == 0) {
		fStatus = B_BAD_VALUE;
		return;
	}
	int divider = (baseClock + kilohertz - 1) / kilohertz;
	if (divider < 1)
		divider = 1;
	if (fPlatform.divider_zero_broken && divider == 1)
		divider = 2;
	if (fRegisters->host_controller_version.specVersion <= 1) {
		int powerOfTwo = 1;
		while (powerOfTwo < divider && powerOfTwo < 256)
			powerOfTwo <<= 1;
		if (powerOfTwo < divider) {
			fStatus = B_NOT_SUPPORTED;
			return;
		}
		divider = powerOfTwo;
	} else if (divider > 2046) {
		fStatus = B_NOT_SUPPORTED;
		return;
	}
	if (fRegisters->host_controller_version.specVersion >= 2) {
		uint16 control2 = fRegisters->host_control_2 & ~(1 << 15);
		if ((fSlotQuirks & SDHCI_SLOT_HS_AS_SDR25) != 0) {
			// UHS mode select: SDR25 with high speed, as Linux does on
			// Intel's controllers (sdhci_intel_set_uhs_signaling)
			control2 = (control2 & ~7) | (highSpeed ? 1 : 0);
		}
		fRegisters->host_control_2 = control2;
	}
	// above the default 25 MHz the card runs in high speed timing, and the
	// host samples for it
	fRegisters->host_control.SetHighSpeed(highSpeed);
	divider = fRegisters->clock_control.SetDivider(divider);
	fRegisters->clock_control.EnableInternal();
	bigtime_t deadline = system_time() + 100000;
	while (!fRegisters->clock_control.InternalStable()) {
		if (system_time() >= deadline) {
			fStatus = B_TIMED_OUT;
			ERROR("Internal clock did not stabilize\n");
			return;
		}
		snooze(100);
	}
	memory_full_barrier();
	fRegisters->clock_control.EnableSD();
	TRACE("SDCLK: requested %d kHz, effective %d kHz\n", kilohertz,
		baseClock / divider);
}


status_t
SdhciBus::DoIO(uint8_t command, IOOperation* operation, bool offsetAsSectors)
{
	if (fStatus != B_OK)
		return fStatus;
	if (operation == NULL || operation->Offset() < 0)
		return B_BAD_VALUE;
	if (fPlatform.read_only && operation->IsWrite())
		return B_READ_ONLY_DEVICE;
	const uint32_t blockSize = 512;
	uint64_t offset = operation->Offset();
	generic_size_t length = operation->Length();
	if (offset % blockSize != 0 || length % blockSize != 0)
		return B_BAD_VALUE;
	if ((operation->IsWrite() && command != SD_WRITE_MULTIPLE_BLOCKS
			&& command != SD_WRITE_SINGLE_BLOCK)
		|| (!operation->IsWrite() && command != SD_READ_MULTIPLE_BLOCKS
			&& command != SD_READ_SINGLE_BLOCK))
		return B_BAD_VALUE;
	if (length == 0)
		return B_OK;
	if ((command == SD_READ_SINGLE_BLOCK || command == SD_WRITE_SINGLE_BLOCK)
		&& length != blockSize)
		return B_BAD_VALUE;
	const generic_io_vec* vecs = operation->Vecs();
	size_t count = operation->VecCount();
	if (vecs == NULL || count == 0)
		return B_BAD_VALUE;

	// How the operation's memory reaches the controller (see sdhci.h):
	// copied through the private buffer (arm64), where all pieces are
	// gathered for a command as long as the buffer; described piece by piece
	// in an ADMA2 table, for one command (x86, "adma2"); or one piece per
	// SDMA command. Commands are what is slow: each one costs a wait for an
	// interrupt and the card's set-up, more than moving 4 KiB does.
	const bool bounce = fDMABuffer != NULL;
	const bool table = !bounce && fAdmaFromMemory;

	// Validate the complete request before any command can change the card.
	generic_size_t remaining = length;
	uint64 cpuAddress = UINT64_MAX;
	for (size_t i = 0; i < count && remaining != 0; i++) {
		generic_size_t size = std::min(remaining, vecs[i].length);
		if (size == 0)
			continue;
		uint64 address = vecs[i].base;
		if (address < fPlatform.cpu_address_floor
			|| size % blockSize != 0 || (address & (blockSize - 1)) != 0
			|| size > UINT64_MAX - address)
			return B_BAD_VALUE;
		if (!bounce) {
			// the controller addresses the piece itself, with 32 bits, and
			// SDMA within a 512 KiB boundary
			if (address >= UINT64_C(0x100000000)
				|| size > UINT64_C(0x100000000) - address)
				return B_BAD_VALUE;
			if (!table && (address & 0x7ffff) + size > 0x80000)
				return B_BAD_VALUE;
		}
		cpuAddress = std::min(cpuAddress, address);
		remaining -= size;
	}
	uint64_t unit = offsetAsSectors ? blockSize : 1;
	if (remaining != 0 || offset / unit > UINT32_MAX
		|| (length - blockSize) / unit > UINT32_MAX - offset / unit)
		return B_BAD_VALUE;

	size_t index = 0;
	generic_size_t vecOffset = 0;
	while (length != 0) {
		while (index < count && vecOffset == vecs[index].length) {
			index++;
			vecOffset = 0;
		}
		if (index == count)
			return B_BAD_VALUE;
		// for the log: where the command's memory starts
		const uint64 pieceAddress = vecs[index].base + vecOffset;

		// this command's bytes
		generic_size_t size;
		if (bounce)
			size = std::min(length, (generic_size_t)kSdhciDmaSize);
		else if (table)
			size = _FillAdmaTable(vecs, count, index, vecOffset, length);
		else
			size = std::min(length, vecs[index].length - vecOffset);
		if (size == 0 || size % blockSize != 0 || size / blockSize > 0xffff)
			return B_BAD_VALUE;

		if (bounce) {
			if (operation->IsWrite()) {
				status_t status = _CopyVecs(true, vecs, count, index,
					vecOffset, size);
				if (status != B_OK)
					return status;
			}
			// (a read needs no clearing: the card writes all of it, and a
			// failed transfer is not copied out)
			memory_full_barrier();
			if (fPlatform.dma_coherent)
				sdhci_clean_dma_alias(fDMAAddress, size);
		}
		if (fAdmaTable != NULL && bounce) {
			// 32-bit ADMA2: attribute (valid, end, transfer), a length of up
			// to 64 KiB (0: 64 KiB), the address
			uint32 descriptor = 0;
			for (generic_size_t done = 0; done < size; done += 65536) {
				uint32 length = std::min((generic_size_t)65536, size - done);
				uint32 attribute = 0x21 | (done + length >= size ? 0x02 : 0);
				fAdmaTable[2 * descriptor] = attribute
					| (length == 65536 ? 0 : length) << 16;
				fAdmaTable[2 * descriptor + 1] = fDMAAddress + done;
				descriptor++;
			}
			memory_full_barrier();
			if (fPlatform.dma_coherent)
				sdhci_clean_dma_alias(fAdmaAddress, 8 * descriptor);
			fRegisters->host_control.SetDMAMode(HostControl::kAdma32);
			fRegisters->adma_system_address = fAdmaAddress;
		} else if (table) {
			// (the table is in ordinary memory: x86 DMA snoops the caches)
			memory_full_barrier();
			fRegisters->host_control.SetDMAMode(HostControl::kAdma32);
			// as two 32-bit writes: not every controller (nor QEMU's)
			// takes a 64-bit one
			volatile uint32* admaAddress = (volatile uint32*)
				((volatile uint8*)fRegisters
					+ offsetof(struct registers, adma_system_address));
			admaAddress[0] = (uint32)fAdmaAddress;
			admaAddress[1] = 0;
		} else {
			fRegisters->host_control.SetDMAMode(HostControl::kSdma);
			fRegisters->system_address = bounce ? fDMAAddress : pieceAddress;
		}
		fRegisters->block_size.ConfigureTransfer(blockSize, BlockSize::kDmaBoundary512K);
		fRegisters->block_count = size / blockSize;
		uint32_t response = 0;
		status_t status = ExecuteCommand(command, offset / unit, &response);
		if (status != B_OK)
			return status;
		if ((response & kMmcR1ErrorMask) != 0) {
			RecoverError();
			return B_IO_ERROR;
		}
		status = WaitForCompletion(SDHCI_INT_TRANS_CMP, 1000000);
		if (status != B_OK) {
			ERROR("Data transfer failed: command=%u offset=%" B_PRIu64
				" bytes=%" B_PRIu64 " status=%s result=%#x IRQ=%#x signal=%#x"
				" present=%#x CPU=%#" B_PRIx64 " DMA32=%#" B_PRIx64 "\n",
				command, (uint64)offset, (uint64)size, strerror(status),
				(uint32)atomic_get(&fCommandResult), (uint32)fRegisters->interrupt_status,
				(uint32)fRegisters->interrupt_signal_enable, fRegisters->present_state.Bits(),
				pieceAddress, (uint64)(table ? fAdmaAddress : fDMAAddress));
			RecoverError();
			return status;
		}
		memory_full_barrier();
		if (fRegisters->present_state.DataInhibit()) {
			RecoverError();
			return B_IO_ERROR;
		}
		if (bounce && !operation->IsWrite()) {
			status = _CopyVecs(false, vecs, count, index, vecOffset, size);
			if (status != B_OK)
				return status;
		} else if (!bounce) {
			// past the memory the command took
			for (generic_size_t done = size; done != 0;) {
				generic_size_t piece = std::min(done,
					vecs[index].length - vecOffset);
				vecOffset += piece;
				done -= piece;
				if (vecOffset == vecs[index].length && done != 0) {
					index++;
					vecOffset = 0;
				}
			}
		}
		length -= size;
		offset += size;
	}
	if (fPlatform.cpu_address_floor != 0) {
		bool& reported = operation->IsWrite() ? fReportedHighCpuWrite : fReportedHighCpuRead;
		if (!reported) {
			TRACE_ALWAYS("CPU buffer %s completed: min=%#" B_PRIx64 " bytes=%" B_PRIu64
				" floor=%#" B_PRIx64 " DMA32=%#" B_PRIx64 "\n",
				operation->IsWrite() ? "write" : "read", cpuAddress,
				(uint64)operation->Length(), fPlatform.cpu_address_floor, (uint64)fDMAAddress);
			reported = true;
		}
	}
	return B_OK;
}


/*!	Copies the next \a size bytes of the operation's memory to the private
	buffer or back, and moves \a index and \a vecOffset past them.
*/
status_t
SdhciBus::_CopyVecs(bool toBuffer, const generic_io_vec* vecs, size_t count,
	size_t& index, generic_size_t& vecOffset, generic_size_t size)
{
	uint8* buffer = (uint8*)fDMABuffer;
	while (size != 0) {
		if (index >= count)
			return B_BAD_VALUE;
		if (vecOffset == vecs[index].length) {
			index++;
			vecOffset = 0;
			continue;
		}
		generic_size_t piece = std::min(size, vecs[index].length - vecOffset);
		status_t status = toBuffer
			? vm_memcpy_from_physical(buffer, vecs[index].base + vecOffset,
				piece, false)
			: vm_memcpy_to_physical(vecs[index].base + vecOffset, buffer,
				piece, false);
		if (status != B_OK)
			return status;
		buffer += piece;
		vecOffset += piece;
		size -= piece;
	}
	return B_OK;
}


/*!	Describes the operation's memory from \a index and \a vecOffset on in
	the ADMA2 table, as much of \a length as fits, and returns how many bytes
	that is (whole blocks): the next command's.
*/
generic_size_t
SdhciBus::_FillAdmaTable(const generic_io_vec* vecs, size_t count,
	size_t index, generic_size_t vecOffset, generic_size_t length)
{
	const generic_size_t kMaxCommand = 0xffff * 512;
		// the block count register's limit
	uint32 descriptor = 0;
	generic_size_t described = 0;
	while (index < count && described < length
		&& descriptor < kSdhciAdmaTableEntries) {
		if (vecOffset == vecs[index].length) {
			index++;
			vecOffset = 0;
			continue;
		}
		generic_size_t piece = std::min(
			std::min(length, kMaxCommand) - described,
			std::min(vecs[index].length - vecOffset,
				(generic_size_t)kSdhciAdmaMaxLength));
		if (piece == 0)
			break;
		// valid, transfer data; the length, the 32-bit address
		fAdmaTable[2 * descriptor] = 0x21 | (uint32)piece << 16;
		fAdmaTable[2 * descriptor + 1] = (uint32)(vecs[index].base + vecOffset);
		descriptor++;
		vecOffset += piece;
		described += piece;
	}
	// A command takes whole blocks: should the table be full in the middle
	// of one, leave that block's pieces to the next command.
	while (described % 512 != 0 && descriptor > 0) {
		descriptor--;
		described -= fAdmaTable[2 * descriptor] >> 16;
	}
	if (descriptor == 0)
		return 0;
	fAdmaTable[2 * (descriptor - 1)] |= 0x02;
		// the end
	return described;
}


status_t
SdhciBus::ReadExtendedCsd(uint8_t data[512])
{
	if (data == NULL || !is_mmc_card(fCardType))
		return B_BAD_VALUE;
	return _ReadDataBlock(MMC_SEND_EXT_CSD, 0, data, 512);
}


status_t
SdhciBus::SwitchSdFunction(uint32_t argument, uint8_t data[64])
{
	if (data == NULL || is_mmc_card(fCardType))
		return B_BAD_VALUE;
	return _ReadDataBlock(SD_SWITCH_FUNCTION, argument, data, 64);
}


/*!	SDIO CMD53 and its data phase, by PIO through the buffer data port: a
	byte mode transfer (\a blockCount 0) of \a blockSize bytes, or
	\a blockCount blocks. The caller has built the argument.
*/
status_t
SdhciBus::SdioTransfer(uint32_t argument, bool write, void* _buffer,
	uint16_t blockSize, uint16_t blockCount, uint32_t* response)
{
	if (fStatus != B_OK)
		return fStatus;
	if (_buffer == NULL || response == NULL || blockSize == 0
		|| blockSize > 2048 || (blockCount == 0 && blockSize > 512))
		return B_BAD_VALUE;

	uint16_t blocks = blockCount == 0 ? 1 : blockCount;
	fRegisters->block_size.ConfigureTransfer(blockSize,
		BlockSize::kDmaBoundary512K);
	fRegisters->block_count = blocks;
	fSdioTransferMode = (write ? TransferMode::kWrite : TransferMode::kRead)
		| (blocks > 1 ? TransferMode::kMulti | TransferMode::kBlockCountEnable
			: 0);
		// no auto CMD12: CMD53 ends by its count

	fSdioTransfer = true;
	status_t status = ExecuteCommand(SD_IO_RW_EXTENDED, argument, response);
	fSdioTransfer = false;
	if (status != B_OK)
		return status;
	if ((*response & 0xcb00) != 0) {
		// R5 error flags: there may be no data phase
		RecoverError();
		return B_IO_ERROR;
	}

	uint8* buffer = (uint8*)_buffer;
	uint32 ready = write ? (1 << 10) : (1 << 11);
		// buffer write/read enable in the present state
	bigtime_t deadline = system_time() + 1000000;
	for (uint16_t block = 0; block < blocks; block++) {
		while ((fRegisters->present_state.Bits() & ready) == 0) {
			if ((atomic_get(&fCommandResult) & SDHCI_INT_ERROR_MASK) != 0) {
				RecoverError();
				return B_IO_ERROR;
			}
			if (system_time() >= deadline) {
				ERROR("SDIO %s: buffer not ready (block %u of %u, present "
					"%#" B_PRIx32 ")\n", write ? "write" : "read", block,
					blocks, fRegisters->present_state.Bits());
				RecoverError();
				return B_TIMED_OUT;
			}
			spin(5);
		}
		for (uint16_t offset = 0; offset < blockSize; offset += 4) {
			uint32_t bytes = blockSize - offset < 4 ? blockSize - offset : 4;
			uint32_t word = 0;
			if (write) {
				memcpy(&word, buffer + offset, bytes);
				fRegisters->buffer_data_port = B_HOST_TO_LENDIAN_INT32(word);
			} else {
				word = B_LENDIAN_TO_HOST_INT32(fRegisters->buffer_data_port);
				memcpy(buffer + offset, &word, bytes);
			}
		}
		buffer += blockSize;
	}
	fRegisters->interrupt_status = SDHCI_INT_BUF_READ_READY
		| SDHCI_INT_BUF_WRITE_READY;

	status = WaitForCompletion(SDHCI_INT_TRANS_CMP, 1000000);
	if (status != B_OK) {
		ERROR("SDIO %s of %u x %u bytes did not complete: %s\n",
			write ? "write" : "read", blocks, blockSize, strerror(status));
		RecoverError();
	}
	return status;
}


bool
SdhciBus::HighSpeedCapable()
{
	return fRegisters->capabilities.HighSpeed();
}


void
SdhciBus::SetSdioInterruptHandler(void (*handler)(void*), void* data)
{
	cpu_status state = disable_interrupts();
	acquire_spinlock(&fInterruptLock);
	fSdioHandler = handler;
	fSdioHandlerData = data;
	release_spinlock(&fInterruptLock);
	restore_interrupts(state);
}


void
SdhciBus::EnableSdioInterrupt(bool enable)
{
	cpu_status state = disable_interrupts();
	acquire_spinlock(&fInterruptLock);
	fSdioInterruptEnabled = enable;
	if (enable) {
		fRegisters->interrupt_status_enable |= SDHCI_INT_CARD_INT;
		fRegisters->interrupt_signal_enable |= SDHCI_INT_CARD_INT;
	} else {
		fRegisters->interrupt_signal_enable &= ~SDHCI_INT_CARD_INT;
		fRegisters->interrupt_status_enable &= ~SDHCI_INT_CARD_INT;
	}
	memory_full_barrier();
	release_spinlock(&fInterruptLock);
	restore_interrupts(state);
}


/*!	A command with a single short data block to read by PIO (EXT_CSD, the SD
	switch status).
*/
status_t
SdhciBus::_ReadDataBlock(uint8_t command, uint32_t argument, uint8_t* data,
	uint32_t length)
{
	if (fStatus != B_OK)
		return fStatus;
	fRegisters->block_size.ConfigureTransfer(length, BlockSize::kDmaBoundary4K);
	fRegisters->block_count = 1;
	uint32_t response = 0;
	fReadingDataBlock = true;
	status_t status = ExecuteCommand(command, argument, &response);
	fReadingDataBlock = false;
	if (status != B_OK)
		return status;
	if ((response & kMmcR1ErrorMask) != 0) {
		RecoverError();
		return B_IO_ERROR;
	}

	bigtime_t deadline = system_time() + 1000000;
	for (unsigned offset = 0; offset < length; offset += sizeof(uint32_t)) {
		while ((fRegisters->present_state.Bits() & (1 << 11)) == 0) {
			if ((atomic_get(&fCommandResult) & SDHCI_INT_ERROR_MASK) != 0) {
				RecoverError();
				return B_IO_ERROR;
			}
			if (system_time() >= deadline) {
				RecoverError();
				return B_TIMED_OUT;
			}
			snooze(10);
		}
		uint32_t word = fRegisters->buffer_data_port;
		for (unsigned byte = 0; byte < sizeof(word); byte++)
			data[offset + byte] = word >> (8 * byte);
	}
	fRegisters->interrupt_status = SDHCI_INT_BUF_READ_READY;
	status = WaitForCompletion(SDHCI_INT_TRANS_CMP, 1000000);
	if (status != B_OK)
		RecoverError();
	return status;
}


void
SdhciBus::SetScanSemaphore(sem_id sem)
{
	fScanSemaphore = sem;
	if (sem < B_OK) {
		fRegisters->interrupt_signal_enable &= ~SDHCI_INT_CARD_INS;
		return;
	}

	// If there is already a card in, start a scan immediately
	if ((fSlotQuirks & SDHCI_SLOT_NON_REMOVABLE) != 0) {
		// soldered: there is nothing to wait for
		release_sem(fScanSemaphore);
		return;
	}
	if (fRegisters->present_state.IsCardInserted())
		release_sem(fScanSemaphore);

	// We can now enable the card insertion interrupt for next time a card
	// is inserted
	EnableInterrupts(SDHCI_INT_CARD_INS);
}


void
SdhciBus::SetBusWidth(int width)
{
	uint8_t widthBits;
	switch(width) {
		case 1:
			widthBits = HostControl::kDataTransfer1Bit;
			break;
		case 4:
			widthBits = HostControl::kDataTransfer4Bit;
			break;
		case 8:
			widthBits = HostControl::kDataTransfer8Bit;
			break;
		default:
			panic("Incorrect bitwidth value");
			return;
	}
	fRegisters->host_control.SetDataTransferWidth(widthBits);
}


void
SdhciBus::SetCardType(card_type type)
{
	fCardType = type;
}


bool
SdhciBus::PowerOn()
{
	if ((fSlotQuirks & SDHCI_SLOT_NON_REMOVABLE) == 0
		&& !fRegisters->present_state.IsCardInserted()) {
		TRACE("Card not inserted, not powering on for now\n");
		return false;
	}

	uint8_t supportedVoltages = fRegisters->capabilities.SupportedVoltages();
	uint8_t voltage;
	if ((supportedVoltages & Capabilities::k3v3) != 0)
		voltage = PowerControl::k3v3;
	else if ((supportedVoltages & Capabilities::k3v0) != 0)
		voltage = PowerControl::k3v0;
	else if ((supportedVoltages & Capabilities::k1v8) != 0)
		voltage = PowerControl::k1v8;
	else {
		fRegisters->power_control.PowerOff();
		ERROR("No voltage is supported\n");
		return false;
	}
	fRegisters->power_control.SetVoltage(voltage);

	if ((fSlotQuirks & SDHCI_SLOT_POWER_RETRY) != 0) {
		// Intel: bus power may not take while the card's presence has not
		// propagated after D3; write it again, for up to 2 ms (Linux:
		// sdhci_intel_set_power)
		for (int i = 0; i < 20 && (fRegisters->power_control.Bits() & 1) == 0;
				i++) {
			spin(100);
			fRegisters->power_control.SetVoltage(voltage);
		}
		if ((fRegisters->power_control.Bits() & 1) == 0)
			ERROR("bus power does not come on\n");
	}

	return true;
}


void
SdhciBus::PowerOff()
{
	fRegisters->power_control.PowerOff();
}


void
SdhciBus::TerminateBus()
{
	CALLED();

	DisableInterrupts();
	fRegisters->clock_control.DisableSD();
	PowerOff();
	/*
	// Debugging.
	uint8_t powerBits = fRegisters->power_control.Bits();
	uint16_t clockBits = fRegisters->clock_control.Bits();
	if ((powerBits & 0x1) != 0 || (clockBits & (1 << 2)) != 0) {
		ERROR("TerminateBus: Not killed. "
			"(power=%#x, clock=%#x)\n", powerBits, clockBits);
	} else {
		TRACE("TerminateBus: killed. (power=%#x, "
			"clock=%#x)\n", powerBits, clockBits);
	}
	*/
}


void
SdhciBus::RecoverError()
{
	uint32_t signals = fRegisters->interrupt_signal_enable;
	fRegisters->interrupt_signal_enable = 0;
	memory_full_barrier();
	if (!fRegisters->software_reset.ResetCommandAndDataLines()) {
		fStatus = B_TIMED_OUT;
		fDMAQuarantined = true;
		ERROR("Command/data reset timed out; controller disabled\n");
		return;
	}
	fRegisters->interrupt_status = SDHCI_INT_CMD_CMP | SDHCI_INT_TRANS_CMP
		| SDHCI_INT_ERROR | SDHCI_INT_ERROR_MASK
		| SDHCI_INT_BUF_READ_READY | SDHCI_INT_BUF_WRITE_READY;
	atomic_set(&fCommandResult, 0);
	// The card interrupt may have come (and been masked) in the meantime.
	cpu_status state = disable_interrupts();
	acquire_spinlock(&fInterruptLock);
	signals &= ~SDHCI_INT_CARD_INT;
	if (fSdioInterruptEnabled)
		signals |= SDHCI_INT_CARD_INT;
	fRegisters->interrupt_signal_enable = signals;
	memory_full_barrier();
	release_spinlock(&fInterruptLock);
	restore_interrupts(state);
}

int32
SdhciBus::HandleInterrupt()
{
#if 0
	// We could use the slot register to quickly see for which slot the
	// interrupt is. But since we have an interrupt handler call for each slot
	// anyway, it's just as simple to let each of them scan its own interrupt
	// status register.
	if ( !(fRegisters->slot_interrupt_status & (1 << fSlot)) ) {
		TRACE("interrupt not for me.\n");
		return B_UNHANDLED_INTERRUPT;
	}
#endif
	
	uint32_t intmask = fRegisters->interrupt_status;

	// Shortcut: exit early if there is no interrupt or if the register is
	// clearly invalid.
	if ((intmask == 0) || (intmask == 0xffffffff)) {
		return B_UNHANDLED_INTERRUPT;
	}
	// Status-only events (PIO readiness and SDMA boundary notifications)
	// can remain latched while another device asserts this shared IRQ.
	// Claiming those events would prevent the next level-triggered handler
	// from running, including USB controllers on the same PCI interrupt.
	intmask &= fRegisters->interrupt_signal_enable;
	if (intmask == 0)
		return B_UNHANDLED_INTERRUPT;

	TRACE("interrupt function called %x\n", intmask);

	// handling card presence interrupts
	if ((intmask & SDHCI_INT_CARD_REM) != 0) {
		// We can get spurious interrupts as the card is inserted or removed,
		// so check the actual state before acting
		if (!fRegisters->present_state.IsCardInserted())
			fRegisters->power_control.PowerOff();
		else
			TRACE("Card removed interrupt, but card is inserted\n");

		fRegisters->interrupt_status = SDHCI_INT_CARD_REM;
		TRACE("Card removal interrupt handled\n");
	}

	if ((intmask & SDHCI_INT_CARD_INS) != 0) {
		// We can get spurious interrupts as the card is inserted or removed,
		// so check the actual state before acting
		if (fRegisters->present_state.IsCardInserted()) {
			if (fScanSemaphore >= B_OK)
				release_sem_etc(fScanSemaphore, 1, B_DO_NOT_RESCHEDULE);
		} else
			TRACE("Card insertion interrupt, but card is removed\n");

		fRegisters->interrupt_status = SDHCI_INT_CARD_INS;
		TRACE("Card presence interrupt handled\n");
	}

	if ((intmask & SDHCI_INT_CARD_INT) != 0) {
		// The SDIO card's interrupt follows its DAT1 line: it is cleared
		// only by disabling it, until the card's functions have been
		// served. The handler (the MMC bus) has them served and enables it
		// again. (The polling thread may get here as well as the interrupt.)
		cpu_status state = disable_interrupts();
		acquire_spinlock(&fInterruptLock);
		bool enabled = fSdioInterruptEnabled;
		if (enabled) {
			fSdioInterruptEnabled = false;
			fRegisters->interrupt_signal_enable &= ~SDHCI_INT_CARD_INT;
			fRegisters->interrupt_status_enable &= ~SDHCI_INT_CARD_INT;
			memory_full_barrier();
		}
		void (*handler)(void*) = fSdioHandler;
		void* handlerData = fSdioHandlerData;
		release_spinlock(&fInterruptLock);
		restore_interrupts(state);
		if (enabled && handler != NULL)
			handler(handlerData);
	}

	uint32_t completion = intmask & (SDHCI_INT_CMD_CMP | SDHCI_INT_TRANS_CMP
		| SDHCI_INT_ERROR | SDHCI_INT_ERROR_MASK);
	if (completion != 0) {
		// A waiter on another CPU can observe fCommandResult before NotifyAll.
		// Finish the acknowledgement before allowing it to submit the next
		// command, otherwise this W1C could erase that command's completion.
		fRegisters->interrupt_status = completion;
		memory_full_barrier();
		atomic_or(&fCommandResult, completion);
		fInterruptNotifier.NotifyAll();
	}

	// Check that all interrupts have been cleared (we check all the ones we
	// enabled, so that should always be the case)
	intmask = fRegisters->interrupt_status & fRegisters->interrupt_signal_enable;
	if (intmask != 0) {
		ERROR("Remaining interrupts at end of handler: %x\n", intmask);
	}

	return B_HANDLED_INTERRUPT;
}


status_t
SdhciBus::_WorkerThread(void* cookie)
{
	SdhciBus* bus = (SdhciBus*)cookie;
	while (bus->fStatus != B_SHUTTING_DOWN) {
		bus->HandleInterrupt();
		// A look every 100 us is 10,000 wake-ups a second for each slot,
		// card or no card, and keeps a processor out of its deep idle
		// states (#182). Once the interrupt has been seen to arrive, it
		// does the work, and this is only a safety net.
		snooze(atomic_get(&bus->fInterruptArrived) != 0 ? 100000 : 100);
	}
	return B_OK;
}


void
uninit_bus(void* bus_cookie)
{
	SdhciBus* bus = (SdhciBus*)bus_cookie;
	delete bus;

	// FIXME do we need to put() the PCI module here?
}


void
bus_removed(void* bus_cookie)
{
	return;
}


static status_t
register_child_devices(void* cookie)
{
	CALLED();
	SdhciDevice* context = (SdhciDevice*)cookie;
	status_t status = B_OK;
	const char* bus;
	device_node* parent = gDeviceManager->get_parent_node(context->fNode);
	status = gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false);
	if (status != B_OK) {
		TRACE("Could not find required attribute device/bus\n");
		return status;
	}

	if (strcmp(bus, "pci") == 0)
		status = register_child_devices_pci(cookie);
	else if (strcmp(bus, "acpi") == 0)
		status = register_child_devices_acpi(cookie);
	else
		status = B_BAD_VALUE;

	return status;
}


static status_t
init_device(device_node* node, void** device_cookie)
{
	CALLED();

	SdhciDevice* context = new(std::nothrow)SdhciDevice;
	if (context == NULL)
		return B_NO_MEMORY;
	context->fNode = node;
	*device_cookie = context;

	status_t status = B_OK;
	const char* bus;
	device_node* parent = gDeviceManager->get_parent_node(node);
	status = gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false);
	if (status != B_OK) {
		TRACE("Could not find required attribute device/bus\n");
		return status;
	}

	if (strcmp(bus, "pci") == 0)
		return init_device_pci(node, context);

	return B_OK;
}


static void
uninit_device(void* device_cookie)
{
	SdhciDevice* context = (SdhciDevice*)device_cookie;
	device_node* parent = gDeviceManager->get_parent_node(context->fNode);

	const char* bus;
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK) {
		TRACE("Could not find required attribute device/bus\n");
	}

	if (strcmp(bus, "pci") == 0)
		uninit_device_pci(context, parent);

	gDeviceManager->put_node(parent);

	delete context;
}


static status_t
register_device(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "SD Host Controller"}},
		{}
	};

	return gDeviceManager->register_node(parent, SDHCI_DEVICE_MODULE_NAME,
		attrs, NULL, NULL);
}


static float
supports_device(device_node* parent)
{
	const char* bus;

	// make sure parent is either an ACPI or PCI SDHCI device node
	if (gDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
		!= B_OK) {
		TRACE("Could not find required attribute device/bus\n");
		return -1;
	}

	if (strcmp(bus, "pci") == 0)
		return supports_device_pci(parent);
	else if (strcmp(bus, "acpi") == 0)
		return supports_device_acpi(parent);

	return 0.0f;
}


module_dependency module_dependencies[] = {
	{ MMC_BUS_MODULE_NAME, (module_info**)&gMMCBusController},
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{}
};

status_t
set_clock(void* controller, uint32_t kilohertz)
{
	SdhciBus* bus = (SdhciBus*)controller;

	// SDIO high speed asks for 50 MHz and goes back to 25 if refused; a
	// controller without high speed timing does not get there.
	if (kilohertz > 25000 && bus->CardType() == CARD_TYPE_SDIO
		&& !bus->HighSpeedCapable())
		return B_NOT_SUPPORTED;

	bus->SetClock(kilohertz, true);
	return bus->InitCheck();
}


status_t
read_extended_csd(void* controller, uint8_t data[512])
{
	return ((SdhciBus*)controller)->ReadExtendedCsd(data);
}


status_t
switch_sd_function(void* controller, uint32_t argument, uint8_t data[64])
{
	return ((SdhciBus*)controller)->SwitchSdFunction(argument, data);
}


status_t
execute_command(void* controller, uint8_t command, uint32_t argument,
	uint32_t* response)
{
	SdhciBus* bus = (SdhciBus*)controller;
	return bus->ExecuteCommand(command, argument, response);
}


status_t
do_io(void* controller, uint8_t command, IOOperation* operation,
	bool offsetAsSectors)
{
	SdhciBus* bus = (SdhciBus*)controller;
	return bus->DoIO(command, operation, offsetAsSectors);
}


void
set_scan_semaphore(void* controller, sem_id sem)
{
	SdhciBus* bus = (SdhciBus*)controller;
	return bus->SetScanSemaphore(sem);
}


void
set_bus_width(void* controller, int width)
{
	SdhciBus* bus = (SdhciBus*)controller;
	return bus->SetBusWidth(width);
}


void
set_card_type(void* controller, card_type type)
{
	SdhciBus* bus = (SdhciBus*)controller;
	bus->SetCardType(type);
}


void
terminate_bus(void* controller)
{
	SdhciBus* bus = (SdhciBus*)controller;
	bus->TerminateBus();
}


status_t
sdio_transfer(void* controller, uint32_t argument, bool write, void* buffer,
	uint16_t blockSize, uint16_t blockCount, uint32_t* response)
{
	return ((SdhciBus*)controller)->SdioTransfer(argument, write, buffer,
		blockSize, blockCount, response);
}


void
set_sdio_interrupt_handler(void* controller, void (*handler)(void*),
	void* data)
{
	((SdhciBus*)controller)->SetSdioInterruptHandler(handler, data);
}


void
enable_sdio_interrupt(void* controller, bool enable)
{
	((SdhciBus*)controller)->EnableSdioInterrupt(enable);
}


// Root device that binds to the ACPI or PCI bus. It will register an mmc_bus_interface
// node for each SD slot in the device.
static driver_module_info sSDHCIDevice = {
	{
		SDHCI_DEVICE_MODULE_NAME,
		0,
		NULL
	},
	supports_device,
	register_device,
	init_device,
	uninit_device,
	register_child_devices,
	NULL,	// rescan
	NULL,	// device removed
};


module_info* modules[] = {
	(module_info* )&sSDHCIDevice,
	(module_info* )&gSDHCIPCIDeviceModule,
	(module_info* )&gSDHCIACPIDeviceModule,
	(module_info* )&gSDHCIFDTDriverModule,
	(module_info* )&gSDHCIFDTDeviceModule,
#if defined(__aarch64__)
	(module_info* )&gSDHCIQcomDriverModule,
	(module_info* )&gSDHCIQcomDeviceModule,
#endif
	NULL
};
