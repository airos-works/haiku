/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "CedarVideoEncoder.h"

#include <Autolock.h>
#include <Locker.h>

#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sunxi_ve.h>

#include "CedarEncoder.h"


enum test_state {
	kUntested = 0,
	kWorks,
	kFails
};

static BLocker sTestLock("cedar encoder self-test");
// H.264 and H.265
static test_state sTestState[2] = { kUntested, kUntested };
static BString sTestReason[2];


static int
test_index(video_codec codec)
{
	return codec == kVideoH265 ? 1 : 0;
}


// #pragma mark - self-test


static const uint32 kTestWidth = 352;
static const uint32 kTestHeight = 288;
static const int kTestPictures = 6;


/*!	A ramp with a textured square moving across it (as NVENC's test):
	easy to encode well, so a working encoder comes back above 40 dB, and a
	mix-up of lines, planes or pictures shows at once. */
static void
make_test_picture(int index, std::vector<uint8>& picture)
{
	picture.assign(kTestWidth * kTestHeight * 3 / 2, 0);
	uint8* luma = picture.data();
	uint8* chroma = luma + kTestWidth * kTestHeight;
	int squareX = 40 + 12 * index;
	int squareY = 60 + 6 * index;
	for (uint32 y = 0; y < kTestHeight; y++) {
		for (uint32 x = 0; x < kTestWidth; x++) {
			int value = 32 + x * 160 / kTestWidth + y * 40 / kTestHeight;
			if ((int)x >= squareX && (int)x < squareX + 64
				&& (int)y >= squareY && (int)y < squareY + 64)
				value = 220 - (((int)x - squareX) + ((int)y - squareY)) / 2;
			luma[y * kTestWidth + x] = (uint8)value;
		}
	}
	for (uint32 y = 0; y < kTestHeight / 2; y++) {
		for (uint32 x = 0; x < kTestWidth / 2; x++) {
			int u = 128 + ((int)x - (int)kTestWidth / 4) / 4;
			int v = 128 + ((int)y - (int)kTestHeight / 4) / 2;
			if (2 * (int)x >= squareX && 2 * (int)x < squareX + 64
				&& 2 * (int)y >= squareY && 2 * (int)y < squareY + 64) {
				u = 90;
				v = 170;
			}
			chroma[(y * (kTestWidth / 2) + x) * 2] = (uint8)u;
			chroma[(y * (kTestWidth / 2) + x) * 2 + 1] = (uint8)v;
		}
	}
}


