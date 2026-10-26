/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef QCOM_AUDIO_DRIVER_H
#define QCOM_AUDIO_DRIVER_H


#include <KernelExport.h>
#include <device_manager.h>

#include <qcom_dp_audio.h>

#include "audioreach.h"
#include "codec.h"
#include "dsp.h"
#include "qcom_smmu.h"


namespace qcom_audio {


static const uint32 kBufferCount = 2;
// 20 ms: whole 1 ms frames of the DSP's containers (the read endpoint
// fills only those), the size Linux uses
static const uint32 kBufferFrames = 960;
static const uint32 kFrameSize = 4;			// two 16-bit channels
static const uint32 kBufferSize = kBufferFrames * kFrameSize;
static const uint8 kHeadphoneGain = 6;		// -9 dB
// capture: as many buffers as playback, the same size (the add-on exchanges
// them together); their own region, mapped for the DSP just above the
// playback buffers
static const uint32 kRecordBufferCount = kBufferCount;
static const uint32 kRecordBufferFrames = kBufferFrames;
static const uint32 kRecordBufferSize = kRecordBufferFrames * kFrameSize;
static const uint64 kRecordDspOffset = 0x10000;


struct AudioDevice;


/*!	A playback output, one multi_audio device each: the headphones (with
	the headset input), HDMI (the DSP's DISPLAY_PORT_RX into the board's
	HDMI bridge) or a USB-C connector's DisplayPort. Its graph, its buffers
	(one physically contiguous area, mapped for the DSP) and the stream's
	state.
*/
struct Output {
	AudioDevice*	device;
	AudioPath		path;
	const char*		name;
	const char*		vendor;
	uint32			dpController;	// QCOM_DP_AUDIO_*, or kNoDisplayPort
	uint32			token;			// its buffers': token | index
	uint64			dspOffset;		// its buffers' above the device's base
	bool			open;			// the graph open, the buffers mapped
	int32			openCount;

	area_id			bufferArea;
	uint8*			buffers;
	phys_addr_t		buffersPhysical;
	uint64			buffersDsp;		// their address for the DSP
	sem_id			doneSem;
	int32			doneCount;

	// the stream (multi_audio): buffers the DSP played, by their token
	bool			running;
	int32			lastPlayed;		// the buffer the DSP played last
	int32			toRefill;		// handed to the add-on, -1: none
	int64			framesPlayed;
	bigtime_t		playedTime;

	Output(Dsp& dsp, AudioPath::Direction direction, const char* name,
			const char* vendor, uint32 dpController, uint32 token,
			uint64 dspOffset)
		:
		device(NULL),
		path(dsp, direction),
		name(name),
		vendor(vendor),
		dpController(dpController),
		token(token),
		dspOffset(dspOffset),
		open(false),
		openCount(0),
		bufferArea(-1),
		buffers(NULL),
		buffersPhysical(0),
		buffersDsp(0),
		doneSem(-1),
		doneCount(0),
		running(false),
		lastPlayed(-1),
		toRefill(-1),
		framesPlayed(0),
		playedTime(0)
	{
	}
};

static const uint32 kNoDisplayPort = ~0u;
// an output's buffers' tokens: its index << 8 | the buffer's; their place
// for the DSP: its index * kOutputDspStride above the headphones' (the
// capture buffers sit at kRecordDspOffset)
static const uint32 kOutputCount = 4;
static const uint64 kOutputDspStride = 0x10000;


struct AudioDevice {
	device_node*	node;
	Dsp				dsp;
	Codec			codec;
	Output			headphones;
	Output			hdmi;
	Output			usbc0;			// DP-1
	Output			usbc1;			// DP-2
	Output*			outputs[kOutputCount];
	AudioPath		capture;
	QcomSmmuDomain	smmu;			// what the DSP reaches of ours
	thread_id		startThread;
	bool			ready;
	bool			startDone;		// the start thread is through
	mutex			lock;
	uint64			dspBase;		// the DSP's address of our buffers
	qcom_dp_audio_module_info* dpAudio;	// HDMI's audio engine

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
	bool			capturing;
	int32			recordToRefill;	// handed to the add-on, -1: none
	int64			framesRecorded;
	bigtime_t		recordedTime;

	spinlock		streamLock;		// the outputs' and the capture's state

	AudioDevice()
		:
		headphones(dsp, AudioPath::kPlayback, "Headphones",
			"Qualcomm SC8280XP / WCD9385", kNoDisplayPort, 0 << 8, 0),
		hdmi(dsp, AudioPath::kDisplayPort, "HDMI",
			"Qualcomm SC8280XP / CH7218A", QCOM_DP_AUDIO_HDMI, 1 << 8,
			2 * kOutputDspStride),
		usbc0(dsp, AudioPath::kUsbcDisplayPort0, "USB-C DisplayPort 1",
			"Qualcomm SC8280XP", QCOM_DP_AUDIO_USBC_0, 2 << 8,
			3 * kOutputDspStride),
		usbc1(dsp, AudioPath::kUsbcDisplayPort1, "USB-C DisplayPort 2",
			"Qualcomm SC8280XP", QCOM_DP_AUDIO_USBC_1, 3 << 8,
			4 * kOutputDspStride),
		capture(dsp, AudioPath::kCapture)
	{
		outputs[0] = &headphones;
		outputs[1] = &hdmi;
		outputs[2] = &usbc0;
		outputs[3] = &usbc1;
		for (Output* output : outputs)
			output->device = this;
	}
};


status_t	output_power_up(Output* output);
void		output_power_down(Output* output);
void		clean_buffer(Output* output, uint32 index);
void		invalidate_record_buffer(AudioDevice* device, uint32 index);
status_t	multi_audio_control(Output* output, uint32 operation,
				void* buffer, size_t length);
void		multi_audio_stop(Output* output);


}	// namespace qcom_audio


#endif	// QCOM_AUDIO_DRIVER_H
