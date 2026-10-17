/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "Formats.h"

#include <string.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}


static const ContainerInfo kContainers[kContainerCount] = {
	{ kContainerMP4, "MP4", "mp4", "mp4", "video/mp4", true },
	{ kContainerMKV, "Matroska (MKV)", "matroska", "mkv", "video/x-matroska",
		true },
	{ kContainerWebM, "WebM", "webm", "webm", "video/webm", true },
	{ kContainerMOV, "QuickTime (MOV)", "mov", "mov", "video/quicktime",
		true },
	{ kContainerM4A, "Audio: MPEG-4 (M4A)", "ipod", "m4a", "audio/mp4",
		false },
	{ kContainerMP3, "Audio: MP3", "mp3", "mp3", "audio/mpeg", false },
	{ kContainerFLAC, "Audio: FLAC", "flac", "flac", "audio/flac", false },
	{ kContainerOgg, "Audio: Ogg", "ogg", "ogg", "audio/ogg", false },
	{ kContainerOpus, "Audio: Opus", "opus", "opus", "audio/ogg", false },
	{ kContainerWAV, "Audio: WAV", "wav", "wav", "audio/x-wav", false },
};


static const VideoCodecInfo kVideoCodecs[kVideoCodecCount] = {
	{ kVideoH264, "H.264", "H.264", AV_CODEC_ID_H264, 8, kSoftwareX264,
		NULL },
	{ kVideoH264_10, "H.264 10-bit", "H.264", AV_CODEC_ID_H264, 10,
		kSoftwareX264, NULL },
	{ kVideoH265, "H.265 (HEVC)", "H.265", AV_CODEC_ID_HEVC, 8,
		kSoftwareX265, NULL },
	{ kVideoH265_10, "H.265 10-bit", "H.265", AV_CODEC_ID_HEVC, 10,
		kSoftwareX265, NULL },
	{ kVideoH265_12, "H.265 12-bit", "H.265", AV_CODEC_ID_HEVC, 12,
		kSoftwareX265, NULL },
	{ kVideoAV1, "AV1", "AV1", AV_CODEC_ID_AV1, 8, kSoftwareSvtAv1, NULL },
	{ kVideoAV1_10, "AV1 10-bit", "AV1", AV_CODEC_ID_AV1, 10,
		kSoftwareSvtAv1, NULL },
	{ kVideoVP9, "VP9", "VP9", AV_CODEC_ID_VP9, 8, kSoftwareLibavcodec,
		"libvpx-vp9" },
	{ kVideoVP9_10, "VP9 10-bit", "VP9", AV_CODEC_ID_VP9, 10,
		kSoftwareLibavcodec, "libvpx-vp9" },
	{ kVideoVP8, "VP8", "VP8", AV_CODEC_ID_VP8, 8, kSoftwareLibavcodec,
		"libvpx" },
	{ kVideoMPEG4, "MPEG-4 Part 2", "MPEG-4", AV_CODEC_ID_MPEG4, 8,
		kSoftwareLibavcodec, "mpeg4" },
	{ kVideoMPEG2, "MPEG-2", "MPEG-2", AV_CODEC_ID_MPEG2VIDEO, 8,
		kSoftwareLibavcodec, "mpeg2video" },
	{ kVideoTheora, "Theora", "Theora", AV_CODEC_ID_THEORA, 8,
		kSoftwareLibavcodec, "libtheora" },
	{ kVideoFFV1, "FFV1 (lossless)", "FFV1", AV_CODEC_ID_FFV1, 8,
		kSoftwareLibavcodec, "ffv1" },
	{ kVideoCopy, "Same as source (copy)", "", AV_CODEC_ID_NONE, 0,
		kSoftwareNone, NULL },
	{ kVideoNone, "No video", "", AV_CODEC_ID_NONE, 0, kSoftwareNone,
		NULL },
};


