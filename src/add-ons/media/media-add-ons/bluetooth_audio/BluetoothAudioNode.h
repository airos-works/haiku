/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef BLUETOOTH_AUDIO_NODE_H
#define BLUETOOTH_AUDIO_NODE_H


#include <BufferConsumer.h>
#include <Locker.h>
#include <MediaEventLooper.h>

#include <A2dpSource.h>
#include <SbcEncoder.h>


class BluetoothAudioAddOn;


// An audio output that encodes what it is given to SBC and streams it to
// the speaker named in the Bluetooth audio setting. It connects when audio
// arrives, suspends the stream after a few seconds of silence and starts it
// again with the next sound.
class BluetoothAudioNode : public BBufferConsumer, public BMediaEventLooper {
public:
								BluetoothAudioNode(BluetoothAudioAddOn* addOn,
									const char* name);
	virtual						~BluetoothAudioNode();

	static	void				GetFormat(media_format& format);

			void				SinkChanged();

			void					ConnectNow();

											// open the stream without waiting for sound
				// the setting names another device, or none: drop the
				// stream and connect to the new one

	// BMediaNode
	virtual	BMediaAddOn*		AddOn(int32* _internalID) const;
	virtual	status_t			HandleMessage(int32 message, const void* data,
									size_t size);

protected:
	virtual	void				NodeRegistered();

	// BBufferConsumer
	virtual	status_t			AcceptFormat(const media_destination& destination,
									media_format* format);
	virtual	status_t			GetNextInput(int32* cookie,
									media_input* _input);
	virtual	void				DisposeInputCookie(int32 cookie);
	virtual	void				BufferReceived(BBuffer* buffer);
	virtual	void				ProducerDataStatus(
									const media_destination& destination,
									int32 status, bigtime_t performanceTime);
	virtual	status_t			GetLatencyFor(
									const media_destination& destination,
									bigtime_t* _latency,
									media_node_id* _timeSource);
	virtual	status_t			Connected(const media_source& producer,
									const media_destination& destination,
									const media_format& format,
									media_input* _input);
	virtual	void				Disconnected(const media_source& producer,
									const media_destination& destination);
	virtual	status_t			FormatChanged(const media_source& producer,
									const media_destination& consumer,
									int32 changeTag,
									const media_format& format);

	// BMediaEventLooper
	virtual	void				HandleEvent(const media_timed_event* event,
									bigtime_t lateness,
									bool realTimeEvent = false);

private:
	static	status_t			_SenderEntry(void* cookie);
			void				_Sender();
	static	status_t			_ListenerEntry(void* cookie);
			void				_Listener();
			bool				_Connect();
			void				_AcceptStream(int signalSocket);
			bool				_StreamReady(const BString& name,
									status_t status);
			void				_SetSource(Bluetooth::A2dpSource* source);
			bool				_EnsureStream();
			void				_CloseStream();
			void				_Queue(const int16* samples, size_t frames);

private:
			BluetoothAudioAddOn* fAddOn;
			media_input			fInput;
			bigtime_t			fLatency;

			BLocker				fLock;
			sem_id				fDataSem;
			thread_id			fSender;
	volatile bool				fQuit;

			int16*				fRing;
			size_t				fRingFrames;
			size_t				fRingRead;
			size_t				fRingFill;

			Bluetooth::A2dpSource* fSource;
			BLocker				fSourceLock;
				// fSource itself, against the listener thread
			thread_id			fListener;
			int					fListenSocket;
			int					fIncomingSignal;
				// a signalling channel the speaker opened, for the sender
			Bluetooth::SbcEncoder fEncoder;
			bigtime_t			fLastConnectAttempt;
			bigtime_t			fLastSound;
			bigtime_t			fConnectedAt;
			bool				fConnectFailed;
	volatile bool				fSinkChanged;
	volatile bool				fConnectNow;
};


#endif	// BLUETOOTH_AUDIO_NODE_H
