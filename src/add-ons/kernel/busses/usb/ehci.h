/*
 * Copyright 2006, Haiku Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */
#ifndef EHCI_H
#define EHCI_H

#include "usb_private.h"
#include "ehci_hardware.h"
#include "ehci_split_iso.h"


struct pci_info;
struct pci_device_module_info;
struct pci_device;

class EHCIRootHub;
class PhysicalMemoryAllocator;

struct ehci_platform_info {
	phys_addr_t register_base;
	size_t register_size;
	uint32 interrupt;
	bool dma_coherent;
};


typedef struct transfer_data {
	Transfer *		transfer;
	ehci_qh *		queue_head;
	ehci_qtd *		data_descriptor;
	bool			incoming;
	bool			canceled;
	transfer_data *	link;
} transfer_data;


// A full-speed isochronous endpoint behind a high-speed hub (split
// transactions through the hub's TT, EHCI 4.12.3): its booked schedule slot
// and where its next packet goes. Kept as the pipe's controller cookie.
typedef struct ehci_split_stream {
	int32						budget_index;	// in fSplitBudget
	uint32						endpoint;		// siTD dword 1
	uint32						schedule;		// siTD dword 2 (S-/C-mask)
	uint16						period;			// frames between packets
	uint16						phase;			// frame (mod period) of packets
	uint16						max_packet_size;
	int32						next_frame;		// -1: nothing queued
	uint32						queued;			// transfers in the schedule
} ehci_split_stream;


// This structure is used to create a list of
// descriptors per isochronous transfer
typedef struct isochronous_transfer_data {
	Transfer *					transfer;
	// The next field is used to keep track
	// of every isochronous descriptor as they are NOT
	// linked to each other in a queue like in every other
	// transfer type
	ehci_itd **					descriptors;
	uint16						last_to_process;
	bool						incoming;
	bool						is_active;
	bool						notify_canceled;
	isochronous_transfer_data *	link;

	size_t						buffer_size;
	void *						buffer_log;
	addr_t						buffer_phy;

	// split transfers (siTDs, one per packet) only
	ehci_sitd **				split_descriptors;
	uint16 *					packet_lengths;
	uint32						packet_count;
	uint32						last_frame;
	ehci_split_stream *			stream;
	size_t						actual_length;
} isochronous_transfer_data;


class EHCI : public BusManager {
public:
									EHCI(pci_info *info, pci_device_module_info* pci,
										pci_device* device, Stack *stack, device_node *node,
										const ehci_platform_info* platform = NULL);
									~EHCI();

		status_t					Start();

virtual	status_t					StartDebugTransfer(Transfer *transfer);
virtual	status_t					CheckDebugTransfer(Transfer *transfer);
		void						LinkAsyncDebugQueueHead(ehci_qh *queueHead);
		void						LinkPeriodicDebugQueueHead(
										ehci_qh *queueHead, Pipe *pipe);
virtual	void						CancelDebugTransfer(Transfer *transfer);
		void						CleanupDebugTransfer(Transfer *transfer);

virtual	status_t					SubmitTransfer(Transfer *transfer);
		status_t					SubmitIsochronous(Transfer *transfer);
		status_t					SubmitSplitIsochronous(Transfer *transfer);

virtual	status_t					CancelQueuedTransfers(Pipe *pipe, bool force);
		status_t					CancelQueuedIsochronousTransfers(Pipe *pipe, bool force);

virtual	status_t					NotifyPipeChange(Pipe *pipe,
										usb_change change);

		// Port operations for root hub
		uint8						PortCount() { return fPortCount; }
		status_t					GetPortStatus(uint8 index, usb_port_status *status);
		status_t					SetPortFeature(uint8 index, uint16 feature);
		status_t					ClearPortFeature(uint8 index, uint16 feature);

		status_t					ResetPort(uint8 index);
		status_t					SuspendPort(uint8 index);

virtual	const char *				TypeName() const { return "ehci"; }

private:
		status_t					AllocateChunk(void** logicalAddress,
										phys_addr_t* physicalAddress, size_t size);
		status_t					FreeChunk(void* logicalAddress,
										phys_addr_t physicalAddress, size_t size);
		area_id						AllocateArea(void** logicalAddress,
										phys_addr_t* physicalAddress, size_t size,
										const char* name);