static bool
run_test(video_codec codec, BString& why)
{
	bool hevc = codec == kVideoH265;
	const AVCodec* decoder = avcodec_find_decoder(hevc ? AV_CODEC_ID_HEVC
		: AV_CODEC_ID_H264);
	AVCodecContext* context = decoder != NULL
		? avcodec_alloc_context3(decoder) : NULL;
	if (context == NULL || avcodec_open2(context, decoder, NULL) < 0) {
		avcodec_free_context(&context);
		why = "There is no decoder to check the encoder's output with.";
		return false;
	}

	CedarEncoder encoder;
	CedarEncoder::Params params;
	params.codec = hevc ? CedarEncoder::kHEVC : CedarEncoder::kH264;
	params.width = kTestWidth;
	params.height = kTestHeight;
	params.qp = 22;
	params.keyInterval = 30;
	if (encoder.Open(params) != B_OK) {
		avcodec_free_context(&context);
		why = encoder.Error();
		return false;
	}

	std::vector<std::vector<uint8> > pictures(kTestPictures);
	AVPacket* packet = av_packet_alloc();
	AVFrame* frame = av_frame_alloc();
	double squared = 0;
	int64 samples = 0;
	int decoded = 0;
	bool failed = false;

	auto takeFrames = [&]() {
		while (avcodec_receive_frame(context, frame) == 0) {
			if (decoded < kTestPictures && frame->width == (int)kTestWidth
				&& frame->height == (int)kTestHeight) {
				const uint8* reference = pictures[decoded].data();
				for (uint32 y = 0; y < kTestHeight; y++) {
					const uint8* line = frame->data[0]
						+ (size_t)y * frame->linesize[0];
					for (uint32 x = 0; x < kTestWidth; x++) {
						double difference = (int)line[x]
							- (int)reference[y * kTestWidth + x];
						squared += difference * difference;
					}
				}
				samples += kTestWidth * kTestHeight;
			}
			decoded++;
			av_frame_unref(frame);
		}
	};

	for (int i = 0; i < kTestPictures && !failed; i++) {
		make_test_picture(i, pictures[i]);
		const uint8* data;
		size_t size;
		bool key;
		if (encoder.Encode(pictures[i].data(), kTestWidth,
				pictures[i].data() + kTestWidth * kTestHeight, kTestWidth,
				false, &data, &size, &key) != B_OK) {
			why = encoder.Error();
			failed = true;
			break;
		}
		if (av_new_packet(packet, (int)size) == 0) {
			memcpy(packet->data, data, size);
			packet->pts = i;
			packet->dts = i;
			avcodec_send_packet(context, packet);
			av_packet_unref(packet);
		}
		takeFrames();
	}
	if (!failed) {
		avcodec_send_packet(context, NULL);
		takeFrames();
	}
	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&context);
	if (failed)
		return false;

	if (decoded < kTestPictures || samples == 0) {
		why.SetToFormat("Only %d of %d test pictures could be decoded.",
			decoded, kTestPictures);
		return false;
	}
	double meanSquared = squared / samples;
	double psnr = meanSquared > 0 ? 10 * log10(255.0 * 255.0 / meanSquared)
		: 99.0;
	if (psnr < 32) {
		why.SetToFormat("The test pictures came back wrong (PSNR %.1f dB).",
			psnr);
		return false;
	}
	return true;
}


bool
CedarVideoEncoder::SelfTest(video_codec codec, BString* why)
{
	BAutolock _(sTestLock);
	int index = test_index(codec);
	if (sTestState[index] == kUntested) {
		BString reason;
		if (!IsAvailable()) {
			sTestState[index] = kFails;
			sTestReason[index] = "There is no A733 video encoder.";
		} else if (run_test(codec, reason))
			sTestState[index] = kWorks;
		else {
			sTestState[index] = kFails;
			sTestReason[index].SetToFormat("The A733 video encoder failed "
				"MediaConverter's %s test: %s",
				codec == kVideoH265 ? "H.265" : "H.264", reason.String());
			fprintf(stderr, "%s\n", sTestReason[index].String());
		}
	}
	if (why != NULL)
		*why = sTestReason[index];
	return sTestState[index] == kWorks;
}


void
CedarVideoEncoder::SelfTestAll()
{
	SelfTest(kVideoH264);
	SelfTest(kVideoH265);
}


bool
CedarVideoEncoder::Tested(video_codec codec, bool* works, BString* why)
{
	BAutolock _(sTestLock);
	int index = test_index(codec);
	if (sTestState[index] == kUntested)
		return false;
	if (works != NULL)
		*works = sTestState[index] == kWorks;
	if (why != NULL)
		*why = sTestReason[index];
	return true;
}


// #pragma mark -


CedarVideoEncoder::CedarVideoEncoder(const char* deviceName)
	:
	fDeviceName(deviceName),
	fEncoder(NULL),
	fDraining(false)
{
}


CedarVideoEncoder::~CedarVideoEncoder()
{
	delete fEncoder;
	for (AVPacket* packet : fPackets)
		av_packet_free(&packet);
}


bool
CedarVideoEncoder::Implements(video_codec codec)
{
	if (codec != kVideoH264 && codec != kVideoH265)
		return false;
	bool works;
	// a failed test takes the format away
	return !Tested(codec, &works) || works;
}


bool
CedarVideoEncoder::IsAvailable()
{
	return CedarEncoder::IsAvailable();
}


