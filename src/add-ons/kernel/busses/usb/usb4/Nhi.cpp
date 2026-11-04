/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * The USB4 host interface: control rings, interrupts and the control
 * channel protocol (USB4 specification chapters 6.4 and 12).
 */


#include "Nhi.h"

#include <ByteOrder.h>
#include <string.h>

#include <util/AutoLock.h>
#include <vm/vm.h>

#include "Domain.h"
#include "usb4_debug.h"


static uint32 sCRC32CTable[256];


static void
init_crc32c()
{
	if (sCRC32CTable[1] != 0)
		return;

	for (uint32 i = 0; i < 256; i++) {
		uint32 crc = i;
		for (int bit = 0; bit < 8; bit++)
			crc = (crc & 1) != 0 ? (crc >> 1) ^ 0x82f63b78 : crc >> 1;
		sCRC32CTable[i] = crc;
	}
}


//! Standard CRC-32C (Castagnoli) of the packet as it goes on the wire.
static uint32
crc32c(const uint8* data, size_t length)
{
	uint32 crc = 0xffffffff;
	for (size_t i = 0; i < length; i++)
		crc = sCRC32CTable[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
	return ~crc;
}


static bool
is_notification_error(uint8 code)
{
	switch (code) {
		case USB4_ERROR_LINK_ERROR:
		case USB4_ERROR_HEC_ERROR_DETECTED:
		case USB4_ERROR_FLOW_CONTROL_ERROR:
		case USB4_ERROR_DP_BW:
		case USB4_ERROR_ROP_CMPLT:
		case USB4_ERROR_POP_CMPLT:
		case USB4_ERROR_PCIE_WAKE:
		case USB4_ERROR_DP_CON_CHANGE:
		case USB4_ERROR_DPTX_DISCOVERY:
		case USB4_ERROR_LINK_RECOVERY:
		case USB4_ERROR_ASYM_LINK:
			return true;
		default:
			return false;
	}
}


/*!	The host interfaces integrated into Intel processors from Ice Lake on
	only run their firmware while "force power" is set.
*/
static bool
is_intel_integrated(const pci_info& info)
{
	if (info.vendor_id != 0x8086)
		return false;
	switch (info.device_id) {
		case 0x8a0d: case 0x8a17:					// Ice Lake
		case 0x9a1b: case 0x9a1d: case 0x9a1f: case 0x9a21:	// Tiger Lake
		case 0x463e: case 0x466d:					// Alder Lake
		case 0xa73e: case 0xa76d:					// Raptor Lake
		case 0x7eb2: case 0x7ec2: case 0x7ec3:		// Meteor Lake
		case 0xa833: case 0xa834:					// Lunar Lake
		case 0xe333: case 0xe334: case 0xe433: case 0xe434:	// Panther Lake
		case 0x4d33:								// Wildcat Lake
			return true;
		default:
			return false;
	}
}


#define VS_CAP_9				0xc8
#define VS_CAP_9_FW_READY		(1u << 31)
#define VS_CAP_15				0xe0
#define VS_CAP_16				0xe4
#define VS_CAP_22				0xfc
#define VS_CAP_22_FORCE_POWER	(1u << 1)


// #pragma mark -


Nhi::Nhi(pci_device_module_info* pci, pci_device* device, const pci_info& info)
	:
	fPCI(pci),
	fDevice(device),
	fPCIInfo(info),
	fDomain(NULL),
	fRegisterArea(-1),
	fRegisters(NULL),
	fCaps(0),
	fHopCount(0),
	fIntelAutoClear(info.vendor_id == 0x8086),
	fIRQ(0),
	fUseMSI(false),
	fUseMSIX(false),
	fInterruptInstalled(false),
	fInterruptCount(0),
	fRingSem(-1),
	fDMAArea(-1),
	fDMA(NULL),
	fTxHead(0),
	fTxTail(0),
	fRxHead(0),
	fCurrentRequest(NULL),
	fNextSeq(0),
	fRingThread(-1),
	fRunning(false),
	fControlRunning(false)
{
	B_INITIALIZE_SPINLOCK(&fTxLock);
	B_INITIALIZE_SPINLOCK(&fCurrentLock);
	mutex_init(&fRequestLock, "usb4 control requests");
	fTxSpace.Init(this, "usb4 tx space");
	init_crc32c();
}


Nhi::~Nhi()
{
	Uninit();
	mutex_destroy(&fRequestLock);
}


status_t
Nhi::Init()
{
	uint16 command = fPCI->read_pci_config(fDevice, PCI_command, 2);
	command &= ~(PCI_command_io | PCI_command_int_disable);
	command |= PCI_command_master | PCI_command_memory;
	fPCI->write_pci_config(fDevice, PCI_command, 2, command);

	if (is_intel_integrated(fPCIInfo)) {
		uint32 value = fPCI->read_pci_config(fDevice, VS_CAP_22, 4);
		value = (value & ~(0xffu << 24)) | (0x22u << 24) | VS_CAP_22_FORCE_POWER;
		fPCI->write_pci_config(fDevice, VS_CAP_22, 4, value);

		bigtime_t deadline = system_time() + 1050000;
		while ((fPCI->read_pci_config(fDevice, VS_CAP_9, 4)
				& VS_CAP_9_FW_READY) == 0) {
			if (system_time() > deadline) {
				TRACE_ERROR("host interface firmware does not start\n");
				return B_TIMED_OUT;
			}
			snooze(3000);
		}

		// the same latency tolerance for snooped and non snooped traffic
		uint32 maxLTR = fPCI->read_pci_config(fDevice, VS_CAP_16, 4) & 0xffff;
		fPCI->write_pci_config(fDevice, VS_CAP_15, 4, maxLTR << 16 | maxLTR);
	}

	phys_addr_t physicalAddress = fPCIInfo.u.h0.base_registers[0];
	if ((fPCIInfo.u.h0.base_register_flags[0] & PCI_address_type)
			== PCI_address_type_64) {
		physicalAddress |= (uint64)fPCIInfo.u.h0.base_registers[1] << 32;
	}
	size_t size = fPCIInfo.u.h0.base_register_sizes[0];
	if (physicalAddress == 0 || size < NHI_FW_STS + 4) {
		TRACE_ERROR("unusable BAR 0 (%#" B_PRIxPHYSADDR ", %#" B_PRIxSIZE
			")\n", physicalAddress, size);
		return B_DEVICE_NOT_FOUND;
	}

	fRegisterArea = map_physical_memory("usb4 nhi registers", physicalAddress,
		size, B_ANY_KERNEL_BLOCK_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fRegisters);
	if (fRegisterArea < 0)
		return fRegisterArea;

	fCaps = ReadReg(NHI_CAPS);
	fHopCount = fCaps & NHI_CAPS_HOP_COUNT_MASK;
	if (fCaps == 0xffffffff || fHopCount == 0) {
		TRACE_ERROR("host interface does not answer (caps %#" B_PRIx32 ")\n",
			fCaps);
		return B_DEVICE_NOT_FOUND;
	}

	TRACE_ALWAYS("host interface %04x:%04x at %02x:%02x.%x, version %#"
		B_PRIx32 ", %" B_PRIu32 " paths\n", fPCIInfo.vendor_id,
		fPCIInfo.device_id, fPCIInfo.bus, fPCIInfo.device, fPCIInfo.function,
		NHI_CAPS_VERSION(fCaps), fHopCount);

	// rings and buffers of the control channel, in one physically
	// contiguous block
	size_t ringBytes = CTL_RING_SIZE * sizeof(nhi_ring_desc);
	size_t bufferBytes = CTL_RING_SIZE * CTL_FRAME_SIZE;
	size_t total = ROUNDUP(2 * ringBytes + 2 * bufferBytes, B_PAGE_SIZE);
	fDMAArea = create_area("usb4 control rings", (void**)&fDMA,
		B_ANY_KERNEL_ADDRESS, total, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (fDMAArea < 0)
		return fDMAArea;
	memset(fDMA, 0, total);

	physical_entry entry;
	status_t status = get_memory_map(fDMA, total, &entry, 1);
	if (status != B_OK)
		return status;
	fDMAPhysical = entry.address;

	fTxRing = (nhi_ring_desc*)fDMA;
	fRxRing = (nhi_ring_desc*)(fDMA + ringBytes);
	fTxBuffers = fDMA + 2 * ringBytes;
	fRxBuffers = fTxBuffers + bufferBytes;
	fTxRingPhysical = fDMAPhysical;
	fRxRingPhysical = fDMAPhysical + ringBytes;
	fTxBuffersPhysical = fDMAPhysical + 2 * ringBytes;
	fRxBuffersPhysical = fTxBuffersPhysical + bufferBytes;

	_DisableAllInterrupts();

	// interrupt throttling: 128 us in 256 ns units, for every vector
	for (uint32 i = 0; i < NHI_MSIX_VECTORS; i++)
		WriteReg(NHI_INT_THROTTLING_RATE + i * 4, (128 * 1000 + 255) / 256);

	fRingSem = create_sem(0, "usb4 nhi interrupt");
	if (fRingSem < 0)
		return fRingSem;

	if (_SetupInterrupts() != B_OK)
		TRACE_ALWAYS("no interrupts, polling the control rings\n");

	fRunning = true;
	fRingThread = spawn_kernel_thread(_RingThread, "usb4 nhi rings",
		B_URGENT_DISPLAY_PRIORITY, this);
	if (fRingThread < 0) {
		fRunning = false;
		return fRingThread;
	}
	resume_thread(fRingThread);

	return B_OK;
}


void
Nhi::Uninit()
{
	StopControl();

	if (fRingThread >= 0) {
		fRunning = false;
		release_sem(fRingSem);
		status_t result;
		wait_for_thread(fRingThread, &result);
		fRingThread = -1;
	}

	_RemoveInterrupts();

	if (fRingSem >= 0) {
		delete_sem(fRingSem);
		fRingSem = -1;
	}
	if (fDMAArea >= 0) {
		delete_area(fDMAArea);
		fDMAArea = -1;
	}
	if (fRegisterArea >= 0) {
		delete_area(fRegisterArea);
		fRegisterArea = -1;
		fRegisters = NULL;
	}
}


const char*
Nhi::InterruptMode() const
{
	if (!fInterruptInstalled)
		return "polling";
	if (fUseMSIX)
		return "MSI-X";
	if (fUseMSI)
		return "MSI";
	return "legacy";
}


status_t
Nhi::_SetupInterrupts()
{
	fIRQ = fPCIInfo.u.h0.interrupt_line;
	if (fIRQ == 0xff)
		fIRQ = 0;

	if (fPCI->get_msi_count(fDevice) >= 1) {
		uint32 vector;
		if (fPCI->configure_msi(fDevice, 1, &vector) == B_OK
			&& fPCI->enable_msi(fDevice) == B_OK) {
			fIRQ = vector;
			fUseMSI = true;
		}
	}
	if (!fUseMSI && fPCI->get_msix_count(fDevice) >= 1) {
		uint32 vector;
		if (fPCI->configure_msix(fDevice, 1, &vector) == B_OK
			&& fPCI->enable_msix(fDevice) == B_OK) {
			fIRQ = vector;
			fUseMSI = true;
			fUseMSIX = true;
		}
	}

	if (fIRQ == 0)
		return B_ERROR;

	if (!fUseMSI) {
		// legacy interrupts must be on in the command register
		uint16 command = fPCI->read_pci_config(fDevice, PCI_command, 2);
		command &= ~PCI_command_int_disable;
		fPCI->write_pci_config(fDevice, PCI_command, 2, command);
	}

	status_t status = install_io_interrupt_handler(fIRQ, _InterruptHandler,
		this, 0);
	if (status != B_OK) {
		if (fUseMSI)
			fPCI->unconfigure_msi(fDevice);
		fUseMSI = fUseMSIX = false;
		return status;
	}
	fInterruptInstalled = true;

	if (fUseMSIX) {
		// all rings use vector 0; Intel controllers clear the status bits
		// themselves, the others must not do so
		uint32 misc = ReadReg(NHI_DMA_MISC);
		misc |= fIntelAutoClear ? NHI_DMA_MISC_INT_AUTO_CLEAR
			: NHI_DMA_MISC_DISABLE_AUTO_CLEAR;
		WriteReg(NHI_DMA_MISC, misc);
		for (uint32 i = 0; i < (2 * fHopCount + 7) / 8; i++)
			WriteReg(NHI_INT_VEC_ALLOC_BASE + i * 4, 0);
	}

	TRACE_ALWAYS("using %s interrupt %" B_PRIu32 "\n", InterruptMode(), fIRQ);
	return B_OK;
}


void
Nhi::_RemoveInterrupts()
{
	if (fRegisters != NULL)
		_DisableAllInterrupts();
	if (fInterruptInstalled) {
		remove_io_interrupt_handler(fIRQ, _InterruptHandler, this);
		fInterruptInstalled = false;
	}
	if (fUseMSI) {
		fPCI->disable_msi(fDevice);
		fPCI->unconfigure_msi(fDevice);
		fUseMSI = fUseMSIX = false;
	}
}


void
Nhi::_DisableAllInterrupts()
{
	uint32 enableRegs = (31 + 2 * fHopCount) / 32;
	uint32 notifyRegs = (31 + 3 * fHopCount) / 32;

	for (uint32 i = 0; i < enableRegs; i++) {
		if (fIntelAutoClear)
			WriteReg(NHI_RING_INTERRUPT_BASE + i * 4, 0);
		else
			WriteReg(NHI_RING_INTERRUPT_MASK_CLEAR_BASE + i * 4, 0xffffffff);
	}
	for (uint32 i = 0; i < notifyRegs; i++) {
		if (fIntelAutoClear)
			ReadReg(NHI_RING_NOTIFY_BASE + i * 4);
		else
			WriteReg(NHI_RING_INT_CLEAR + i * 4, 0xffffffff);
	}
}


void
Nhi::_EnableRingInterrupt(uint32 bit, bool enable)
{
	uint32 reg = NHI_RING_INTERRUPT_BASE + bit / 32 * 4;
	uint32 mask = 1u << (bit % 32);
	if (enable)
		WriteReg(reg, ReadReg(reg) | mask);
	else if (fIntelAutoClear)
		WriteReg(reg, ReadReg(reg) & ~mask);
	else
		WriteReg(NHI_RING_INTERRUPT_MASK_CLEAR_BASE + bit / 32 * 4, mask);
}


/*static*/ int32
Nhi::_InterruptHandler(void* data)
{
	Nhi* nhi = (Nhi*)data;
	if (nhi->fRegisters == NULL)
		return B_UNHANDLED_INTERRUPT;

	// the status bits clear when read
	uint32 notifyRegs = (31 + 3 * nhi->fHopCount) / 32;
	uint32 any = 0;
	for (uint32 i = 0; i < notifyRegs; i++)
		any |= nhi->ReadReg(NHI_RING_NOTIFY_BASE + i * 4);

	if (any == 0 && !nhi->fUseMSI)
		return B_UNHANDLED_INTERRUPT;
	if (any == 0xffffffff)
		return B_UNHANDLED_INTERRUPT;

	atomic_add(&nhi->fInterruptCount, 1);
	release_sem_etc(nhi->fRingSem, 1, B_DO_NOT_RESCHEDULE);
	return B_INVOKE_SCHEDULER;
}


/*static*/ status_t
Nhi::_RingThread(void* data)
{
	((Nhi*)data)->_RingLoop();
	return B_OK;
}


void
Nhi::_RingLoop()
{
	while (fRunning) {
		// without interrupts poll quickly; with them, look now and then in
		// case one gets lost
		bigtime_t timeout = !fControlRunning ? 1000000
			: fInterruptCount > 0 ? 250000 : 2000;
		acquire_sem_etc(fRingSem, 1, B_RELATIVE_TIMEOUT, timeout);
		if (!fRunning)
			break;

		// drain extra releases
		int32 count;
		if (get_sem_count(fRingSem, &count) == B_OK && count > 0)
			acquire_sem_etc(fRingSem, count, B_RELATIVE_TIMEOUT, 0);

		if (!fControlRunning)
			continue;

		_ProcessTx();
		_ProcessRx();
	}
}


// #pragma mark - control channel


status_t
Nhi::StartControl()
{
	if (fControlRunning)
		return B_OK;

	// TX ring 0
	uint32 tx = NHI_TX_RING_BASE;
	WriteReg(tx + 0, (uint32)fTxRingPhysical);
	WriteReg(tx + 4, (uint32)((uint64)fTxRingPhysical >> 32));
	WriteReg(tx + 8, 0);
	WriteReg(tx + 12, CTL_RING_SIZE);
	WriteReg(NHI_TX_OPTIONS_BASE + 4, 0);
	WriteReg(NHI_TX_OPTIONS_BASE + 0, RING_FLAG_ENABLE | RING_FLAG_RAW);
	fTxHead = fTxTail = 0;

	// RX ring 0, taking frames of any type
	uint32 rx = NHI_RX_RING_BASE;
	WriteReg(rx + 0, (uint32)fRxRingPhysical);
	WriteReg(rx + 4, (uint32)((uint64)fRxRingPhysical >> 32));
	WriteReg(rx + 8, 0);
	WriteReg(rx + 12, (CTL_FRAME_SIZE << 16) | CTL_RING_SIZE);
	WriteReg(NHI_RX_OPTIONS_BASE + 4, (0xffffu << 16) | 0xffff);
	WriteReg(NHI_RX_OPTIONS_BASE + 0, RING_FLAG_ENABLE | RING_FLAG_RAW);

	_EnableRingInterrupt(0, true);
	_EnableRingInterrupt(fHopCount, true);

	// give the controller all but one descriptor (a full ring would look
	// empty)
	for (uint32 i = 0; i < CTL_RING_SIZE - 1; i++) {
		fRxRing[i].address = fRxBuffersPhysical + i * CTL_FRAME_SIZE;
		fRxRing[i].time = 0;
		fRxRing[i].control = RING_DESC_MAKE(0, 0, 0,
			RING_DESC_POSTED | RING_DESC_INTERRUPT);
	}
	fRxHead = 0;
	memory_write_barrier();
	WriteReg(rx + 8, CTL_RING_SIZE - 1);

	fControlRunning = true;
	TRACE("control channel started\n");
	return B_OK;
}


void
Nhi::StopControl()
{
	if (!fControlRunning || fRegisters == NULL)
		return;

	fControlRunning = false;
	_EnableRingInterrupt(0, false);
	_EnableRingInterrupt(fHopCount, false);

	WriteReg(NHI_TX_OPTIONS_BASE + 0, 0);
	WriteReg(NHI_TX_RING_BASE + 0, 0);
	WriteReg(NHI_TX_RING_BASE + 4, 0);
	WriteReg(NHI_TX_RING_BASE + 8, 0);
	WriteReg(NHI_TX_RING_BASE + 12, 0);
	WriteReg(NHI_RX_OPTIONS_BASE + 0, 0);
	WriteReg(NHI_RX_RING_BASE + 0, 0);
	WriteReg(NHI_RX_RING_BASE + 4, 0);
	WriteReg(NHI_RX_RING_BASE + 8, 0);
	WriteReg(NHI_RX_RING_BASE + 12, 0);

	// fail whatever still waits
	InterruptsSpinLocker locker(fCurrentLock);
	if (fCurrentRequest != NULL) {
		fCurrentRequest->result.status = B_CANCELED;
		fCurrentRequest->done = true;
		fCurrentRequest->condition.NotifyAll();
		fCurrentRequest = NULL;
	}
}


status_t
Nhi::_Transmit(const uint32* dwords, size_t count, uint8 pdf)
{
	if (!fControlRunning)
		return B_NOT_INITIALIZED;
	if ((count + 1) * 4 > CTL_FRAME_SIZE)
		return B_BAD_VALUE;

	bigtime_t deadline = system_time() + 100000;
	while (true) {
		InterruptsSpinLocker locker(fTxLock);
		if ((fTxHead + 1) % CTL_RING_SIZE != fTxTail) {
			uint32 slot = fTxHead;
			uint32* buffer = (uint32*)(fTxBuffers + slot * CTL_FRAME_SIZE);
			for (size_t i = 0; i < count; i++)
				buffer[i] = B_HOST_TO_BENDIAN_INT32(dwords[i]);
			buffer[count] = B_HOST_TO_BENDIAN_INT32(
				crc32c((const uint8*)buffer, count * 4));

			nhi_ring_desc& desc = fTxRing[slot];
			desc.address = fTxBuffersPhysical + slot * CTL_FRAME_SIZE;
			desc.time = 0;
			desc.control = RING_DESC_MAKE((count + 1) * 4, pdf, pdf,
				RING_DESC_POSTED | RING_DESC_INTERRUPT);
			fTxHead = (fTxHead + 1) % CTL_RING_SIZE;
			memory_write_barrier();
			WriteReg(NHI_TX_RING_BASE + 8, fTxHead << 16);
			return B_OK;
		}
		locker.Unlock();

		// the ring is full: reclaim what the controller has sent
		_ProcessTx();
		if (system_time() > deadline)
			return B_TIMED_OUT;
		snooze(50);
	}
}


void
Nhi::_ProcessTx()
{
	InterruptsSpinLocker locker(fTxLock);
	while (fTxTail != fTxHead) {
		uint32 control = fTxRing[fTxTail].control;
		if ((RING_DESC_FLAGS(control) & RING_DESC_COMPLETED) == 0)
			break;
		fTxRing[fTxTail].control = 0;
		fTxTail = (fTxTail + 1) % CTL_RING_SIZE;
	}
}


void
Nhi::_ProcessRx()
{
	while (true) {
		nhi_ring_desc& desc = fRxRing[fRxHead];
		uint32 control = desc.control;
		if ((RING_DESC_FLAGS(control) & RING_DESC_COMPLETED) == 0)
			break;
		memory_read_barrier();

		uint32 slot = fRxHead;
		size_t length = RING_DESC_LENGTH(control);
		if (length == 0)
			length = 4096;
		uint8 pdf = RING_DESC_EOF(control);
		uint32* data = (uint32*)(fRxBuffers + slot * CTL_FRAME_SIZE);

		if (length >= 8 && length <= CTL_FRAME_SIZE && length % 4 == 0) {
			uint32 crc = B_BENDIAN_TO_HOST_INT32(data[length / 4 - 1]);
			uint32 expected = crc32c((const uint8*)data, length - 4);
			uint32 frame[CTL_FRAME_SIZE / 4];
			for (size_t i = 0; i < length / 4 - 1; i++)
				frame[i] = B_BENDIAN_TO_HOST_INT32(data[i]);

			if (crc != expected && pdf != USB4_PDF_ICM_EVENT
				&& pdf != USB4_PDF_ICM_RESP) {
				TRACE_ERROR("RX: checksum mismatch (pdf %u, %" B_PRIuSIZE
					" bytes), dropped\n", pdf, length);
			} else
				_HandleFrame(frame, length - 4, pdf);
		} else {
			TRACE_ERROR("RX: bad frame size %" B_PRIuSIZE ", dropped\n",
				length);
		}

		// hand the slot back: the empty slot is the one before this one
		uint32 post = (slot + CTL_RING_SIZE - 1) % CTL_RING_SIZE;
		fRxRing[post].address = fRxBuffersPhysical + post * CTL_FRAME_SIZE;
		fRxRing[post].time = 0;
		fRxRing[post].control = RING_DESC_MAKE(0, 0, 0,
			RING_DESC_POSTED | RING_DESC_INTERRUPT);
		desc.control = 0;
		fRxHead = (fRxHead + 1) % CTL_RING_SIZE;
		memory_write_barrier();
		WriteReg(NHI_RX_RING_BASE + 8, slot);
	}
}


void
Nhi::_HandleFrame(uint32* frame, size_t bytes, uint8 pdf)
{
	size_t dwords = bytes / 4;
	if (dwords < 2)
		return;

	uint64 route = ((uint64)(frame[0] & USB4_HEADER_ROUTE_HI_MASK) << 32)
		| frame[1];

	TRACE_VERBOSE("RX pdf %u route %#" B_PRIx64 " %" B_PRIuSIZE " dwords: %08"
		B_PRIx32 " %08" B_PRIx32 " %08" B_PRIx32 "\n", pdf, route, dwords,
		frame[0], frame[1], dwords > 2 ? frame[2] : 0);

	switch (pdf) {
		case USB4_PDF_EVENT:
		{
			if (dwords < 3)
				return;
			uint8 adapter = USB4_EVENT_PORT(frame[2]);
			bool unplug = (frame[2] & USB4_EVENT_UNPLUG) != 0;
			AckPlug(route, adapter, unplug);
			if (fDomain != NULL)
				fDomain->QueueHotplug(route, adapter, unplug);
			return;
		}

		case USB4_PDF_ERROR:
		{
			if (dwords < 3)
				return;
			uint8 code = USB4_ERROR_CODE(frame[2]);
			if (is_notification_error(code)) {
				switch (code) {
					case USB4_ERROR_PCIE_WAKE:
					case USB4_ERROR_DP_CON_CHANGE:
					case USB4_ERROR_DPTX_DISCOVERY:
					case USB4_ERROR_DP_BW:
						AckNotification(route);
						break;
				}
				if (fDomain != NULL) {
					fDomain->QueueNotification(route,
						USB4_ERROR_PORT(frame[2]), code);
				}
				return;
			}
			break;
		}

		case USB4_PDF_READ:
		case USB4_PDF_WRITE:
		case USB4_PDF_RESET:
			break;

		default:
			TRACE("RX: unexpected packet type %u from %#" B_PRIx64 "\n", pdf,
				route);
			return;
	}

	// a response or an error for the outstanding request
	InterruptsSpinLocker locker(fCurrentLock);
	ControlRequest* request = fCurrentRequest;
	if (request == NULL || request->done) {
		TRACE("RX: response (pdf %u) from %#" B_PRIx64 " without a request\n",
			pdf, route);
		return;
	}

	if (pdf == USB4_PDF_ERROR) {
		request->result.status = B_DEV_NOT_READY;
		request->result.error = USB4_ERROR_CODE(frame[2]);
		request->result.port = USB4_ERROR_PORT(frame[2]);
		request->result.route = route;
	} else {
		if (pdf != request->responseType || route != request->route
			|| dwords != request->responseDwords
			|| (frame[0] & USB4_HEADER_RESPONSE) == 0) {
			TRACE("RX: stray response pdf %u route %#" B_PRIx64 " %"
				B_PRIuSIZE " dwords\n", pdf, route, dwords);
			return;
		}
		if ((pdf == USB4_PDF_READ || pdf == USB4_PDF_WRITE)
			&& USB4_ADDR_SEQ(frame[2]) != request->seq) {
			TRACE("RX: response with old sequence number\n");
			return;
		}
		memcpy(request->response, frame, dwords * 4);
		request->result.status = B_OK;
		request->result.route = route;
		request->result.port = dwords > 2 ? USB4_ADDR_PORT(frame[2]) : 0;
	}

	request->done = true;
	fCurrentRequest = NULL;
	request->condition.NotifyAll();
}


ControlResult
Nhi::_Request(ControlRequest& request, bigtime_t timeout)
{
	MutexLocker requestLocker(fRequestLock);

	ControlResult result = {};
	for (int attempt = 0; attempt < 4; attempt++) {
		request.seq = fNextSeq++ & 0x3;
		if (request.requestType == USB4_PDF_READ
			|| request.requestType == USB4_PDF_WRITE) {
			request.request[2] = (request.request[2] & ~(0x3u << 27))
				| ((uint32)request.seq << 27);
		}
		request.done = false;
		request.result.status = B_TIMED_OUT;
		request.condition.Init(&request, "usb4 control request");

		ConditionVariableEntry entry;
		{
			InterruptsSpinLocker locker(fCurrentLock);
			fCurrentRequest = &request;
			request.condition.Add(&entry);
		}

		status_t status = _Transmit(request.request, request.requestDwords,
			request.requestType);
		if (status == B_OK)
			status = entry.Wait(B_RELATIVE_TIMEOUT, timeout);

		InterruptsSpinLocker locker(fCurrentLock);
		if (fCurrentRequest == &request)
			fCurrentRequest = NULL;
		bool done = request.done;
		locker.Unlock();

		if (!done) {
			if (status == B_OK)
				status = B_TIMED_OUT;
			result.status = status == B_TIMED_OUT ? B_TIMED_OUT : status;
			if (status != B_TIMED_OUT && status != B_WOULD_BLOCK)
				return result;
			snooze(50);
			continue;
		}
		return request.result;
	}

	result.status = B_TIMED_OUT;
	return result;
}


ControlResult
Nhi::Read(uint64 route, uint8 adapter, usb4_config_space space, uint16 offset,
	uint32* buffer, size_t dwords, bigtime_t timeout)
{
	ControlResult result = {};
	if (dwords == 0 || dwords > USB4_MAX_CONFIG_DWORDS) {
		result.status = B_BAD_VALUE;
		return result;
	}

	ControlRequest request;
	request.request[0] = (uint32)(route >> 32) & USB4_HEADER_ROUTE_HI_MASK;
	request.request[1] = (uint32)route;
	request.request[2] = USB4_ADDR_MAKE(offset, dwords, adapter, space, 0);
	request.requestDwords = 3;
	request.requestType = USB4_PDF_READ;
	request.responseType = USB4_PDF_READ;
	request.responseDwords = 3 + dwords;
	request.route = route;

	result = _Request(request, timeout);
	if (result.status != B_OK)
		return result;

	uint32 address = request.response[2];
	if (USB4_ADDR_OFFSET(address) != offset
		|| USB4_ADDR_LENGTH(address) != dwords
		|| USB4_ADDR_SPACE(address) != (uint32)space) {
		TRACE_ERROR("read %#" B_PRIx64 ":%u: mismatched response %#" B_PRIx32
			"\n", route, adapter, address);
		result.status = B_IO_ERROR;
		return result;
	}

	memcpy(buffer, request.response + 3, dwords * 4);
	return result;
}


ControlResult
Nhi::Write(uint64 route, uint8 adapter, usb4_config_space space,
	uint16 offset, const uint32* buffer, size_t dwords, bigtime_t timeout)
{
	ControlResult result = {};
	if (dwords == 0 || dwords > USB4_MAX_CONFIG_DWORDS) {
		result.status = B_BAD_VALUE;
		return result;
	}

	ControlRequest request;
	request.request[0] = (uint32)(route >> 32) & USB4_HEADER_ROUTE_HI_MASK;
	request.request[1] = (uint32)route;
	request.request[2] = USB4_ADDR_MAKE(offset, dwords, adapter, space, 0);
	memcpy(request.request + 3, buffer, dwords * 4);
	request.requestDwords = 3 + dwords;
	request.requestType = USB4_PDF_WRITE;
	request.responseType = USB4_PDF_WRITE;
	request.responseDwords = 3;
	request.route = route;

	result = _Request(request, timeout);
	if (result.status != B_OK)
		return result;

	uint32 address = request.response[2];
	if (USB4_ADDR_OFFSET(address) != offset
		|| USB4_ADDR_LENGTH(address) != dwords
		|| USB4_ADDR_SPACE(address) != (uint32)space) {
		TRACE_ERROR("write %#" B_PRIx64 ":%u: mismatched response %#" B_PRIx32
			"\n", route, adapter, address);
		result.status = B_IO_ERROR;
	}
	return result;
}


status_t
Nhi::AckPlug(uint64 route, uint8 adapter, bool unplug)
{
	uint32 packet[3];
	packet[0] = (uint32)(route >> 32) & USB4_HEADER_ROUTE_HI_MASK;
	packet[1] = (uint32)route;
	packet[2] = USB4_ERROR_MAKE(USB4_ERROR_ACK_PLUG_EVENT, adapter,
		unplug ? USB4_ERROR_PG_HOT_UNPLUG : USB4_ERROR_PG_HOT_PLUG);
	return _Transmit(packet, 3, USB4_PDF_ERROR);
}


status_t
Nhi::AckNotification(uint64 route)
{
	uint32 packet[2];
	packet[0] = (uint32)(route >> 32) & USB4_HEADER_ROUTE_HI_MASK;
	packet[1] = (uint32)route;
	return _Transmit(packet, 2, USB4_PDF_NOTIFY_ACK);
}


status_t
Nhi::ResetHostRouter()
{
	// only USB4 version 2 host interfaces have a host router reset
	if (NHI_CAPS_VERSION(fCaps) < NHI_CAPS_VERSION_2)
		return B_NOT_SUPPORTED;

	WriteReg(NHI_RESET, NHI_RESET_HRR);
	snooze(100000);
	bigtime_t deadline = system_time() + 500000;
	while (system_time() < deadline) {
		if ((ReadReg(NHI_RESET) & NHI_RESET_HRR) == 0) {
			TRACE_ALWAYS("host router reset\n");
			return B_OK;
		}
		snooze(20);
	}
	TRACE_ERROR("timeout resetting the host router\n");
	return B_TIMED_OUT;
}
