/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	SDIO cards on the MMC bus: finding and setting up the card, publishing
	its functions, their I/O and their interrupts. The protocol itself is in
	sdio_card.cpp. */


#include "mmc_bus.h"

#include <Errors.h>

#include <string.h>


//	#pragma mark - the protocol on this bus


bool
MMCBus::_SdioCapable() const
{
	return fController->sdio_transfer != NULL;
}


/*static*/ status_t
MMCBus::_SdioCommand(void* cookie, uint8 command, uint32 argument,
	uint32* response)
{
	MMCBus* bus = (MMCBus*)cookie;
	// CMD52 goes to the selected card; CMD5 and CMD3 come before it has an
	// address.
	if (command == SD_IO_RW_DIRECT && bus->fSdioRca != 0)
		return bus->ExecuteCommand(bus->fSdioRca, command, argument, response);
	return bus->fController->execute_command(bus->fCookie, command, argument,
		response);
}


/*static*/ status_t
MMCBus::_SdioHostTransfer(void* cookie, uint32 argument, bool write,
	void* buffer, uint16 blockSize, uint16 blockCount, uint32* response)
{
	MMCBus* bus = (MMCBus*)cookie;
	if (bus->fSdioRca == 0)
		return B_DEVICE_NOT_FOUND;
	status_t status = bus->_ActivateDevice(bus->fSdioRca);
	if (status != B_OK)
		return status;
	return bus->fController->sdio_transfer(bus->fCookie, argument, write,
		buffer, blockSize, blockCount, response);
}


/*static*/ void
MMCBus::_SdioSleep(void* cookie, bigtime_t duration)
{
	snooze(duration);
}


/*static*/ bigtime_t
MMCBus::_SdioTime(void* cookie)
{
	return system_time();
}


sdio_host_ops
MMCBus::_SdioHostOps()
{
	sdio_host_ops ops = {
		this,
		_SdioCommand,
		_SdioHostTransfer,
		_SdioSleep,
		_SdioTime
	};
	return ops;
}


status_t
MMCBus::_SdioDirect(bool write, uint8 function, uint32 address, uint8 value,
	uint8* _value, bool readAfterWrite)
{
	sdio_host_ops ops = _SdioHostOps();
	return sdio_rw_direct(ops, write, function, address, value, _value,
		readAfterWrite);
}


