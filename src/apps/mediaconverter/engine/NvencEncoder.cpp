/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "NvencEncoder.h"

#include <Autolock.h>
#include <FindDirectory.h>
#include <Locker.h>
#include <Path.h>
#include <image.h>

#include <stdio.h>
#include <string.h>


static BLocker sLock("nvenc add-on");
static bool sTried;
static const nvenc_api* sApi;
static uint32 sCodecs;
static BString sReason;
static BString sName;


static const nvenc_api*
nvenc_api_get()
{
	BAutolock _(sLock);
	if (sTried)
		return sApi;
	sTried = true;

	const directory_which kDirectories[] = {
		B_USER_NONPACKAGED_ADDONS_DIRECTORY,
		B_SYSTEM_NONPACKAGED_ADDONS_DIRECTORY,
		B_SYSTEM_ADDONS_DIRECTORY,
	};
	image_id image = -1;
	for (size_t i = 0; i < sizeof(kDirectories) / sizeof(kDirectories[0]);
			i++) {
		BPath path;
		if (find_directory(kDirectories[i], &path) != B_OK)
			continue;
		path.Append("media/encoders/nvenc");
		image = load_add_on(path.Path());
		if (image >= 0)
			break;
	}
	if (image < 0) {
		sReason = "The NVENC add-on is not installed.";
		return NULL;
	}

	nvenc_get_api_func getApi;
	if (get_image_symbol(image, NVENC_GET_API_SYMBOL, B_SYMBOL_TYPE_TEXT,
			(void**)&getApi) != B_OK) {
		unload_add_on(image);
		sReason = "The NVENC add-on is broken.";
		return NULL;
	}
	const nvenc_api* api = getApi();
	if (api == NULL || api->version != NVENC_API_VERSION) {
		unload_add_on(image);
		sReason = "The NVENC add-on is of another version.";
		return NULL;
	}

	char name[128] = "";
	char reason[256] = "";
	uint32_t codecs = 0;
	if (api->probe(&codecs, name, sizeof(name), reason, sizeof(reason)) != 0) {
		// keep the add-on loaded: it may say why
		sReason = reason[0] != '\0' ? reason : "NVENC did not start.";
		return NULL;
	}
	sCodecs = codecs;
	sName = name;
	sApi = api;
	return sApi;
}


NvencEncoder::NvencEncoder(const char* deviceName)
	:
	fDeviceName(deviceName),
	fApi(NULL),
	fSession(NULL),
	fBitDepth(8),
	fDrained(false)
{
}


NvencEncoder::~NvencEncoder()
{
	if (fApi != NULL && fSession != NULL)
		fApi->close(fSession);
}


static uint32
nvenc_codec(video_codec codec)
{
	switch (codec) {
		case kVideoH264:
			return NVENC_CODEC_H264;
		case kVideoH265:
			return NVENC_CODEC_HEVC;
		case kVideoH265_10:
			return NVENC_CODEC_HEVC_10;
		case kVideoAV1:
			return NVENC_CODEC_AV1;
		case kVideoAV1_10:
			return NVENC_CODEC_AV1_10;
		default:
			return 0;
	}
}


bool
NvencEncoder::Implements(video_codec codec)
{
	uint32 bit = nvenc_codec(codec);
	if (bit == 0)
		return false;
	// The add-on decides for the card; with no add-on nothing is implemented
	return nvenc_api_get() != NULL && (sCodecs & bit) != 0;
}


bool
NvencEncoder::IsAvailable(BString* why)
{
	bool available = nvenc_api_get() != NULL;
	if (!available && why != NULL)
		*why = sReason;
	return available;
}


uint32
NvencEncoder::Codecs()
{
	return nvenc_api_get() != NULL ? sCodecs : 0;
}


