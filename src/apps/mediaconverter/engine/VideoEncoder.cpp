/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "VideoEncoder.h"

#include <OS.h>

#include <math.h>
#include <string.h>


VideoEncoderParams::VideoEncoderParams()
	:
	codec(kVideoH264),
	width(0),
	height(0),
	timeBase((AVRational){1, 90000}),
	frameRate((AVRational){25, 1}),
	constantFrameRate(false),
	sampleAspect((AVRational){1, 1}),
	rateMode(kRateConstantQuality),
	quality(60),
	bitrate(6000),
	speed(kSpeedBalanced),
	keyInterval(250),
	globalHeader(true),
	primaries(AVCOL_PRI_UNSPECIFIED),
	transfer(AVCOL_TRC_UNSPECIFIED),
	matrix(AVCOL_SPC_UNSPECIFIED),
	range(AVCOL_RANGE_MPEG),
	chromaLocation(AVCHROMA_LOC_LEFT),
	haveMastering(false),
	haveLightLevel(false)
{
	memset(&mastering, 0, sizeof(mastering));
	memset(&lightLevel, 0, sizeof(lightLevel));
}


VideoEncoder::~VideoEncoder()
{
}


const std::vector<uint8>&
VideoEncoder::Extradata() const
{
	return fExtradata;
}


BString
VideoEncoder::Description() const
{
	return BString();
}


int
quality_to_crf(int32 quality, int best, int worst)
{
	if (quality < 0)
		quality = 0;
	if (quality > 100)
		quality = 100;
	return (int)lround(worst - (worst - best) * quality / 100.0);
}


int
encoder_thread_count()
{
	system_info info;
	get_system_info(&info);
	int count = (int)info.cpu_count;
	return count > 64 ? 64 : count;
}