/*!	Identifies an I/O card and sets it up: address, selection, four data
	lines and high speed if host and card have them. The bus is locked; on
	success fSdioRca and fSdio describe the card.
*/
status_t
MMCBus::_ProbeSdio()
{
	fSdioRca = 0;
	SetCardType(CARD_TYPE_SDIO);
		// CMD3 then answers with R6 on every host

	// An I/O card keeps its state over CMD0 (a previous driver, or the
	// firmware, may have set it up): reset its I/O part first. A card that
	// is not selected ignores this. (Linux does it before every scan.)
	uint32 response;
	if (fActiveDevice != 0) {
		fController->execute_command(fCookie, SELECT_DESELECT_CARD, 0, NULL);
		fActiveDevice = 0;
	}
	fController->execute_command(fCookie, SD_IO_RW_DIRECT,
		sdio_direct_argument(true, 0, SDIO_CCCR_ABORT, SDIO_ABORT_RESET,
			false), &response);
	fController->execute_command(fCookie, GO_IDLE_STATE, 0, NULL);
	snooze(2000);

	sdio_host_ops ops = _SdioHostOps();
	uint32 ocr;
	status_t status = sdio_negotiate_ocr(ops, 0x00ff8000, &ocr);
		// 2.7 to 3.6 V: the hosts supply 3.3 V
	if (status != B_OK) {
		TRACE("no SDIO card: %s\n", strerror(status));
		SetCardType(CARD_TYPE_UNKNOWN);
		return status;
	}
	if ((ocr & SDIO_OCR_MEMORY_PRESENT) != 0)
		TRACE_ALWAYS("SDIO combo card: its memory is not used\n");

	status = fController->execute_command(fCookie, SD_SEND_RELATIVE_ADDR, 0,
		&response);
	uint16 rca = response >> 16;
	if (status != B_OK || rca == 0) {
		ERROR("SDIO card: no address (%s, %#" B_PRIx32 ")\n",
			strerror(status), response);
		return status != B_OK ? status : B_BAD_DATA;
	}
	fSdioRca = rca;

	// The card's capabilities first, for its clock: every card but a low
	// speed one runs at 25 MHz from here on.
	uint8 capabilities = 0;
	status = _SdioDirect(false, 0, SDIO_CCCR_CAPABILITY, 0, &capabilities,
		false);
	if (status != B_OK) {
		ERROR("SDIO card at %#x does not answer CMD52: %s\n", rca,
			strerror(status));
		fSdioRca = 0;
		return status;
	}
	bool lowSpeed = (capabilities & SDIO_CAP_LSC) != 0;
	if (!lowSpeed && SetClock(25000) != B_OK) {
		fSdioRca = 0;
		return B_ERROR;
	}

	status = sdio_read_card(ops, ocr, &fSdio);
	if (status != B_OK) {
		ERROR("SDIO card at %#x: %s\n", rca, strerror(status));
		fSdioRca = 0;
		return status;
	}

	// four data lines, unless the card is a one bit low speed card
	uint8 maxWidth = 4;
	gDeviceManager->get_attr_uint8(fNode, kMmcMaxBusWidthAttribute, &maxWidth,
		true);
	fSdio.info.bus_width = 1;
	if (maxWidth >= 4 && (!lowSpeed || (capabilities & SDIO_CAP_4BLS) != 0)) {
		uint8 value;
		status = _SdioDirect(false, 0, SDIO_CCCR_BUS_INTERFACE, 0, &value,
			false);
		if (status == B_OK) {
			value = (value & ~SDIO_BUS_WIDTH_MASK) | SDIO_BUS_WIDTH_4BIT;
			status = _SdioDirect(true, 0, SDIO_CCCR_BUS_INTERFACE, value,
				NULL, false);
		}
		if (status == B_OK) {
			SetBusWidth(4);
			fSdio.info.bus_width = 4;
		} else
			ERROR("SDIO card stays at one data line: %s\n", strerror(status));
	}

	// high speed (50 MHz) where the host has it and the card too (CCCR 1.20
	// on); otherwise what the CIS says, up to 25 MHz
	uint32 clock = lowSpeed ? 400 : min_c(fSdio.max_clock, (uint32)25000);
	uint8 hostHighSpeed = 0;
	gDeviceManager->get_attr_uint8(fNode, kMmcSdioHighSpeedAttribute,
		&hostHighSpeed, true);
	uint8 speed = 0;
	if (hostHighSpeed != 0 && !lowSpeed && fSdio.info.cccr_revision >= 2
		&& _SdioDirect(false, 0, SDIO_CCCR_SPEED, 0, &speed, false) == B_OK
		&& (speed & SDIO_SPEED_SHS) != 0) {
		uint8 readBack = 0;
		if (_SdioDirect(true, 0, SDIO_CCCR_SPEED, speed | SDIO_SPEED_EHS,
				&readBack, true) == B_OK
			&& (readBack & SDIO_SPEED_EHS) != 0) {
			clock = 50000;
			fSdio.info.high_speed = true;
		}
	}
	if (clock != 25000 || lowSpeed) {
		status = SetClock(clock);
		if (status != B_OK && fSdio.info.high_speed) {
			// back to default speed
			fSdio.info.high_speed = false;
			_SdioDirect(true, 0, SDIO_CCCR_SPEED, speed & ~SDIO_SPEED_EHS,
				NULL, false);
			clock = 25000;
			status = SetClock(clock);
		}
		if (status != B_OK) {
			fSdioRca = 0;
			return status;
		}
	}
	fSdio.info.clock = clock;

	TRACE_ALWAYS("SDIO card %04x:%04x at RCA %#x: %u function(s), CCCR %u, "
		"SDIO %u, capabilities %#x, %u-bit, %" B_PRIu32 " kHz%s\n",
		fSdio.info.vendor, fSdio.info.device, rca, fSdio.info.function_count,
		fSdio.info.cccr_revision, fSdio.info.sdio_revision,
		fSdio.info.capabilities, fSdio.info.bus_width, clock,
		fSdio.info.high_speed ? " (high speed)" : "");
	return B_OK;
}


