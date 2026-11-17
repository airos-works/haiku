/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Host controller driver for the Synopsys DesignWare USB 2.0 OTG core
	("DWC2") in host mode, the only USB controller of the Raspberry Pi 3
	(BCM2837; also the BCM2835/6 and the Pi 4's OTG port).

	The core has no schedule in memory: software programs one of its host
	channels (8 on the BCM283x) with one transaction sequence at a time, in
	buffer DMA mode, and the channel halts with an interrupt when it is done
	or needs attention. The design follows Linux' dwc2 in buffer DMA mode,
	with a different scheduler:

	- The interrupt handler only acknowledges and records channel halts,
	  port changes and start-of-frame; a worker thread does everything else
	  under one mutex: it reads what a halted channel did, starts the next
	  run, schedules endpoints onto free channels and completes transfers.
	- Bulk and control endpoints take a channel while they have transfers.
	  The core retries their NAKs itself (no interrupts). A NAKing IN that
	  holds a channel while others wait for one is halted and requeued.
	- Interrupt endpoints take a channel per poll; the worker sleeps until
	  the next poll is due (no start-of-frame interrupts for that). The
	  start-of-frame interrupt is enabled only while a periodic split
	  transaction waits for a particular microframe, which keeps the
	  BCM283x's well-known 8000 SOF interrupts a second away.
	- Full and low speed devices behind a high-speed hub are reached with
	  split transactions (start split, then complete splits), interrupt
	  start splits in microframes 0-3 and their complete splits two
	  microframes later, as EHCI's usual S-mask 0x01/C-mask 0x1c.
	- Data goes through a bounce buffer per channel, below the controller's
	  DMA limit and translated by the device tree's "dma-ranges" (the
	  BCM2837's DMA sees RAM at 0xc0000000). DMA is not cache-coherent:
	  the buffers are cached and cleaned/invalidated around each run.
	- Isochronous endpoints are not supported yet.
*/


#include "dwc2.h"

#include <stdio.h>
#include <string.h>

#include <AutoDeleterOS.h>
#include <bus/FDT.h>
#include <kernel.h>
#include <KernelExport.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include "usb_fdt.h"


#define USB_MODULE_NAME	"dwc2"

//#define TRACE_DWC2
	// every run, halt and completion
//#define TRACE_DWC2_DATA
	// what interrupt endpoints deliver
#ifdef TRACE_DWC2
#	define TRACE_D(x...)	dprintf("dwc2: " x)
#else
#	define TRACE_D(x...)
#endif
#ifdef TRACE_DWC2_DATA
#	define TRACE_DATA(x...)	dprintf("dwc2: " x)
#else
#	define TRACE_DATA(x...)
#endif
#define TRACE_ALWAYS_D(x...)	dprintf("dwc2: " x)


// Keep the bounce buffers uncached (Normal Non-cacheable) instead of cleaning
// and invalidating cached ones around every run.
#define DWC2_UNCACHED_BUFFERS	0


static const size_t kBufferSize = 64 * 1024;
	// per channel; also the largest run of a bulk or control transfer
static const uint32 kPeriodicReserve = 2;
	// channels that bulk and control endpoints leave to interrupt endpoints
static const bigtime_t kNakBackoff = 1000;
	// a split or OUT endpoint that keeps NAKing retries after this
static const uint8 kSplitNakLimit = 3;
static const uint8 kOutNakLimit = 8;
static const bigtime_t kPreemptAfter = 1000;
	// a NAKing IN gives up its channel to a waiter after this
static const bigtime_t kHaltTimeout = 100000;
static const uint8 kMaxErrors = 3;


device_manager_info* gDeviceManager;
usb_for_controller_interface* gUSB;


//	#pragma mark - helpers


static size_t sCacheLineSize = 64;


static inline void
dwc2_clean_cache(const void* address, size_t length)
{
#if defined(__aarch64__) && !DWC2_UNCACHED_BUFFERS
	addr_t line = (addr_t)address & ~(sCacheLineSize - 1);
	addr_t end = (addr_t)address + length;
	for (; line < end; line += sCacheLineSize)
		asm volatile("dc cvac, %0" : : "r"(line) : "memory");
	asm volatile("dsb sy" : : : "memory");
#else
	memory_full_barrier();
#endif
}


/*!	Removes the buffer's lines from the cache. Before the controller writes
	(lines are clean then, they're only ever written before an OUT run, and
	cleaned) and after it wrote, as the core may have fetched the lines
	speculatively meanwhile.
*/
static inline void
dwc2_invalidate_cache(const void* address, size_t length, bool clean)
{
#if defined(__aarch64__) && !DWC2_UNCACHED_BUFFERS
	addr_t line = (addr_t)address & ~(sCacheLineSize - 1);
	addr_t end = (addr_t)address + length;
	if (clean) {
		for (; line < end; line += sCacheLineSize)
			asm volatile("dc civac, %0" : : "r"(line) : "memory");
	} else {
		for (; line < end; line += sCacheLineSize)
			asm volatile("dc ivac, %0" : : "r"(line) : "memory");
	}
	asm volatile("dsb sy" : : : "memory");
#else
	memory_full_barrier();
#endif
}


//! The signed distance of two 14 bit (micro)frame numbers.
static inline int32
frame_diff(uint16 a, uint16 b)
{
	int32 difference = (a - b) & DWC2_HFNUM_FRNUM_MASK;
	return difference >= 0x2000 ? difference - 0x4000 : difference;
}


static inline uint16
frame_add(uint16 frame, int32 count)
{
	return (frame + count) & DWC2_HFNUM_FRNUM_MASK;
}


static size_t
vector_length(Transfer* transfer)
{
	size_t length = 0;
	generic_io_vec* vector = transfer->Vector();
	for (size_t i = 0; i < transfer->VectorCount(); i++)
		length += vector[i].length;
	return length;
}


/*!	Copies \a length bytes between the transfer's data, from \a offset on,
	and \a buffer.
*/
static status_t
copy_transfer_data(Transfer* transfer, size_t offset, uint8* buffer,
	size_t length, bool toTransfer)
{
	generic_io_vec* vector = transfer->Vector();
	bool physical = transfer->IsPhysical();
	for (size_t i = 0; i < transfer->VectorCount() && length > 0; i++) {
		if (offset >= vector[i].length) {
			offset -= vector[i].length;
			continue;
		}
		size_t chunk = min_c(vector[i].length - offset, length);
		generic_addr_t data = vector[i].base + offset;
		status_t status;
		if (toTransfer) {
			status = generic_memcpy(data, physical, (generic_addr_t)buffer,
				false, chunk);
		} else {
			status = generic_memcpy((generic_addr_t)buffer, false, data,
				physical, chunk);
		}
		if (status != B_OK)
			return status;
		buffer += chunk;
		length -= chunk;
		offset = 0;
	}
	return length == 0 ? B_OK : B_BAD_VALUE;
}


//	#pragma mark - controller


DWC2::DWC2(Stack* stack, device_node* node, const dwc2_platform_info* platform)
	:
	BusManager(stack, node),
	fStatus(B_NO_INIT),
	fPlatform(*platform),
	fStack(stack),
	fRegisterArea(-1),
	fRegisters(NULL),
	fBufferArea(-1),
	fSNPSID(0),
	fChannelCount(0),
	fMaxTransferSize(0),
	fMaxPacketCount(0),
	fIRQ(platform->interrupt),
	fInterruptInstalled(false),
	fHaltedChannels(0),
	fPortChange(0),
	fPortEvent(false),
	fInterruptMask(0),
	fEndpoints(NULL),
	fNextNonPeriodic(0),
	fSOFEnabled(false),
	fWorkSem(-1),
	fWorkPending(0),
	fWorkerThread(-1),
	fStopWorker(false),
	fRootHub(NULL),
	fRootHubAddress(0),
	fPortSpeed(USB_SPEED_HIGHSPEED),
	fPortResetChange(false)
{
	B_INITIALIZE_SPINLOCK(&fInterruptLock);
	mutex_init(&fLock, "dwc2 schedule");
	memset(fChannels, 0, sizeof(fChannels));
	memset(fChannelInterrupts, 0, sizeof(fChannelInterrupts));

	if (BusManager::InitCheck() != B_OK)
		return;

#ifdef __aarch64__
	uint64 ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	sCacheLineSize = 4UL << ((ctr >> 16) & 15);
#endif

	phys_addr_t base = fPlatform.register_base & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	size_t offset = fPlatform.register_base - base;
	fRegisterArea = map_physical_memory("dwc2 registers", base,
		ROUNDUP(offset + 0x1000, B_PAGE_SIZE), B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fRegisters);
	if (fRegisterArea < 0) {
		fStatus = fRegisterArea;
		return;
	}
	fRegisters += offset;

	if (fPlatform.bcm2835)
		_PowerOn();

	fSNPSID = _Read(DWC2_GSNPSID);
	uint32 id = fSNPSID & DWC2_GSNPSID_ID_MASK;
	if (id != DWC2_GSNPSID_OTG_2 && id != DWC2_GSNPSID_OTG_3
		&& id != DWC2_GSNPSID_OTG_4) {
		TRACE_ALWAYS_D("no DWC2 core at %#" B_PRIxPHYSADDR " (ID %#" B_PRIx32
			")\n", fPlatform.register_base, fSNPSID);
		fStatus = B_NO_INIT;
		return;
	}

	uint32 hwcfg2 = _Read(DWC2_GHWCFG2);
	uint32 hwcfg3 = _Read(DWC2_GHWCFG3);
	if (DWC2_GHWCFG2_ARCH(hwcfg2) != DWC2_GHWCFG2_ARCH_INT_DMA) {
		TRACE_ALWAYS_D("core %#" B_PRIx32 " has no internal DMA (hwcfg2 %#"
			B_PRIx32 "); unsupported\n", fSNPSID, hwcfg2);
		fStatus = B_NOT_SUPPORTED;
		return;
	}
	fChannelCount = min_c(DWC2_GHWCFG2_CHANNELS(hwcfg2), DWC2_MAX_CHANNELS);
	fMaxTransferSize = min_c((1u << DWC2_GHWCFG3_XFER_WIDTH(hwcfg3)) - 1,
		65535u);
	fMaxPacketCount = min_c((1u << DWC2_GHWCFG3_PKT_WIDTH(hwcfg3)) - 1, 511u);

	// one bounce buffer per channel, where the controller reaches it
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = fPlatform.dma_limit;
	uint8* buffers;
	size_t buffersSize = fChannelCount * kBufferSize;
	fBufferArea = create_area_etc(B_SYSTEM_TEAM, "dwc2 buffers", buffersSize,
		B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, (void**)&buffers);
	if (fBufferArea < 0) {
		fStatus = fBufferArea;
		return;
	}
	physical_entry entry;
	status_t status = get_memory_map(buffers, buffersSize, &entry, 1);
	if (status != B_OK || entry.size < buffersSize
		|| entry.address + buffersSize > fPlatform.dma_limit) {
		fStatus = B_NO_MEMORY;
		return;
	}
	dwc2_invalidate_cache(buffers, buffersSize, true);
#if DWC2_UNCACHED_BUFFERS
	status = vm_set_area_memory_type(fBufferArea, entry.address,
		B_WRITE_COMBINING_MEMORY);
	if (status != B_OK) {
		fStatus = status;
		return;
	}
#endif
	for (uint32 i = 0; i < fChannelCount; i++) {
		fChannels[i].buffer = buffers + i * kBufferSize;
		fChannels[i].buffer_physical = entry.address + i * kBufferSize;
	}

	TRACE_ALWAYS_D("core %" B_PRIx32 ".%03" B_PRIx32 " at %#" B_PRIxPHYSADDR
		", IRQ %" B_PRIu32 ", %" B_PRIu32 " channels, runs up to %" B_PRIu32
		" bytes/%" B_PRIu32 " packets, DMA offset %#" B_PRIx64 "\n",
		(fSNPSID >> 12) & 0xf, fSNPSID & 0xfff, fPlatform.register_base, fIRQ,
		fChannelCount, fMaxTransferSize, fMaxPacketCount,
		fPlatform.dma_offset);

	status = _CoreReset();
	if (status == B_OK)
		status = _InitHost();
	if (status != B_OK) {
		fStatus = status;
		return;
	}

	fWorkSem = create_sem(0, "dwc2 work");
	if (fWorkSem < 0) {
		fStatus = fWorkSem;
		return;
	}
	fWorkerThread = spawn_kernel_thread(_WorkerThread, "dwc2 worker",
		B_REAL_TIME_DISPLAY_PRIORITY, this);
	if (fWorkerThread < 0) {
		fStatus = fWorkerThread;
		return;
	}
	resume_thread(fWorkerThread);

	status = install_io_interrupt_handler(fIRQ, _InterruptHandler, this, 0);
	if (status != B_OK) {
		fStatus = status;
		return;
	}
	fInterruptInstalled = true;

	_Write(DWC2_GINTSTS, 0xffffffff);
	_Write(DWC2_GAHBCFG, _Read(DWC2_GAHBCFG) | DWC2_GAHBCFG_GLBLINTRMSK);

	fStatus = B_OK;
	fInitOK = true;
}


DWC2::~DWC2()
{
	if (fRegisters != NULL && fStatus == B_OK) {
		_Write(DWC2_GAHBCFG, _Read(DWC2_GAHBCFG) & ~DWC2_GAHBCFG_GLBLINTRMSK);
		_Write(DWC2_GINTMSK, 0);
		_HaltAllChannels();
	}
	if (fInterruptInstalled)
		remove_io_interrupt_handler(fIRQ, _InterruptHandler, this);

	if (fWorkerThread >= 0) {
		fStopWorker = true;
		release_sem(fWorkSem);
		status_t result;
		wait_for_thread(fWorkerThread, &result);
	}
	if (fWorkSem >= 0)
		delete_sem(fWorkSem);

	delete fRootHub;

	while (fEndpoints != NULL) {
		dwc2_endpoint* endpoint = fEndpoints;
		fEndpoints = endpoint->link;
		delete endpoint;
	}

	if (fBufferArea >= 0)
		delete_area(fBufferArea);
	if (fRegisterArea >= 0)
		delete_area(fRegisterArea);
	mutex_destroy(&fLock);
}


status_t
DWC2::InitCheck()
{
	return fStatus;
}


/*!	The BCM283x's USB block sits in a power domain of the VideoCore
	firmware. Ask the firmware to power it ("set power state" of device 3,
	"USB HCD", waiting), as Linux' raspberrypi-power, U-Boot and FreeBSD do.
*/
status_t
DWC2::_PowerOn()
{
	const phys_addr_t mailboxBase = (fPlatform.register_base & ~0xffffffULL)
		+ 0xb000;
	const uint32 kRead = 0x880, kStatus = 0x898, kWrite = 0x8a0;
	const uint32 kFull = 0x80000000, kEmpty = 0x40000000;
	const uint32 kPropertyChannel = 8;

	volatile uint8* mailbox;
	area_id mailboxArea = map_physical_memory("dwc2 mailbox", mailboxBase,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&mailbox);
	if (mailboxArea < 0)
		return mailboxArea;
	AreaDeleter mailboxDeleter(mailboxArea);

	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = fPlatform.dma_limit;
	uint32* message;
	area_id messageArea = create_area_etc(B_SYSTEM_TEAM, "dwc2 mailbox message",
		B_PAGE_SIZE, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		0, 0, &virtualRestrictions, &physicalRestrictions, (void**)&message);
	if (messageArea < 0)
		return messageArea;
	AreaDeleter messageDeleter(messageArea);

	physical_entry entry;
	status_t status = get_memory_map(message, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK)
		return status;
	// the firmware answers in place: keep the page out of the cache
	dwc2_invalidate_cache(message, B_PAGE_SIZE, true);
	vm_set_area_memory_type(messageArea, entry.address,
		B_WRITE_COMBINING_MEMORY);

	message[0] = 8 * 4;
	message[1] = 0;				// request
	message[2] = 0x00028001;	// set power state
	message[3] = 8;
	message[4] = 0;
	message[5] = 3;				// USB HCD
	message[6] = 3;				// on, wait
	message[7] = 0;
	memory_full_barrier();

	uint32 address = (uint32)(entry.address + fPlatform.dma_offset)
		| kPropertyChannel;
	bigtime_t timeout = system_time() + 1000000;
	while ((*(volatile uint32*)(mailbox + kStatus) & kFull) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		snooze(100);
	}
	*(volatile uint32*)(mailbox + kWrite) = address;
	while (true) {
		while ((*(volatile uint32*)(mailbox + kStatus) & kEmpty) != 0) {
			if (system_time() > timeout) {
				TRACE_ALWAYS_D("firmware did not answer the power request\n");
				return B_TIMED_OUT;
			}
			snooze(100);
		}
		if (*(volatile uint32*)(mailbox + kRead) == address)
			break;
	}
	memory_full_barrier();

	bool on = message[1] == 0x80000000 && (message[6] & 1) != 0
		&& (message[6] & 2) == 0;
	TRACE_ALWAYS_D("firmware power domain: %s (%#" B_PRIx32 ", %#" B_PRIx32
		")\n", on ? "on" : "failed", message[1], message[6]);
	return on ? B_OK : B_ERROR;
}


status_t
DWC2::_CoreReset()
{
	bigtime_t timeout = system_time() + 100000;
	while ((_Read(DWC2_GRSTCTL) & DWC2_GRSTCTL_AHBIDLE) == 0) {
		if (system_time() > timeout) {
			TRACE_ALWAYS_D("AHB master not idle\n");
			return B_TIMED_OUT;
		}
		spin(10);
	}

	_Write(DWC2_GRSTCTL, _Read(DWC2_GRSTCTL) | DWC2_GRSTCTL_CSFTRST);
	timeout = system_time() + 100000;
	while ((_Read(DWC2_GRSTCTL) & DWC2_GRSTCTL_CSFTRST) != 0) {
		if (system_time() > timeout) {
			TRACE_ALWAYS_D("core soft reset timed out\n");
			return B_TIMED_OUT;
		}
		spin(10);
	}
	timeout = system_time() + 100000;
	while ((_Read(DWC2_GRSTCTL) & DWC2_GRSTCTL_AHBIDLE) == 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		spin(10);
	}
	// the PHY clocks have to run a few cycles
	snooze(1000);
	return B_OK;
}


status_t
DWC2::_InitHost()
{
	_Write(DWC2_GAHBCFG, 0);
	_Write(DWC2_GINTMSK, 0);

	// The high-speed PHY: UTMI+ of the width the core was built with, as
	// Linux' dwc2_hs_phy_init(). These bits survive a soft reset, which
	// has to follow a change.
	uint32 hwcfg2 = _Read(DWC2_GHWCFG2);
	uint32 hwcfg4 = _Read(DWC2_GHWCFG4);
	uint32 usbcfg = _Read(DWC2_GUSBCFG);
	uint32 phy = usbcfg;
	uint32 hsPhy = DWC2_GHWCFG2_HS_PHY(hwcfg2);
	if (hsPhy == DWC2_GHWCFG2_HS_PHY_UTMI
		|| hsPhy == DWC2_GHWCFG2_HS_PHY_BOTH) {
		phy &= ~(DWC2_GUSBCFG_ULPI_UTMI_SEL | DWC2_GUSBCFG_PHYIF16
			| DWC2_GUSBCFG_PHYSEL);
		if (DWC2_GHWCFG4_UTMI_WIDTH(hwcfg4) == DWC2_GHWCFG4_UTMI_WIDTH_16)
			phy |= DWC2_GUSBCFG_PHYIF16;
	}
	if (phy != usbcfg) {
		_Write(DWC2_GUSBCFG, phy);
		status_t status = _CoreReset();
		if (status != B_OK)
			return status;
	}

	usbcfg = _Read(DWC2_GUSBCFG);
	usbcfg &= ~(DWC2_GUSBCFG_ULPI_FS_LS | DWC2_GUSBCFG_ULPI_CLK_SUSP_M
		| DWC2_GUSBCFG_HNPCAP | DWC2_GUSBCFG_SRPCAP
		| DWC2_GUSBCFG_FORCEDEVMODE);
	usbcfg |= DWC2_GUSBCFG_FORCEHOSTMODE;
	_Write(DWC2_GUSBCFG, usbcfg);

	// The ID pin filter takes up to 100 ms to settle into host mode.
	bigtime_t timeout = system_time() + 110000;
	while ((_Read(DWC2_GINTSTS) & DWC2_GINT_CURMODE_HOST) == 0) {
		if (system_time() > timeout) {
			TRACE_ALWAYS_D("core did not enter host mode\n");
			return B_TIMED_OUT;
		}
		snooze(1000);
	}

	// Buffer DMA, 16 beat bursts on the BCM283x (Linux' value), else 4.
	_Write(DWC2_GAHBCFG, DWC2_GAHBCFG_DMA_EN
		| (fPlatform.bcm2835 ? 0x10 : (3 << 1)));

	_Write(DWC2_PCGCTL, 0);

	// The UTMI+ PHY's 30/60 MHz clock also for full and low speed.
	uint32 hcfg = _Read(DWC2_HCFG);
	hcfg &= ~(DWC2_HCFG_FSLSPCLKSEL_MASK | DWC2_HCFG_FSLSSUPP);
	if ((usbcfg & DWC2_GUSBCFG_PHYSEL) != 0)
		hcfg |= 1;
			// 48 MHz full-speed PHY
	_Write(DWC2_HCFG, hcfg);

	// FIFOs: what Linux uses on the BCM283x, or the core's depth in thirds.
	uint32 depth = DWC2_GHWCFG3_FIFO_DEPTH(_Read(DWC2_GHWCFG3));
	uint32 rxSize = 774, nonPeriodicSize = 256, periodicSize = 512;
	if (rxSize + nonPeriodicSize + periodicSize > depth) {
		rxSize = depth / 2;
		nonPeriodicSize = depth / 4;
		periodicSize = depth - rxSize - nonPeriodicSize;
	}
	_Write(DWC2_GRXFSIZ, rxSize);
	_Write(DWC2_GNPTXFSIZ, (nonPeriodicSize << 16) | rxSize);
	_Write(DWC2_HPTXFSIZ, (periodicSize << 16) | (rxSize + nonPeriodicSize));

	_Write(DWC2_GRSTCTL, DWC2_GRSTCTL_TXFFLSH | DWC2_GRSTCTL_TXFNUM_ALL);
	timeout = system_time() + 10000;
	while ((_Read(DWC2_GRSTCTL) & DWC2_GRSTCTL_TXFFLSH) != 0
		&& system_time() < timeout) {
		spin(1);
	}
	_Write(DWC2_GRSTCTL, DWC2_GRSTCTL_RXFFLSH);
	timeout = system_time() + 10000;
	while ((_Read(DWC2_GRSTCTL) & DWC2_GRSTCTL_RXFFLSH) != 0
		&& system_time() < timeout) {
		spin(1);
	}
	spin(1);

	_HaltAllChannels();
	_Write(DWC2_HAINTMSK, (1u << fChannelCount) - 1);

	// port power
	uint32 port = _Read(DWC2_HPRT0) & ~DWC2_HPRT0_W1C_MASK;
	if ((port & DWC2_HPRT0_PWR) == 0)
		_Write(DWC2_HPRT0, port | DWC2_HPRT0_PWR);

	_Write(DWC2_GINTSTS, 0xffffffff);
	fInterruptMask = DWC2_GINT_PRTINT | DWC2_GINT_HCHINT
		| DWC2_GINT_DISCONNINT;
	_Write(DWC2_GINTMSK, fInterruptMask);

	TRACE_ALWAYS_D("host mode; GUSBCFG %#" B_PRIx32 ", HCFG %#" B_PRIx32
		", FIFOs %" B_PRIu32 "/%" B_PRIu32 "/%" B_PRIu32 " of %" B_PRIu32
		" words, HPRT0 %#" B_PRIx32 "\n", _Read(DWC2_GUSBCFG),
		_Read(DWC2_HCFG), rxSize, nonPeriodicSize, periodicSize, depth,
		_Read(DWC2_HPRT0));
	return B_OK;
}


void
DWC2::_HaltAllChannels()
{
	// first drop requests queued for a channel, then halt each
	for (uint32 i = 0; i < fChannelCount; i++) {
		_Write(DWC2_HCINTMSK(i), 0);
		uint32 hcchar = _Read(DWC2_HCCHAR(i));
		hcchar &= ~(DWC2_HCCHAR_CHENA | DWC2_HCCHAR_EPDIR_IN);
		_Write(DWC2_HCCHAR(i), hcchar | DWC2_HCCHAR_CHDIS);
	}
	for (uint32 i = 0; i < fChannelCount; i++) {
		uint32 hcchar = _Read(DWC2_HCCHAR(i)) & ~DWC2_HCCHAR_EPDIR_IN;
		_Write(DWC2_HCCHAR(i), hcchar | DWC2_HCCHAR_CHENA | DWC2_HCCHAR_CHDIS);
		bigtime_t timeout = system_time() + 10000;
		while ((_Read(DWC2_HCCHAR(i)) & DWC2_HCCHAR_CHENA) != 0) {
			if (system_time() > timeout) {
				TRACE_ALWAYS_D("channel %" B_PRIu32 " does not halt\n", i);
				break;
			}
			spin(10);
		}
		_Write(DWC2_HCINT(i), 0xffffffff);
	}
}


status_t
DWC2::Start()
{
	fRootHubAddress = AllocateAddress();
	fRootHub = new(std::nothrow) DWC2RootHub(RootObject(), fRootHubAddress);
	if (fRootHub == NULL)
		return B_NO_MEMORY;
	if (fRootHub->InitCheck() != B_OK)
		return fRootHub->InitCheck();
	SetRootHub(fRootHub);
	fRootHub->RegisterNode(Node());

	TRACE_ALWAYS_D("controller started\n");
	return BusManager::Start();
}


//	#pragma mark - root port


status_t
DWC2::GetPortStatus(uint8 index, usb_port_status* status)
{
	if (index >= PortCount())
		return B_BAD_INDEX;

	uint32 port = _Read(DWC2_HPRT0);
	status->status = status->change = 0;
	if ((port & DWC2_HPRT0_CONNSTS) != 0)
		status->status |= PORT_STATUS_CONNECTION;
	if ((port & DWC2_HPRT0_ENA) != 0) {
		status->status |= PORT_STATUS_ENABLE;
		switch (DWC2_HPRT0_SPD(port)) {
			case DWC2_HPRT0_SPD_HIGH:
				status->status |= PORT_STATUS_HIGH_SPEED;
				break;
			case DWC2_HPRT0_SPD_LOW:
				status->status |= PORT_STATUS_LOW_SPEED;
				break;
		}
	}
	if ((port & DWC2_HPRT0_SUSP) != 0)
		status->status |= PORT_STATUS_SUSPEND;
	if ((port & DWC2_HPRT0_OVRCURRACT) != 0)
		status->status |= PORT_STATUS_OVER_CURRENT;
	if ((port & DWC2_HPRT0_RST) != 0)
		status->status |= PORT_STATUS_RESET;
	if ((port & DWC2_HPRT0_PWR) != 0)
		status->status |= PORT_STATUS_POWER;

	InterruptsSpinLocker locker(fInterruptLock);
	status->change = fPortChange;
	if (fPortResetChange)
		status->change |= PORT_STATUS_RESET;
	return B_OK;
}


status_t
DWC2::SetPortFeature(uint8 index, uint16 feature)
{
	if (index >= PortCount())
		return B_BAD_INDEX;

	switch (feature) {
		case PORT_RESET:
			return _ResetPort();

		case PORT_POWER:
		case PORT_SUSPEND:
		{
			InterruptsSpinLocker locker(fInterruptLock);
			uint32 port = _Read(DWC2_HPRT0) & ~DWC2_HPRT0_W1C_MASK;
			_Write(DWC2_HPRT0, port | (feature == PORT_POWER
				? DWC2_HPRT0_PWR : DWC2_HPRT0_SUSP));
			return B_OK;
		}
	}
	return B_BAD_VALUE;
}


status_t
DWC2::ClearPortFeature(uint8 index, uint16 feature)
{
	if (index >= PortCount())
		return B_BAD_INDEX;

	InterruptsSpinLocker locker(fInterruptLock);
	uint32 port = _Read(DWC2_HPRT0) & ~DWC2_HPRT0_W1C_MASK;
	switch (feature) {
		case PORT_ENABLE:
			// writing one disables the port
			_Write(DWC2_HPRT0, port | DWC2_HPRT0_ENA);
			return B_OK;
		case PORT_POWER:
			_Write(DWC2_HPRT0, port & ~DWC2_HPRT0_PWR);
			return B_OK;
		case PORT_SUSPEND:
			if ((port & DWC2_HPRT0_SUSP) == 0)
				return B_OK;
			_Write(DWC2_HPRT0, port | DWC2_HPRT0_RES);
			locker.Unlock();
			snooze(20000);
			locker.Lock();
			port = _Read(DWC2_HPRT0) & ~DWC2_HPRT0_W1C_MASK;
			_Write(DWC2_HPRT0, port & ~DWC2_HPRT0_RES);
			fPortChange |= PORT_STATUS_SUSPEND;
			return B_OK;
		case C_PORT_CONNECTION:
			fPortChange &= ~PORT_STATUS_CONNECTION;
			return B_OK;
		case C_PORT_ENABLE:
			fPortChange &= ~PORT_STATUS_ENABLE;
			return B_OK;
		case C_PORT_SUSPEND:
			fPortChange &= ~PORT_STATUS_SUSPEND;
			return B_OK;
		case C_PORT_OVER_CURRENT:
			fPortChange &= ~PORT_STATUS_OVER_CURRENT;
			return B_OK;
		case C_PORT_RESET:
			fPortResetChange = false;
			return B_OK;
	}
	return B_BAD_VALUE;
}


status_t
DWC2::_ResetPort()
{
	_Write(DWC2_PCGCTL, 0);
	{
		InterruptsSpinLocker locker(fInterruptLock);
		uint32 port = _Read(DWC2_HPRT0) & ~DWC2_HPRT0_W1C_MASK;
		_Write(DWC2_HPRT0, port | DWC2_HPRT0_PWR | DWC2_HPRT0_RST);
	}
	snooze(60000);
	{
		InterruptsSpinLocker locker(fInterruptLock);
		uint32 port = _Read(DWC2_HPRT0) & ~DWC2_HPRT0_W1C_MASK;
		_Write(DWC2_HPRT0, port & ~DWC2_HPRT0_RST);
	}

	uint32 port = 0;
	for (int32 i = 0; i < 100; i++) {
		port = _Read(DWC2_HPRT0);
		if ((port & DWC2_HPRT0_ENA) != 0
			|| (port & DWC2_HPRT0_CONNSTS) == 0) {
			break;
		}
		snooze(1000);
	}

	if ((port & DWC2_HPRT0_ENA) != 0) {
		MutexLocker locker(fLock);
		switch (DWC2_HPRT0_SPD(port)) {
			case DWC2_HPRT0_SPD_HIGH:
				fPortSpeed = USB_SPEED_HIGHSPEED;
				break;
			case DWC2_HPRT0_SPD_FULL:
				fPortSpeed = USB_SPEED_FULLSPEED;
				break;
			default:
				fPortSpeed = USB_SPEED_LOWSPEED;
				break;
		}
		locker.Unlock();
		_UpdateFrameInterval();
		TRACE_ALWAYS_D("port enabled, %s speed\n",
			fPortSpeed == USB_SPEED_HIGHSPEED ? "high"
				: fPortSpeed == USB_SPEED_FULLSPEED ? "full" : "low");
	} else {
		TRACE_ALWAYS_D("port did not enable after reset (HPRT0 %#" B_PRIx32
			")\n", port);
	}

	InterruptsSpinLocker locker(fInterruptLock);
	fPortResetChange = true;
	// the reset's enable is not a change the hub has to report
	fPortChange &= ~PORT_STATUS_ENABLE;
	return B_OK;
}


/*!	The frame interval for the port's speed, in PHY clocks (Linux'
	dwc2_calc_frame_interval()): it is set after every port enable.
*/
void
DWC2::_UpdateFrameInterval()
{
	uint32 usbcfg = _Read(DWC2_GUSBCFG);
	uint32 clock = 60;
	if ((usbcfg & DWC2_GUSBCFG_PHYSEL) != 0)
		clock = 48;
	else if ((usbcfg & DWC2_GUSBCFG_ULPI_UTMI_SEL) == 0
		&& (usbcfg & DWC2_GUSBCFG_PHYIF16) != 0) {
		clock = (usbcfg & DWC2_GUSBCFG_PHY_LP_CLK_SEL) != 0 ? 48 : 30;
	}
	uint32 interval = (fPortSpeed == USB_SPEED_HIGHSPEED ? 125 : 1000)
		* clock - 1;
	uint32 hfir = _Read(DWC2_HFIR) & ~DWC2_HFIR_FRINT_MASK;
	_Write(DWC2_HFIR, hfir | interval);
}


//	#pragma mark - interrupts


int32
DWC2::_InterruptHandler(void* data)
{
	return ((DWC2*)data)->_Interrupt();
}


int32
DWC2::_Interrupt()
{
	SpinLocker locker(fInterruptLock);

	uint32 status = _Read(DWC2_GINTSTS) & fInterruptMask;
	if (status == 0)
		return B_UNHANDLED_INTERRUPT;

	if ((status & DWC2_GINT_PRTINT) != 0) {
		// the change bits are write-one-to-clear; PrtEna must stay zero
		uint32 port = _Read(DWC2_HPRT0);
		uint32 changes = port & (DWC2_HPRT0_CONNDET | DWC2_HPRT0_ENACHG
			| DWC2_HPRT0_OVRCURRCHG);
		if ((changes & DWC2_HPRT0_CONNDET) != 0)
			fPortChange |= PORT_STATUS_CONNECTION;
		if ((changes & DWC2_HPRT0_ENACHG) != 0
			&& (port & DWC2_HPRT0_ENA) == 0) {
			fPortChange |= PORT_STATUS_ENABLE;
		}
		if ((changes & DWC2_HPRT0_OVRCURRCHG) != 0)
			fPortChange |= PORT_STATUS_OVER_CURRENT;
		_Write(DWC2_HPRT0, (port & ~DWC2_HPRT0_W1C_MASK) | changes);
		fPortEvent = true;
	}
	if ((status & DWC2_GINT_DISCONNINT) != 0) {
		_Write(DWC2_GINTSTS, DWC2_GINT_DISCONNINT);
		fPortChange |= PORT_STATUS_CONNECTION;
		fPortEvent = true;
	}
	if ((status & DWC2_GINT_HCHINT) != 0) {
		uint32 channels = _Read(DWC2_HAINT);
		for (uint32 i = 0; i < fChannelCount; i++) {
			if ((channels & (1u << i)) == 0)
				continue;
			uint32 hcint = _Read(DWC2_HCINT(i));
			_Write(DWC2_HCINT(i), hcint);
			fChannelInterrupts[i] |= hcint;
			if ((hcint & DWC2_HCINT_CHHLTD) != 0)
				fHaltedChannels |= 1u << i;
		}
	}
	if ((status & DWC2_GINT_SOF) != 0)
		_Write(DWC2_GINTSTS, DWC2_GINT_SOF);

	if (atomic_get_and_set(&fWorkPending, 1) == 0) {
		release_sem_etc(fWorkSem, 1, B_DO_NOT_RESCHEDULE);
		return B_INVOKE_SCHEDULER;
	}
	return B_HANDLED_INTERRUPT;
}


void
DWC2::_SetSOFInterrupt(bool enable)
{
	if (enable == fSOFEnabled)
		return;
	fSOFEnabled = enable;

	InterruptsSpinLocker locker(fInterruptLock);
	if (enable) {
		_Write(DWC2_GINTSTS, DWC2_GINT_SOF);
		fInterruptMask |= DWC2_GINT_SOF;
	} else
		fInterruptMask &= ~DWC2_GINT_SOF;
	_Write(DWC2_GINTMSK, fInterruptMask);
}


inline uint16
DWC2::_Frame()
{
	return _Read(DWC2_HFNUM) & DWC2_HFNUM_FRNUM_MASK;
}


//	#pragma mark - worker


status_t
DWC2::_WorkerThread(void* data)
{
	((DWC2*)data)->_Worker();
	return B_OK;
}


void
DWC2::_Wake()
{
	if (atomic_get_and_set(&fWorkPending, 1) == 0)
		release_sem(fWorkSem);
}


void
DWC2::_Worker()
{
	bigtime_t deadline = B_INFINITE_TIMEOUT;
	while (!fStopWorker) {
		if (deadline == B_INFINITE_TIMEOUT)
			acquire_sem(fWorkSem);
		else
			acquire_sem_etc(fWorkSem, 1, B_ABSOLUTE_TIMEOUT, deadline);
		atomic_set(&fWorkPending, 0);
		if (fStopWorker)
			break;

		dwc2_transfer* done = NULL;
		bool portEvent;
		{
			MutexLocker locker(fLock);
			deadline = _WorkPass(&done);
			InterruptsSpinLocker interruptLocker(fInterruptLock);
			portEvent = fPortEvent;
			fPortEvent = false;
		}
		if (portEvent)
			fStack->TriggerExplore();

		while (done != NULL) {
			dwc2_transfer* transfer = done;
			done = transfer->next;
			TRACE_D("transfer %p done: %s, %" B_PRIuSIZE " bytes\n",
				transfer->transfer, strerror(transfer->status),
				transfer->offset);
			transfer->transfer->Finished(transfer->status, transfer->offset);
			delete transfer->transfer;
			delete transfer;
		}
	}
}


/*!	One round of the worker: handles the halted channels, starts what is due
	and returns when it next has to run (or B_INFINITE_TIMEOUT).
*/
bigtime_t
DWC2::_WorkPass(dwc2_transfer** _done)
{
	uint32 halted;
	uint32 interrupts[DWC2_MAX_CHANNELS];
	uint32 errors = 0;
	{
		InterruptsSpinLocker locker(fInterruptLock);
		halted = fHaltedChannels;
		fHaltedChannels = 0;
		for (uint32 i = 0; i < fChannelCount; i++) {
			if ((halted & (1u << i)) != 0) {
				interrupts[i] = fChannelInterrupts[i];
				fChannelInterrupts[i] = 0;
			} else if ((fChannelInterrupts[i] & DWC2_HCINT_AHBERR) != 0)
				errors |= 1u << i;
		}
	}

	for (uint32 i = 0; i < fChannelCount; i++) {
		if ((halted & (1u << i)) != 0)
			_HandleHalt(i, interrupts[i], _done);
	}

	bigtime_t now = system_time();
	bigtime_t deadline = B_INFINITE_TIMEOUT;

	// channels that have to be halted, and halts that don't come
	for (uint32 i = 0; i < fChannelCount; i++) {
		dwc2_channel* channel = &fChannels[i];
		if (!channel->running)
			continue;
		if ((errors & (1u << i)) != 0
			&& channel->halt_request == DWC2_HALT_NONE) {
			TRACE_ALWAYS_D("channel %" B_PRIu32 ": AHB error, halting\n", i);
			_RequestHalt(i, DWC2_HALT_NONE);
		}
		if (channel->halt_requested == 0)
			continue;
		if (now - channel->halt_requested < kHaltTimeout) {
			deadline = min_c(deadline, channel->halt_requested + kHaltTimeout);
			continue;
		}
		uint32 hcchar = _Read(DWC2_HCCHAR(i));
		TRACE_ALWAYS_D("channel %" B_PRIu32 " did not halt (HCCHAR %#" B_PRIx32
			", HCINT %#" B_PRIx32 ")\n", i, hcchar, _Read(DWC2_HCINT(i)));
		uint32 hcint;
		{
			InterruptsSpinLocker locker(fInterruptLock);
			hcint = fChannelInterrupts[i] | _Read(DWC2_HCINT(i));
			fChannelInterrupts[i] = 0;
			fHaltedChannels &= ~(1u << i);
		}
		_Write(DWC2_HCINT(i), 0xffffffff);
		_HandleHalt(i, hcint | DWC2_HCINT_CHHLTD, _done);
	}

	// periodic endpoints: due polls and complete splits
	uint16 frame = _Frame();
	bool highSpeedRoot = fPortSpeed == USB_SPEED_HIGHSPEED;
	bool needSOF = false;
	for (dwc2_endpoint* endpoint = fEndpoints; endpoint != NULL;
			endpoint = endpoint->link) {
		if (endpoint->destroyed || endpoint->first == NULL
			|| !_IsPeriodic(endpoint)) {
			continue;
		}
		if (endpoint->state == DWC2_EP_FRAME_WAIT) {
			frame = _Frame();
			if (frame_diff(endpoint->start_frame, frame_add(frame, 1)) <= 0)
				_StartRun(endpoint, frame_add(frame, 1));
			else
				needSOF = true;
			continue;
		}
		if (endpoint->state != DWC2_EP_WAITING || endpoint->channel >= 0)
			continue;
		if (endpoint->wake_time > now) {
			deadline = min_c(deadline, endpoint->wake_time);
			continue;
		}
		frame = _Frame();
		if (highSpeedRoot && _UsesSplit(endpoint->pipe)
			&& (frame_add(frame, 1) & 7) > 3) {
			// a start split goes in microframes 0 to 3 only
			needSOF = true;
			continue;
		}
		int32 index = _AllocateChannel(true);
		if (index < 0)
			continue;
		fChannels[index].endpoint = endpoint;
		endpoint->channel = index;
		_StartRun(endpoint, frame_add(frame, 1));
	}

	// bulk and control endpoints, in turns
	bool waiting = false;
	for (dwc2_endpoint* endpoint = fEndpoints; endpoint != NULL;
			endpoint = endpoint->link) {
		if (endpoint->destroyed || endpoint->first == NULL
			|| _IsPeriodic(endpoint) || endpoint->state != DWC2_EP_WAITING
			|| endpoint->channel >= 0) {
			continue;
		}
		if (endpoint->wake_time > now) {
			deadline = min_c(deadline, endpoint->wake_time);
			continue;
		}
		int32 index = _AllocateChannel(false);
		if (index < 0) {
			waiting = true;
			continue;
		}
		fChannels[index].endpoint = endpoint;
		endpoint->channel = index;
		_StartRun(endpoint, 0);
	}

	// A NAKing IN holds its channel (the core retries by itself). Give it to
	// the waiting endpoints when it has had it for a while.
	if (waiting) {
		now = system_time();
		for (uint32 i = 0; i < fChannelCount; i++) {
			dwc2_channel* channel = &fChannels[i];
			if (!channel->running || channel->halt_request != DWC2_HALT_NONE
				|| channel->endpoint == NULL || !channel->incoming
				|| channel->split || _IsPeriodic(channel->endpoint)
				|| now - channel->started < kPreemptAfter) {
				continue;
			}
			if ((_Read(DWC2_HCINT(i)) & DWC2_HCINT_NAK) != 0)
				_RequestHalt(i, DWC2_HALT_PREEMPT);
		}
		deadline = min_c(deadline, now + kPreemptAfter);
	}

	_SetSOFInterrupt(needSOF);
	return deadline;
}


int32
DWC2::_AllocateChannel(bool periodic)
{
	uint32 free = 0;
	int32 index = -1;
	for (uint32 i = 0; i < fChannelCount; i++) {
		if (fChannels[i].endpoint == NULL) {
			if (index < 0)
				index = i;
			free++;
		}
	}
	if (!periodic && free <= kPeriodicReserve)
		return -1;
	return index;
}


void
DWC2::_ReleaseChannel(dwc2_endpoint* endpoint)
{
	if (endpoint->channel < 0)
		return;
	dwc2_channel* channel = &fChannels[endpoint->channel];
	channel->endpoint = NULL;
	channel->running = false;
	channel->halt_request = DWC2_HALT_NONE;
	channel->halt_requested = 0;
	endpoint->channel = -1;
}


void
DWC2::_RequestHalt(int32 index, uint8 reason)
{
	dwc2_channel* channel = &fChannels[index];
	if (channel->halt_requested == 0)
		channel->halt_requested = system_time();
	if (reason != DWC2_HALT_NONE)
		channel->halt_request = reason;

	// Periodic channels can't be halted in buffer DMA mode: they halt by
	// themselves at the end of their (micro)frame.
	dwc2_endpoint* endpoint = channel->endpoint;
	if (endpoint != NULL && _IsPeriodic(endpoint) && !channel->split)
		return;

	uint32 hcchar = _Read(DWC2_HCCHAR(index));
	if ((hcchar & DWC2_HCCHAR_CHENA) != 0) {
		_Write(DWC2_HCCHAR(index),
			hcchar | DWC2_HCCHAR_CHENA | DWC2_HCCHAR_CHDIS);
	}
}


void
DWC2::_Requeue(dwc2_endpoint* endpoint, bigtime_t wakeTime)
{
	endpoint->state = DWC2_EP_WAITING;
	endpoint->wake_time = wakeTime;

	// to the end of the list: the others get a channel first
	if (endpoint->link == NULL)
		return;
	dwc2_endpoint** link = &fEndpoints;
	while (*link != endpoint)
		link = &(*link)->link;
	*link = endpoint->link;
	while (*link != NULL)
		link = &(*link)->link;
	*link = endpoint;
	endpoint->link = NULL;
}


void
DWC2::_NextPoll(dwc2_endpoint* endpoint, bigtime_t now)
{
	_ReleaseChannel(endpoint);
	endpoint->complete_split = false;
	if (endpoint->first == NULL) {
		endpoint->state = DWC2_EP_IDLE;
		return;
	}
	bigtime_t next = endpoint->poll_time + endpoint->period;
	_Requeue(endpoint, next > now ? next : now);
}


//	#pragma mark - runs


bool
DWC2::_UsesSplit(Pipe* pipe) const
{
	return fPortSpeed == USB_SPEED_HIGHSPEED
		&& pipe->Speed() < USB_SPEED_HIGHSPEED;
}


/*!	The time between two polls of an interrupt endpoint. High-speed
	endpoints asking for less than a millisecond are polled every
	millisecond, keeping NAK interrupts at a thousand a second.
*/
bigtime_t
DWC2::_Period(Pipe* pipe) const
{
	uint8 interval = pipe->Interval();
	if (pipe->Speed() == USB_SPEED_HIGHSPEED) {
		interval = min_c(max_c(interval, 1), 16);
		bigtime_t period = 125LL << (interval - 1);
		return max_c(period, 1000);
	}
	return max_c(interval, 1) * 1000LL;
}


/*!	Programs the endpoint's channel with the next run of its first
	transfer and enables it. A periodic run goes in (micro)frame \a frame.
*/
status_t
DWC2::_StartRun(dwc2_endpoint* endpoint, uint16 frame)
{
	int32 index = endpoint->channel;
	dwc2_channel* channel = &fChannels[index];
	dwc2_transfer* transfer = endpoint->first;
	Pipe* pipe = endpoint->pipe;

	bool split = _UsesSplit(pipe);
	bool complete = split && endpoint->complete_split;
	bool periodic = _IsPeriodic(endpoint);
	uint32 maxPacketSize = pipe->MaxPacketSize() & DWC2_HCCHAR_MPS_MASK;
	if (maxPacketSize == 0)
		maxPacketSize = 8;

	bool in;
	uint8 pid;
	size_t remaining = transfer->length - transfer->offset;
	bool setup = false;
	if (endpoint->type == DWC2_EPTYPE_CONTROL) {
		switch (transfer->phase) {
			case DWC2_PHASE_SETUP:
				in = false;
				pid = DWC2_PID_SETUP;
				setup = true;
				break;
			case DWC2_PHASE_DATA:
				in = transfer->incoming;
				pid = endpoint->control_pid;
				break;
			default:
				// the status stage goes the other way, IN without data
				in = !transfer->incoming || transfer->length == 0;
				pid = DWC2_PID_DATA1;
				remaining = 0;
				break;
		}
	} else {
		in = pipe->Direction() == Pipe::In;
		pid = pipe->DataToggle() ? DWC2_PID_DATA1 : DWC2_PID_DATA0;
	}

	uint32 length;
	uint32 packets = 1;
	uint32 multiCount = 1;
	if (setup) {
		length = complete ? 0 : 8;
	} else if (split) {
		// one packet per split transaction
		if (complete)
			length = in ? maxPacketSize : 0;
		else
			length = in ? maxPacketSize : min_c(remaining, maxPacketSize);
		if (periodic)
			multiCount = 3;
	} else if (periodic) {
		uint32 mult = pipe->Speed() == USB_SPEED_HIGHSPEED
			? pipe->MaxBurst() + 1 : 1;
		packets = (remaining + maxPacketSize - 1) / maxPacketSize;
		packets = max_c(min_c(packets, mult), 1);
		length = in ? packets * maxPacketSize
			: min_c(remaining, packets * maxPacketSize);
		multiCount = packets;
	} else {
		uint32 maxRun = min_c(min_c(fMaxTransferSize, kBufferSize),
			fMaxPacketCount * maxPacketSize);
		maxRun -= maxRun % maxPacketSize;
		if (in) {
			length = min_c(ROUNDUP(remaining, maxPacketSize), maxRun);
		} else
			length = min_c(remaining, maxRun);
		packets = max_c((length + maxPacketSize - 1) / maxPacketSize, 1);
	}

	// the data
	if (!in && length > 0) {
		if (setup) {
			memcpy(channel->buffer, transfer->transfer->RequestData(), 8);
		} else {
			if (transfer->transfer->PrepareKernelAccess() != B_OK
				|| copy_transfer_data(transfer->transfer, transfer->offset,
					channel->buffer, length, false) != B_OK) {
				TRACE_ALWAYS_D("cannot read the data of transfer %p\n",
					transfer->transfer);
				memset(channel->buffer, 0, length);
			}
		}
		dwc2_clean_cache(channel->buffer, length);
	} else if (in && length > 0)
		dwc2_invalidate_cache(channel->buffer, length, true);
	if (split && !complete && !in)
		endpoint->split_out_length = length;

	uint32 hcchar = maxPacketSize
		| (uint32)(pipe->EndpointAddress() & 0xf) << DWC2_HCCHAR_EPNUM_SHIFT
		| (in ? DWC2_HCCHAR_EPDIR_IN : 0)
		| (pipe->Speed() == USB_SPEED_LOWSPEED ? DWC2_HCCHAR_LSPDDEV : 0)
		| (uint32)endpoint->type << DWC2_HCCHAR_EPTYPE_SHIFT
		| multiCount << DWC2_HCCHAR_MC_SHIFT
		| (uint32)(pipe->DeviceAddress() & 0x7f) << DWC2_HCCHAR_DEVADDR_SHIFT;
	uint32 hcsplt = 0;
	if (split) {
		hcsplt = DWC2_HCSPLT_SPLTENA | DWC2_HCSPLT_XACTPOS_ALL
			| (uint32)(pipe->HubAddress() & 0x7f) << DWC2_HCSPLT_HUBADDR_SHIFT
			| (pipe->HubPort() & DWC2_HCSPLT_PRTADDR_MASK)
			| (complete ? DWC2_HCSPLT_COMPSPLT : 0);
	}
	bool ping = endpoint->ping && !in && !split
		&& pipe->Speed() == USB_SPEED_HIGHSPEED;
	endpoint->ping = false;
	uint32 hctsiz = length | packets << DWC2_HCTSIZ_PKTCNT_SHIFT
		| (uint32)pid << DWC2_HCTSIZ_PID_SHIFT
		| (ping ? DWC2_HCTSIZ_DOPNG : 0);

	channel->running = true;
	channel->halt_request = DWC2_HALT_NONE;
	channel->halt_requested = 0;
	channel->started = system_time();
	channel->incoming = in;
	channel->split = split;
	channel->length = length;
	channel->packets = packets;
	channel->max_packet_size = maxPacketSize;
	channel->pid = pid;
	endpoint->state = DWC2_EP_RUNNING;
	if (periodic && !complete)
		endpoint->poll_time = channel->started;

	_Write(DWC2_HCINTMSK(index), DWC2_HCINT_CHHLTD | DWC2_HCINT_AHBERR);
	_Write(DWC2_HCINT(index), 0xffffffff);
	{
		// nothing of an earlier run may be taken for this one
		InterruptsSpinLocker locker(fInterruptLock);
		fHaltedChannels &= ~(1u << index);
		fChannelInterrupts[index] = 0;
	}
	_Write(DWC2_HCSPLT(index), hcsplt);
	_Write(DWC2_HCTSIZ(index), hctsiz);
	_Write(DWC2_HCDMA(index),
		(uint32)(channel->buffer_physical + fPlatform.dma_offset));

	if (periodic) {
		// The channel runs in the next (micro)frame of the given parity.
		uint16 next = frame_add(_Frame(), 1);
		if (frame_diff(frame, next) < 0)
			frame = next;
		channel->target_frame = frame;
		if ((frame & 1) != 0)
			hcchar |= DWC2_HCCHAR_ODDFRM;
	}
	_Write(DWC2_HCCHAR(index), hcchar | DWC2_HCCHAR_CHENA);

	TRACE_D("ch%" B_PRId32 " start: dev %d ep %#x type %u %s%s%s len %" B_PRIu32
		" pkts %" B_PRIu32 " pid %u frame %u\n", index, pipe->DeviceAddress(),
		pipe->EndpointAddress(), endpoint->type, in ? "in" : "out",
		split ? " split" : "", complete ? " complete" : "", length, packets,
		pid, frame);
	return B_OK;
}


/*!	Copies \a length bytes of an IN run from the channel's buffer to the
	transfer.
*/
bool
DWC2::_CopyIn(dwc2_channel* channel, dwc2_transfer* transfer, size_t length)
{
	if (length == 0)
		return true;
	dwc2_invalidate_cache(channel->buffer, length, false);
	return transfer->transfer->PrepareKernelAccess() == B_OK
		&& copy_transfer_data(transfer->transfer, transfer->offset,
			channel->buffer, length, true) == B_OK;
}


/*!	Completes the endpoint's first transfer with \a status and lets the
	endpoint go on with the next one.
*/
void
DWC2::_FinishRun(dwc2_endpoint* endpoint, status_t status,
	dwc2_transfer** _done)
{
	dwc2_transfer* transfer = endpoint->first;
	if (transfer != NULL) {
		endpoint->first = transfer->next;
		if (endpoint->first == NULL)
			endpoint->last = NULL;
		transfer->status = status;
		transfer->next = NULL;
		dwc2_transfer** link = _done;
		while (*link != NULL)
			link = &(*link)->next;
		*link = transfer;
	}

	endpoint->error_count = 0;
	endpoint->nak_count = 0;
	endpoint->nyet_count = 0;
	endpoint->complete_split = false;
	endpoint->ping = false;

	bigtime_t now = system_time();
	if (_IsPeriodic(endpoint)) {
		_NextPoll(endpoint, now);
		return;
	}
	_ReleaseChannel(endpoint);
	if (endpoint->first != NULL)
		_Requeue(endpoint, now);
	else
		endpoint->state = DWC2_EP_IDLE;
}


/*!	A channel halted: find out why from its interrupts and decide what the
	endpoint does next (Linux' dwc2_hc_chhltd_intr_dma()).
*/
void
DWC2::_HandleHalt(int32 index, uint32 hcint, dwc2_transfer** _done)
{
	dwc2_channel* channel = &fChannels[index];
	dwc2_endpoint* endpoint = channel->endpoint;
	if (!channel->running || endpoint == NULL) {
		if (endpoint == NULL)
			channel->running = false;
		return;
	}
	channel->running = false;
	uint8 request = channel->halt_request;
	channel->halt_request = DWC2_HALT_NONE;
	channel->halt_requested = 0;

	uint32 hctsiz = _Read(DWC2_HCTSIZ(index));
	uint32 leftBytes = hctsiz & DWC2_HCTSIZ_XFERSIZE_MASK;
	uint32 leftPackets = (hctsiz >> DWC2_HCTSIZ_PKTCNT_SHIFT)
		& DWC2_HCTSIZ_PKTCNT_MASK;
	uint8 nextPid = (hctsiz >> DWC2_HCTSIZ_PID_SHIFT) & DWC2_HCTSIZ_PID_MASK;

	TRACE_D("ch%" B_PRId32 " halt: hcint %#" B_PRIx32 " hctsiz %#" B_PRIx32
		" request %u\n", index, hcint, hctsiz, request);

	if (endpoint->destroyed) {
		_ReleaseChannel(endpoint);
		_FreeEndpoint(endpoint);
		return;
	}

	Pipe* pipe = endpoint->pipe;
	bool periodic = _IsPeriodic(endpoint);
	bool toggles = endpoint->type == DWC2_EPTYPE_BULK
		|| endpoint->type == DWC2_EPTYPE_INTERRUPT;
	bool validPid = nextPid == DWC2_PID_DATA0 || nextPid == DWC2_PID_DATA1;
	dwc2_transfer* transfer = endpoint->first;
	bigtime_t now = system_time();

	// what went over the bus: whole packets unless the run completed
	size_t moved = 0;
	if ((hcint & DWC2_HCINT_XFERCOMPL) != 0) {
		if (channel->incoming)
			moved = channel->length - min_c(leftBytes, channel->length);
		else if (channel->split)
			moved = endpoint->split_out_length;
		else
			moved = channel->length;
	} else if (!channel->split && channel->packets > leftPackets) {
		moved = min_c((channel->packets - leftPackets)
			* channel->max_packet_size, channel->length);
	}

	if (request == DWC2_HALT_CANCEL || transfer == NULL) {
		// what moved belonged to the canceled transfer; keep the toggle
		if (toggles && !channel->split && moved > 0 && validPid)
			pipe->SetDataToggle(nextPid == DWC2_PID_DATA1);
		_ReleaseChannel(endpoint);
		endpoint->complete_split = false;
		if (endpoint->first != NULL)
			_Requeue(endpoint, now);
		else
			endpoint->state = DWC2_EP_IDLE;
		return;
	}

	bool control = endpoint->type == DWC2_EPTYPE_CONTROL;
	bool dataStage = !control || transfer->phase == DWC2_PHASE_DATA;
	size_t remaining = transfer->length - transfer->offset;

	// Takes the data of a run that ended without completing: whole packets
	// (a short one would have completed it).
	struct Partial {
		static void Take(DWC2* dwc2, dwc2_channel* channel,
			dwc2_endpoint* endpoint, dwc2_transfer* transfer, size_t moved,
			bool dataStage, bool toggles, bool validPid, uint8 nextPid)
		{
			if (moved == 0 || !dataStage)
				return;
			moved = min_c(moved, transfer->length - transfer->offset);
			if (channel->incoming)
				dwc2->_CopyIn(channel, transfer, moved);
			transfer->offset += moved;
			if (validPid) {
				if (toggles)
					endpoint->pipe->SetDataToggle(nextPid == DWC2_PID_DATA1);
				else
					endpoint->control_pid = nextPid;
			}
		}
	};

	if ((hcint & DWC2_HCINT_XFERCOMPL) != 0) {
		endpoint->error_count = 0;
		endpoint->nak_count = 0;
		endpoint->nyet_count = 0;
		endpoint->complete_split = false;

		if (control && transfer->phase == DWC2_PHASE_SETUP) {
			transfer->phase = transfer->length > 0
				? DWC2_PHASE_DATA : DWC2_PHASE_STATUS;
			endpoint->control_pid = DWC2_PID_DATA1;
			_StartRun(endpoint, 0);
			return;
		}
		if (control && transfer->phase == DWC2_PHASE_STATUS) {
			_FinishRun(endpoint, B_OK, _done);
			return;
		}

		// a data run
		status_t status = B_OK;
		size_t taken = min_c(moved, remaining);
		if (channel->incoming) {
			if (moved > remaining)
				status = B_DEV_DATA_OVERRUN;
			if (!_CopyIn(channel, transfer, taken))
				status = B_DEV_DATA_OVERRUN;
		}
#ifdef TRACE_DWC2_DATA
		if (periodic && channel->incoming && taken > 0) {
			const uint8* data = channel->buffer;
			TRACE_DATA("ch%" B_PRId32 " dev %d ep %#x: %" B_PRIuSIZE " bytes"
				" %02x %02x %02x %02x %02x %02x %02x %02x\n", index,
				pipe->DeviceAddress(), pipe->EndpointAddress(), taken,
				data[0], data[1], data[2], data[3], data[4], data[5], data[6],
				data[7]);
		}
#endif
		transfer->offset += taken;
		if (validPid) {
			if (toggles)
				pipe->SetDataToggle(nextPid == DWC2_PID_DATA1);
			else
				endpoint->control_pid = nextPid;
		}
		bool shortPacket = channel->incoming && moved < channel->length;
		bool finished = status != B_OK || shortPacket
			|| transfer->offset >= transfer->length;
		if (control) {
			if (status != B_OK) {
				_FinishRun(endpoint, status, _done);
				return;
			}
			if (finished)
				transfer->phase = DWC2_PHASE_STATUS;
			_StartRun(endpoint, 0);
			return;
		}
		if (finished) {
			_FinishRun(endpoint, status, _done);
			return;
		}
		if (periodic)
			_NextPoll(endpoint, now);
		else
			_StartRun(endpoint, 0);
		return;
	}

	if ((hcint & DWC2_HCINT_STALL) != 0) {
		_FinishRun(endpoint, B_DEV_STALLED, _done);
		return;
	}

	if ((hcint & DWC2_HCINT_BBLERR) != 0) {
		_FinishRun(endpoint, B_DEV_DATA_OVERRUN, _done);
		return;
	}

	if ((hcint & DWC2_HCINT_AHBERR) != 0) {
		TRACE_ALWAYS_D("ch%" B_PRId32 ": AHB error (DMA %#" B_PRIx32 ")\n",
			index, _Read(DWC2_HCDMA(index)));
		_FinishRun(endpoint, channel->incoming
			? B_DEV_FIFO_OVERRUN : B_DEV_FIFO_UNDERRUN, _done);
		return;
	}

	if ((hcint & (DWC2_HCINT_XACTERR | DWC2_HCINT_DATATGLERR)) != 0) {
		if (!channel->split) {
			Partial::Take(this, channel, endpoint, transfer, moved, dataStage,
				toggles, validPid, nextPid);
		}
		endpoint->complete_split = false;
		endpoint->nyet_count = 0;
		if (++endpoint->error_count >= kMaxErrors) {
			TRACE_ALWAYS_D("dev %d ep %#x: transaction errors (%#" B_PRIx32
				")\n", pipe->DeviceAddress(), pipe->EndpointAddress(), hcint);
			_FinishRun(endpoint, B_DEV_CRC_ERROR, _done);
			return;
		}
		if (periodic)
			_NextPoll(endpoint, now);
		else
			_StartRun(endpoint, 0);
		return;
	}

	if (request == DWC2_HALT_PREEMPT) {
		Partial::Take(this, channel, endpoint, transfer, moved, dataStage,
			toggles, validPid, nextPid);
		if (dataStage && transfer->offset >= transfer->length && !control) {
			_FinishRun(endpoint, B_OK, _done);
			return;
		}
		_ReleaseChannel(endpoint);
		_Requeue(endpoint, now);
		return;
	}

	if ((hcint & DWC2_HCINT_FRMOVRUN) != 0) {
		// the periodic run did not fit into its (micro)frame
		endpoint->complete_split = false;
		if (periodic) {
			_ReleaseChannel(endpoint);
			_Requeue(endpoint, now);
		} else
			_StartRun(endpoint, 0);
		return;
	}

	if ((hcint & DWC2_HCINT_NYET) != 0) {
		if (channel->split && endpoint->complete_split) {
			// the hub's transaction translator has no answer yet
			if (!periodic) {
				_StartRun(endpoint, 0);
				return;
			}
			uint16 next = frame_add(_Frame(), 1);
			if (++endpoint->nyet_count < 4
				&& frame_diff(next, endpoint->split_frame) <= 6) {
				_StartRun(endpoint, next);
				return;
			}
			// missed: poll again, the data was not taken
			endpoint->nyet_count = 0;
			_NextPoll(endpoint, now);
			return;
		}
		// high-speed OUT: the device took the packet but wants a PING
		Partial::Take(this, channel, endpoint, transfer, moved, dataStage,
			toggles, validPid, nextPid);
		endpoint->ping = true;
		if (dataStage && !control && transfer->offset >= transfer->length) {
			_FinishRun(endpoint, B_OK, _done);
			return;
		}
		_StartRun(endpoint, 0);
		return;
	}

	if ((hcint & DWC2_HCINT_NAK) != 0) {
		endpoint->error_count = 0;
		if (channel->split) {
			endpoint->complete_split = false;
			endpoint->nyet_count = 0;
			if (periodic) {
				_NextPoll(endpoint, now);
				return;
			}
			// Don't spin through start and complete splits for an endpoint
			// that has nothing: after a few NAKs, retry a millisecond later
			// (Linux' DWC2_NAKS_BEFORE_DELAY). A control IN is expected to
			// answer soon.
			if (++endpoint->nak_count >= kSplitNakLimit
				&& !(control && channel->incoming)) {
				_ReleaseChannel(endpoint);
				_Requeue(endpoint, now + kNakBackoff);
				return;
			}
			_StartRun(endpoint, 0);
			return;
		}
		if (periodic) {
			_NextPoll(endpoint, now);
			return;
		}
		// a bulk or control OUT that halts on NAK (cores before 2.71a)
		Partial::Take(this, channel, endpoint, transfer, moved, dataStage,
			toggles, validPid, nextPid);
		endpoint->ping = true;
		if (++endpoint->nak_count >= kOutNakLimit) {
			endpoint->nak_count = 0;
			_ReleaseChannel(endpoint);
			_Requeue(endpoint, now + kNakBackoff);
			return;
		}
		_StartRun(endpoint, 0);
		return;
	}

	if ((hcint & DWC2_HCINT_ACK) != 0 && channel->split
		&& !endpoint->complete_split) {
		// the hub took the start split: collect the result
		endpoint->complete_split = true;
		endpoint->nyet_count = 0;
		endpoint->nak_count = 0;
		if (!periodic) {
			_StartRun(endpoint, 0);
			return;
		}
		// two microframes after the start split (C-mask 0x1c)
		endpoint->split_frame = channel->target_frame;
		endpoint->start_frame = frame_add(channel->target_frame, 2);
		endpoint->state = DWC2_EP_FRAME_WAIT;
		return;
	}

	// no reason: a periodic run the core could not fit in its frame
	endpoint->complete_split = false;
	if (periodic) {
		_ReleaseChannel(endpoint);
		_Requeue(endpoint, now);
		return;
	}
	TRACE_ALWAYS_D("ch%" B_PRId32 ": halted without a reason (HCINT %#"
		B_PRIx32 ")\n", index, hcint);
	if (++endpoint->error_count >= kMaxErrors) {
		_FinishRun(endpoint, B_DEV_CRC_ERROR, _done);
		return;
	}
	_StartRun(endpoint, 0);
}


//	#pragma mark - transfers


dwc2_endpoint*
DWC2::_Endpoint(Pipe* pipe)
{
	dwc2_endpoint* endpoint = (dwc2_endpoint*)pipe->ControllerCookie();
	if (endpoint != NULL)
		return endpoint;

	endpoint = new(std::nothrow) dwc2_endpoint;
	if (endpoint == NULL)
		return NULL;
	memset(endpoint, 0, sizeof(*endpoint));
	endpoint->pipe = pipe;
	endpoint->channel = -1;
	endpoint->state = DWC2_EP_IDLE;
	uint32 type = pipe->Type();
	if ((type & USB_OBJECT_CONTROL_PIPE) != 0)
		endpoint->type = DWC2_EPTYPE_CONTROL;
	else if ((type & USB_OBJECT_INTERRUPT_PIPE) != 0)
		endpoint->type = DWC2_EPTYPE_INTERRUPT;
	else if ((type & USB_OBJECT_ISO_PIPE) != 0)
		endpoint->type = DWC2_EPTYPE_ISOCHRONOUS;
	else
		endpoint->type = DWC2_EPTYPE_BULK;

	// at the end of the list
	dwc2_endpoint** link = &fEndpoints;
	while (*link != NULL)
		link = &(*link)->link;
	*link = endpoint;

	pipe->SetControllerCookie(endpoint);
	return endpoint;
}


void
DWC2::_FreeEndpoint(dwc2_endpoint* endpoint)
{
	dwc2_endpoint** link = &fEndpoints;
	while (*link != NULL && *link != endpoint)
		link = &(*link)->link;
	if (*link == endpoint)
		*link = endpoint->link;
	delete endpoint;
}


status_t
DWC2::SubmitTransfer(Transfer* transfer)
{
	Pipe* pipe = transfer->TransferPipe();
	if (pipe->DeviceAddress() == fRootHubAddress)
		return DWC2RootHub::ProcessTransfer(this, transfer);

	if ((pipe->Type() & USB_OBJECT_ISO_PIPE) != 0)
		return B_NOT_SUPPORTED;

	status_t status = transfer->InitKernelAccess();
	if (status != B_OK)
		return status;

	dwc2_transfer* data = new(std::nothrow) dwc2_transfer;
	if (data == NULL)
		return B_NO_MEMORY;
	data->transfer = transfer;
	data->next = NULL;
	data->length = vector_length(transfer);
	data->offset = 0;
	data->phase = DWC2_PHASE_SETUP;
	data->status = B_OK;
	if ((pipe->Type() & USB_OBJECT_CONTROL_PIPE) != 0) {
		usb_request_data* request = transfer->RequestData();
		if (request == NULL) {
			delete data;
			return B_BAD_VALUE;
		}
		data->incoming = (request->RequestType & USB_REQTYPE_DEVICE_IN) != 0;
		data->length = min_c(data->length, request->Length);
	} else {
		data->incoming = pipe->Direction() == Pipe::In;
		data->phase = DWC2_PHASE_DATA;
	}

	MutexLocker locker(fLock);
	dwc2_endpoint* endpoint = _Endpoint(pipe);
	if (endpoint == NULL) {
		delete data;
		return B_NO_MEMORY;
	}
	if (endpoint->last != NULL)
		endpoint->last->next = data;
	else
		endpoint->first = data;
	endpoint->last = data;

	if (endpoint->state == DWC2_EP_IDLE) {
		bigtime_t now = system_time();
		endpoint->state = DWC2_EP_WAITING;
		endpoint->wake_time = now;
		if (_IsPeriodic(endpoint)) {
			endpoint->period = _Period(pipe);
			bigtime_t next = endpoint->poll_time + endpoint->period;
			if (next > now)
				endpoint->wake_time = next;
		}
	}
	locker.Unlock();

	_Wake();
	return B_OK;
}


status_t
DWC2::CancelQueuedTransfers(Pipe* pipe, bool force)
{
	MutexLocker locker(fLock);
	dwc2_endpoint* endpoint = (dwc2_endpoint*)pipe->ControllerCookie();
	if (endpoint == NULL)
		return B_OK;

	dwc2_transfer* canceled = endpoint->first;
	endpoint->first = endpoint->last = NULL;

	if (endpoint->channel >= 0) {
		dwc2_channel* channel = &fChannels[endpoint->channel];
		if (channel->running)
			_RequestHalt(endpoint->channel, DWC2_HALT_CANCEL);
		else
			_ReleaseChannel(endpoint);
	}
	endpoint->state = DWC2_EP_IDLE;
	endpoint->complete_split = false;
	endpoint->error_count = 0;
	endpoint->nak_count = 0;
	endpoint->nyet_count = 0;
	locker.Unlock();

	while (canceled != NULL) {
		dwc2_transfer* transfer = canceled;
		canceled = transfer->next;
		if (!force)
			transfer->transfer->Finished(B_CANCELED, 0);
		delete transfer->transfer;
		delete transfer;
	}
	return B_OK;
}


status_t
DWC2::NotifyPipeChange(Pipe* pipe, usb_change change)
{
	switch (change) {
		case USB_CHANGE_CREATED:
		{
			MutexLocker locker(fLock);
			if (_Endpoint(pipe) == NULL)
				return B_NO_MEMORY;
			return B_OK;
		}

		case USB_CHANGE_DESTROYED:
		{
			CancelQueuedTransfers(pipe, true);
			MutexLocker locker(fLock);
			dwc2_endpoint* endpoint
				= (dwc2_endpoint*)pipe->ControllerCookie();
			if (endpoint == NULL)
				return B_OK;
			pipe->SetControllerCookie(NULL);
			endpoint->destroyed = true;
			endpoint->pipe = NULL;
			// a channel still halting frees it when it halted
			if (endpoint->channel < 0)
				_FreeEndpoint(endpoint);
			return B_OK;
		}

		case USB_CHANGE_PIPE_POLICY_CHANGED:
			return B_OK;
	}
	return B_OK;
}


//	#pragma mark - device tree attachment


#define DWC2_FDT_DRIVER	"busses/usb/dwc2/fdt/driver_v1"
#define DWC2_FDT_BUS	"busses/usb/dwc2/fdt/device_v1"


static const char* const kCompatibles[] = {
	"brcm,bcm2835-usb",
	"brcm,bcm2708-usb",
	NULL
};


struct dwc2_fdt_info {
	device_node*		node;
	dwc2_platform_info	platform;
};


static uint32
fdt_cells(fdt_device_module_info* fdt, fdt_device* device, const char* name,
	uint32 defaultValue)
{
	int length;
	const uint32* value = (const uint32*)fdt->get_prop(device, name, &length);
	if (value == NULL || length != 4)
		return defaultValue;
	return B_BENDIAN_TO_HOST_INT32(*value);
}


static uint64
fdt_address(const uint32* cells, uint32 count)
{
	uint64 address = 0;
	for (uint32 i = 0; i < count; i++)
		address = (address << 32) | B_BENDIAN_TO_HOST_INT32(cells[i]);
	return address;
}


/*!	The controller's view of RAM from the first "dma-ranges" above \a node:
	the bus address of physical address 0 and the end of the window. The
	BCM2837's /soc has <0xc0000000 0x0 0x3f000000>.
*/
static void
fdt_dma_window(device_node* node, uint64* _offset, phys_addr_t* _limit)
{
	*_offset = 0;
	*_limit = 0x100000000ULL;

	device_node* current = gDeviceManager->get_parent_node(node);
	while (current != NULL) {
		const char* bus;
		fdt_device_module_info* fdt;
		fdt_device* device;
		if (gDeviceManager->get_attr_string(current, B_DEVICE_BUS, &bus,
				false) != B_OK || strcmp(bus, "fdt") != 0
			|| gDeviceManager->get_driver(current, (driver_module_info**)&fdt,
				(void**)&device) != B_OK) {
			gDeviceManager->put_node(current);
			return;
		}
		int length;
		const uint32* ranges = (const uint32*)fdt->get_prop(device,
			"dma-ranges", &length);
		device_node* parent = gDeviceManager->get_parent_node(current);
		if (ranges != NULL && length > 0) {
			uint32 childCells = fdt_cells(fdt, device, "#address-cells", 2);
			uint32 sizeCells = fdt_cells(fdt, device, "#size-cells", 1);
			uint32 parentCells = 2;
			fdt_device_module_info* parentFDT;
			fdt_device* parentDevice;
			if (parent != NULL && gDeviceManager->get_attr_string(parent,
					B_DEVICE_BUS, &bus, false) == B_OK
				&& strcmp(bus, "fdt") == 0
				&& gDeviceManager->get_driver(parent,
					(driver_module_info**)&parentFDT, (void**)&parentDevice)
					== B_OK) {
				parentCells = fdt_cells(parentFDT, parentDevice,
					"#address-cells", 2);
			}
			uint32 entryCells = childCells + parentCells + sizeCells;
			if (childCells <= 2 && parentCells <= 2 && sizeCells <= 2
				&& length >= (int)(entryCells * 4)) {
				uint64 child = fdt_address(ranges, childCells);
				uint64 physical = fdt_address(ranges + childCells,
					parentCells);
				uint64 size = fdt_address(ranges + childCells + parentCells,
					sizeCells);
				*_offset = child - physical;
				*_limit = min_c(physical + size, 0x100000000ULL);
			}
			if (parent != NULL)
				gDeviceManager->put_node(parent);
			gDeviceManager->put_node(current);
			return;
		}
		gDeviceManager->put_node(current);
		current = parent;
	}
}


static float
supports_fdt(device_node* parent)
{
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (!usb_fdt_get_device(gDeviceManager, parent, kCompatibles, &fdt,
			&device)) {
		return 0;
	}
	// the OTG port as a gadget is not ours
	if (usb_fdt_has_string(fdt, device, "dr_mode", "peripheral"))
		return 0;
	return 0.8f;
}


static status_t
register_fdt(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "DWC2 FDT" } },
		{}
	};
	return gDeviceManager->register_node(parent, DWC2_FDT_DRIVER, attrs, NULL,
		NULL);
}