status_t
CedarVideoEncoder::Open(const VideoEncoderParams& params, BString& error)
{
	if (params.codec != kVideoH264 && params.codec != kVideoH265) {
		error = "The A733's video encoder encodes 8 bit H.264 and H.265 "
			"only.";
		return B_NOT_SUPPORTED;
	}
	// nothing is encoded before the engine has shown it can
	if (!SelfTest(params.codec, &error))
		return B_NOT_SUPPORTED;

	CedarEncoder::Params config;
	config.codec = params.codec == kVideoH265 ? CedarEncoder::kHEVC
		: CedarEncoder::kH264;
	config.width = params.width;
	config.height = params.height;
	if (params.frameRate.num > 0 && params.frameRate.den > 0) {
		config.frameRateNum = params.frameRate.num;
		config.frameRateDen = params.frameRate.den;
	}
	double rate = (double)config.frameRateNum / config.frameRateDen;
	if (params.rateMode == kRateAverageBitrate) {
		config.bitrate = (uint32)params.bitrate * 1000;
		config.qp = 28;
	} else
		config.qp = quality_to_crf(params.quality, 16, 40);
	config.keyInterval = params.keyInterval > 0 ? params.keyInterval
		: (uint32)lround(rate * 2);
	if (params.sampleAspect.num > 0 && params.sampleAspect.den > 0) {
		config.sarNum = params.sampleAspect.num;
		config.sarDen = params.sampleAspect.den;
	}
	if (params.primaries != AVCOL_PRI_UNSPECIFIED)
		config.colourPrimaries = params.primaries;
	if (params.transfer != AVCOL_TRC_UNSPECIFIED)
		config.transfer = params.transfer;
	if (params.matrix != AVCOL_SPC_UNSPECIFIED)
		config.matrix = params.matrix;
	config.fullRange = params.range == AVCOL_RANGE_JPEG;

	fEncoder = new(std::nothrow) CedarEncoder;
	if (fEncoder == NULL)
		return B_NO_MEMORY;
	status_t status = fEncoder->Open(config);
	if (status != B_OK) {
		error = fEncoder->Error();
		delete fEncoder;
		fEncoder = NULL;
		return status;
	}
	size_t size;
	const uint8* headers = fEncoder->Headers(&size);
	fExtradata.assign(headers, headers + size);
	return B_OK;
}


AVPixelFormat
CedarVideoEncoder::InputFormat() const
{
	return AV_PIX_FMT_NV12;
}


status_t
CedarVideoEncoder::SendFrame(const AVFrame* frame)
{
	if (fEncoder == NULL)
		return B_NO_INIT;
	if (frame == NULL) {
		fDraining = true;
		return B_OK;
	}

	const uint8* data;
	size_t size;
	bool key;
	status_t status = fEncoder->Encode(frame->data[0], frame->linesize[0],
		frame->data[1], frame->linesize[1],
		frame->pict_type == AV_PICTURE_TYPE_I, &data, &size, &key);
	if (status != B_OK) {
		fprintf(stderr, "A733 video encoder: %s\n", fEncoder->Error());
		return status;
	}
	AVPacket* packet = av_packet_alloc();
	if (packet == NULL || av_new_packet(packet, (int)size) != 0) {
		av_packet_free(&packet);
		return B_NO_MEMORY;
	}
	memcpy(packet->data, data, size);
	// no B pictures: decoding order is the order shown
	packet->pts = frame->pts;
	packet->dts = frame->pts;
	if (key)
		packet->flags |= AV_PKT_FLAG_KEY;
	fPackets.push_back(packet);
	return B_OK;
}


status_t
CedarVideoEncoder::ReceivePacket(AVPacket* packet)
{
	if (fEncoder == NULL)
		return B_NO_INIT;
	if (fPackets.empty())
		return fDraining ? B_LAST_BUFFER_ERROR : B_WOULD_BLOCK;
	AVPacket* next = fPackets.front();
	fPackets.pop_front();
	av_packet_move_ref(packet, next);
	av_packet_free(&next);
	return B_OK;
}


encoder_backend
CedarVideoEncoder::Backend() const
{
	return kBackendCedar;
}


BString
CedarVideoEncoder::Name() const
{
	BString name;
	name.SetToFormat("%s (Cedar)", fDeviceName.String());
	return name;
}


BString
CedarVideoEncoder::Description() const
{
	return fEncoder != NULL ? BString(fEncoder->Description()) : BString();
}