bool
MMCBus::_SdioAlive()
{
	uint8 revision;
	return fSdioRca != 0
		&& _SdioDirect(false, 0, SDIO_CCCR_REVISION, 0, &revision, false)
			== B_OK;
}


void
MMCBus::_PublishSdioFunctions()
{
	for (uint8 function = 1; function <= fSdio.info.function_count;
			function++) {
		char name[64];
		snprintf(name, sizeof(name), "SDIO function %u (%04x:%04x)", function,
			fSdio.function_vendor[function], fSdio.function_device[function]);
		TRACE_ALWAYS("function %u: %04x:%04x, class %u, max block size %u\n",
			function, fSdio.function_vendor[function],
			fSdio.function_device[function], fSdio.function_class[function],
			fSdio.info.max_block_size[function]);

		device_attr attrs[] = {
			{ B_DEVICE_BUS, B_STRING_TYPE, { .string = "mmc" }},
			{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = name }},
			{ B_DEVICE_FLAGS, B_UINT32_TYPE, { .ui32 = B_KEEP_DRIVER_LOADED }},
				// legacy drivers find the node and use its cookie
			{ kMmcTypeAttribute, B_UINT8_TYPE, { .ui8 = CARD_TYPE_SDIO }},
			{ kMmcRcaAttribute, B_UINT16_TYPE, { .ui16 = fSdioRca }},
			{ SDIO_VENDOR_ATTRIBUTE, B_UINT16_TYPE,
				{ .ui16 = fSdio.function_vendor[function] }},
			{ SDIO_DEVICE_ATTRIBUTE, B_UINT16_TYPE,
				{ .ui16 = fSdio.function_device[function] }},
			{ SDIO_FUNCTION_ATTRIBUTE, B_UINT8_TYPE, { .ui8 = function }},
			{ SDIO_CLASS_ATTRIBUTE, B_UINT8_TYPE,
				{ .ui8 = fSdio.function_class[function] }},
			{ SDIO_MAX_BLOCK_SIZE_ATTRIBUTE, B_UINT16_TYPE,
				{ .ui16 = fSdio.info.max_block_size[function] }},
			{ SDIO_FUNCTION_COUNT_ATTRIBUTE, B_UINT8_TYPE,
				{ .ui8 = fSdio.info.function_count }},
			{}
		};
		status_t status = gDeviceManager->register_node(fNode,
			SDIO_FUNCTION_MODULE_NAME, attrs, NULL, NULL);
		if (status != B_OK && status != B_NAME_IN_USE) {
			ERROR("cannot publish SDIO function %u: %s\n", function,
				strerror(status));
		}
	}
}


//	#pragma mark - for the function drivers


status_t
MMCBus::SdioReadByte(uint8 function, uint32 address, uint8* _value)
{
	if (_value == NULL)
		return B_BAD_VALUE;
	AcquireBus();
	status_t status = fSdioRca == 0 ? B_DEVICE_NOT_FOUND
		: _SdioDirect(false, function, address, 0, _value, false);
	ReleaseBus();
	return status;
}


status_t
MMCBus::SdioWriteByte(uint8 function, uint32 address, uint8 value,
	uint8* _readBack)
{
	AcquireBus();
	status_t status = fSdioRca == 0 ? B_DEVICE_NOT_FOUND
		: _SdioDirect(true, function, address, value, _readBack,
			_readBack != NULL);
	ReleaseBus();
	return status;
}


