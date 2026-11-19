/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _USB_NCM_DEVICE_H_
#define _USB_NCM_DEVICE_H_


#include <condition_variable.h>
#include <ether_driver.h>
#include <lock.h>

#include "Driver.h"
#include "NTB.h"
#include "usb_ncm.h"


struct ncm_function {
	uint32	configuration;
		// index
	uint8	control_interface;
	uint8	data_interface;
	uint8	data_alternate;
	uint8	mac_address_index;
	uint16	max_segment_size;
	uint8	capabilities;
		// bmNetworkCapabilities
};


class NCMDevice {
public:
	static	bool				FindFunction(usb_device device,
									ncm_function* function);

								NCMDevice(usb_device device);
								~NCMDevice();

			status_t			InitCheck() const { return fStatus; }

			status_t			Open();
			status_t			Close();
			status_t			Free();

			status_t			Read(uint8* buffer, size_t* numBytes);
			status_t			Write(const uint8* buffer, size_t* numBytes);
			status_t			Control(uint32 op, void* buffer,
									size_t length);
			status_t			GetInfo(void* buffer, size_t length);

			void				Removed();

private:
			enum {
				kMaxRxTransfers = 12,
				kMaxTxTransfers = 12,
					// the xHCI driver takes 15 transfers per endpoint
				kMaxRxDatagrams = 512
			};

			enum tx_state {
				TX_FREE = 0,
				TX_FILLING,
				TX_IN_FLIGHT
			};

			enum flush_reason {
				FLUSH_IDLE,
				FLUSH_FULL,
				FLUSH_COMPLETION
			};

			enum {
				RECOVER_NOTIFY = 0x01,
				RECOVER_WRITE = 0x02
			};

			struct rx_slot {
				NCMDevice*		device;
				uint8*			buffer;
				size_t			length;
				status_t		status;
				bool			queued;
			};

			struct tx_slot {
				NCMDevice*		device;
				uint8*			buffer;
				uint32			datagrams;
				tx_state		state;
			};

			status_t			_SetupDevice();
			status_t			_ReadMACAddress(uint8* buffer);
			void				_ReadSettings();
			status_t			_ClassRequest(bool in, uint8 request,
									uint16 value, void* data,
									size_t length);
			status_t			_Negotiate();
			void				_SetMaxDatagramSize();
			status_t			_SetAltInterface(uint8 alternate);
			status_t			_SetPacketFilter();
			status_t			_AllocateBuffers();
			void				_FreeBuffers();

			// receiving
			status_t			_StartReceiving();
			void				_QueueRead(uint32 index);
			status_t			_NextDatagram(bool block);
			status_t			_Receive(bool block, net_buffer** _buffer,
									uint8* data, size_t* _length);
			status_t			_ReceiveBatch(ether_receive_batch* batch);
	static	void				_ReadCallback(void* cookie, int32 status,
									void* data, size_t actualLength);

			// sending
			status_t			_Send(net_buffer* buffer, const uint8* data,
									size_t length);
			void				_SubmitLocked(flush_reason reason);
	static	void				_WriteCallback(void* cookie, int32 status,
									void* data, size_t actualLength);

			// "debug" in the settings: the bytes to the syslog
			void				_DumpNTB(const char* what, uint64 number,
									const uint8* data, size_t length);

	static	void				_NotifyCallback(void* cookie, int32 status,
									void* data, size_t actualLength);
			status_t			_QueueNotify();
			void				_StopNotifications();
			void				_WaitForTransfers();
			void				_ScheduleRecovery(uint32 what);
	static	status_t			_RecoveryThread(void* data);

			// Realtek RTL815x PHY power
			bool				_HasRealtekPHY() const;
			status_t			_RealtekRead(uint16 index, uint32* value);
			status_t			_RealtekWriteWord(uint16 index, uint16 value);
			status_t			_RealtekPHYRead(uint16 reg, uint16* value);
			status_t			_RealtekPHYWrite(uint16 reg, uint16 value);
			void				_SetPHYPower(bool on);
			void				_SetKeepAwake(bool keepAwake);

			status_t			fStatus;
			bool				fOpen;
			bool				fRemoved;
			bool				fNonBlocking;
			bool				fPromiscuous;
			int32				fInsideNotify;
			usb_device			fDevice;
			uint16				fVendorID;
			uint16				fProductID;
			bool				fKeepAwake;

			// interfaces, from the descriptors
			const usb_configuration_info* fConfiguration;
			uint8				fControlInterface;
			uint8				fDataInterface;
			uint8				fDataAlternate;
			uint8				fMACAddressIndex;
			uint8				fNetworkCapabilities;
			uint16				fMaxSegmentSize;
			uint8				fMACAddress[6];

			usb_pipe			fNotifyEndpoint;
			usb_pipe			fReadEndpoint;
			usb_pipe			fWriteEndpoint;
			uint16				fWriteMaxPacketSize;

			// negotiated
			uint16				fNTBFormats;
			bool				fNTB32;
			uint32				fDeviceRxMax;
			uint32				fDeviceTxMax;
			uint32				fRxNTBSize;
			uint32				fTxNTBSize;
			uint16				fTxDivisor;
			uint16				fTxRemainder;
			uint16				fTxAlignment;
			uint32				fTxMaxDatagrams;
			uint32				fMaxFrame;

			// tunables (driver settings file "usb_ncm")
			uint32				fRxNTBSetting;
			uint32				fTxNTBSetting;
			uint32				fRxTransfers;
			uint32				fTxTransfers;
			uint32				fTxEager;
			uint32				fTxDatagramSetting;
			uint32				fDebug;
				// setup requests, the first fDebugNTBs NTBs each way
			uint32				fDebugNTBs;
			uint32				fDebugTxCompletions;
			uint32				fDebugNotifications;

			// receiving: completed reads in the order they completed
			rx_slot				fRxSlots[kMaxRxTransfers];
			uint32				fRxBufferSize;
			uint32				fRxDone[kMaxRxTransfers];
			uint32				fRxDoneHead;
			uint32				fRxDoneCount;
			spinlock			fRxDoneLock;
			sem_id				fRxSem;
				// counts completed reads
			bool				fRxRunning;
			mutex				fRxLock;
				// the reader's state below
			int32				fRxCurrent;
			uint32				fRxNext;
			uint32				fRxCount;
			ncm::datagram		fRxDatagrams[kMaxRxDatagrams];

			// sending
			tx_slot				fTxSlots[kMaxTxTransfers];
			uint32				fTxBufferSize;
			mutex				fTxLock;
			ConditionVariable	fTxCondition;
			ncm::NTBWriter		fTxWriter;
			int32				fTxFill;
				// the slot being filled, or -1
			uint32				fTxInFlight;
			uint16				fTxSequence;
			bool				fTxRunning;
			int32				fTxStalled;

			uint8*				fNotifyBuffer;
			uint32				fNotifyBufferLength;
			int32				fNotifyStopping;
				// nothing may queue notification transfers anymore
			int32				fPendingTransfers;
				// queued transfers whose hook has not finished yet
			mutex				fRecoveryLock;
			uint32				fRecoveryFlags;
			thread_id			fRecoveryThread;

			sem_id				fLinkStateChangeSem;
			bool				fHasConnection;
			uint32				fDownstreamSpeed;
			uint32				fUpstreamSpeed;

			usb_ncm_info		fStats;
};


#endif	// _USB_NCM_DEVICE_H_
