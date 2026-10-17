/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "AudioPreview.h"

#include <SoundPlayer.h>

#include <stdio.h>
#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}


static const int kSampleRate = 48000;
static const int kChannels = 2;
static const size_t kBufferFrames = 1024;


AudioPreview::AudioPreview()
	:
	fFormat(NULL),
	fDecoder(NULL),
	fResampler(NULL),
	fStream(-1),
	fDuration(0),
	fPlayer(NULL),
	fThread(-1),
	fCapacity(kSampleRate),	// a second
	fWritten(0),
	fRead(0),
	fBase(0),
	fBaseFrame(0),
	fSerial(0),
	fSeekTo(-1),
	fDecoded(false),
	fQuit(false),
	fPlaying(false),
	fVolume(1.0f),
	fMuted(false),
	fGain(1.0f)
{
	fRing.resize(fCapacity * kChannels);
}


AudioPreview::~AudioPreview()
{
	Close();
}


status_t
AudioPreview::Open(const char* path, BString* error)
{
	Close();
	if (avformat_open_input(&fFormat, path, NULL, NULL) < 0) {
		fFormat = NULL;
		*error = "The file could not be opened.";
		return B_ERROR;
	}
	const AVCodec* codec = NULL;
	if (avformat_find_stream_info(fFormat, NULL) < 0
		|| (fStream = av_find_best_stream(fFormat, AVMEDIA_TYPE_AUDIO, -1, -1,
			&codec, 0)) < 0) {
		*error = "The file has no sound.";
		Close();
		return B_ERROR;
	}
	fDecoder = avcodec_alloc_context3(codec);
	avcodec_parameters_to_context(fDecoder,
		fFormat->streams[fStream]->codecpar);
	if (avcodec_open2(fDecoder, codec, NULL) < 0) {
		*error = "There is no decoder for this sound.";
		Close();
		return B_ERROR;
	}
	AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
	AVChannelLayout source;
	if (fDecoder->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
		av_channel_layout_default(&source, fDecoder->ch_layout.nb_channels);
	else
		av_channel_layout_copy(&source, &fDecoder->ch_layout);
	int result = swr_alloc_set_opts2(&fResampler, &stereo, AV_SAMPLE_FMT_FLT,
		kSampleRate, &source, fDecoder->sample_fmt, fDecoder->sample_rate, 0,
		NULL);
	av_channel_layout_uninit(&source);
	if (result < 0 || swr_init(fResampler) < 0) {
		*error = "The sound could not be converted for playing.";
		Close();
		return B_ERROR;
	}
	fDuration = fFormat->duration != AV_NOPTS_VALUE ? fFormat->duration : 0;

	media_raw_audio_format format = media_raw_audio_format::wildcard;
	format.frame_rate = kSampleRate;
	format.channel_count = kChannels;
	format.format = media_raw_audio_format::B_AUDIO_FLOAT;
	format.byte_order = B_MEDIA_HOST_ENDIAN;
	format.buffer_size = kBufferFrames * kChannels * sizeof(float);
	fPlayer = new(std::nothrow) BSoundPlayer(&format, "MediaConverter preview",
		_PlayBuffer, NULL, this);
	if (fPlayer == NULL || fPlayer->InitCheck() != B_OK
		|| fPlayer->Start() != B_OK) {
		*error = "The sound card could not be opened.";
		Close();
		return B_ERROR;
	}
	fPlayer->SetHasData(true);

	{
		std::lock_guard<std::mutex> lock(fLock);
		fQuit = false;
		fWritten = fRead = fBaseFrame = 0;
		fBase = 0;
		fSeekTo = -1;
		fDecoded = false;
		fPlaying = false;
	}
	fThread = spawn_thread(_DecodeThread, "preview decoder",
		B_NORMAL_PRIORITY, this);
	if (fThread < 0 || resume_thread(fThread) != B_OK) {
		fThread = -1;
		*error = "The preview could not start.";
		Close();
		return B_ERROR;
	}
	return B_OK;
}


void
AudioPreview::Close()
{
	{
		std::lock_guard<std::mutex> lock(fLock);
		fQuit = true;
		fPlaying = false;
		fChanged.notify_all();
	}
	if (fThread >= 0) {
		status_t result;
		wait_for_thread(fThread, &result);
		fThread = -1;
	}
	if (fPlayer != NULL) {
		fPlayer->Stop();
		delete fPlayer;
		fPlayer = NULL;
	}
	swr_free(&fResampler);
	avcodec_free_context(&fDecoder);
	avformat_close_input(&fFormat);
	fStream = -1;
	fDuration = 0;
}


void
AudioPreview::Play()
{
	std::lock_guard<std::mutex> lock(fLock);
	if (fFormat == NULL)
		return;
	// from the start again once the end was played
	if (fDecoded && fRead >= fWritten) {
		fSerial++;
		fSeekTo = 0;
		fWritten = fRead = fBaseFrame = 0;
		fBase = 0;
		fDecoded = false;
		fChanged.notify_all();
	}
	fPlaying = true;
}


void
AudioPreview::Pause()
{
	std::lock_guard<std::mutex> lock(fLock);
	fPlaying = false;
}


bool
AudioPreview::AtEnd()
{
	std::lock_guard<std::mutex> lock(fLock);
	if (fFormat == NULL || !fDecoded || fRead < fWritten)
		return false;
	fPlaying = false;
	return true;
}


void
AudioPreview::Seek(bigtime_t time)
{
	std::lock_guard<std::mutex> lock(fLock);
	if (time < 0)
		time = 0;
	if (fDuration > 0 && time > fDuration)
		time = fDuration;
	fSerial++;
	fSeekTo = time;
	fWritten = fRead = fBaseFrame = 0;
	fBase = time;
	fDecoded = false;
	fChanged.notify_all();
}


bigtime_t
AudioPreview::Position()
{
	std::lock_guard<std::mutex> lock(fLock);
	bigtime_t position = fBase
		+ (bigtime_t)((fRead - fBaseFrame) * 1000000 / kSampleRate);
	if (fDuration > 0 && position > fDuration)
		position = fDuration;
	return position;
}


void
AudioPreview::SetVolume(float volume)
{
	std::lock_guard<std::mutex> lock(fLock);
	fVolume = volume < 0 ? 0 : volume > 1 ? 1 : volume;
}


void
AudioPreview::SetMuted(bool muted)
{
	std::lock_guard<std::mutex> lock(fLock);
	fMuted = muted;
}


// #pragma mark - decoding


/*static*/ status_t
AudioPreview::_DecodeThread(void* data)
{
	((AudioPreview*)data)->_Decode();
	return B_OK;
}


/*!	Blocks while the ring is full; false when the samples are no longer
	wanted (a seek, or closing). */
bool
AudioPreview::_Write(const float* samples, size_t frames, int serial)
{
	std::unique_lock<std::mutex> lock(fLock);
	while (frames > 0) {
		if (fQuit || serial != fSerial)
			return false;
		size_t used = fWritten - fRead;
		size_t free = fCapacity - used;
		if (free == 0) {
			fChanged.wait_for(lock, std::chrono::milliseconds(20));
			continue;
		}
		size_t count = frames < free ? frames : free;
		for (size_t i = 0; i < count; i++) {
			size_t at = (size_t)((fWritten + i) % fCapacity) * kChannels;
			fRing[at] = samples[i * kChannels];
			fRing[at + 1] = samples[i * kChannels + 1];
		}
		fWritten += count;
		samples += count * kChannels;
		frames -= count;
	}
	return true;
}


void
AudioPreview::_Decode()
{
	AVPacket* packet = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	std::vector<float> converted;
	AVRational timeBase = fFormat->streams[fStream]->time_base;
	bigtime_t discardUntil = -1;
	int serial;
	{
		std::lock_guard<std::mutex> lock(fLock);
		serial = fSerial;
	}

	while (true) {
		bigtime_t seekTo = -1;
		{
			std::unique_lock<std::mutex> lock(fLock);
			// at the end, wait for a seek
			while (!fQuit && fDecoded && fSeekTo < 0)
				fChanged.wait(lock);
			if (fQuit)
				break;
			if (fSeekTo >= 0) {
				seekTo = fSeekTo;
				fSeekTo = -1;
				serial = fSerial;
			}
		}
		if (seekTo >= 0) {
			int64 start = fFormat->start_time != AV_NOPTS_VALUE
				? fFormat->start_time : 0;
			av_seek_frame(fFormat, -1, start + seekTo, AVSEEK_FLAG_BACKWARD);
			avcodec_flush_buffers(fDecoder);
			swr_init(fResampler);
			discardUntil = seekTo;
		}

		bool ended = false;
		int result = av_read_frame(fFormat, packet);
		if (result < 0) {
			avcodec_send_packet(fDecoder, NULL);
			ended = true;
		} else if (packet->stream_index == fStream) {
			avcodec_send_packet(fDecoder, packet);
			av_packet_unref(packet);
		} else {
			av_packet_unref(packet);
			continue;
		}

		bool wanted = true;
		while (wanted && avcodec_receive_frame(fDecoder, frame) == 0) {
			int64 stamp = frame->best_effort_timestamp;
			int64 start = fFormat->streams[fStream]->start_time;
			bigtime_t pts = stamp != AV_NOPTS_VALUE
				? av_rescale_q(stamp - (start != AV_NOPTS_VALUE ? start : 0),
					timeBase, AV_TIME_BASE_Q) : -1;
			bigtime_t length = (bigtime_t)frame->nb_samples * 1000000
				/ (frame->sample_rate > 0 ? frame->sample_rate : kSampleRate);
			if (discardUntil >= 0 && pts >= 0 && pts + length <= discardUntil) {
				av_frame_unref(frame);
				continue;
			}
			int frames = swr_get_out_samples(fResampler, frame->nb_samples);
			converted.resize((size_t)frames * kChannels);
			uint8* output = (uint8*)converted.data();
			frames = swr_convert(fResampler, &output, frames,
				(const uint8**)frame->extended_data, frame->nb_samples);
			av_frame_unref(frame);
			if (frames <= 0)
				continue;
			const float* samples = converted.data();
			if (discardUntil >= 0) {
				// the first samples after a seek start where it was asked
				if (pts >= 0 && pts < discardUntil) {
					size_t skip = (size_t)((discardUntil - pts) * kSampleRate
						/ 1000000);
					if (skip >= (size_t)frames)
						continue;
					samples += skip * kChannels;
					frames -= (int)skip;
				}
				discardUntil = -1;
			}
			wanted = _Write(samples, frames, serial);
		}

		if (ended) {
			std::lock_guard<std::mutex> lock(fLock);
			if (serial == fSerial)
				fDecoded = true;
		}
	}
	av_frame_free(&frame);
	av_packet_free(&packet);
}


// #pragma mark - the sound card's side


/*static*/ void
AudioPreview::_PlayBuffer(void* cookie, void* buffer, size_t size,
	const media_raw_audio_format& format)
{
	AudioPreview* preview = (AudioPreview*)cookie;
	if (format.format != media_raw_audio_format::B_AUDIO_FLOAT
		|| (int)format.channel_count != kChannels) {
		memset(buffer, 0, size);
		return;
	}
	preview->_Fill((float*)buffer, size / (sizeof(float) * kChannels));
}


void
AudioPreview::_Fill(float* buffer, size_t frames)
{
	std::lock_guard<std::mutex> lock(fLock);
	size_t available = fWritten - fRead;
	if (!fPlaying || available == 0) {
		memset(buffer, 0, frames * kChannels * sizeof(float));
		return;
	}
	size_t count = frames < available ? frames : available;
	// a squared volume sounds more even across the slider, as in airTime
	float target = fMuted ? 0.0f : fVolume * fVolume;
	float gain = fGain;
	float step = (target - gain) / (float)count;
	for (size_t i = 0; i < count; i++) {
		size_t at = (size_t)((fRead + i) % fCapacity) * kChannels;
		buffer[i * kChannels] = fRing[at] * gain;
		buffer[i * kChannels + 1] = fRing[at + 1] * gain;
		gain += step;
	}
	fGain = target;
	fRead += count;
	if (count < frames)
		memset(buffer + count * kChannels, 0,
			(frames - count) * kChannels * sizeof(float));
	fChanged.notify_all();
}
