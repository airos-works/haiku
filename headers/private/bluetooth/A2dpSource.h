/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BLUETOOTH_A2DP_SOURCE_H_
#define _BLUETOOTH_A2DP_SOURCE_H_


#include <Locker.h>
#include <Messenger.h>
#include <OS.h>
#include <String.h>

#include <bluetooth/bluetooth.h>

#include <AudioSinkSetting.h>
#include <SbcEncoder.h>


namespace Bluetooth {


struct A2dpSinkInfo {
	uint16	avdtpVersion;
	uint16	a2dpVersion;
	uint16	features;
	uint16	psm;
};


// Messages an A2dpSource sends to its target when the sink changes the
// stream on its own, or the link goes away.
enum {
	B_A2DP_STREAM_STARTED	= 'a2st',
	B_A2DP_STREAM_SUSPENDED	= 'a2su',
	B_A2DP_STREAM_CLOSED	= 'a2cl'
};


// The source side of the Advanced Audio Distribution Profile: connects to a
// sink (speaker, headset) and streams SBC to it. Signalling follows AVDTP
// 1.3; this end acts as initiator, and answers what the sink may ask on its
// own (discover, capabilities, start, suspend, close, delay reports).
class A2dpSource {
public:
								A2dpSource(const bdaddr_t& address);
								~A2dpSource();

	static	status_t			QuerySink(const bdaddr_t& address,
									A2dpSinkInfo& info,
									BString* error = NULL);
		// SDP: does the device have an Audio Sink, and with which versions.
		// Needs an ACL link (Connect() makes one).

			status_t			Connect(bigtime_t timeout = 30000000);
		// Brings up the ACL link (pairing if needed, which may ask the
		// user), then configures and opens an SBC stream.
			status_t			Start();
			status_t			Suspend();
			void				Disconnect();

			void				SetTarget(const BMessenger& target);

			bool				IsOpen() const;
			bool				IsStreaming() const;
			const SbcConfiguration& Configuration() const
									{ return fConfiguration; }
			const A2dpSinkInfo&	SinkInfo() const
									{ return fSinkInfo; }
			size_t				MediaMTU() const
									{ return fMediaMTU; }
			bigtime_t			SinkDelay() const
									{ return fDelayReport * 100; }
				// what the sink reported it adds, or 0
			const char*			LastError() const
									{ return fError.String(); }

			status_t			SendMediaPacket(const void* data,
									size_t size);
		// One RTP packet on the media channel, at most MediaMTU() bytes.
			uint32				MaxFramesPerPacket() const;
			status_t			SendFrames(const uint8* frames,
									uint32 count);
		// SBC frames of Configuration().FrameSize() bytes, at most
		// MaxFramesPerPacket() of them, as one RTP packet (A2DP 1.3
		// section 4.3.4). The timestamps count samples from Start().

			void				SetPreferredSampleRate(uint32 rate)
									{ fPreferredRate = rate; }
			void				SetPageParameters(uint8 repetitionMode,
									uint16 clockOffset)
									{ fPageRepetitionMode = repetitionMode;
									  fClockOffset = clockOffset; }
				// from an inquiry: they make paging faster
			void				SetDeviceClass(uint32 deviceClass)
									{ fDeviceClass = deviceClass; }
				// also from an inquiry; stored with the paired device
			void				SetMaxBitpool(uint8 bitpool)
									{ fBitpoolLimit = bitpool; }

private:
	struct Response;

			status_t			_EnsureLink(bigtime_t deadline);
			status_t			_LinkState(bool& connected,
									bool& encrypted);
			status_t			_OpenChannel(uint16 psm, int& socket,
									bigtime_t deadline);
			status_t			_Discover();
			status_t			_Configure();
			status_t			_Open();

			status_t			_Command(uint8 signal, const void* data,
									size_t length, Response* response,
									bigtime_t timeout = 5000000);
			status_t			_Send(int socket, uint8 label,
									uint8 messageType, uint8 signal,
									const void* data, size_t length);

	static	status_t			_ReaderEntry(void* cookie);
			void				_Reader();
			void				_HandleMessage(const uint8* data,
									size_t length);
			void				_HandleCommand(uint8 label, uint8 signal,
									const uint8* data, size_t length);
			void				_Notify(uint32 what);
			void				_CloseMedia();
			status_t			_Fail(status_t status, const char* format,
									...)
									__attribute__((format(printf, 3, 4)));

private:
	enum State {
		IDLE,
		CONFIGURED,
		OPEN,
		STREAMING,
		CLOSING
	};

			bdaddr_t			fAddress;
			BLocker				fLock;
			BMessenger			fTarget;
			BString				fError;

			int					fSignalSocket;
			int					fMediaSocket;
			size_t				fMediaMTU;
			thread_id			fReader;
	volatile bool				fQuit;

			sem_id				fResponseSem;
			Response*			fPending;
			uint8				fPendingLabel;
			uint8				fPendingSignal;
			uint8				fNextLabel;

			uint8*				fAssembly;
			size_t				fAssembled;
			uint8				fAssemblySignal;
			uint8				fAssemblyLabel;
			uint8				fAssemblyType;

	volatile State				fState;
			uint8				fRemoteSEID;
			uint8				fCapabilities[4];
				// the sink's SBC codec information element
			bool				fSinkDelayReporting;
			A2dpSinkInfo		fSinkInfo;
			SbcConfiguration	fConfiguration;
			uint32				fPreferredRate;
			uint8				fBitpoolLimit;
			uint16				fDelayReport;
			uint16				fSequence;
			uint32				fTimestamp;
			uint8				fPageRepetitionMode;
			uint16				fClockOffset;
			uint32				fDeviceClass;
};


} // namespace Bluetooth


#endif	// _BLUETOOTH_A2DP_SOURCE_H_
