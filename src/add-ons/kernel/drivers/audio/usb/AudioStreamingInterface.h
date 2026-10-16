/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Distributed under the terms of the MIT license.
 *
 */
#ifndef _AUDIO_STREAMING_INTERFACE_H_
#define _AUDIO_STREAMING_INTERFACE_H_


#include <USB3.h>
#include <util/VectorMap.h>

#include "AudioControlInterface.h"


class ASInterfaceDescriptor {
public:
							ASInterfaceDescriptor(
								usb_audio_streaming_interface_descriptor*
								Descriptor, bool isUAC2);
							~ASInterfaceDescriptor();

// protected:
			uint8			fTerminalLink;
			uint8			fDelay;
			uint16			fFormatTag;
				// for USB Audio 2.0 the R1 tag of its format, if it has one
			uint8			fNumChannels;
				// USB Audio 2.0 only: R1 has it in the format descriptor
};


class ASEndpointDescriptor {
public:
							ASEndpointDescriptor(
								usb_endpoint_descriptor* Endpoint,
								usb_audio_streaming_endpoint_descriptor*
								Descriptor, bool isUAC2);
							~ASEndpointDescriptor();

// protected:
			uint8			fCSAttributes;
			uint8			fLockDelayUnits;
			uint16			fLockDelay;
			uint16			fMaxPacketSize;
			uint8			fEndpointAddress;
			uint8			fEndpointAttributes;
			uint8			fInterval;

			// the explicit feedback endpoint of an asynchronous output
			// stream, if the alternate has one
			uint8			fFeedbackAddress;
			uint16			fFeedbackMaxPacketSize;
			uint8			fFeedbackInterval;
};


class _ASFormatDescriptor {
public:
							_ASFormatDescriptor(
								usb_audio_format_descriptor* Descriptor);
	virtual					~_ASFormatDescriptor();

// protected:
			uint8			fFormatType;
	static	uint32			GetSamFreq(const usb_audio_sampling_freq& freq);
	static	usb_audio_sampling_freq	GetSamFreq(uint32 rate);
};


class TypeIFormatDescriptor : public _ASFormatDescriptor {
public:
							TypeIFormatDescriptor(
								usb_audio_format_descriptor* Descriptor,
								bool isUAC2 = false);
	virtual					~TypeIFormatDescriptor();

			status_t		Init(usb_audio_format_descriptor* Descriptor);
			void			SetSampleFrequencies(const Vector<uint32>& rates);

// protected:
			uint8			fNumChannels;
			uint8			fSubframeSize;
			uint8			fBitResolution;
			uint8			fSampleFrequencyType;
			Vector<uint32>	fSampleFrequencies;
};


class TypeIIFormatDescriptor : public _ASFormatDescriptor {
public:
							TypeIIFormatDescriptor(
								usb_audio_format_descriptor* Descriptor);
	virtual					~TypeIIFormatDescriptor();

// protected:
			uint16			fMaxBitRate;
			uint16			fSamplesPerFrame;
			uint8			fSampleFrequencyType;
			Vector<uint32>	fSampleFrequencies;
};


class TypeIIIFormatDescriptor : public TypeIFormatDescriptor {
public:
							TypeIIIFormatDescriptor(
								usb_audio_format_descriptor* Descriptor);
	virtual					~TypeIIIFormatDescriptor();

// protected:
};


class AudioStreamAlternate {
public:
									AudioStreamAlternate(size_t alternate,
										ASInterfaceDescriptor* interface,
										ASEndpointDescriptor* endpoint,
										_ASFormatDescriptor* format);
									~AudioStreamAlternate();

			ASInterfaceDescriptor*	Interface()	{ return fInterface; }
			ASEndpointDescriptor*	Endpoint()	{ return fEndpoint;	 }
			_ASFormatDescriptor*	Format()	{ return fFormat;	 }

			status_t				SetSamplingRate(uint32 newRate);
			status_t				SetSamplingRateById(uint32 newId);
			uint32					GetSamplingRate() { return fSamplingRate; }
			uint32					GetSamplingRateId(uint32 rate);
			uint32					GetSamplingRateIds();
			uint32					GetFormatId();
			status_t				SetFormatId(uint32 newFormatId);
			uint32					SamplingRateFromId(uint32 id);

protected:

			size_t					fAlternate;
			ASInterfaceDescriptor*	fInterface;
			ASEndpointDescriptor*	fEndpoint;
			_ASFormatDescriptor*	fFormat;
			uint32					fSamplingRate;
};


class AudioStreamingInterface {
public:
								AudioStreamingInterface(
									AudioControlInterface*	controlInterface,
									size_t interface, usb_interface_list* List);
								~AudioStreamingInterface();

	//		status_t			InitCheck() { return fStatus; }
			uint8				TerminalLink();
			bool				IsInput() { return fIsInput; }
			bool				IsUAC2() { return fIsUAC2; }

			AudioChannelCluster* ChannelCluster();

			void				GetFormatsAndRates(
									multi_description* Description);
protected:
			size_t				fInterface;
			AudioControlInterface*	fControlInterface;

	//		status_t			fStatus;
			bool				fIsInput;
			bool				fIsUAC2;
		// alternates of the streams
			Vector<AudioStreamAlternate*>	fAlternates;
			size_t				fActiveAlternate;
};


#endif // _AUDIO_STREAMING_INTERFACE_H_

