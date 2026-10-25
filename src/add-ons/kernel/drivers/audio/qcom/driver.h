/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef QCOM_AUDIO_DRIVER_H
#define QCOM_AUDIO_DRIVER_H


#include <KernelExport.h>
#include <device_manager.h>

#include "audioreach.h"
#include "codec.h"
#include "dsp.h"
#include "qcom_smmu.h"


namespace qcom_audio {


static const uint32 kBufferCount = 2;
static const uint32 kBufferFrames = 1024;
static const uint32 kFrameSize = 4;			// two 16-bit channels
static const uint32 kBufferSize = kBufferFrames * kFrameSize;
static const uint8 kHeadphoneGain = 6;		// -9 dB
// capture: as many buffers, the same size; their own region, mapped for the
// DSP just above the playback buffers
static const uint32 kRecordBufferCount = 4;
// (the read endpoint fills whole 1 ms frames of the container: 20 of them,
// as Linux' 0xf00 bytes)
static const uint32 kRecordBufferFrames = 960;
static const uint32 kRecordBufferSize = kRecordBufferFrames * kFrameSize;
static const uint64 kRecordDspOffset = 0x10000;


struct AudioDevice {
	device_node*	node;
	Dsp				dsp;
	Codec			codec;
	AudioPath		playback;
	AudioPath		capture;
	QcomSmmuDomain	smmu;			// what the DSP reaches of ours
	thread_id		startThread;
	bool			ready;
	bool			startDone;		// the start thread is through
	mutex			lock;
	int32			openCount;

	// the playback buffers: one physically contiguous area
	area_id			bufferArea;
	uint8*			buffers;
	phys_addr_t		buffersPhysical;
	uint64			buffersDsp;		// their address for the DSP
	sem_id			doneSem;
	int32			doneCount;

	// the capture buffers
	area_id			recordArea;
	uint8*			recordBuffers;
	phys_addr_t		recordPhysical;
	uint64			recordDsp;
	sem_id			recordSem;
	int32			recordDoneCount;
	int32			lastRecorded;	// the buffer the DSP filled last
	uint32			lastRecordedSize;
	bool			captureOpen;

	// the stream (multi_audio): buffers the DSP played, by their token
	spinlock		streamLock;
	bool			running;
	int32			lastPlayed;		// the buffer the DSP played last
	int32			toRefill;		// handed to the add-on, -1: none
	int64			framesPlayed;
	bigtime_t		playedTime;

	AudioDevice()
		:
		playback(dsp, AudioPath::kPlayback),
		capture(dsp, AudioPath::kCapture)
	{
	}
};


void		clean_buffer(AudioDevice* device, uint32 index);
void		invalidate_record_buffer(AudioDevice* device, uint32 index);
status_t	multi_audio_control(AudioDevice* device, uint32 operation,
				void* buffer, size_t length);
void		multi_audio_stop(AudioDevice* device);


}	// namespace qcom_audio


#endif	// QCOM_AUDIO_DRIVER_H
