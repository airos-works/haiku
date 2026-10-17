/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "LavcVideoEncoder.h"

#include <stdio.h>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}


LavcVideoEncoder::LavcVideoEncoder(AVPixelFormat sourceFormat)
	:
	fSourceFormat(sourceFormat),
	fContext(NULL),
	fFormat(AV_PIX_FMT_YUV420P)
{
}


LavcVideoEncoder::~LavcVideoEncoder()
{
	avcodec_free_context(&fContext);
}


bool
LavcVideoEncoder::IsAvailable(video_codec codec)
{
	const VideoCodecInfo& info = video_codec_info(codec);
	if (info.software != kSoftwareLibavcodec || info.lavcEncoder == NULL)
		return false;
	const AVCodec* encoder = avcodec_find_encoder_by_name(info.lavcEncoder);
	if (encoder == NULL)
		return false;
	if (info.bitDepth <= 8 || encoder->pix_fmts == NULL)
		return true;
	for (const AVPixelFormat* format = encoder->pix_fmts;
			*format != AV_PIX_FMT_NONE; format++) {
		const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(*format);
		if (descriptor != NULL && descriptor->comp[0].depth == info.bitDepth)
			return true;
	}
	return false;
}


AVPixelFormat
LavcVideoEncoder::_ChooseFormat(const AVCodec* codec, int bitDepth) const
{
	if (codec->pix_fmts == NULL)
		return AV_PIX_FMT_YUV420P;

	// A lossless encoder keeps the source's pixels when it can.
	if (codec->id == AV_CODEC_ID_FFV1 && fSourceFormat != AV_PIX_FMT_NONE) {
		for (const AVPixelFormat* format = codec->pix_fmts;
				*format != AV_PIX_FMT_NONE; format++) {
			if (*format == fSourceFormat)
				return *format;
		}
	}

	AVPixelFormat wanted = bitDepth > 8 ? AV_PIX_FMT_YUV420P10LE
		: AV_PIX_FMT_YUV420P;
	for (const AVPixelFormat* format = codec->pix_fmts;
			*format != AV_PIX_FMT_NONE; format++) {
		if (*format == wanted)
			return *format;
	}
	return codec->pix_fmts[0];
}


