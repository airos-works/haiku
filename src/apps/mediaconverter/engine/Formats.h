/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef FORMATS_H
#define FORMATS_H


#include <SupportDefs.h>

extern "C" {
#include <libavcodec/codec_id.h>
}


/*!	What MediaConverter can write: the containers, and the video and audio
	formats HandBrake offers, plus audio-only files (the old MediaConverter
	converted music too). The tables in Formats.cpp say which format goes in
	which container and which encoder makes it. */


enum container_kind {
	kContainerMP4 = 0,
	kContainerMKV,
	kContainerWebM,
	kContainerMOV,
	// audio only
	kContainerM4A,
	kContainerMP3,
	kContainerFLAC,
	kContainerOgg,
	kContainerOpus,
	kContainerWAV,

	kContainerCount
};


enum video_codec {
	kVideoH264 = 0,
	kVideoH264_10,
	kVideoH265,
	kVideoH265_10,
	kVideoH265_12,
	kVideoAV1,
	kVideoAV1_10,
	kVideoVP9,
	kVideoVP9_10,
	kVideoVP8,
	kVideoMPEG4,
	kVideoMPEG2,
	kVideoTheora,
	kVideoFFV1,
	kVideoCopy,		// the source's video, unchanged
	kVideoNone,		// no video in the output

	kVideoCodecCount
};


enum audio_codec {
	kAudioAAC = 0,
	kAudioHEAAC,
	kAudioMP3,
	kAudioMP2,
	kAudioAC3,
	kAudioEAC3,
	kAudioTrueHD,
	kAudioFLAC16,
	kAudioFLAC24,
	kAudioALAC16,
	kAudioALAC24,
	kAudioOpus,
	kAudioVorbis,
	kAudioPCM16,
	kAudioPCM24,
	kAudioCopy,		// the source's audio, unchanged where the container allows
	kAudioNone,

	kAudioCodecCount
};


enum audio_mixdown {
	kMixdownSource = 0,
	kMixdownMono,
	kMixdownStereo,
	kMixdown51,
	kMixdown71,

	kMixdownCount
};


enum subtitle_mode {
	kSubtitlesNone = 0,
	kSubtitlesCopy,			// every track that the container can hold
	kSubtitlesBurnForced,	// burn in the first forced (or default) track
	kSubtitlesBurnFirst,	// burn in the first track

	kSubtitleModeCount
};


enum encoder_backend {
	kBackendSoftware = 0,
	kBackendVAAPI,			// AMD (and other VA-API drivers)
	kBackendNVENC,			// NVIDIA

	kBackendCount
};


// The software encoder library behind a video format.
enum software_encoder {
	kSoftwareNone = 0,
	kSoftwareX264,
	kSoftwareX265,
	kSoftwareSvtAv1,
	kSoftwareLibavcodec,	// one of FFmpeg's own, or one it links
};


struct ContainerInfo {
	container_kind	kind;
	const char*		name;			// shown
	const char*		muxer;			// libavformat's name
	const char*		extension;
	const char*		mimeType;
	bool			video;			// can hold video
};


struct VideoCodecInfo {
	video_codec		codec;
	const char*		name;			// shown: "H.265 10-bit"
	const char*		family;			// "H.265": what a GPU table lists
	AVCodecID		avCodec;
	int				bitDepth;
	software_encoder software;
	const char*		lavcEncoder;	// for kSoftwareLibavcodec
};


struct AudioCodecInfo {
	audio_codec		codec;
	const char*		name;
	AVCodecID		avCodec;
	const char*		lavcEncoder;
	bool			lossless;
	int				defaultBitrate;	// kbit/s per stereo pair, 0 for lossless
	int				maxChannels;
};


const ContainerInfo&	container_info(container_kind kind);
const VideoCodecInfo&	video_codec_info(video_codec codec);
const AudioCodecInfo&	audio_codec_info(audio_codec codec);
const char*				mixdown_name(audio_mixdown mixdown);
const char*				subtitle_mode_name(subtitle_mode mode);
const char*				backend_name(encoder_backend backend);

bool					container_holds_video(container_kind kind,
							video_codec codec);
bool					container_holds_audio(container_kind kind,
							audio_codec codec);
// Whether a source stream can be copied as it is.
bool					container_takes_stream(container_kind kind,
							AVCodecID codec, bool video);
// Whether the container can carry an album picture of this kind: MP3 and
// FLAC as tag pictures, M4A as its "covr" atom.
bool					container_takes_cover(container_kind kind,
							AVCodecID codec);
bool					container_takes_subtitle(container_kind kind,
							AVCodecID codec, AVCodecID* convertTo);

// The video format a container would pick if the current one cannot go in.
video_codec				default_video_codec(container_kind kind);
audio_codec				default_audio_codec(container_kind kind);

// Which codec family a GPU table entry means: H.264, H.265, AV1, VP9 ...
bool					same_family(video_codec a, video_codec b);


#endif	// FORMATS_H