static const AudioCodecInfo kAudioCodecs[kAudioCodecCount] = {
	{ kAudioAAC, "AAC", AV_CODEC_ID_AAC, "aac", false, 160, 8 },
	{ kAudioHEAAC, "HE-AAC", AV_CODEC_ID_AAC, "libfdk_aac", false, 80, 8 },
	{ kAudioMP3, "MP3", AV_CODEC_ID_MP3, "libmp3lame", false, 192, 2 },
	{ kAudioMP2, "MP2", AV_CODEC_ID_MP2, "mp2", false, 224, 2 },
	{ kAudioAC3, "AC-3 (Dolby Digital)", AV_CODEC_ID_AC3, "ac3", false, 448,
		6 },
	{ kAudioEAC3, "E-AC-3 (Dolby Digital Plus)", AV_CODEC_ID_EAC3, "eac3",
		false, 448, 8 },
	{ kAudioTrueHD, "TrueHD (lossless)", AV_CODEC_ID_TRUEHD, "truehd", true,
		0, 8 },
	{ kAudioFLAC16, "FLAC 16-bit (lossless)", AV_CODEC_ID_FLAC, "flac", true,
		0, 8 },
	{ kAudioFLAC24, "FLAC up to 24-bit (lossless)", AV_CODEC_ID_FLAC, "flac", true,
		0, 8 },
	{ kAudioALAC16, "ALAC 16-bit (lossless)", AV_CODEC_ID_ALAC, "alac", true,
		0, 8 },
	{ kAudioALAC24, "ALAC up to 24-bit (lossless)", AV_CODEC_ID_ALAC, "alac", true,
		0, 8 },
	{ kAudioOpus, "Opus", AV_CODEC_ID_OPUS, "libopus", false, 128, 8 },
	{ kAudioVorbis, "Vorbis", AV_CODEC_ID_VORBIS, "libvorbis", false, 160,
		8 },
	{ kAudioPCM16, "PCM 16-bit", AV_CODEC_ID_PCM_S16LE, "pcm_s16le", true, 0,
		8 },
	{ kAudioPCM24, "PCM 24-bit", AV_CODEC_ID_PCM_S24LE, "pcm_s24le", true, 0,
		8 },
	{ kAudioCopy, "Same as source (copy)", AV_CODEC_ID_NONE, NULL, false, 0,
		0 },
	{ kAudioNone, "No audio", AV_CODEC_ID_NONE, NULL, false, 0, 0 },
};


const ContainerInfo&
container_info(container_kind kind)
{
	if (kind < 0 || kind >= kContainerCount)
		kind = kContainerMP4;
	return kContainers[kind];
}


const VideoCodecInfo&
video_codec_info(video_codec codec)
{
	if (codec < 0 || codec >= kVideoCodecCount)
		codec = kVideoH264;
	return kVideoCodecs[codec];
}


const AudioCodecInfo&
audio_codec_info(audio_codec codec)
{
	if (codec < 0 || codec >= kAudioCodecCount)
		codec = kAudioAAC;
	return kAudioCodecs[codec];
}


const char*
mixdown_name(audio_mixdown mixdown)
{
	switch (mixdown) {
		case kMixdownMono:
			return "Mono";
		case kMixdownStereo:
			return "Stereo";
		case kMixdown51:
			return "5.1 surround";
		case kMixdown71:
			return "7.1 surround";
		case kMixdownSource:
		default:
			return "Same as source";
	}
}


const char*
subtitle_mode_name(subtitle_mode mode)
{
	switch (mode) {
		case kSubtitlesCopy:
			return "Keep all tracks";
		case kSubtitlesBurnForced:
			return "Burn in forced subtitles";
		case kSubtitlesBurnFirst:
			return "Burn in the first track";
		case kSubtitlesNone:
		default:
			return "None";
	}
}


const char*
backend_name(encoder_backend backend)
{
	switch (backend) {
		case kBackendVAAPI:
			return "VA-API";
		case kBackendNVENC:
			return "NVENC";
		case kBackendSoftware:
		default:
			return "CPU";
	}
}


bool
container_holds_video(container_kind kind, video_codec codec)
{
	if (codec == kVideoNone)
		return true;
	if (!container_info(kind).video)
		return false;
	if (codec == kVideoCopy)
		return true;

	switch (kind) {
		case kContainerMKV:
			return true;
		case kContainerWebM:
			return codec == kVideoVP8 || codec == kVideoVP9
				|| codec == kVideoVP9_10 || codec == kVideoAV1
				|| codec == kVideoAV1_10;
		case kContainerMP4:
		case kContainerMOV:
			return codec == kVideoH264 || codec == kVideoH264_10
				|| codec == kVideoH265 || codec == kVideoH265_10
				|| codec == kVideoH265_12 || codec == kVideoAV1
				|| codec == kVideoAV1_10 || codec == kVideoVP9
				|| codec == kVideoVP9_10 || codec == kVideoMPEG4
				|| codec == kVideoMPEG2;
		default:
			return false;
	}
}