status_t
LavcVideoEncoder::Open(const VideoEncoderParams& params, BString& error)
{
	const VideoCodecInfo& info = video_codec_info(params.codec);
	const AVCodec* codec = info.lavcEncoder != NULL
		? avcodec_find_encoder_by_name(info.lavcEncoder) : NULL;
	if (codec == NULL) {
		error.SetToFormat("FFmpeg has no %s encoder here.", info.name);
		return B_NOT_SUPPORTED;
	}
	fName = codec->long_name != NULL ? codec->long_name : codec->name;

	fContext = avcodec_alloc_context3(codec);
	if (fContext == NULL)
		return B_NO_MEMORY;

	fFormat = _ChooseFormat(codec, info.bitDepth);
	fContext->pix_fmt = fFormat;
	fContext->width = params.width;
	fContext->height = params.height;
	fContext->time_base = params.timeBase;
	fContext->framerate = params.frameRate;
	fContext->sample_aspect_ratio = params.sampleAspect;
	fContext->gop_size = params.keyInterval;
	fContext->thread_count = encoder_thread_count();
	fContext->color_primaries = params.primaries;
	fContext->color_trc = params.transfer;
	fContext->colorspace = params.matrix;
	fContext->color_range = params.range;
	fContext->chroma_sample_location = params.chromaLocation;
	if (params.globalHeader)
		fContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	bool quality = params.rateMode == kRateConstantQuality;
	if (!quality)
		fContext->bit_rate = (int64)params.bitrate * 1000;

	AVDictionary* options = NULL;
	char value[32];
	switch (codec->id) {
		case AV_CODEC_ID_VP9:
		case AV_CODEC_ID_VP8:
		{
			// cpu-used: 0 is the slowest; "realtime" goes much faster
			static const int kCpuUsed[kSpeedCount] = { 8, 5, 3, 2, 1 };
			bool vp9 = codec->id == AV_CODEC_ID_VP9;
			av_dict_set(&options, "deadline",
				params.speed == kSpeedFastest ? "realtime" : "good", 0);
			snprintf(value, sizeof(value), "%d",
				vp9 ? kCpuUsed[params.speed] : min_c(kCpuUsed[params.speed],
					5));
			av_dict_set(&options, "cpu-used", value, 0);
			if (quality) {
				int crf = quality_to_crf(params.quality, 15, 50);
				snprintf(value, sizeof(value), "%d", crf);
				av_dict_set(&options, "crf", value, 0);
				if (vp9)
					fContext->bit_rate = 0;
				else
					fContext->bit_rate = 20 * 1000 * 1000;
				fDescription.SetToFormat("CRF %d", crf);
			} else
				fDescription.SetToFormat("%" B_PRId32 " kbit/s",
					params.bitrate);
			if (vp9) {
				av_dict_set(&options, "row-mt", "1", 0);
				int tiles = params.width >= 3840 ? 3
					: params.width >= 1920 ? 2 : params.width >= 960 ? 1 : 0;
				snprintf(value, sizeof(value), "%d", tiles);
				av_dict_set(&options, "tile-columns", value, 0);
			}
			av_dict_set(&options, "auto-alt-ref", "1", 0);
			av_dict_set(&options, "lag-in-frames", "25", 0);
			break;
		}

		case AV_CODEC_ID_MPEG4:
		case AV_CODEC_ID_MPEG2VIDEO:
		{
			fContext->max_b_frames = 2;
			if (codec->id == AV_CODEC_ID_MPEG2VIDEO
				&& fContext->gop_size > 18)
				fContext->gop_size = 15;
			if (quality) {
				int q = quality_to_crf(params.quality, 2, 20);
				fContext->flags |= AV_CODEC_FLAG_QSCALE;
				fContext->global_quality = FF_QP2LAMBDA * q;
				fDescription.SetToFormat("quantizer %d", q);
			} else {
				fContext->rc_max_rate = fContext->bit_rate * 2;
				fContext->rc_buffer_size = (int)min_c(fContext->bit_rate * 2,
					(int64)INT32_MAX);
				fDescription.SetToFormat("%" B_PRId32 " kbit/s",
					params.bitrate);
			}
			if (codec->id == AV_CODEC_ID_MPEG4)
				fContext->codec_tag = 0;
			break;
		}

		case AV_CODEC_ID_THEORA:
		{
			if (quality) {
				int q = (params.quality + 5) / 10;
				fContext->flags |= AV_CODEC_FLAG_QSCALE;
				fContext->global_quality = FF_QP2LAMBDA * q;
				fDescription.SetToFormat("quality %d of 10", q);
			} else
				fDescription.SetToFormat("%" B_PRId32 " kbit/s",
					params.bitrate);
			break;
		}

		case AV_CODEC_ID_FFV1:
			fContext->gop_size = 1;
			fContext->bit_rate = 0;
			av_dict_set(&options, "level", "3", 0);
			av_dict_set(&options, "slicecrc", "1", 0);
			av_dict_set(&options, "slices", "16", 0);
			fDescription = "lossless, version 3";
			break;

		default:
			break;
	}

	int result = avcodec_open2(fContext, codec, &options);
	av_dict_free(&options);
	if (result < 0) {
		char message[AV_ERROR_MAX_STRING_SIZE];
		av_strerror(result, message, sizeof(message));
		error.SetToFormat("The %s encoder did not start: %s", info.name,
			message);
		return B_ERROR;
	}

	if (fContext->extradata != NULL && fContext->extradata_size > 0) {
		fExtradata.assign(fContext->extradata,
			fContext->extradata + fContext->extradata_size);
	}
	return B_OK;
}


AVPixelFormat
LavcVideoEncoder::InputFormat() const
{
	return fFormat;
}


status_t
LavcVideoEncoder::SendFrame(const AVFrame* frame)
{
	int result = avcodec_send_frame(fContext, frame);
	if (result == AVERROR_EOF)
		return B_OK;
	return result < 0 ? B_ERROR : B_OK;
}


status_t
LavcVideoEncoder::ReceivePacket(AVPacket* packet)
{
	int result = avcodec_receive_packet(fContext, packet);
	if (result == AVERROR(EAGAIN))
		return B_WOULD_BLOCK;
	if (result == AVERROR_EOF)
		return B_LAST_BUFFER_ERROR;
	return result < 0 ? B_ERROR : B_OK;
}


encoder_backend
LavcVideoEncoder::Backend() const
{
	return kBackendSoftware;
}


BString
LavcVideoEncoder::Name() const
{
	return fName;
}


BString
LavcVideoEncoder::Description() const
{
	return fDescription;
}