status_t
MMCBus::SdioTransfer(bool write, uint8 function, uint32 address,
	bool increment, uint8* buffer, size_t length)
{
	if (function >= SDIO_MAX_FUNCTIONS)
		return B_BAD_VALUE;
	if (length == 0)
		return B_OK;

	AcquireBus();
	status_t status = B_DEVICE_NOT_FOUND;
	if (fSdioRca != 0) {
		uint16 maxByteSize = fSdio.info.max_block_size[function];
		sdio_host_ops ops = _SdioHostOps();
		status = sdio_rw_extended(ops, write, function, address, increment,
			buffer, length, fSdio.info.block_size[function],
			(fSdio.info.capabilities & SDIO_CAP_SMB) != 0, maxByteSize);
	}
	ReleaseBus();
	return status;
}


status_t
MMCBus::SdioSetBlockSize(uint8 function, uint16 size)
{
	if (function >= SDIO_MAX_FUNCTIONS)
		return B_BAD_VALUE;

	// 0: the default, the most the function has up to 512 (as Linux)
	uint16 maximum = fSdio.info.max_block_size[function];
	if (size == 0)
		size = maximum != 0 && maximum < 512 ? maximum : 512;
	if ((maximum != 0 && size > maximum) || size > 2048)
		return B_BAD_VALUE;

	AcquireBus();
	status_t status = B_DEVICE_NOT_FOUND;
	if (fSdioRca != 0) {
		uint32 base = function == 0 ? SDIO_CCCR_BLOCK_SIZE
			: SDIO_FBR_BASE(function) + SDIO_FBR_BLOCK_SIZE;
		status = _SdioDirect(true, 0, base, size & 0xff, NULL, false);
		if (status == B_OK)
			status = _SdioDirect(true, 0, base + 1, size >> 8, NULL, false);
		if (status == B_OK)
			fSdio.info.block_size[function] = size;
	}
	ReleaseBus();
	return status;
}


status_t
MMCBus::SdioEnableFunction(uint8 function, bool enable)
{
	if (function == 0 || function >= SDIO_MAX_FUNCTIONS)
		return B_BAD_VALUE;

	AcquireBus();
	uint8 value = 0;
	status_t status = fSdioRca == 0 ? B_DEVICE_NOT_FOUND
		: _SdioDirect(false, 0, SDIO_CCCR_IO_ENABLE, 0, &value, false);
	if (status == B_OK) {
		if (enable)
			value |= 1 << function;
		else
			value &= ~(1 << function);
		status = _SdioDirect(true, 0, SDIO_CCCR_IO_ENABLE, value, NULL, false);
	}
	bigtime_t timeout = fSdio.enable_timeout[function] * 1000LL;
	ReleaseBus();
	if (status != B_OK || !enable)
		return status;

	// The bus is not held while the function gets ready.
	bigtime_t deadline = system_time() + timeout;
	while (true) {
		status = SdioReadByte(0, SDIO_CCCR_IO_READY, &value);
		if (status != B_OK)
			return status;
		if ((value & (1 << function)) != 0)
			return B_OK;
		if (system_time() >= deadline)
			return B_TIMED_OUT;
		snooze(1000);
	}
}


status_t
MMCBus::SdioGetCardInfo(sdio_card_info* info)
{
	if (info == NULL)
		return B_BAD_VALUE;
	AcquireBus();
	status_t status = fSdioRca == 0 ? B_DEVICE_NOT_FOUND : B_OK;
	if (status == B_OK)
		*info = fSdio.info;
	ReleaseBus();
	return status;
}


//	#pragma mark - interrupts


/*!	The host's card interrupt, in interrupt context. The host has masked
	it.
*/
/*static*/ void
MMCBus::_SdioInterrupt(void* data)
{
	MMCBus* bus = (MMCBus*)data;
	release_sem_etc(bus->fSdioInterruptSemaphore, 1, B_DO_NOT_RESCHEDULE);
}