		// Controller resets
		status_t					ControllerReset();
		status_t					LightReset();

		// Interrupt functions
static	int32						InterruptHandler(void *data);
		int32						Interrupt();
static	int32						InterruptPollThread(void *data);

		// Transfer management
		status_t					AddPendingTransfer(Transfer *transfer,
										ehci_qh *queueHead,
										ehci_qtd *dataDescriptor,
										bool directionIn);
		status_t					AddPendingIsochronousTransfer(
										Transfer *transfer,
										ehci_itd **isoRequest, uint32 lastIndex,
										bool directionIn, addr_t bufferPhy,
										void *bufferLog, size_t bufferSize);
		status_t					CancelAllPendingTransfers();


static	int32						FinishThread(void *data);
		void						FinishTransfers();
static	int32						CleanupThread(void *data);
		void						Cleanup();

		// Isochronous transfer functions
static int32						FinishIsochronousThread(void *data);
		void						FinishIsochronousTransfers();
		isochronous_transfer_data *	FindIsochronousTransfer(ehci_itd *itd);

		// Split isochronous transfers (siTDs)
		ehci_split_stream *			_SplitStream(Pipe *pipe,
										status_t *_status);
		void						_FreeSplitStream(Pipe *pipe);
		uint32						_CurrentFrame();
static	int32						FinishSplitIsochronousThread(void *data);
		void						FinishSplitIsochronousTransfers();
		void						_RetireSplitFrame(uint32 frame,
										isochronous_transfer_data **_done);
		void						_CompleteSplitTransfer(
										isochronous_transfer_data *transfer);
		void						_FreeSplitTransfer(
										isochronous_transfer_data *transfer);
		status_t					_CopyIsochronousData(Transfer *transfer,
										size_t offset, void *buffer,
										size_t length, bool toTransfer);
		void						LinkITDescriptors(ehci_itd *itd,
										ehci_itd **last);
		void						LinkSITDescriptors(ehci_sitd *sitd,
										ehci_sitd **last);
		void						UnlinkITDescriptors(ehci_itd *itd,
										ehci_itd **last);
		void						UnlinkSITDescriptors(ehci_sitd *sitd,
										ehci_sitd **last);

		// Queue Head functions
		ehci_qh *					CreateQueueHead();
		status_t					InitQueueHead(ehci_qh *queueHead,
										Pipe *pipe);
		void						FreeQueueHead(ehci_qh *queueHead);

		status_t					LinkQueueHead(ehci_qh *queueHead);
		status_t					LinkInterruptQueueHead(ehci_qh *queueHead,
										Pipe *pipe);
		status_t					UnlinkQueueHead(ehci_qh *queueHead,
										ehci_qh **freeList);

		// Queue functions
		status_t					FillQueueWithRequest(Transfer *transfer,
										ehci_qh *queueHead,
										ehci_qtd **dataDescriptor,
										bool *directionIn,
										bool prepareKernelAccess);
		status_t					FillQueueWithData(Transfer *transfer,
										ehci_qh *queueHead,
										ehci_qtd **dataDescriptor,
										bool *directionIn,
										bool prepareKernelAccess);
		status_t					_FillQueueWithPhysicalData(Transfer *transfer,
										ehci_qh *queueHead,
										ehci_qtd **dataDescriptor,
										bool *directionIn);

		bool						LockIsochronous();
		void						UnlockIsochronous();

		// Descriptor functions
		ehci_qtd *					CreateDescriptor(
										size_t bufferSizeToAllocate,
										uint8 pid);
		status_t					CreateDescriptorChain(Pipe *pipe,
										ehci_qtd **firstDescriptor,
										ehci_qtd **lastDescriptor,
										ehci_qtd *strayDescriptor,
										uint8 pid,
										size_t buffersLength,
										int32 descriptorsCount = -1);
		ehci_itd*					CreateItdDescriptor();
		ehci_sitd*					CreateSitdDescriptor();

