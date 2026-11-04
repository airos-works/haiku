/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef USB4_NHI_H
#define USB4_NHI_H


#include <KernelExport.h>
#include <PCI.h>
#include <bus/PCI.h>

#include <condition_variable.h>
#include <lock.h>

#include "usb4_regs.h"


class Domain;


//! The result of a configuration request.
struct ControlResult {
	status_t	status;
		// B_OK, B_TIMED_OUT, B_DEV_NOT_READY (USB4 error), ...
	uint8		error;
		// USB4 error code when status is B_DEV_NOT_READY
	uint8		port;
		// adapter that answered (the upstream adapter of the router
		// for reads and writes)
	uint64		route;
};


struct ControlRequest {
	uint32		request[3 + USB4_MAX_CONFIG_DWORDS];
	size_t		requestDwords;
	uint8		requestType;
	uint8		responseType;
	size_t		responseDwords;
	uint32		response[3 + USB4_MAX_CONFIG_DWORDS];
	uint64		route;
	uint8		seq;
	bool		done;
	ControlResult result;
	ConditionVariable condition;
};


/*!	A USB4 host interface: the PCI function of a host router. It owns the
	control channel (TX and RX ring 0), delivers responses to waiting
	requests and hands hot plug and notification packets to the domain.
*/
class Nhi {
public:
								Nhi(pci_device_module_info* pci,
									pci_device* device,
									const pci_info& info);
								~Nhi();

			status_t			Init();
			void				Uninit();

			const pci_info&		Info() const { return fPCIInfo; }
			uint32				HopCount() const { return fHopCount; }
			uint32				Version() const
									{ return NHI_CAPS_VERSION(fCaps); }

			void				SetDomain(Domain* domain)
									{ fDomain = domain; }

			// control channel
			status_t			StartControl();
			void				StopControl();

			ControlResult		Read(uint64 route, uint8 adapter,
									usb4_config_space space, uint16 offset,
									uint32* buffer, size_t dwords,
									bigtime_t timeout = 100000);
			ControlResult		Write(uint64 route, uint8 adapter,
									usb4_config_space space, uint16 offset,
									const uint32* buffer, size_t dwords,
									bigtime_t timeout = 100000);
			status_t			AckPlug(uint64 route, uint8 adapter,
									bool unplug);
			status_t			AckNotification(uint64 route);

			status_t			ResetHostRouter();

			uint32				ReadReg(uint32 offset)
									{ return *(volatile uint32*)
										(fRegisters + offset); }
			void				WriteReg(uint32 offset, uint32 value)
									{ *(volatile uint32*)(fRegisters + offset)
										= value; }

			bool				InterruptsWorking() const
									{ return fInterruptCount > 0; }
			uint32				InterruptCount() const
									{ return fInterruptCount; }
			const char*			InterruptMode() const;

			struct Statistics {
				int32	requests;
				int32	timeouts;
				int32	errors;
				int32	rxFrames;
				int32	rxDropped;
				int32	events;
				int32	notifications;
				int32	stray;
				uint32	txHead;
				uint32	txTail;
				uint32	rxHead;
				uint32	rxRegister;
				uint32	txRegister;
			};
			void				GetStatistics(Statistics& stats);

private:
	static	int32				_InterruptHandler(void* data);
	static	status_t			_RingThread(void* data);
			void				_RingLoop();
			void				_ProcessTx();
			void				_ProcessRx();
			void				_HandleFrame(uint32* data, size_t bytes,
									uint8 pdf);

			status_t			_SetupInterrupts();
			void				_RemoveInterrupts();
			void				_EnableRingInterrupt(uint32 bit, bool enable);
			void				_DisableAllInterrupts();

			status_t			_Transmit(const uint32* dwords, size_t count,
									uint8 pdf);
			ControlResult		_Request(ControlRequest& request,
									bigtime_t timeout);

private:
			pci_device_module_info* fPCI;
			pci_device*			fDevice;
			pci_info			fPCIInfo;
			Domain*				fDomain;

			area_id				fRegisterArea;
			uint8*				fRegisters;
			uint32				fCaps;
			uint32				fHopCount;
			bool				fIntelAutoClear;

			// interrupts
			uint32				fIRQ;
			bool				fUseMSI;
			bool				fUseMSIX;
			bool				fInterruptInstalled;
			int32				fInterruptCount;
			Statistics			fStats;
			sem_id				fRingSem;

			// DMA memory of the control rings
			area_id				fDMAArea;
			uint8*				fDMA;
			phys_addr_t			fDMAPhysical;
			nhi_ring_desc*		fTxRing;
			nhi_ring_desc*		fRxRing;
			uint8*				fTxBuffers;
			uint8*				fRxBuffers;
			phys_addr_t			fTxRingPhysical;
			phys_addr_t			fRxRingPhysical;
			phys_addr_t			fTxBuffersPhysical;
			phys_addr_t			fRxBuffersPhysical;
			uint32				fTxHead;
			uint32				fTxTail;
			uint32				fRxHead;
				// next RX descriptor the controller completes

			spinlock			fTxLock;
			ConditionVariable	fTxSpace;

			mutex				fRequestLock;
				// serializes requests: one is outstanding at a time
			spinlock			fCurrentLock;
			ControlRequest*		fCurrentRequest;
			uint8				fNextSeq;

			thread_id			fRingThread;
			volatile bool		fRunning;
			volatile bool		fControlRunning;
};


#define CTL_RING_SIZE		16


#endif	// USB4_NHI_H
