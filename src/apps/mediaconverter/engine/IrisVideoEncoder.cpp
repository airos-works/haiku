/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "IrisVideoEncoder.h"

#include <new>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <qcom_iris.h>

#include "IrisEncoder.h"


static const bigtime_t kTimeout = 2000000;


IrisVideoEncoder::IrisVideoEncoder(const char* deviceName)
	:
	fDeviceName(deviceName),
	fEncoder(NULL),
	fDraining(false),
	fDrained(false)
{
}


IrisVideoEncoder::~IrisVideoEncoder()
{
	delete fEncoder;
	for (AVPacket* packet : fPackets)
		av_packet_free(&packet);
}


bool
IrisVideoEncoder::Implements(video_codec codec)
{
	return codec == kVideoH264 || codec == kVideoH265;
}


bool
IrisVideoEncoder::IsAvailable()
{
	return access(QCOM_IRIS_DEVICE, F_OK) == 0;
}


status_t
IrisVideoEncoder::Open(const VideoEncoderParams& params, BString& error)
{
	if (!Implements(params.codec)) {
		error = "The Iris codec encodes H.264 and H.265 (eight bits) only.";
		return B_NOT_SUPPORTED;
	}

	IrisEncoder::Params config;
	config.codec = params.codec == kVideoH265 ? IrisEncoder::HEVC
		: IrisEncoder::H264;
	config.width = params.width;
	config.height = params.height;
	uint32 rate = 30;
	if (params.frameRate.num > 0 && params.frameRate.den > 0) {
		rate = (uint32)((params.frameRate.num + params.frameRate.den / 2)
			/ params.frameRate.den);
	}
	config.framesPerSecond = rate > 0 ? rate : 1;
	if (params.rateMode == kRateAverageBitrate)
		config.bitrate = (uint32)params.bitrate * 1000;
	else {
		// no constant quality mode on this firmware: a bitrate for the
		// quality asked, 0.03 to 0.2 bits a pixel (H.265 needs less)
		double bitsPerPixel = 0.03 + 0.17 * params.quality / 100.0;
		if (config.codec == IrisEncoder::HEVC)
			bitsPerPixel *= 0.65;
		config.bitrate = (uint32)(bitsPerPixel * params.width * params.height
			* config.framesPerSecond);
	}
	config.keyInterval = params.keyInterval > 0 ? params.keyInterval
		: config.framesPerSecond * 2;

	// The container wants the parameter sets before the first picture: a
	// session of the same settings encodes a picture of black for them.
	{
		IrisEncoder probe;
		BString reason;
		status_t status = probe.Open(config, &reason);
		if (status != B_OK) {
			error = reason;
			return status;
		}
		size_t lumaSize = (size_t)params.width * params.height;
		uint8* black = (uint8*)malloc(lumaSize * 3 / 2);
		if (black == NULL)
			return B_NO_MEMORY;
		memset(black, 16, lumaSize);
		memset(black + lumaSize, 128, lumaSize / 2);
		status = probe.PutPicture(black, params.width, black + lumaSize,
			params.width, 0, kTimeout);
		free(black);
		if (status == B_OK)
			status = probe.EndOfStream();
		while (status == B_OK && probe.ParameterSets().empty()) {
			IrisEncoder::Packet packet;
			status = probe.NextPacket(packet, kTimeout);
			if (status == B_OK)
				probe.ReleasePacket(packet);
		}
		if (probe.ParameterSets().empty()) {
			error.SetToFormat("The Iris codec gave no parameter sets (%s).",
				probe.Error());
			return B_ERROR;
		}
		fExtradata = probe.ParameterSets();
	}

	fEncoder = new(std::nothrow) IrisEncoder;
	if (fEncoder == NULL)
		return B_NO_MEMORY;
	BString reason;
	status_t status = fEncoder->Open(config, &reason);
	if (status != B_OK) {
		error = reason;
		delete fEncoder;
		fEncoder = NULL;
		return status;
	}
	fDescription.SetToFormat("%s, VBR %" B_PRIu32 " kbit/s, IDR every %"
		B_PRIu32 ", IPPP",
		config.codec == IrisEncoder::HEVC ? "HEVC Main" : "H.264 High",
		config.bitrate / 1000, config.keyInterval);
	return B_OK;
}


AVPixelFormat
IrisVideoEncoder::InputFormat() const
{
	return AV_PIX_FMT_NV12;
}


status_t
IrisVideoEncoder::_Take(bigtime_t timeout)
{
	IrisEncoder::Packet result;
	status_t status = fEncoder->NextPacket(result, timeout);
	if (status != B_OK)
		return status;
	AVPacket* packet = av_packet_alloc();
	if (packet == NULL || av_new_packet(packet, (int)result.size) != 0) {
		av_packet_free(&packet);
		fEncoder->ReleasePacket(result);
		return B_NO_MEMORY;
	}
	memcpy(packet->data, result.data, result.size);
	packet->pts = result.pts;
	packet->dts = result.pts;
	if (result.key)
		packet->flags |= AV_PKT_FLAG_KEY;
	fEncoder->ReleasePacket(result);
	fPackets.push_back(packet);
	return B_OK;
}


status_t
IrisVideoEncoder::SendFrame(const AVFrame* frame)
{
	if (fEncoder == NULL)
		return B_NO_INIT;
	if (frame == NULL) {
		fDraining = true;
		return fEncoder->EndOfStream();
	}

	if (frame->pict_type == AV_PICTURE_TYPE_I
		&& fEncoder->PicturesEncoded() > 0) {
		fEncoder->RequestKeyPicture();
	}
	while (true) {
		status_t status = fEncoder->PutPicture(frame->data[0],
			frame->linesize[0], frame->data[1], frame->linesize[1], frame->pts,
			kTimeout);
		if (status != B_WOULD_BLOCK)
			return status;
		// what is done first, to make room
		if (_Take(0) != B_OK)
			return B_ERROR;
	}
}


status_t
IrisVideoEncoder::ReceivePacket(AVPacket* packet)
{
	if (fEncoder == NULL)
		return B_NO_INIT;
	if (fPackets.empty() && !fDrained) {
		status_t status = _Take(fDraining ? kTimeout : 0);
		if (status == B_LAST_BUFFER_ERROR
			|| (fDraining && status == B_WOULD_BLOCK)) {
			fDrained = true;
		} else if (status != B_OK && status != B_WOULD_BLOCK)
			return status;
	}
	if (fPackets.empty())
		return fDrained ? B_LAST_BUFFER_ERROR : B_WOULD_BLOCK;

	AVPacket* next = fPackets.front();
	fPackets.pop_front();
	av_packet_move_ref(packet, next);
	av_packet_free(&next);
	return B_OK;
}


encoder_backend
IrisVideoEncoder::Backend() const
{
	return kBackendIris;
}


BString
IrisVideoEncoder::Name() const
{
	BString name;
	name.SetToFormat("%s (Iris)", fDeviceName.String());
	return name;
}


BString
IrisVideoEncoder::Description() const
{
	return fDescription;
}
