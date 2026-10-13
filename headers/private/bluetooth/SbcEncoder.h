/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BLUETOOTH_SBC_ENCODER_H_
#define _BLUETOOTH_SBC_ENCODER_H_


#include <SupportDefs.h>


namespace Bluetooth {


// SBC parameters of a stream (A2DP 1.3 section 4.3). The bit values are
// those of the codec information element, so a set of capabilities is the
// OR of them.
enum {
	SBC_FREQUENCY_16000		= 0x80,
	SBC_FREQUENCY_32000		= 0x40,
	SBC_FREQUENCY_44100		= 0x20,
	SBC_FREQUENCY_48000		= 0x10,

	SBC_MODE_MONO			= 0x08,
	SBC_MODE_DUAL_CHANNEL	= 0x04,
	SBC_MODE_STEREO			= 0x02,
	SBC_MODE_JOINT_STEREO	= 0x01,

	SBC_BLOCKS_4			= 0x80,
	SBC_BLOCKS_8			= 0x40,
	SBC_BLOCKS_12			= 0x20,
	SBC_BLOCKS_16			= 0x10,

	SBC_SUBBANDS_4			= 0x08,
	SBC_SUBBANDS_8			= 0x04,

	SBC_ALLOCATION_SNR		= 0x02,
	SBC_ALLOCATION_LOUDNESS	= 0x01
};


struct SbcConfiguration {
	uint32	sampleRate;
	uint8	channelMode;
		// one SBC_MODE_*
	uint8	blocks;
		// 4, 8, 12 or 16
	uint8	subbands;
		// 4 or 8
	uint8	allocation;
		// SBC_ALLOCATION_*
	uint8	bitpool;
	uint8	minBitpool;
	uint8	maxBitpool;
		// the range the sink accepts

	uint32	Channels() const
				{ return channelMode == SBC_MODE_MONO ? 1 : 2; }
	size_t	FrameSize() const;
		// bytes of one encoded frame (A2DP 1.3 section 12.9)
	uint32	FrameSamples() const
				{ return blocks * subbands; }
	uint32	BitRate() const;
};


// Encoder for the low complexity subband codec of A2DP 1.3 appendix B.
class SbcEncoder {
public:
								SbcEncoder();

			status_t			SetTo(const SbcConfiguration& config);
			void				Reset();
				// forgets the filter history, for a new stream

			uint32				FrameSamples() const
									{ return fBlocks * fSubbands; }
				// per channel
			size_t				FrameSize() const
									{ return fFrameSize; }

			size_t				Encode(const int16* samples, uint8* frame);
				// One frame from FrameSamples() interleaved samples per
				// channel. Returns the frame size.

private:
			void				_Analyse(const int16* samples);
			void				_AllocateBits(int32 bits[2][8],
									const int32 scaleFactors[2][8]);
			void				_AllocateChannel(int32 bits[8],
									const int32 scaleFactors[8],
									int32 bitpool);

private:
			uint32				fSampleRate;
			uint8				fChannelMode;
			uint32				fChannels;
			uint32				fBlocks;
			uint32				fSubbands;
			uint8				fAllocation;
			uint32				fBitpool;
			size_t				fFrameSize;
			uint8				fHeader[2];

			float				fHistory[2][160];
				// the last 10 * subbands samples of each channel, twice,
				// so that a window never wraps
			uint32				fPosition;
			float				fSamples[16][2][8];
				// subband samples of the frame: block, channel, subband
			float				fMatrix[8][16];
};


} // namespace Bluetooth


#endif	// _BLUETOOTH_SBC_ENCODER_H_