		void						FreeDescriptor(ehci_qtd *descriptor);
		void						FreeDescriptorChain(ehci_qtd *topDescriptor);
		void						FreeDescriptor(ehci_itd *descriptor);
		void						FreeDescriptor(ehci_sitd *descriptor);
		void						FreeIsochronousData(
										isochronous_transfer_data *data);

		void						LinkDescriptors(ehci_qtd *first,
										ehci_qtd *last, ehci_qtd *alt);

		size_t						WriteDescriptorChain(
										ehci_qtd *topDescriptor,
										generic_io_vec *vector, size_t vectorCount,
										bool physical);
		size_t						ReadDescriptorChain(ehci_qtd *topDescriptor,
										generic_io_vec *vector, size_t vectorCount,
										bool physical, bool *nextDataToggle);
		size_t						ReadActualLength(ehci_qtd *topDescriptor,
										bool *nextDataToggle);
		size_t						WriteIsochronousDescriptorChain(
										isochronous_transfer_data *transfer);
		size_t						ReadIsochronousDescriptorChain(
										isochronous_transfer_data *transfer);

		// Operational register functions
inline	void						WriteOpReg(uint32 reg, uint32 value);
inline	uint32						ReadOpReg(uint32 reg);

		// Capability register functions
inline	uint8						ReadCapReg8(uint32 reg);
inline	uint16						ReadCapReg16(uint32 reg);
inline	uint32						ReadCapReg32(uint32 reg);

		uint8 *						fCapabilityRegisters;
		uint8 *						fOperationalRegisters;
		area_id						fRegisterArea;
		pci_info *					fPCIInfo;
		pci_device_module_info*		fPci;
		pci_device*					fDevice;
		Stack *						fStack;
		PhysicalMemoryAllocator*	fDMAAllocator;
		uint32						fEnabledInterrupts;
		uint32						fThreshold;

		// Periodic transfer framelist and interrupt entries
		area_id						fPeriodicFrameListArea;
		uint32 *					fPeriodicFrameList;
		interrupt_entry *			fInterruptEntries;
		ehci_itd **					fItdEntries;
		ehci_sitd **				fSitdEntries;

		// Async transfer queue management
		ehci_qh *					fAsyncQueueHead;
		sem_id						fAsyncAdvanceSem;

		// Maintain a linked list of transfers
		transfer_data *				fFirstTransfer;
		transfer_data *				fLastTransfer;
		sem_id						fFinishTransfersSem;
		thread_id					fFinishThread;
		Pipe *						fProcessingPipe;

		ehci_qh *					fFreeListHead;
		sem_id						fCleanupSem;
		thread_id					fCleanupThread;
		bool						fStopThreads;
		int32						fNextStartingFrame;

		// fFrameBandwidth[n] holds the available bandwidth
		// of the nth frame in microseconds
		uint16 *					fFrameBandwidth;

		// Maintain a linked list of isochronous transfers
		isochronous_transfer_data *	fFirstIsochronousTransfer;
		isochronous_transfer_data *	fLastIsochronousTransfer;
		sem_id						fFinishIsochronousTransfersSem;
		thread_id					fFinishIsochronousThread;
		mutex						fIsochronousLock;

		// Split isochronous transfers: their own list and finisher, the
		// next frame the finisher retires, and the booked TT and
		// high-speed bandwidth of the split streams.
		isochronous_transfer_data *	fFirstSplitTransfer;
		isochronous_transfer_data *	fLastSplitTransfer;
		sem_id						fFinishSplitSem;
		thread_id					fFinishSplitThread;
		uint32						fSplitRetireFrame;
		ehci_split::budget_entry	fSplitBudget[16];

		// Root Hub
		EHCIRootHub *				fRootHub;
		uint8						fRootHubAddress;

		// Port management
		uint8						fPortCount;
		uint16						fPortResetChange;
		uint16						fPortSuspendChange;

		// Interrupt polling
		thread_id					fInterruptPollThread;
		uint32						fIRQ;
		bool						fUseMSI;
		bool						fInterruptInstalled;
};


class EHCIRootHub : public Hub {
public:
									EHCIRootHub(Object *rootObject,
										int8 deviceAddress);

static	status_t					ProcessTransfer(EHCI *ehci,
										Transfer *transfer);
};


#endif // !EHCI_H
