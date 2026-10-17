/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "Presets.h"

#include <string.h>

#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

#include "HardwareCaps.h"
#include "LavcVideoEncoder.h"
#include "SoftwareEncoders.h"


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


/*!	For songs: the channels and the rate of the source unless a preset says
	otherwise, the tags, the artwork and the chapters kept. */
static OutputSettings
sound(container_kind container, audio_codec codec, int32 bitrate,
	int32 sampleRate = 0)
{
	OutputSettings settings;
	settings.container = container;
	settings.videoCodec = kVideoNone;
	settings.audioCodec = codec;
	settings.audioBitrate = bitrate;
	settings.mixdown = kMixdownSource;
	settings.sampleRate = sampleRate;
	settings.audioTracks = kAudioTracksFirst;
	settings.subtitles = kSubtitlesNone;
	settings.copyChapters = true;
	settings.copyMetadata = true;
	return settings;
}


static void
add(const char* category, const char* name, const char* description,
	const OutputSettings& settings, bool forSound = false)
{
	Preset preset;
	preset.category = category;
	preset.name = name;
	preset.description = description;
	preset.settings = settings;
	preset.settings.Validate();
	preset.sound = forSound;
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

	// for songs
	add("MP3", "MP3, 128 kbit/s", "Small MP3s for speech and casual listening.",
		sound(kContainerMP3, kAudioMP3, 128), true);
	add("MP3", "MP3, 192 kbit/s", "MP3 that sounds good on most music.",
		sound(kContainerMP3, kAudioMP3, 192), true);
	add("MP3", "MP3, 256 kbit/s", "High quality MP3.",
		sound(kContainerMP3, kAudioMP3, 256), true);
	add("MP3", "MP3, 320 kbit/s", "The best MP3 there is; plays everywhere.",
		sound(kContainerMP3, kAudioMP3, 320), true);
	add("AAC", "AAC, 128 kbit/s", "AAC in M4A: as good as MP3 at 192 kbit/s.",
		sound(kContainerM4A, kAudioAAC, 128), true);
	add("AAC", "AAC, 192 kbit/s", "AAC in M4A for phones and cars.",
		sound(kContainerM4A, kAudioAAC, 192), true);
	add("AAC", "AAC, 256 kbit/s", "AAC in M4A at the rate music stores sell.",
		sound(kContainerM4A, kAudioAAC, 256), true);
	add("FLAC", "FLAC, CD quality", "Lossless FLAC at 16 bits and 44.1 kHz.",
		sound(kContainerFLAC, kAudioFLAC16, 0, 44100), true);
	add("FLAC", "FLAC, as the source", "Lossless FLAC keeping the source's "
		"bits (up to 24) and rate.",
		sound(kContainerFLAC, kAudioFLAC24, 0), true);
	add("Other", "Apple Lossless", "Lossless ALAC in M4A, for Apple devices.",
		sound(kContainerM4A, kAudioALAC24, 0), true);
	add("Other", "Opus, 160 kbit/s", "Opus: the best quality for its size.",
		sound(kContainerOpus, kAudioOpus, 160), true);
	add("Other", "Ogg Vorbis, 192 kbit/s", "Vorbis in Ogg.",
		sound(kContainerOgg, kAudioVorbis, 192), true);
	add("Other", "WAV", "Uncompressed 16-bit PCM.",
		sound(kContainerWAV, kAudioPCM16, 0), true);
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


static bool
can_encode_audio(audio_codec codec)
{
	if (codec == kAudioCopy || codec == kAudioNone)
		return true;
	const AudioCodecInfo& info = audio_codec_info(codec);
	if (info.lavcEncoder != NULL
		&& avcodec_find_encoder_by_name(info.lavcEncoder) != NULL)
		return true;
	// the stand-ins the transcoder takes
	switch (codec) {
		case kAudioOpus:
			return avcodec_find_encoder_by_name("opus") != NULL;
		case kAudioVorbis:
			return avcodec_find_encoder_by_name("vorbis") != NULL;
		case kAudioHEAAC:
			return avcodec_find_encoder_by_name("aac") != NULL;
		default:
			return false;
	}
}


static bool
can_make(const OutputSettings& settings)
{
	if (!can_encode_audio(settings.audioCodec))
		return false;
	video_codec codec = settings.videoCodec;
	if (codec == kVideoCopy || codec == kVideoNone
		|| !container_info(settings.container).video)
		return true;
	if (SoftwareEncoders::IsAvailable(codec)
		|| LavcVideoEncoder::IsAvailable(codec))
		return true;
	HardwareInventory& inventory = HardwareInventory::Default();
	if (!inventory.IsProbed())
		inventory.Probe();
	return inventory.EncoderFor(codec, 1920, 1080) != NULL;
}


int32
Presets::DefaultIndex(bool forSound)
{
	init_presets();
	// "MP3, 320 kbit/s" for songs; the first video preset this computer
	// can make (without x264, as on arm64, not "Fast 1080p")
	if (forSound) {
		int32 mp3 = IndexOf("MP3, 320 kbit/s");
		if (mp3 >= 0 && can_make(At(mp3).settings))
			return mp3;
	}
	for (int32 i = 0; i < Count(); i++) {
		if (At(i).sound == forSound && can_make(At(i).settings))
			return i;
	}
	for (int32 i = 0; i < Count(); i++) {
		if (At(i).sound == forSound)
			return i;
	}
	return 0;
}

