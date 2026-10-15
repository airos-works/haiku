/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SUNXI_DISPLAY_TYPEC_H
#define SUNXI_DISPLAY_TYPEC_H


/*	The Cubie A7S's second USB-C port as a DisplayPort source: a Hynetek
	HUSB311 Type-C port controller (TCPCI) on S_TWI1, driven as a power
	source and downstream-facing port that enters the DisplayPort alternate
	mode of whatever adapter or monitor is plugged in.

	A small USB Power Delivery 2.0 policy engine: attach detection, VBUS and
	VCONN, a 5 V contract, then the structured VDMs of the DisplayPort alt
	mode (Discover Identity, SVIDs and Modes, Enter Mode, DP Status Update,
	DP Configure) and the HPD state from the partner's status and Attention
	messages. Everything runs from Poll(), called by the driver's thread. */


#include "twi.h"


namespace sunxi {


class TypeCPort {
public:
								TypeCPort();
								~TypeCPort();

			status_t			Init();
			void				Poll();

			// the alt mode is entered and configured
			bool				DisplayPortReady() const
									{ return fState == kStateDisplayPort; }
			bool				HotPlug() const { return fHotPlug; }
			bool				HotPlugInterrupt();
				// an IRQ_HPD came since the last call
			bool				Flipped() const { return fFlipped; }
			uint32				Lanes() const;
			char				PinAssignment() const;
			int32				Changes() const { return fChanges; }
			// nothing is going on that needs timers
			bool				Idle() const
									{ return fState == kStateUnattached
										|| fState >= kStateDisplayPort; }
			// a Power Delivery exchange is under way: the partner expects
			// answers within tReceiverResponse (15 ms), and gives up on a
			// contract with a hard reset after tSenderResponse (24-30 ms)
			bool				Negotiating() const
									{ return fState >= kStateSendCapabilities
										&& fState < kStateDisplayPort; }

private:
			enum State {
				kStateUnattached,
				kStateAttachWait,
				kStateSendCapabilities,
				kStateWaitRequest,
				kStateContract,
				kStateDiscoverIdentity,
				kStateDiscoverSvids,
				kStateDiscoverModes,
				kStateEnterMode,
				kStateStatusUpdate,
				kStateConfigure,
				kStateDisplayPort,
				kStateNoDisplayPort,
				kStateNoPowerDelivery
			};

			struct Message {
				uint16	header;
				uint32	objects[7];
				int		Count() const { return (header >> 12) & 7; }
				int		Type() const { return header & 0x1f; }
			};

			status_t			_ReadCc(uint8& cc1, uint8& cc2);
			void				_Attach(bool flipped, bool vconn);
			void				_Detach();
			void				_SetState(State state);
			status_t			_Transmit(uint8 type, const uint32* objects,
									int count, bool isData);
			status_t			_SendControl(uint8 type)
									{ return _Transmit(type, NULL, 0, false); }
			status_t			_SendVdm(uint32 header, const uint32* vdos,
									int count);
			bool				_Receive(Message& message);
			void				_HandleMessage(const Message& message);
			void				_HandleVdm(const Message& message);
			void				_RequestNext();
			void				_UpdateDisplayPortStatus(uint32 status);
			void				_HardReset();

			Twi					fTwi;
			bool				fInitialized;
			State				fState;
			bigtime_t			fStateSince;
			bigtime_t			fLastSend;
			int32				fRetries;
			uint8				fTxMessageId;
			int8				fRxMessageId;

			bool				fFlipped;
			bool				fVconn;
			uint32				fDpModeVdo;
			int32				fDpModePosition;
			uint8				fPinAssignments;	// the partner's UFP_D ones
			char				fPinAssignment;
			bool				fHotPlug;
			bool				fHotPlugIrq;
			int32				fChanges;
			uint16				fVendorId;
			uint16				fProductId;
};


}	// namespace sunxi


#endif	// SUNXI_DISPLAY_TYPEC_H
