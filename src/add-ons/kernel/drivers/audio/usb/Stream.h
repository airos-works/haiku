/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Distributed under the terms of the MIT license.
 *
 */
#ifndef _USB_AUDIO_STREAM_H_
#define _USB_AUDIO_STREAM_H_


#include "AudioStreamingInterface.h"


class Device;

class Stream : public AudioStreamingInterface {
	friend	class			Device;
public:
							Stream(Device* device, size_t interface,
								usb_interface_list* List);
							~Stream();

			status_t		Init();
			status_t		InitCheck() { return fStatus; }

			status_t		Start();
			status_t		Stop();
			bool			IsRunning() { return fIsRunning; }
			void			OnRemove();
			void			OrphanBuffers();

			status_t		GetBuffers(multi_buffer_list* List);

			status_t		OnSetConfiguration(usb_device device,
							const usb_configuration_info* config);

			bool			ExchangeBuffer(multi_buffer_info* Info);
			status_t		GetEnabledChannels(uint32& offset,
								multi_channel_enable* Enable);
			status_t		SetEnabledChannels(uint32& offset,
								multi_channel_enable* Enable);
			status_t		GetGlobalFormat(multi_format_info* Format);
			status_t		SetGlobalFormat(multi_format_info* Format);

protected:
	enum {
		kFeedbackTransfers = 2,
		kMaxFeedbackPackets = 8,
		kMaxFeedbackPacketSize = 4
	};

			Device*			fDevice;
			status_t		fStatus;

			usb_pipe		fStreamEndpoint;

			bool			fIsRunning;
			area_id			fArea, fKernelArea;
			size_t			fAreaSize;
			usb_iso_packet_descriptor* fDescriptors;
			size_t			fDescriptorsCount;
			uint8*			fBuffers;
			uint8*			fKernelBuffers;
			size_t			fCurrentBuffer;
			size_t			fSamplesCount;

			// the packets of a buffer: one per service interval of the
			// endpoint, fPacketsPerBuffer at the nominal rate (and room for
			// fMaxPacketsPerBuffer when the device's feedback sets them)
			uint32			fFramesPerSecond;
				// USB (micro)frames: 1000 at full speed, 8000 at high speed
			uint32			fPacketsPerSecond;
			uint32			fPacketShift;
			uint32			fPacketsPerBuffer;
			uint32			fMaxPacketsPerBuffer;
			uint32			fSamplesPerBuffer;
			uint32			fFrameSize;
			uint32			fMaxPacketFrames;

			// explicit feedback of an asynchronous output: the device says
			// how many frames it takes per (micro)frame
			bool			fUseFeedback;
			usb_pipe		fFeedbackEndpoint;
			uint32			fFeedbackPacketSize;
			uint32			fFeedbackPackets;
			uint32			fFeedbackSlot;
			int32			fFeedbackFailures;
			uint32			fFeedbackFrame;
			uint32			fNominalFeedback;
				// frames per (micro)frame at the nominal rate, 16.16
			int32			fFeedbackShift;
			int32			fFeedbackFrames;
				// frames per packet the device asks for, 16.16; 0 = nominal
			bool			fFeedbackLogged;
			int64			fPhase;
				// frames owed to the device, 16.16
			usb_iso_packet_descriptor
							fFeedbackDescriptors[kFeedbackTransfers]
								[kMaxFeedbackPackets];
			uint8			fFeedbackData[kFeedbackTransfers]
								[kMaxFeedbackPackets * kMaxFeedbackPacketSize];

			bigtime_t		fRealTime;
			uint32			fStartingFrame;
			int32			fProcessedBuffers;
			int32			fInsideNotify;

private:
			status_t		_ChooseAlternate();
			status_t		_SetupBuffers();
			bool			_IsHighSpeed();
			status_t		_SetAlternate(const usb_configuration_info* config,
								size_t alternate);
			uint8			_ClockID();
			void			_InitClockRates();
			status_t		_QueueNextTransfer(size_t buffer, bool start);
			size_t			_FeedbackPackets(
								usb_iso_packet_descriptor* descriptors,
								size_t& dataLength);
			status_t		_QueueFeedbackTransfer(uint32 slot);
			void			_UpdateFeedback(uint32 value);
	static	void			_TransferCallback(void* cookie, status_t status,
								void* data, size_t actualLength);
	static	void			_FeedbackCallback(void* cookie, status_t status,
								void* data, size_t actualLength);
			void			_DumpDescriptors();
};


#endif // _USB_AUDIO_STREAM_H_

