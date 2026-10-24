/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef QCOM_AUDIO_AUDIOREACH_H
#define QCOM_AUDIO_AUDIOREACH_H


#include <OS.h>


namespace qcom_audio {


class Dsp;
struct SubGraph;


/*!	A playback path through the DSP's AudioReach framework, as Linux' q6apm
	and audioreach.c run it: the stream's sub-graph (a shared memory write
	endpoint, the PCM decoder and converter, volume, MFC, a logger) joined
	to the device's sub-graph (a logger, MFC, the codec DMA or DisplayPort
	sink), both as the board's topology describes them; opened, given their
	media format, prepared and started; then fed buffers from one shared
	memory region, each answered once it is played.
*/
class Playback {
public:
								Playback(Dsp& dsp);
								~Playback();

			status_t			Open(uint32 sampleRate, uint32 channels,
									uint32 bitsPerSample);
			void				Close();

			//! The region the buffers come from (physically contiguous).
			status_t			MapMemory(phys_addr_t address, size_t size);
			status_t			Start(bool startDevice = true);
			status_t			Stop();
			status_t			SetVolume(uint16 gain);

			//! Hands a buffer to the DSP; \a token comes back with its
			//! "done" event (the data port's DATA_CMD_RSP_..._DONE_V2).
			status_t			Write(phys_addr_t address, uint32 size,
									uint32 token);

	static const uint32			kWriteDone = 0x05001004;

private:
			status_t			_OpenGraph(const SubGraph& subGraph,
									bool withConnection);
			status_t			_GraphCommand(uint32 opcode,
									const SubGraph& subGraph);
			status_t			_SetFormats(const SubGraph& subGraph);
			status_t			_SetConfig(const void* payload, uint32 size);

			Dsp&				fDsp;
			uint32				fSampleRate;
			uint32				fChannels;
			uint32				fBits;
			uint32				fMemoryHandle;
			bool				fFrontOpen;
			bool				fBackOpen;
			bool				fRunning;
};


}	// namespace qcom_audio


#endif	// QCOM_AUDIO_AUDIOREACH_H
