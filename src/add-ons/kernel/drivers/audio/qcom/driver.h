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


struct AudioDevice {
	device_node*	node;
	Dsp				dsp;
	Codec			codec;
	Playback		playback;
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
	sem_id			doneSem;
	int32			doneCount;

	// the stream (multi_audio): buffers the DSP played, by their token
	spinlock		streamLock;
	bool			running;
	int32			lastPlayed;		// the buffer the DSP played last
	int32			toRefill;		// handed to the add-on, -1: none
	int64			framesPlayed;
	bigtime_t		playedTime;

	AudioDevice() : playback(dsp) {}
};


void		clean_buffer(AudioDevice* device, uint32 index);
status_t	multi_audio_control(AudioDevice* device, uint32 operation,
				void* buffer, size_t length);
void		multi_audio_stop(AudioDevice* device);


}	// namespace qcom_audio


#endif	// QCOM_AUDIO_DRIVER_H
