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


static const multi_channel_info kChannels[] = {
	{0, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0},
	{1, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0},
	{2, B_MULTI_OUTPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO},
	{3, B_MULTI_OUTPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO},
};


static status_t
get_description(multi_description* userData)
{
	multi_description description;
	if (user_memcpy(&description, userData, sizeof(description)) != B_OK)
		return B_BAD_ADDRESS;
	description.interface_version = B_CURRENT_INTERFACE_VERSION;
	description.interface_minimum = B_CURRENT_INTERFACE_VERSION;
	strlcpy(description.friendly_name, "Headphones",
		sizeof(description.friendly_name));
	strlcpy(description.vendor_info, "Qualcomm SC8280XP / WCD9385",
		sizeof(description.vendor_info));
	description.output_channel_count = 2;
	description.input_channel_count = 0;
	description.output_bus_channel_count = 2;
	description.input_bus_channel_count = 0;
	description.aux_bus_channel_count = 0;
	description.output_rates = B_SR_48000;
	description.input_rates = 0;
	description.max_cvsr_rate = 0;
	description.min_cvsr_rate = 0;
	description.output_formats = B_FMT_16BIT;
	description.input_formats = 0;
	description.lock_sources = B_MULTI_LOCK_INTERNAL;
	description.timecode_sources = 0;
	description.interface_flags = B_MULTI_INTERFACE_PLAYBACK;
	description.start_latency = 30000;
	description.control_panel[0] = '\0';
	if (user_memcpy(userData, &description, sizeof(description)) != B_OK)
		return B_BAD_ADDRESS;
	if (description.request_channel_count >= (int32)B_COUNT_OF(kChannels)
		&& user_memcpy(description.channels, kChannels, sizeof(kChannels))
			!= B_OK) {
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
	B_SET_CHANNEL(data.enable_bits, 0, true);
	B_SET_CHANNEL(data.enable_bits, 1, true);
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
get_buffers(AudioDevice* device, multi_buffer_list* userData)
{
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

	multi_audio_stop(device);
	memset(device->buffers, 0, kBufferCount * kBufferSize);
	data.flags = B_MULTI_BUFFER_PLAYBACK;
	data.return_playback_buffers = kBufferCount;
	data.return_playback_channels = 2;
	data.return_playback_buffer_size = kBufferFrames;
	data.return_record_buffers = 0;
	data.return_record_channels = 0;
	data.return_record_buffer_size = 0;
	for (uint32 index = 0; index < kBufferCount; index++) {
		buffer_desc descriptors[2];
		for (uint32 channel = 0; channel < 2; channel++) {
			descriptors[channel].base = (char*)device->buffers
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


/*!	The headphone path up (the RX macro, the codec), the graph started, and
	both buffers (silent) handed to the DSP.
*/
static status_t
start(AudioDevice* device)
{
	if (device->running)
		return B_OK;
	device->codec.EnableHeadphonePath(48000);
	status_t status = device->codec.EnableHeadphones(kHeadphoneGain);
	if (status == B_OK)
		status = device->playback.Start();
	if (status != B_OK) {
		device->codec.DisableHeadphones();
		device->codec.DisableHeadphonePath();
		return status;
	}
	while (acquire_sem_etc(device->doneSem, 1, B_RELATIVE_TIMEOUT, 0) == B_OK)
		;
	cpu_status state = disable_interrupts();
	acquire_spinlock(&device->streamLock);
	device->lastPlayed = -1;
	device->toRefill = -1;
	device->framesPlayed = 0;
	device->playedTime = system_time();
	release_spinlock(&device->streamLock);
	restore_interrupts(state);
	for (uint32 index = 0; index < kBufferCount; index++) {
		clean_buffer(device, index);
		device->playback.Write(device->buffersDsp + index * kBufferSize,
			kBufferSize, index);
	}
	device->running = true;
	return B_OK;
}


void
multi_audio_stop(AudioDevice* device)
{
	if (!device->running)
		return;
	device->running = false;
	device->playback.Stop();
	device->codec.DisableHeadphones();
	device->codec.DisableHeadphonePath();
}


/*!	The buffer the add-on was handed last is filled now: back to the DSP.
	Then the next one the DSP played is the add-on's to fill.
*/
static status_t
buffer_exchange(AudioDevice* device, multi_buffer_info* userInfo)
{
	multi_buffer_info info;
	if (user_memcpy(&info, userInfo, sizeof(info)) != B_OK)
		return B_BAD_ADDRESS;

	{
		MutexLocker locker(device->lock);
		status_t status = start(device);
		if (status != B_OK)
			return status;
		if (device->toRefill >= 0) {
			clean_buffer(device, device->toRefill);
			device->playback.Write(device->buffersDsp
				+ device->toRefill * kBufferSize, kBufferSize,
				device->toRefill);
			device->toRefill = -1;
		}
	}

	status_t status = acquire_sem_etc(device->doneSem, 1,
		B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT, 500000);
	if (status != B_OK) {
		if (status == B_TIMED_OUT) {
			TRACE("the DSP played no buffer for 0.5 s\n");
			device->smmu.Faults("buffer exchange");
		}
		return status;
	}
	cpu_status state = disable_interrupts();
	acquire_spinlock(&device->streamLock);
	int32 played = device->lastPlayed;
	info.played_frames_count = device->framesPlayed;
	info.played_real_time = device->playedTime;
	release_spinlock(&device->streamLock);
	restore_interrupts(state);
	device->toRefill = played;
	info.playback_buffer_cycle = played;
	info.flags = B_MULTI_BUFFER_PLAYBACK;
	return user_memcpy(userInfo, &info, sizeof(info));
}


status_t
multi_audio_control(AudioDevice* device, uint32 operation, void* buffer,
	size_t length)
{
	// the exchange waits for the DSP without the lock
	if (operation == B_MULTI_BUFFER_EXCHANGE)
		return buffer_exchange(device, (multi_buffer_info*)buffer);
	MutexLocker locker(device->lock);
	switch (operation) {
		case B_MULTI_GET_DESCRIPTION:
			return get_description((multi_description*)buffer);
		case B_MULTI_GET_ENABLED_CHANNELS:
			return get_enabled_channels((multi_channel_enable*)buffer);
		case B_MULTI_SET_ENABLED_CHANNELS:
			return B_OK;
		case B_MULTI_GET_GLOBAL_FORMAT:
			return get_global_format((multi_format_info*)buffer);
		case B_MULTI_SET_GLOBAL_FORMAT:
			return set_global_format((const multi_format_info*)buffer);
		case B_MULTI_GET_BUFFERS:
			return get_buffers(device, (multi_buffer_list*)buffer);
		case B_MULTI_BUFFER_FORCE_STOP:
			multi_audio_stop(device);
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