/*static*/ status_t
MMCBus::_SdioInterruptThread(void* data)
{
	MMCBus* bus = (MMCBus*)data;

	while (acquire_sem(bus->fSdioInterruptSemaphore) == B_OK) {
		RecursiveLocker locker(bus->fSdioHandlerLock);

		uint8 handlers = 0;
		for (uint8 function = 1; function < SDIO_MAX_FUNCTIONS; function++) {
			if (bus->fSdioHandlers[function] != NULL)
				handlers |= 1 << function;
		}
		if (handlers == 0) {
			// stays masked until a handler comes
			continue;
		}

		// With one handler the card's pending register is not read: that
		// handler is called (Linux does the same).
		bool single = (handlers & (handlers - 1)) == 0;
		uint8 pending = 0;
		if (!single
			&& bus->SdioReadByte(0, SDIO_CCCR_INT_PENDING, &pending) != B_OK) {
			// served all the same: a function that has nothing finds that
			pending = handlers;
		}

		uint8 serve = sdio_interrupts_to_serve(handlers, pending, !single);
		for (uint8 function = 1; function < SDIO_MAX_FUNCTIONS; function++) {
			if ((serve & (1 << function)) != 0)
				bus->fSdioHandlers[function](bus->fSdioHandlerData[function]);
		}

		bus->fController->enable_sdio_interrupt(bus->fCookie, true);
	}
	return B_OK;
}


status_t
MMCBus::SdioSetInterruptHandler(uint8 function, sdio_interrupt_handler handler,
	void* data)
{
	if (function == 0 || function >= SDIO_MAX_FUNCTIONS)
		return B_BAD_VALUE;
	if (fController->set_sdio_interrupt_handler == NULL
		|| fController->enable_sdio_interrupt == NULL)
		return B_NOT_SUPPORTED;

	// Held across a dispatch: once a handler is taken away, it is not
	// running any more (unless it takes itself away).
	RecursiveLocker locker(fSdioHandlerLock);

	if (handler != NULL && fSdioInterruptThread < 0) {
		fSdioInterruptSemaphore = create_sem(0, "sdio interrupt");
		if (fSdioInterruptSemaphore < 0)
			return fSdioInterruptSemaphore;
		fSdioInterruptThread = spawn_kernel_thread(_SdioInterruptThread,
			"sdio interrupt", B_URGENT_DISPLAY_PRIORITY, this);
		if (fSdioInterruptThread < 0) {
			status_t status = fSdioInterruptThread;
			delete_sem(fSdioInterruptSemaphore);
			fSdioInterruptSemaphore = -1;
			return status;
		}
		resume_thread(fSdioInterruptThread);
	}

	uint8 enable = sdio_interrupt_enable(fSdioInterruptEnable, function,
		handler != NULL);
	if (enable != fSdioInterruptEnable) {
		AcquireBus();
		status_t status = fSdioRca == 0 ? B_DEVICE_NOT_FOUND
			: _SdioDirect(true, 0, SDIO_CCCR_INT_ENABLE, enable, NULL, false);
		ReleaseBus();
		if (status != B_OK)
			return status;
		fSdioInterruptEnable = enable;
	}

	fSdioHandlerData[function] = data;
	fSdioHandlers[function] = handler;

	if (enable != 0 && !fSdioHostHandlerSet) {
		fController->set_sdio_interrupt_handler(fCookie, _SdioInterrupt, this);
		fSdioHostHandlerSet = true;
		fController->enable_sdio_interrupt(fCookie, true);
	} else if (enable == 0 && fSdioHostHandlerSet) {
		fController->enable_sdio_interrupt(fCookie, false);
		fController->set_sdio_interrupt_handler(fCookie, NULL, NULL);
		fSdioHostHandlerSet = false;
	}
	return B_OK;
}


void
MMCBus::_StopSdioInterrupts()
{
	if (fSdioHostHandlerSet) {
		fController->enable_sdio_interrupt(fCookie, false);
		fController->set_sdio_interrupt_handler(fCookie, NULL, NULL);
		fSdioHostHandlerSet = false;
	}
	if (fSdioInterruptSemaphore >= 0) {
		delete_sem(fSdioInterruptSemaphore);
		fSdioInterruptSemaphore = -1;
	}
	if (fSdioInterruptThread >= 0) {
		status_t result;
		wait_for_thread(fSdioInterruptThread, &result);
		fSdioInterruptThread = -1;
	}
}
