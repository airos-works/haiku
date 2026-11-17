/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef DWC2_H
#define DWC2_H


#include "usb_private.h"
#include "dwc2_hardware.h"


class DWC2RootHub;


struct dwc2_platform_info {
	phys_addr_t		register_base;
	size_t			register_size;
	uint32			interrupt;
	// The controller's bus address of physical address 0, and the end of the
	// physical memory it reaches (the device tree's "dma-ranges").
	uint64			dma_offset;
	phys_addr_t		dma_limit;
	bool			bcm2835;
};


// A queued Transfer and how far it got.
struct dwc2_transfer {
	Transfer*		transfer;
	dwc2_transfer*	next;
	size_t			length;			// of the data (stage)
	size_t			offset;			// data moved so far
	uint8			phase;			// control transfers: dwc2_control_phase
	bool			incoming;		// direction of the data (stage)
	status_t		status;			// on completion
};


enum dwc2_control_phase {
	DWC2_PHASE_SETUP = 0,
	DWC2_PHASE_DATA,
	DWC2_PHASE_STATUS
};


enum dwc2_endpoint_state {
	DWC2_EP_IDLE = 0,		// nothing queued
	DWC2_EP_WAITING,		// queued: waits for a channel until fWakeTime
	DWC2_EP_RUNNING,		// its channel is enabled
	DWC2_EP_FRAME_WAIT		// holds a channel, starts in fStartFrame
};


// The controller's state of a Pipe (its controller cookie).
struct dwc2_endpoint {
	Pipe*			pipe;
	dwc2_endpoint*	link;			// in DWC2::fEndpoints
	dwc2_transfer*	first;
	dwc2_transfer*	last;
	uint8			type;			// DWC2_EPTYPE_*
	uint8			state;
	int8			channel;		// held channel or -1
	bool			destroyed;		// the pipe is gone

	bigtime_t		wake_time;		// earliest start when waiting
	bigtime_t		period;			// periodic: microseconds between polls
	bigtime_t		poll_time;		// periodic: when the last poll started

	uint8			error_count;	// transaction errors in a row
	uint8			nak_count;		// NAKs in a row (splits, OUT)
	bool			ping;			// high-speed OUT: start with a PING

	// split transactions
	bool			complete_split;	// the next run is a complete split
	uint8			nyet_count;
	uint16			split_frame;	// (micro)frame of the start split
	uint16			start_frame;	// FRAME_WAIT: the (micro)frame to run in
	uint32			split_out_length; // OUT data sent with the start split

	uint8			control_pid;	// next data stage PID
};


enum dwc2_halt_request {
	DWC2_HALT_NONE = 0,
	DWC2_HALT_CANCEL,		// the transfer was canceled
	DWC2_HALT_PREEMPT		// a NAKing IN gives its channel to a waiter
};


struct dwc2_channel {
	dwc2_endpoint*	endpoint;		// NULL: free
	uint8*			buffer;			// bounce buffer
	phys_addr_t		buffer_physical;
	bool			running;		// enabled, its halt not handled yet
	uint8			halt_request;	// dwc2_halt_request
	bigtime_t		started;
	bigtime_t		halt_requested;
	uint16			target_frame;	// periodic: the (micro)frame it runs in
	// the current run
	bool			incoming;
	bool			split;
	uint32			length;			// bytes programmed
	uint32			packets;		// packets programmed
	uint16			max_packet_size;
	uint8			pid;
};


class DWC2 : public BusManager {
public:
									DWC2(Stack* stack, device_node* node,
										const dwc2_platform_info* platform);
									~DWC2();

		status_t					InitCheck();
		status_t					Start();

virtual	status_t					SubmitTransfer(Transfer* transfer);
virtual	status_t					CancelQueuedTransfers(Pipe* pipe,
										bool force);
virtual	status_t					NotifyPipeChange(Pipe* pipe,
										usb_change change);

