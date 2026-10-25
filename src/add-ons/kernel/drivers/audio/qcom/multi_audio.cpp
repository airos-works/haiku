/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "driver.h"

#include <string.h>

#include <hmulti_audio.h>
#include <kernel.h>
#include <util/AutoLock.h>


#define TRACE(x...) dprintf("qcom_audio: " x)


namespace qcom_audio {


// the headphones' two channels, the headset input's two (its microphone is
// the left one), then their busses; without capture the outputs only
static const multi_channel_info kChannels[] = {
	{0, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0},
	{1, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0},
	{2, B_MULTI_INPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0},
	{3, B_MULTI_INPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0},
	{4, B_MULTI_OUTPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO},
	{5, B_MULTI_OUTPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO},
	{6, B_MULTI_INPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO},
	{7, B_MULTI_INPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO},
};
static const multi_channel_info kOutputChannels[] = {
	{0, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0},
	{1, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0},
	{2, B_MULTI_OUTPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO},
	{3, B_MULTI_OUTPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO},
};
// a DisplayPort's (HDMI's, USB-C's) two
static const multi_channel_info kDisplayChannels[] = {
	{0, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0},
	{1, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0},
	{2, B_MULTI_OUTPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0},
	{3, B_MULTI_OUTPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0},
};


static status_t
get_description(Output* output, multi_description* userData)
{
	bool display = output->dpController != kNoDisplayPort;
	bool input = !display && output->device->captureOpen;
	multi_description description;
	if (user_memcpy(&description, userData, sizeof(description)) != B_OK)
		return B_BAD_ADDRESS;
	description.interface_version = B_CURRENT_INTERFACE_VERSION;
	description.interface_minimum = B_CURRENT_INTERFACE_VERSION;
	strlcpy(description.friendly_name,
		input ? "Headset" : output->name, sizeof(description.friendly_name));
	strlcpy(description.vendor_info, output->vendor,
		sizeof(description.vendor_info));
	description.output_channel_count = 2;
	description.input_channel_count = input ? 2 : 0;
	description.output_bus_channel_count = 2;
	description.input_bus_channel_count = input ? 2 : 0;
	description.aux_bus_channel_count = 0;
	description.output_rates = B_SR_48000;
	description.input_rates = input ? B_SR_48000 : 0;
	description.max_cvsr_rate = 0;
	description.min_cvsr_rate = 0;
	description.output_formats = B_FMT_16BIT;
	description.input_formats = input ? B_FMT_16BIT : 0;
	description.lock_sources = B_MULTI_LOCK_INTERNAL;
	description.timecode_sources = 0;
	description.interface_flags = B_MULTI_INTERFACE_PLAYBACK
		| (input ? B_MULTI_INTERFACE_RECORD : 0);
	description.start_latency = 30000;
	description.control_panel[0] = '\0';
	if (user_memcpy(userData, &description, sizeof(description)) != B_OK)
		return B_BAD_ADDRESS;
	const multi_channel_info* channels
		= display ? kDisplayChannels : input ? kChannels : kOutputChannels;
	size_t size = display ? sizeof(kDisplayChannels)
		: input ? sizeof(kChannels) : sizeof(kOutputChannels);
	if (description.request_channel_count
			>= (int32)(size / sizeof(multi_channel_info))
		&& user_memcpy(description.channels, channels, size) != B_OK) {
		return B_BAD_ADDRESS;
	}
	return B_OK;
}


static status_t
get_enabled_channels(multi_channel_enable* userData)
{
	multi_channel_enable data;
	if (user_memcpy(&data, userData, sizeof(data)) != B_OK)
		return B_BAD_ADDRESS;
	for (uint32 channel = 0; channel < 4; channel++)
		B_SET_CHANNEL(data.enable_bits, channel, true);
	data.lock_source = B_MULTI_LOCK_INTERNAL;
	return user_memcpy(userData, &data, sizeof(data));
}


static status_t
get_global_format(multi_format_info* userData)
{
	multi_format_info data;
	memset(&data, 0, sizeof(data));
	data.output.rate = B_SR_48000;
	data.output.format = B_FMT_16BIT;
	data.input.rate = B_SR_48000;
	data.input.format = B_FMT_16BIT;
	data.output_latency = 0;
	return user_memcpy(userData, &data, sizeof(data));
}


static status_t
set_global_format(const multi_format_info* userData)
{
	multi_format_info data;
	if (user_memcpy(&data, userData, sizeof(data)) != B_OK)
		return B_BAD_ADDRESS;
	return data.output.rate == B_SR_48000 && data.output.format == B_FMT_16BIT
		? B_OK : B_NOT_SUPPORTED;
}


//!	The buffers are the driver's (allocated with the graph): their layout.
static status_t
get_buffers(Output* output, multi_buffer_list* userData)
{
	AudioDevice* device = output->device;
	multi_buffer_list data;
	if (user_memcpy(&data, userData, sizeof(data)) != B_OK)
		return B_BAD_ADDRESS;
	if (data.request_playback_buffers < (int32)kBufferCount
		|| data.request_playback_channels < 2
		|| data.playback_buffers == NULL)
		return B_BAD_VALUE;
	buffer_desc* playbackBuffers[kBufferCount];
	if (!IS_USER_ADDRESS(data.playback_buffers)
		|| user_memcpy(playbackBuffers, data.playback_buffers,
			sizeof(playbackBuffers)) != B_OK)
		return B_BAD_ADDRESS;

	multi_audio_stop(output);
	memset(output->buffers, 0, kBufferCount * kBufferSize);
	data.flags = B_MULTI_BUFFER_PLAYBACK;
	data.return_playback_buffers = kBufferCount;
	data.return_playback_channels = 2;
	data.return_playback_buffer_size = kBufferFrames;
	bool record = output == &device->headphones && device->captureOpen
		&& data.request_record_buffers
			>= (int32)kRecordBufferCount
		&& data.request_record_channels >= 2 && data.record_buffers != NULL;
	data.return_record_buffers = record ? kRecordBufferCount : 0;
	data.return_record_channels = record ? 2 : 0;
	data.return_record_buffer_size = record ? kRecordBufferFrames : 0;
	if (record) {
		data.flags |= B_MULTI_BUFFER_RECORD;
		buffer_desc* recordBuffers[kRecordBufferCount];
		if (!IS_USER_ADDRESS(data.record_buffers)
			|| user_memcpy(recordBuffers, data.record_buffers,
				sizeof(recordBuffers)) != B_OK)
			return B_BAD_ADDRESS;
		for (uint32 index = 0; index < kRecordBufferCount; index++) {
			buffer_desc descriptors[2];
			for (uint32 channel = 0; channel < 2; channel++) {
				descriptors[channel].base = (char*)device->recordBuffers
					+ index * kRecordBufferSize + channel * 2;
				descriptors[channel].stride = kFrameSize;
			}
			if (!IS_USER_ADDRESS(recordBuffers[index])
				|| user_memcpy(recordBuffers[index], descriptors,
					sizeof(descriptors)) != B_OK)
				return B_BAD_ADDRESS;
		}
	}
	for (uint32 index = 0; index < kBufferCount; index++) {
		buffer_desc descriptors[2];
		for (uint32 channel = 0; channel < 2; channel++) {
			descriptors[channel].base = (char*)output->buffers
				+ index * kBufferSize + channel * 2;
			descriptors[channel].stride = kFrameSize;
		}
		if (!IS_USER_ADDRESS(playbackBuffers[index])
			|| user_memcpy(playbackBuffers[index], descriptors,
				sizeof(descriptors)) != B_OK)
			return B_BAD_ADDRESS;
	}
	return user_memcpy(userData, &data, sizeof(data));
}


/*!	The output's path up (the headphones' RX macro and codec, a
	DisplayPort audio), the graph started, and both buffers (silent) handed
	to the DSP; the headset input with the headphones.
*/
static void start_capture(AudioDevice* device);


static status_t
start(Output* output)
{
	if (output->running)
		return B_OK;
	AudioDevice* device = output->device;
	status_t status = output_power_up(output);
	if (status == B_OK)
		status = output->path.Start();
	if (status != B_OK) {
		output_power_down(output);
		return status;
	}
	while (acquire_sem_etc(output->doneSem, 1, B_RELATIVE_TIMEOUT, 0) == B_OK)
		;
	cpu_status state = disable_interrupts();
	acquire_spinlock(&device->streamLock);
	output->lastPlayed = -1;
	output->toRefill = -1;
	output->framesPlayed = 0;
	output->playedTime = system_time();
	release_spinlock(&device->streamLock);
	restore_interrupts(state);
	for (uint32 index = 0; index < kBufferCount; index++) {
		clean_buffer(output, index);
		output->path.Write(output->buffersDsp + index * kBufferSize,
			kBufferSize, output->token | index);
	}
	output->running = true;
	if (output == &device->headphones)
		start_capture(device);
	return B_OK;
}


/*!	The headset input alongside: the microphone path, the capture stream
	started, every buffer queued, then its device (as Linux does it). Without
	it, playback goes on.
*/
static void
start_capture(AudioDevice* device)
{
	if (!device->captureOpen || device->capturing)
		return;
	while (acquire_sem_etc(device->recordSem, 1, B_RELATIVE_TIMEOUT, 0)
			== B_OK)
		;
	cpu_status state = disable_interrupts();
	acquire_spinlock(&device->streamLock);
	device->lastRecorded = -1;
	device->recordToRefill = -1;
	device->framesRecorded = 0;
	device->recordedTime = system_time();
	release_spinlock(&device->streamLock);
	restore_interrupts(state);
	status_t status = device->codec.EnableHeadsetMic(48000);
	if (status == B_OK)
		status = device->capture.Start(false);
	for (uint32 index = 0; status == B_OK && index < kRecordBufferCount;
			index++) {
		status = device->capture.Read(device->recordDsp
			+ index * kRecordBufferSize, kRecordBufferSize, index);
	}
	if (status == B_OK)
		status = device->capture.StartDevice();
	if (status != B_OK) {
		TRACE("the headset input does not start: %s\n", strerror(status));
		device->capture.Stop();
		device->codec.DisableHeadsetMic();
		return;
	}
	device->capturing = true;
}


void
multi_audio_stop(Output* output)
{
	if (!output->running)
		return;
	AudioDevice* device = output->device;
	output->running = false;
	output->path.Stop();
	output_power_down(output);
	if (output == &device->headphones && device->capturing) {
		device->capturing = false;
		device->capture.Stop();
		device->codec.DisableHeadsetMic();
	}
}


/*!	The buffer the add-on was handed last is filled now: back to the DSP.
	Then the next one the DSP played is the add-on's to fill.
*/
static status_t
buffer_exchange(Output* output, multi_buffer_info* userInfo)
{
	AudioDevice* device = output->device;
	multi_buffer_info info;
	if (user_memcpy(&info, userInfo, sizeof(info)) != B_OK)
		return B_BAD_ADDRESS;

	{
		MutexLocker locker(device->lock);
		status_t status = start(output);
		if (status != B_OK)
			return status;
		if (output->toRefill >= 0) {
			clean_buffer(output, output->toRefill);
			output->path.Write(output->buffersDsp
				+ output->toRefill * kBufferSize, kBufferSize,
				output->token | output->toRefill);
			output->toRefill = -1;
		}
	}

	status_t status = acquire_sem_etc(output->doneSem, 1,
		B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT, 500000);
	if (status != B_OK) {
		if (status == B_TIMED_OUT) {
			TRACE("the DSP played no %s buffer for 0.5 s\n", output->name);
			device->smmu.Faults("buffer exchange");
		}
		return status;
	}
	cpu_status state = disable_interrupts();
	acquire_spinlock(&device->streamLock);
	int32 played = output->lastPlayed;
	info.played_frames_count = output->framesPlayed;
	info.played_real_time = output->playedTime;
	release_spinlock(&device->streamLock);
	restore_interrupts(state);
	output->toRefill = played;
	info.playback_buffer_cycle = played;
	info.flags = B_MULTI_BUFFER_PLAYBACK;

	// the record buffer the DSP filled meanwhile (it fills at the rate the
	// other plays: one an exchange); the one handed out before goes back
	info.record_buffer_cycle = -1;
	if (output == &device->headphones && device->capturing) {
		if (device->recordToRefill >= 0) {
			device->capture.Read(device->recordDsp
				+ device->recordToRefill * kRecordBufferSize,
				kRecordBufferSize, device->recordToRefill);
			device->recordToRefill = -1;
		}
		if (acquire_sem_etc(device->recordSem, 1, B_RELATIVE_TIMEOUT,
				kBufferFrames * 1000000LL / 48000 / 2) == B_OK) {
			state = disable_interrupts();
			acquire_spinlock(&device->streamLock);
			int32 recorded = device->lastRecorded;
			info.recorded_frames_count = device->framesRecorded;
			info.recorded_real_time = device->recordedTime;
			release_spinlock(&device->streamLock);
			restore_interrupts(state);
			if (recorded >= 0) {
				invalidate_record_buffer(device, recorded);
				device->recordToRefill = recorded;
				info.record_buffer_cycle = recorded;
				info.flags |= B_MULTI_BUFFER_RECORD;
			}
		}
	}
	return user_memcpy(userInfo, &info, sizeof(info));
}


status_t
multi_audio_control(Output* output, uint32 operation, void* buffer,
	size_t length)
{
	// the exchange waits for the DSP without the lock
	if (operation == B_MULTI_BUFFER_EXCHANGE)
		return buffer_exchange(output, (multi_buffer_info*)buffer);
	MutexLocker locker(output->device->lock);
	switch (operation) {
		case B_MULTI_GET_DESCRIPTION:
			return get_description(output, (multi_description*)buffer);
		case B_MULTI_GET_ENABLED_CHANNELS:
			return get_enabled_channels((multi_channel_enable*)buffer);
		case B_MULTI_SET_ENABLED_CHANNELS:
			return B_OK;
		case B_MULTI_GET_GLOBAL_FORMAT:
			return get_global_format((multi_format_info*)buffer);
		case B_MULTI_SET_GLOBAL_FORMAT:
			return set_global_format((const multi_format_info*)buffer);
		case B_MULTI_GET_BUFFERS:
			return get_buffers(output, (multi_buffer_list*)buffer);
		case B_MULTI_BUFFER_FORCE_STOP:
			multi_audio_stop(output);
			return B_OK;
		case B_MULTI_LIST_MIX_CONTROLS:
		{
			multi_mix_control_info info;
			if (user_memcpy(&info, buffer, sizeof(info)) != B_OK)
				return B_BAD_ADDRESS;
			info.control_count = 0;
			return user_memcpy(buffer, &info, sizeof(info));
		}
		case B_MULTI_GET_EVENT_INFO:
		case B_MULTI_SET_EVENT_INFO:
		case B_MULTI_GET_EVENT:
		case B_MULTI_GET_CHANNEL_FORMATS:
		case B_MULTI_SET_CHANNEL_FORMATS:
		case B_MULTI_GET_MIX:
		case B_MULTI_SET_MIX:
		case B_MULTI_LIST_MIX_CHANNELS:
		case B_MULTI_LIST_MIX_CONNECTIONS:
		case B_MULTI_SET_BUFFERS:
		case B_MULTI_SET_START_TIME:
			return B_NOT_SUPPORTED;
	}
	return B_DEV_INVALID_IOCTL;
}


}	// namespace qcom_audio