static status_t
init_fdt(device_node* node, void** cookie)
{
	device_node* parent = gDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = gDeviceManager->get_driver(parent,
		(driver_module_info**)&fdt, (void**)&device);
	if (status != B_OK) {
		gDeviceManager->put_node(parent);
		return status;
	}

	dwc2_platform_info platform = {};
	uint64 base, size, irq;
	if (!fdt->get_reg(device, 0, &base, &size)
		|| !fdt->get_interrupt(device, 0, NULL, &irq)
		|| size < 0x1000 || irq == 0) {
		dprintf("dwc2: unsupported FDT resources for %s\n",
			fdt->get_name(device));
		gDeviceManager->put_node(parent);
		return B_NOT_SUPPORTED;
	}
	platform.register_base = base;
	platform.register_size = size;
	platform.interrupt = irq;
	platform.bcm2835 = usb_fdt_has_string(fdt, device, "compatible",
			"brcm,bcm2835-usb")
		|| usb_fdt_has_string(fdt, device, "compatible", "brcm,bcm2708-usb");
	fdt_dma_window(parent, &platform.dma_offset, &platform.dma_limit);
	gDeviceManager->put_node(parent);

	dwc2_fdt_info* info = new(std::nothrow) dwc2_fdt_info;
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;
	info->platform = platform;
	*cookie = info;
	dprintf("dwc2: FDT registers %#" B_PRIx64 ", IRQ %" B_PRIu64
		", DMA at %#" B_PRIx64 " below %#" B_PRIxPHYSADDR "\n", base, irq,
		platform.dma_offset, platform.dma_limit);
	return B_OK;
}