bool
container_holds_audio(container_kind kind, audio_codec codec)
{
	if (codec == kAudioNone || codec == kAudioCopy)
		return true;

	switch (kind) {
		case kContainerMKV:
			return true;
		case kContainerWebM:
			return codec == kAudioOpus || codec == kAudioVorbis;
		case kContainerMP4:
			return codec == kAudioAAC || codec == kAudioHEAAC
				|| codec == kAudioMP3 || codec == kAudioMP2
				|| codec == kAudioAC3 || codec == kAudioEAC3
				|| codec == kAudioTrueHD || codec == kAudioFLAC16
				|| codec == kAudioFLAC24 || codec == kAudioALAC16
				|| codec == kAudioALAC24 || codec == kAudioOpus;
		case kContainerMOV:
			return codec == kAudioAAC || codec == kAudioHEAAC
				|| codec == kAudioMP3 || codec == kAudioAC3
				|| codec == kAudioEAC3 || codec == kAudioALAC16
				|| codec == kAudioALAC24 || codec == kAudioPCM16
				|| codec == kAudioPCM24;
		case kContainerM4A:
			return codec == kAudioAAC || codec == kAudioHEAAC
				|| codec == kAudioALAC16 || codec == kAudioALAC24;
		case kContainerMP3:
			return codec == kAudioMP3;
		case kContainerFLAC:
			return codec == kAudioFLAC16 || codec == kAudioFLAC24;
		case kContainerOgg:
			return codec == kAudioVorbis || codec == kAudioOpus
				|| codec == kAudioFLAC16 || codec == kAudioFLAC24;
		case kContainerOpus:
			return codec == kAudioOpus;
		case kContainerWAV:
			return codec == kAudioPCM16 || codec == kAudioPCM24;
		default:
			return false;
	}
}


bool
container_takes_stream(container_kind kind, AVCodecID codec, bool video)
{
	if (codec == AV_CODEC_ID_NONE)
		return false;
	if (video && !container_info(kind).video)
		return false;

	if (kind == kContainerWebM) {
		return codec == AV_CODEC_ID_VP8 || codec == AV_CODEC_ID_VP9
			|| codec == AV_CODEC_ID_AV1 || codec == AV_CODEC_ID_OPUS
			|| codec == AV_CODEC_ID_VORBIS;
	}

	const AVOutputFormat* format
		= av_guess_format(container_info(kind).muxer, NULL, NULL);
	if (format == NULL)
		return false;
	return avformat_query_codec(format, codec, FF_COMPLIANCE_NORMAL) == 1;
}


bool
container_takes_cover(container_kind kind, AVCodecID codec)
{
	// what the muxers write as pictures; avformat_query_codec() does not
	// know them, as no muxer has a picture codec as a track of its own
	switch (kind) {
		case kContainerMP3:
		case kContainerFLAC:
			return codec == AV_CODEC_ID_MJPEG || codec == AV_CODEC_ID_PNG
				|| codec == AV_CODEC_ID_BMP || codec == AV_CODEC_ID_GIF;
		case kContainerM4A:
			return codec == AV_CODEC_ID_MJPEG || codec == AV_CODEC_ID_PNG
				|| codec == AV_CODEC_ID_BMP;
		default:
			return false;
	}
}


static bool
is_text_subtitle(AVCodecID codec)
{
	const AVCodecDescriptor* descriptor = avcodec_descriptor_get(codec);
	return descriptor != NULL
		&& (descriptor->props & AV_CODEC_PROP_TEXT_SUB) != 0;
}


bool
container_takes_subtitle(container_kind kind, AVCodecID codec,
	AVCodecID* convertTo)
{
	*convertTo = AV_CODEC_ID_NONE;
	bool text = is_text_subtitle(codec);

	switch (kind) {
		case kContainerMKV:
			if (codec == AV_CODEC_ID_MOV_TEXT || codec == AV_CODEC_ID_TEXT
				|| codec == AV_CODEC_ID_EIA_608) {
				*convertTo = AV_CODEC_ID_SUBRIP;
				return codec != AV_CODEC_ID_EIA_608;
			}
			return true;
		case kContainerMP4:
		case kContainerMOV:
			if (codec == AV_CODEC_ID_MOV_TEXT
				|| codec == AV_CODEC_ID_DVD_SUBTITLE)
				return true;
			if (text) {
				*convertTo = AV_CODEC_ID_MOV_TEXT;
				return true;
			}
			return false;
		case kContainerWebM:
			if (codec == AV_CODEC_ID_WEBVTT)
				return true;
			if (text) {
				*convertTo = AV_CODEC_ID_WEBVTT;
				return true;
			}
			return false;
		default:
			return false;
	}
}


video_codec
default_video_codec(container_kind kind)
{
	switch (kind) {
		case kContainerWebM:
			return kVideoVP9;
		case kContainerMP4:
		case kContainerMKV:
		case kContainerMOV:
			return kVideoH264;
		default:
			return kVideoNone;
	}
}


audio_codec
default_audio_codec(container_kind kind)
{
	switch (kind) {
		case kContainerWebM:
		case kContainerOpus:
			return kAudioOpus;
		case kContainerOgg:
			return kAudioVorbis;
		case kContainerMP3:
			return kAudioMP3;
		case kContainerFLAC:
			return kAudioFLAC16;
		case kContainerWAV:
			return kAudioPCM16;
		default:
			return kAudioAAC;
	}
}


bool
same_family(video_codec a, video_codec b)
{
	return strcmp(video_codec_info(a).family, video_codec_info(b).family)
		== 0;
}