status_t
NvencEncoder::Open(const VideoEncoderParams& params, BString& error)
{
	fApi = nvenc_api_get();
	if (fApi == NULL) {
		error = sReason;
		return B_NOT_SUPPORTED;
	}
	if (!sName.IsEmpty())
		fDeviceName = sName;
	fBitDepth = video_codec_info(params.codec).bitDepth;

	nvenc_config config;
	memset(&config, 0, sizeof(config));
	config.codec = nvenc_codec(params.codec);
	config.width = params.width;
	config.height = params.height;
	config.frame_rate_num = params.frameRate.num;
	config.frame_rate_den = params.frameRate.den;
	if (params.sampleAspect.num > 0) {
		config.sar_num = params.sampleAspect.num;
		config.sar_den = params.sampleAspect.den;
	}
	bool av1 = params.codec == kVideoAV1 || params.codec == kVideoAV1_10;
	if (params.rateMode == kRateConstantQuality) {
		config.rate_control = NVENC_RC_CONSTQP;
		config.qp = av1 ? quality_to_crf(params.quality, 60, 180)
			: quality_to_crf(params.quality, 16, 36);
	} else {
		config.rate_control = NVENC_RC_VBR;
		config.bitrate = (uint32)params.bitrate * 1000;
		config.max_bitrate = config.bitrate / 2 * 3;
	}
	config.key_interval = params.keyInterval;
	static const uint32 kPresets[kSpeedCount] = { 1, 2, 4, 6, 7 };
	config.preset = kPresets[params.speed];
	config.colour_primaries = params.primaries;
	config.transfer = params.transfer;
	config.matrix = params.matrix;
	config.full_range = params.range == AVCOL_RANGE_JPEG;
	config.chroma_location = params.chromaLocation > 0
		? params.chromaLocation - 1 : 0;

	char reason[256] = "";
	fSession = fApi->open(&config, reason, sizeof(reason));
	if (fSession == NULL) {
		error = reason[0] != '\0' ? reason : "NVENC did not start.";
		return B_ERROR;
	}

	uint8 headers[1024];
	size_t size = sizeof(headers);
	if (fApi->headers(fSession, headers, &size) == 0 && size > 0)
		fExtradata.assign(headers, headers + size);
	return B_OK;
}


AVPixelFormat
NvencEncoder::InputFormat() const
{
	return fBitDepth > 8 ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
}


status_t
NvencEncoder::SendFrame(const AVFrame* frame)
{
	if (frame == NULL)
		return fApi->encode(fSession, NULL) == 0 ? B_OK : B_ERROR;

	nvenc_picture picture;
	memset(&picture, 0, sizeof(picture));
	picture.planes[0] = frame->data[0];
	picture.planes[1] = frame->data[1];
	picture.strides[0] = frame->linesize[0];
	picture.strides[1] = frame->linesize[1];
	picture.pts = frame->pts;
	picture.force_key = frame->pict_type == AV_PICTURE_TYPE_I;
	return fApi->encode(fSession, &picture) == 0 ? B_OK : B_ERROR;
}


status_t
NvencEncoder::ReceivePacket(AVPacket* packet)
{
	if (fDrained)
		return B_LAST_BUFFER_ERROR;
	nvenc_packet result;
	memset(&result, 0, sizeof(result));
	int status = fApi->receive(fSession, &result);
	if (status == 1)
		return B_WOULD_BLOCK;
	if (status == 2) {
		fDrained = true;
		return B_LAST_BUFFER_ERROR;
	}
	if (status != 0)
		return B_ERROR;

	status_t error = B_OK;
	if (av_new_packet(packet, (int)result.size) == 0) {
		memcpy(packet->data, result.data, result.size);
		packet->pts = result.pts;
		packet->dts = result.dts;
		if (result.key)
			packet->flags |= AV_PKT_FLAG_KEY;
	} else
		error = B_NO_MEMORY;
	fApi->release(fSession, &result);
	return error;
}


encoder_backend
NvencEncoder::Backend() const
{
	return kBackendNVENC;
}


BString
NvencEncoder::Name() const
{
	BString name;
	name.SetToFormat("%s (NVENC)", fDeviceName.String());
	return name;
}


BString
NvencEncoder::Description() const
{
	if (fApi == NULL || fSession == NULL)
		return BString();
	return BString(fApi->describe(fSession));
}
