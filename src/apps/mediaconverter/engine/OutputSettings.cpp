/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "OutputSettings.h"


OutputSettings::OutputSettings()
	:
	container(kContainerMP4),
	videoCodec(kVideoH264),
	encoderPreference(kPreferHardware),
	rateMode(kRateConstantQuality),
	quality(60),
	videoBitrate(6000),
	speed(kSpeedBalanced),
	maxHeight(0),
	frameRate(0),
	peakFrameRate(true),
	deinterlace(kDeinterlaceAuto),
	denoise(kFilterOff),
	sharpen(kFilterOff),
	grayscale(false),
	autoCrop(false),
	rotation(0),
	flip(false),
	audioCodec(kAudioAAC),
	audioBitrate(160),
	mixdown(kMixdownStereo),
	sampleRate(0),
	audioTracks(kAudioTracksAll),
	passthroughWhenPossible(false),
	subtitles(kSubtitlesCopy),
	copyChapters(true),
	copyMetadata(true),
	start(0),
	end(0)
{
}


status_t
OutputSettings::Archive(BMessage& archive) const
{
	archive.MakeEmpty();
	archive.AddInt32("container", container);
	archive.AddInt32("video codec", videoCodec);
	archive.AddInt32("encoder preference", encoderPreference);
	archive.AddString("gpu", gpu);
	archive.AddInt32("rate mode", rateMode);
	archive.AddInt32("quality", quality);
	archive.AddInt32("video bitrate", videoBitrate);
	archive.AddInt32("speed", speed);
	archive.AddInt32("max height", maxHeight);
	archive.AddInt32("frame rate", frameRate);
	archive.AddBool("peak frame rate", peakFrameRate);
	archive.AddInt32("deinterlace", deinterlace);
	archive.AddInt32("denoise", denoise);
	archive.AddInt32("sharpen", sharpen);
	archive.AddBool("grayscale", grayscale);
	archive.AddBool("auto crop", autoCrop);
	archive.AddInt32("rotation", rotation);
	archive.AddBool("flip", flip);
	archive.AddInt32("audio codec", audioCodec);
	archive.AddInt32("audio bitrate", audioBitrate);
	archive.AddInt32("mixdown", mixdown);
	archive.AddInt32("sample rate", sampleRate);
	archive.AddInt32("audio tracks", audioTracks);
	archive.AddBool("passthrough", passthroughWhenPossible);
	archive.AddInt32("subtitles", subtitles);
	archive.AddBool("chapters", copyChapters);
	archive.AddBool("metadata", copyMetadata);
	return B_OK;
}


template<typename T>
static void
find_enum(const BMessage& archive, const char* name, T& value, int32 count)
{
	int32 stored;
	if (archive.FindInt32(name, &stored) == B_OK && stored >= 0
		&& stored < count)
		value = (T)stored;
}


static void
find_int(const BMessage& archive, const char* name, int32& value)
{
	int32 stored;
	if (archive.FindInt32(name, &stored) == B_OK)
		value = stored;
}


static void
find_bool(const BMessage& archive, const char* name, bool& value)
{
	bool stored;
	if (archive.FindBool(name, &stored) == B_OK)
		value = stored;
}


void
OutputSettings::Unarchive(const BMessage& archive)
{
	find_enum(archive, "container", container, kContainerCount);
	find_enum(archive, "video codec", videoCodec, kVideoCodecCount);
	find_enum(archive, "encoder preference", encoderPreference, 2);
	archive.FindString("gpu", &gpu);
	find_enum(archive, "rate mode", rateMode, 2);
	find_int(archive, "quality", quality);
	find_int(archive, "video bitrate", videoBitrate);
	find_enum(archive, "speed", speed, kSpeedCount);
	find_int(archive, "max height", maxHeight);
	find_int(archive, "frame rate", frameRate);
	find_bool(archive, "peak frame rate", peakFrameRate);
	find_enum(archive, "deinterlace", deinterlace, 3);
	find_enum(archive, "denoise", denoise, 4);
	find_enum(archive, "sharpen", sharpen, 4);
	find_bool(archive, "grayscale", grayscale);
	find_bool(archive, "auto crop", autoCrop);
	find_int(archive, "rotation", rotation);
	find_bool(archive, "flip", flip);
	find_enum(archive, "audio codec", audioCodec, kAudioCodecCount);
	find_int(archive, "audio bitrate", audioBitrate);
	find_enum(archive, "mixdown", mixdown, kMixdownCount);
	find_int(archive, "sample rate", sampleRate);
	find_enum(archive, "audio tracks", audioTracks, 3);
	find_bool(archive, "passthrough", passthroughWhenPossible);
	find_enum(archive, "subtitles", subtitles, kSubtitleModeCount);
	find_bool(archive, "chapters", copyChapters);
	find_bool(archive, "metadata", copyMetadata);
	Validate();
}


bool
OutputSettings::Validate()
{
	bool changed = false;
	if (!container_info(container).video && videoCodec != kVideoNone) {
		videoCodec = kVideoNone;
		changed = true;
	} else if (container_info(container).video
		&& !container_holds_video(container, videoCodec)) {
		videoCodec = default_video_codec(container);
		changed = true;
	}
	if (!container_holds_audio(container, audioCodec)) {
		audioCodec = default_audio_codec(container);
		changed = true;
	}
	if (quality < 0 || quality > 100) {
		quality = 60;
		changed = true;
	}
	if (videoBitrate < 100) {
		videoBitrate = 100;
		changed = true;
	}
	if (audioBitrate < 16) {
		audioBitrate = 16;
		changed = true;
	}
	if (rotation % 90 != 0 || rotation < 0 || rotation >= 360) {
		rotation = 0;
		changed = true;
	}
	return changed;
}


bool
OutputSettings::operator==(const OutputSettings& other) const
{
	BMessage a, b;
	Archive(a);
	other.Archive(b);
	return a.HasSameData(b);
}


const char*
speed_name(speed_preset speed)
{
	switch (speed) {
		case kSpeedFastest:
			return "Fastest";
		case kSpeedFast:
			return "Fast";
		case kSpeedSlow:
			return "Slow (smaller files)";
		case kSpeedSlowest:
			return "Slowest (smallest files)";
		case kSpeedBalanced:
		default:
			return "Balanced";
	}
}
