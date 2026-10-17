/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "Presets.h"

#include <string.h>

#include <vector>


static std::vector<Preset>* sPresets;


static OutputSettings
video(container_kind container, video_codec codec, int32 quality,
	int32 maxHeight, speed_preset speed, audio_codec audio,
	int32 audioBitrate)
{
	OutputSettings settings;
	settings.container = container;
	settings.videoCodec = codec;
	settings.quality = quality;
	settings.maxHeight = maxHeight;
	settings.speed = speed;
	settings.audioCodec = audio;
	settings.audioBitrate = audioBitrate;
	return settings;
}


static OutputSettings
audio(container_kind container, audio_codec codec, int32 bitrate)
{
	OutputSettings settings;
	settings.container = container;
	settings.videoCodec = kVideoNone;
	settings.audioCodec = codec;
	settings.audioBitrate = bitrate;
	settings.audioTracks = kAudioTracksFirst;
	settings.subtitles = kSubtitlesNone;
	settings.copyChapters = false;
	return settings;
}


static void
add(const char* category, const char* name, const char* description,
	const OutputSettings& settings)
{
	Preset preset;
	preset.category = category;
	preset.name = name;
	preset.description = description;
	preset.settings = settings;
	preset.settings.Validate();
	sPresets->push_back(preset);
}


static void
init_presets()
{
	if (sPresets != NULL)
		return;
	sPresets = new std::vector<Preset>;

	add("General", "Fast 1080p",
		"H.264 and AAC in MP4, up to 1080p: plays everywhere.",
		video(kContainerMP4, kVideoH264, 60, 1080, kSpeedFast, kAudioAAC,
			160));
	add("General", "High quality 1080p",
		"H.264 at a higher quality, slower to encode.",
		video(kContainerMP4, kVideoH264, 72, 1080, kSpeedSlow, kAudioAAC,
			160));
	add("General", "Small 720p (H.265)",
		"H.265 at 720p: about half the size of H.264 for the same picture.",
		video(kContainerMP4, kVideoH265, 52, 720, kSpeedBalanced, kAudioAAC,
			128));
	add("General", "4K H.265 10-bit",
		"H.265 10-bit in Matroska, keeps HDR and 4K.",
		video(kContainerMKV, kVideoH265_10, 64, 2160, kSpeedBalanced,
			kAudioEAC3, 640));
	add("General", "AV1 10-bit (MKV)",
		"AV1 10-bit with Opus: the smallest files, slow without a GPU.",
		video(kContainerMKV, kVideoAV1_10, 58, 0, kSpeedBalanced, kAudioOpus,
			160));

	OutputSettings keep = video(kContainerMKV, kVideoCopy, 60, 0,
		kSpeedBalanced, kAudioCopy, 0);
	add("General", "Change container (MKV)",
		"Copies every track into Matroska without converting anything.",
		keep);
	keep.container = kContainerMP4;
	add("General", "Change container (MP4)",
		"Copies the tracks MP4 can hold without converting them.", keep);

	add("Web", "WebM (VP9)", "VP9 and Opus in WebM for the web, up to 1080p.",
		video(kContainerWebM, kVideoVP9, 58, 1080, kSpeedFast, kAudioOpus,
			128));
	add("Web", "WebM (AV1)", "AV1 and Opus in WebM, up to 1080p.",
		video(kContainerWebM, kVideoAV1, 56, 1080, kSpeedBalanced,
			kAudioOpus, 128));
	OutputSettings discord = video(kContainerMP4, kVideoH264, 45, 720,
		kSpeedFast, kAudioAAC, 96);
	discord.rateMode = kRateAverageBitrate;
	discord.videoBitrate = 1500;
	add("Web", "Small for messaging",
		"720p H.264 at 1.5 Mbit/s: small enough to share.", discord);

	OutputSettings apple = video(kContainerMP4, kVideoH265, 62, 2160,
		kSpeedBalanced, kAudioAAC, 160);
	add("Devices", "Apple (H.265)",
		"H.265 and AAC in MP4 for iPhone, iPad, Mac and Apple TV.", apple);
	add("Devices", "Android and Chromecast",
		"H.264 and AAC in MP4, up to 1080p.",
		video(kContainerMP4, kVideoH264, 60, 1080, kSpeedBalanced,
			kAudioAAC, 160));
	OutputSettings dvd = video(kContainerMKV, kVideoMPEG2, 70, 576,
		kSpeedBalanced, kAudioAC3, 448);
	add("Devices", "Old players (MPEG-2)", "MPEG-2 and AC-3 for older players.",
		dvd);

	OutputSettings archive = video(kContainerMKV, kVideoFFV1, 100, 0,
		kSpeedBalanced, kAudioFLAC24, 0);
	archive.deinterlace = kDeinterlaceOff;
	add("Production", "Lossless archive (FFV1)",
		"FFV1 and FLAC in Matroska: bit-exact pictures, very large files.",
		archive);

	add("Audio", "AAC (M4A)", "Music for any device.",
		audio(kContainerM4A, kAudioAAC, 256));
	add("Audio", "Apple Lossless (M4A)", "Lossless, for Apple devices.",
		audio(kContainerM4A, kAudioALAC16, 0));
	add("Audio", "MP3", "MP3 at 256 kbit/s.", audio(kContainerMP3, kAudioMP3,
		256));
	add("Audio", "FLAC", "Lossless FLAC.", audio(kContainerFLAC, kAudioFLAC16,
		0));
	add("Audio", "Opus", "Opus: the best quality for its size.",
		audio(kContainerOpus, kAudioOpus, 160));
	add("Audio", "Ogg Vorbis", "Vorbis in Ogg.", audio(kContainerOgg,
		kAudioVorbis, 192));
	add("Audio", "WAV", "Uncompressed PCM.", audio(kContainerWAV, kAudioPCM16,
		0));
}


int32
Presets::Count()
{
	init_presets();
	return (int32)sPresets->size();
}


const Preset&
Presets::At(int32 index)
{
	init_presets();
	if (index < 0 || index >= (int32)sPresets->size())
		index = 0;
	return (*sPresets)[index];
}


int32
Presets::IndexOf(const char* name)
{
	init_presets();
	for (size_t i = 0; i < sPresets->size(); i++) {
		if (strcmp((*sPresets)[i].name, name) == 0)
			return (int32)i;
	}
	return -1;
}


int32
Presets::DefaultIndex()
{
	return 0;
}