		// root hub port operations
		uint8						PortCount() const { return 1; }
		status_t					GetPortStatus(uint8 index,
										usb_port_status* status);
		status_t					SetPortFeature(uint8 index,
										uint16 feature);
		status_t					ClearPortFeature(uint8 index,
										uint16 feature);

virtual	const char*					TypeName() const { return "dwc2"; }

private:
		// hardware set up
		status_t					_PowerOn();
		status_t					_CoreReset();
		status_t					_InitHost();
		void						_HaltAllChannels();
		status_t					_ResetPort();
		void						_UpdateFrameInterval();

		// interrupts
static	int32						_InterruptHandler(void* data);
		int32						_Interrupt();

		// the worker thread: channel halts, scheduling, completions
static	status_t					_WorkerThread(void* data);
		void						_Worker();
		void						_Wake();
		bigtime_t					_WorkPass(dwc2_transfer** _done);

		// channels
		int32						_AllocateChannel(bool periodic);
		void						_ReleaseChannel(dwc2_endpoint* endpoint);
		void						_RequestHalt(int32 index, uint8 reason);
		status_t					_StartRun(dwc2_endpoint* endpoint,
										uint16 frame);
		void						_HandleHalt(int32 index, uint32 hcint,
										dwc2_transfer** _done);
		void						_FinishRun(dwc2_endpoint* endpoint,
										status_t status,
										dwc2_transfer** _done);
		void						_NextPoll(dwc2_endpoint* endpoint,
										bigtime_t now);
		void						_Requeue(dwc2_endpoint* endpoint,
										bigtime_t wakeTime);
		bool						_CopyIn(dwc2_channel* channel,
										dwc2_transfer* transfer,
										size_t length);
		void						_SetSOFInterrupt(bool enable);

		// endpoints
		dwc2_endpoint*				_Endpoint(Pipe* pipe);
		void						_FreeEndpoint(dwc2_endpoint* endpoint);
		bool						_IsPeriodic(dwc2_endpoint* endpoint) const
										{ return endpoint->type
											== DWC2_EPTYPE_INTERRUPT
											|| endpoint->type
											== DWC2_EPTYPE_ISOCHRONOUS; }
		bool						_UsesSplit(Pipe* pipe) const;
		bigtime_t					_Period(Pipe* pipe) const;

		inline	uint16				_Frame();

		// registers
inline	uint32						_Read(uint32 reg)
										{ return *(volatile uint32*)
											(fRegisters + reg); }
inline	void						_Write(uint32 reg, uint32 value)
										{ *(volatile uint32*)
											(fRegisters + reg) = value; }

		status_t					fStatus;
		dwc2_platform_info			fPlatform;
		Stack*						fStack;

		area_id						fRegisterArea;
		uint8*						fRegisters;
		area_id						fBufferArea;

		uint32						fSNPSID;
		uint32						fChannelCount;
		uint32						fMaxTransferSize;
		uint32						fMaxPacketCount;
		uint32						fIRQ;
		bool						fInterruptInstalled;

		// IRQ shared state, under fInterruptLock
		spinlock					fInterruptLock;
		uint32						fHaltedChannels;
		uint32						fChannelInterrupts[DWC2_MAX_CHANNELS];
		uint16						fPortChange;
		bool						fPortEvent;
		uint32						fInterruptMask;

		// scheduling state, under fLock (the worker holds it for a pass)
		mutex						fLock;
		dwc2_channel				fChannels[DWC2_MAX_CHANNELS];
		dwc2_endpoint*				fEndpoints;
		int32						fNextNonPeriodic;
		bool						fSOFEnabled;

		sem_id						fWorkSem;
		int32						fWorkPending;
		thread_id					fWorkerThread;
		bool						fStopWorker;

		// root hub and port
		DWC2RootHub*				fRootHub;
		int8						fRootHubAddress;
		usb_speed					fPortSpeed;
		bool						fPortResetChange;
};


class DWC2RootHub : public Hub {
public:
									DWC2RootHub(Object* rootObject,
										int8 deviceAddress);

static	status_t					ProcessTransfer(DWC2* dwc2,
										Transfer* transfer);
};


#endif	// DWC2_H