static void
uninit_fdt(void* cookie)
{
	delete (dwc2_fdt_info*)cookie;
}


static status_t
register_fdt_children(void* cookie)
{
	dwc2_fdt_info* info = (dwc2_fdt_info*)cookie;
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "DWC2 Controller" } },
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{ .string = USB_FOR_CONTROLLER_MODULE_NAME } },
		{}
	};
	return gDeviceManager->register_node(info->node, DWC2_FDT_BUS, attrs, NULL,
		NULL);
}


static status_t
init_fdt_bus(device_node* node, void** cookie)
{
	device_node* parent = gDeviceManager->get_parent_node(node);
	dwc2_fdt_info* info;
	status_t status = gDeviceManager->get_driver(parent, NULL, (void**)&info);
	gDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	Stack* stack;
	status = gUSB->get_stack((void**)&stack);
	if (status != B_OK)
		return status;

	DWC2* dwc2 = new(std::nothrow) DWC2(stack, node, &info->platform);
	if (dwc2 == NULL)
		return B_NO_MEMORY;
	status = dwc2->InitCheck();
	if (status == B_OK)
		status = dwc2->Start();
	if (status != B_OK) {
		delete dwc2;
		return status;
	}
	*cookie = dwc2;
	return B_OK;
}


static void
uninit_fdt_bus(void* cookie)
{
	delete (DWC2*)cookie;
}


module_dependency module_dependencies[] = {
	{ USB_FOR_CONTROLLER_MODULE_NAME, (module_info**)&gUSB },
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&gDeviceManager },
	{}
};


static driver_module_info sDWC2FDTDriver = {
	{ DWC2_FDT_DRIVER, 0, NULL },
	supports_fdt,
	register_fdt,
	init_fdt,
	uninit_fdt,
	register_fdt_children,
	NULL,
	NULL
};


static usb_bus_interface sDWC2FDTBus = {
	{
		{ DWC2_FDT_BUS, 0, NULL },
		NULL, NULL,
		init_fdt_bus,
		uninit_fdt_bus,
		NULL, NULL, NULL
	}
};


module_info* modules[] = {
	(module_info*)&sDWC2FDTDriver,
	(module_info*)&sDWC2FDTBus,
	NULL
};
