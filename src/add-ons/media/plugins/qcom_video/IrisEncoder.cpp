/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "IrisEncoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*	The properties are venus' for SM8250 (venc.c) with HFI's own codes,
	which iris' generation 1 encoder does not always send; the buffer sizes
	are iris_vpu_buffer.c's (the same as venus' hfi_plat_bufs_v6.c). */

#define HFI_PROPERTY_PARAM_UNCOMPRESSED_PLANE_ACTUAL_INFO 0x1002
#define HFI_PROPERTY_CONFIG_FRAME_RATE		0x2001
#define HFI_PROPERTY_PARAM_VENC_H264_ENTROPY_CONTROL 0x2005002
#define HFI_PROPERTY_PARAM_VENC_H264_DEBLOCK_CONTROL 0x2005003
#define HFI_PROPERTY_PARAM_VENC_RATE_CONTROL 0x2005004
#define HFI_PROPERTY_PARAM_VENC_SESSION_QP_RANGE_V2 0x2005009
#define HFI_PROPERTY_PARAM_VENC_H264_VUI_TIMING_INFO 0x200501e
#define HFI_PROPERTY_PARAM_VENC_H264_TRANSFORM_8X8 0x2005025
#define HFI_PROPERTY_CONFIG_VENC_TARGET_BITRATE 0x2006001
#define HFI_PROPERTY_CONFIG_VENC_IDR_PERIOD	0x2006002
#define HFI_PROPERTY_CONFIG_VENC_INTRA_PERIOD 0x2006003
#define HFI_PROPERTY_CONFIG_VENC_SYNC_FRAME_SEQUENCE_HEADER 0x2006008

#define HFI_RATE_CONTROL_OFF				0x1000001
#define HFI_RATE_CONTROL_VBR_CFR			0x1000003
#define HFI_RATE_CONTROL_CBR_CFR			0x1000005

#define HFI_H264_PROFILE_HIGH				0x04
#define HFI_H264_LEVEL_31					0x00200
#define HFI_H264_LEVEL_41					0x01000
#define HFI_H264_LEVEL_42					0x02000
#define HFI_H264_LEVEL_51					0x08000
#define HFI_H264_LEVEL_52					0x10000
#define HFI_HEVC_PROFILE_MAIN				0x1
#define HFI_HEVC_LEVEL_41					0x040
#define HFI_HEVC_LEVEL_51					0x100
#define HFI_HEVC_LEVEL_52					0x200

#define HFI_H264_ENTROPY_CABAC				0x2
#define HFI_H264_CABAC_MODEL_0				0x1
#define HFI_H264_DB_MODE_ALL_BOUNDARY		0x3

#define ALIGN(x, a)		IRIS_ALIGN(x, a)
#define TRACE			IRIS_TRACE

static const uint32 kInputBuffers = 4;
static const uint32 kOutputBuffers = 6;
static const uint32 kMaxBitrate = 160000000;	// with CABAC
// the encoder's output tags: apart from what the decoder uses, to tell them
// apart in a trace
static const uint32 kOutputTag = 0x100;


// #pragma mark - buffer sizes (iris_vpu_buffer.c, iris_buffer.c)


static const uint32 kVppPipes = 4;
static const uint32 kWorkMode2 = 2;
static const uint32 kMaxColFrame = 6;
static const uint32 kMbs720p = (1280 / 16) * (736 / 16);
static const uint32 kMbs4k = ((4096 + 15) / 16) * ((2304 + 15) / 16);
static const uint32 kPersistSize = 204800;


static inline uint32
max_u32(uint32 a, uint32 b)
{
	return a > b ? a : b;
}


//!	The raw pictures: luma rows, then rows of chroma pairs, 128 byte stride.
static uint32
enc_nv12_size(uint32 width, uint32 height)
{
	uint32 stride = ALIGN(width, 128);
	return ALIGN(stride * ALIGN(height, 32)
		+ stride * ALIGN((height + 1) >> 1, 16), 4096);
}


//!	What the firmware takes a coded picture to need at most.
static uint32
enc_frame_size(uint32 width, uint32 height, uint32 rateControl)
{
	uint32 mbs = (ALIGN(ALIGN(height, 32), 16) * ALIGN(ALIGN(width, 32), 16))
		/ 256;
	uint32 size = width * height * 3;
	if (mbs < kMbs720p)
		size <<= 1;
	else if (mbs <= kMbs4k)
		size >>= 2;
	else
		size >>= 3;
	if (rateControl == HFI_RATE_CONTROL_OFF)
		size <<= 1;
	size = size * 5 / 4;
	return ALIGN(size, 4096);
}


static uint32
enc_scratch_size(uint32 width, uint32 height, uint32 rateControl)
{
	uint32 bitstream = ALIGN(enc_frame_size(width, height, rateControl), 256);
	// work mode 2
	uint32 buffers = 3;
	uint32 bitbin = ALIGN(bitstream * 17 / 10, 256);
	uint32 single = ALIGN(bitbin / 2, 256);
	uint32 sao = (64 * (((width + 32) * (height + 32)) >> 10)) + 384;
	single = ALIGN(sao + ALIGN(single, 256), 256);
	return ALIGN(single * kVppPipes, 256) * buffers + 512;
}


static uint32
enc_scratch1_size(uint32 width, uint32 height, uint32 lcu, uint32 references,
	bool hevc)
{
	const uint32 p = kVppPipes;
	const uint32 pipeAlign = 256u << (p - 1);
	uint32 wl = (width + lcu - 1) / lcu, hl = (height + lcu - 1) / lcu;
	uint32 nl = wl * hl;
	uint32 wc = wl * lcu, hc = hl * lcu;
	uint32 lcuMbs = (hc / lcu) * ((wc + lcu * 8) / lcu);
	uint32 sliceInfo = ALIGN(256 + (nl << 4), 256);
	uint32 lineControl = ALIGN(wc, 256), lineControlId2 = ALIGN(wc, 256);
	uint32 lineData = ALIGN(8 * wc + 1024, 256)
		+ ALIGN((8 * wc + 1024) >> 1, 256) * 2;
	uint32 leftControl = hevc ? ((hc + 32) / 32 * 4 * 16)
		: ((hc + 15) / 16 * 5 * 16);
	if (p > 1)
		leftControl = ALIGN(leftControl + 512, 512) * p;
	leftControl = ALIGN(leftControl, 256);
	uint32 leftRecon = ALIGN(2 * hc + 256, pipeAlign);
	uint32 topFe = ALIGN(hevc ? (64 * (wc >> 5)) : (256 + 16 * (wc >> 4)),
		256);
	uint32 leftFe = ALIGN(256 + 64 * (hc >> 4), pipeAlign) * p;
	uint32 leftMetaY = ALIGN(256 + 64 * (hc / 64), 256) * p;
	uint32 leftMetaUV = ALIGN(256 + 64 * (hc / 32), 256) * p;
	uint32 lineRecon = ALIGN(2 * wc, 256);
	uint32 colmv = hevc ? (16 * ((nl << 2) + 32)) : (3 * 16 * (nl + 32));
	colmv = ALIGN(colmv, 256) * (references + 1);
	uint32 colrc = ((wl + 7) >> 3) * 16 * 2 * hl;
	if (p > 1)
		colrc = ALIGN(colrc, 256) * p;
	colrc = ALIGN(colrc, 256) * kMaxColFrame;
	uint32 framerc = hevc ? (256 + 16 * (14 + (((hc >> 5) + 7) >> 3)))
		: (256 + 16 * (14 + (((hc >> 4) + 7) >> 3)));
	framerc *= 6;
	if (p > 1)
		framerc = ALIGN(framerc, 256) * p;
	framerc = ALIGN(framerc, 512) * kMaxColFrame;
	uint32 bitcount = ALIGN(256 + 4 * nl, 256);
	uint32 bitmap = ALIGN(256 + (nl >> 3), 256);
	uint32 sde = ALIGN(256 + 16 * (wc >> 4), 256);
	uint32 seStats;
	if (wc * hc > 4096 * 2160)
		seStats = 0;
	else if (wc * hc > 1920 * 1088)
		seStats = 40 * 4 * nl + 256 + 256;
	else
		seStats = 1024 * nl + 256 + 256;
	seStats = ALIGN(seStats, 256) * 2;
	uint32 override = ALIGN(16 * ((lcuMbs + 7) >> 3), 256) * 2;
	uint32 intraRefresh = ALIGN((nl << 1), 8) * 3;
	uint32 vpss = ((((max_u32(wc, hc) + 3) >> 2) << 5) + 256) * 16
		+ (((8192 >> 2) << 5) * p) + 64;
	uint32 sao = ALIGN(16 * (wc >> 5), 256);

	return lineControl + lineData + lineControlId2 + leftControl + vpss + colmv
		+ topFe + leftFe + lineRecon + leftRecon + leftMetaY + leftMetaUV
		+ colrc + framerc + bitcount + bitmap + sde + sao + override
		+ ((512 << 3) * 4) + ((2048 << 3) * 10) + (2048 + 4096)
		+ ALIGN(20480, 256) + ((8192 << 2) * 6) + intraRefresh + sliceInfo
		+ (256 * 11) + seStats + 1024;
}


//!	The reconstructed and reference pictures (UBWC).
static uint32
enc_scratch2_size(uint32 width, uint32 height, uint32 references)
{
	uint32 lumaRows = ALIGN(height, 32), chromaRows = ALIGN(height >> 1, 32);
	uint32 stride = ALIGN(width, 128);
	uint32 metaStride = ALIGN((width + 31) / 32, 64);
	uint32 metaHeight = ALIGN((height + 7) / 8, 16);
	uint32 meta = ALIGN(metaStride * metaHeight, 4096);
	uint32 one = (lumaRows + chromaRows) * stride + 2 * meta;
	return one * (references + 3) + 4096;
}


// #pragma mark - IrisEncoder


IrisEncoder::IrisEncoder()
	:
	fStride(0),
	fLumaRows(0),
	fInputSize(0),
	fEndOfStream(false),
	fEnded(false),
	fEncoded(0)
{
}


IrisEncoder::~IrisEncoder()
{
	Close();
}


status_t
IrisEncoder::Open(const Params& params, BString* _error)
{
	if (params.width < 128 || params.height < 128 || params.width > 4096
		|| params.height > 2304 || (params.width & 1) != 0
		|| (params.height & 1) != 0 || params.framesPerSecond == 0) {
		_Fail(B_BAD_VALUE, "the codec encodes even sizes from 128x128 to "
			"4096x2304");
		if (_error != NULL)
			*_error = fError;
		return B_BAD_VALUE;
	}
	fParams = params;
	if (fParams.bitrate > kMaxBitrate)
		fParams.bitrate = kMaxBitrate;
	if (fParams.keyInterval == 0)
		fParams.keyInterval = 1;
	fStride = ALIGN(params.width, 128);
	fLumaRows = ALIGN(params.height, 32);
	fInputSize = enc_nv12_size(params.width, params.height);

	status_t status = _OpenSession(HFI_SESSION_TYPE_ENC,
		params.codec == HEVC ? HFI_VIDEO_CODEC_HEVC : HFI_VIDEO_CODEC_H264);
	if (status == B_OK)
		status = _Configure();
	if (status == B_OK) {
		status = _Simple(HFI_CMD_SESSION_LOAD_RESOURCES,
			HFI_MSG_SESSION_LOAD_RESOURCES);
	}
	if (status == B_OK)
		status = _Simple(HFI_CMD_SESSION_START, HFI_MSG_SESSION_START);
	if (status == B_OK)
		fStarted = true;

	for (uint32 i = 0; status == B_OK && i < kInputBuffers; i++) {
		IrisDevice::Buffer buffer;
		status = fDevice.Allocate(fInputSize, buffer);
		if (status != B_OK) {
			_Fail(status, "no memory for the pictures");
			break;
		}
		fInputs.push_back(buffer);
		fInputQueued.push_back(false);
	}
	uint32 outputSize = enc_frame_size(fStride, fLumaRows,
		HFI_RATE_CONTROL_OFF);
	for (uint32 i = 0; status == B_OK && i < kOutputBuffers; i++) {
		Output output = {};
		status = fDevice.Allocate(outputSize, output.buffer);
		if (status != B_OK) {
			_Fail(status, "no memory for the stream");
			break;
		}
		fOutputs.push_back(output);
		status = _Queue(fOutputs.size() - 1);
	}

	if (status != B_OK) {
		BString error = fError;
		Close();
		fError = error;
		if (_error != NULL)
			*_error = fError;
		return status;
	}
	TRACE("encoder session %#" B_PRIx32 " started, %" B_PRIu32 "x%" B_PRIu32
		"\n", fSession, params.width, params.height);
	return B_OK;
}


void
IrisEncoder::Close()
{
	if (fOpen && fStarted && !fFailed) {
		_Simple(HFI_CMD_SESSION_STOP, HFI_MSG_SESSION_STOP);
		if (!fFailed) {
			_Simple(HFI_CMD_SESSION_RELEASE_RESOURCES,
				HFI_MSG_SESSION_RELEASE_RESOURCES);
		}
		if (!fFailed)
			_ReleaseInternal(HFI_BUFFER_INTERNAL_SCRATCH, fScratch);
		if (!fFailed)
			_ReleaseInternal(HFI_BUFFER_INTERNAL_SCRATCH_1, fScratch1);
		if (!fFailed)
			_ReleaseInternal(HFI_BUFFER_INTERNAL_SCRATCH_2, fScratch2);
		if (!fFailed)
			_ReleaseInternal(HFI_BUFFER_INTERNAL_PERSIST, fPersist);
	}
	fStarted = false;
	_EndSession();

	for (size_t i = 0; i < fInputs.size(); i++)
		fDevice.Free(fInputs[i]);
	fInputs.clear();
	fInputQueued.clear();
	for (size_t i = 0; i < fOutputs.size(); i++)
		fDevice.Free(fOutputs[i].buffer);
	fOutputs.clear();
	fReady.clear();
	fDevice.Free(fPersist);
	fDevice.Free(fScratch);
	fDevice.Free(fScratch1);
	fDevice.Free(fScratch2);
	fDevice.Close();
	fEndOfStream = false;
	fEnded = false;
	fFailed = false;
}


status_t
IrisEncoder::_Configure()
{
	const Params& params = fParams;
	bool hevc = params.codec == HEVC;
	uint32 rateControl = params.constantBitrate ? HFI_RATE_CONTROL_CBR_CFR
		: HFI_RATE_CONTROL_VBR_CFR;

	// the level the picture size and rate need (macroblocks per second)
	uint32 mbs = IRIS_DIV_ROUND_UP(params.width, 16)
		* IRIS_DIV_ROUND_UP(params.height, 16);
	uint32 mbsPerSecond = mbs * params.framesPerSecond;
	uint32 profile, level;
	if (hevc) {
		profile = HFI_HEVC_PROFILE_MAIN;
		level = mbs <= 8704 && mbsPerSecond <= 261120 ? HFI_HEVC_LEVEL_41
			: mbsPerSecond <= 1044480 ? HFI_HEVC_LEVEL_51 : HFI_HEVC_LEVEL_52;
	} else {
		profile = HFI_H264_PROFILE_HIGH;
		level = mbs <= 3600 && mbsPerSecond <= 108000 ? HFI_H264_LEVEL_31
			: mbs <= 8192 && mbsPerSecond <= 245760 ? HFI_H264_LEVEL_41
			: mbs <= 8704 && mbsPerSecond <= 522240 ? HFI_H264_LEVEL_42
			: mbsPerSecond <= 983040 ? HFI_H264_LEVEL_51 : HFI_H264_LEVEL_52;
	}

	uint32 planes[6] = { HFI_BUFFER_INPUT, 2, fStride, fLumaRows, fStride,
		fLumaRows / 2 };
	// QP 1..51 for I, P and B pictures: min, max, then four reserved words
	uint32 qpRange[16] = { 0x010101, 0xff, 7, 0, 0, 0,
		0x333333, 0xff, 7, 0, 0, 0, 0, 0, 0, 0 };

	status_t status = _SetProperty(HFI_PROPERTY_CONFIG_VIDEOCORES_USAGE, 1);
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_UNCOMPRESSED_PLANE_ACTUAL_INFO,
			planes, 6);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_FRAME_SIZE, HFI_BUFFER_INPUT,
			params.width, params.height);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_FRAME_SIZE,
			HFI_BUFFER_OUTPUT, params.width, params.height);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_UNCOMPRESSED_FORMAT_SELECT,
			HFI_BUFFER_INPUT, HFI_COLOR_FORMAT_NV12);
	}
	if (status == B_OK)
		status = _SetProperty(HFI_PROPERTY_PARAM_WORK_MODE, kWorkMode2);
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_CONFIG_FRAME_RATE, HFI_BUFFER_OUTPUT,
			params.framesPerSecond << 16);
	}
	if (status == B_OK && !hevc) {
		status = _SetProperty(HFI_PROPERTY_PARAM_VENC_H264_ENTROPY_CONTROL,
			HFI_H264_ENTROPY_CABAC, HFI_H264_CABAC_MODEL_0);
		if (status == B_OK) {
			status = _SetProperty(HFI_PROPERTY_PARAM_VENC_H264_DEBLOCK_CONTROL,
				HFI_H264_DB_MODE_ALL_BOUNDARY, 0, 0);
		}
		if (status == B_OK) {
			status = _SetProperty(HFI_PROPERTY_PARAM_VENC_H264_TRANSFORM_8X8,
				1);
		}
	}
	// every intra picture an IDR picture, one every keyInterval
	if (status == B_OK)
		status = _SetProperty(HFI_PROPERTY_CONFIG_VENC_IDR_PERIOD, 1);
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_CONFIG_VENC_INTRA_PERIOD,
			params.keyInterval - 1, 0);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_VENC_RATE_CONTROL,
			rateControl);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_CONFIG_VENC_TARGET_BITRATE,
			fParams.bitrate, 0);
	}
	if (status == B_OK) {
		status = _SetProperty(
			HFI_PROPERTY_CONFIG_VENC_SYNC_FRAME_SEQUENCE_HEADER, 1);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_VENC_SESSION_QP_RANGE_V2,
			qpRange, 16);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_PROFILE_LEVEL_CURRENT,
			profile, level);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_BUFFER_COUNT_ACTUAL,
			HFI_BUFFER_INPUT, kInputBuffers, kInputBuffers);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_BUFFER_COUNT_ACTUAL,
			HFI_BUFFER_OUTPUT, kOutputBuffers, kOutputBuffers);
	}

	uint32 lcu = hevc ? 32 : 16;
	if (status == B_OK) {
		status = _SetInternal(HFI_BUFFER_INTERNAL_SCRATCH, fScratch,
			enc_scratch_size(fStride, fLumaRows, HFI_RATE_CONTROL_OFF));
	}
	if (status == B_OK) {
		status = _SetInternal(HFI_BUFFER_INTERNAL_SCRATCH_1, fScratch1,
			enc_scratch1_size(fStride, fLumaRows, lcu, 1, hevc));
	}
	if (status == B_OK) {
		status = _SetInternal(HFI_BUFFER_INTERNAL_SCRATCH_2, fScratch2,
			enc_scratch2_size(fStride, fLumaRows, 1));
	}
	if (status == B_OK) {
		status = _SetInternal(HFI_BUFFER_INTERNAL_PERSIST, fPersist,
			kPersistSize);
	}
	return status;
}


status_t
IrisEncoder::_Queue(int32 index)
{
	Output& output = fOutputs[index];
	uint32 packet[11] = { 44, HFI_CMD_SESSION_FILL_BUFFER, fSession, 0, 0,
		(uint32)output.buffer.size, 0, kOutputTag + index,
		output.buffer.deviceAddress, 0, 0 };
	status_t status = _Send(packet);
	if (status == B_OK)
		output.queued = true;
	return status;
}


//!	A free input buffer, waiting up to \a timeout while no output is ready.
status_t
IrisEncoder::_InputBuffer(int32& index, bigtime_t timeout)
{
	if (!fOpen || fFailed)
		return fFailed ? B_ERROR : B_NO_INIT;
	if (fEndOfStream)
		return B_NOT_ALLOWED;

	bigtime_t deadline = system_time() + timeout;
	while (true) {
		for (size_t i = 0; i < fInputs.size(); i++) {
			if (!fInputQueued[i]) {
				index = i;
				return B_OK;
			}
		}
		if (!fReady.empty())
			return B_WOULD_BLOCK;
		bigtime_t left = deadline - system_time();
		if (left <= 0)
			return B_WOULD_BLOCK;
		status_t status = _Pump(left);
		if (status == B_TIMED_OUT)
			return B_WOULD_BLOCK;
		if (status != B_OK)
			return status;
	}
}


status_t
IrisEncoder::_SendInput(int32 index, int64 pts)
{
	IrisDevice::Buffer& buffer = fInputs[index];
	uint32 packet[16] = { 64, HFI_CMD_SESSION_EMPTY_BUFFER, fSession, 0,
		(uint32)((uint64)pts >> 32), (uint32)pts, 0, 0, 0,
		(uint32)buffer.size, (uint32)fInputSize, 0, (uint32)index,
		buffer.deviceAddress, 0, 0 };
	status_t status = _Send(packet);
	if (status == B_OK)
		fInputQueued[index] = true;
	return status;
}


status_t
IrisEncoder::PutPicture(const uint8* luma, uint32 lumaStride,
	const uint8* chroma, uint32 chromaStride, int64 pts, bigtime_t timeout)
{
	int32 index;
	status_t status = _InputBuffer(index, timeout);
	if (status != B_OK)
		return status;

	// the rows below the picture repeat its last one: what the codec reads
	// of the last macroblock row compresses best so
	uint8* target = (uint8*)fInputs[index].address;
	uint32 width = fParams.width, height = fParams.height;
	for (uint32 y = 0; y < fLumaRows; y++) {
		uint32 row = y < height ? y : height - 1;
		memcpy(target + (size_t)y * fStride, luma + (size_t)row * lumaStride,
			width);
	}
	uint8* targetChroma = target + (size_t)fStride * fLumaRows;
	uint32 chromaRows = height / 2;
	uint32 alignedRows = ALIGN((height + 1) >> 1, 16);
	for (uint32 y = 0; y < alignedRows; y++) {
		uint32 row = y < chromaRows ? y : chromaRows - 1;
		memcpy(targetChroma + (size_t)y * fStride,
			chroma + (size_t)row * chromaStride, width);
	}
	return _SendInput(index, pts);
}


status_t
IrisEncoder::PutPicture420(const uint8* luma, uint32 lumaStride,
	const uint8* cb, const uint8* cr, uint32 chromaStride, int64 pts,
	bigtime_t timeout)
{
	int32 index;
	status_t status = _InputBuffer(index, timeout);
	if (status != B_OK)
		return status;

	uint8* target = (uint8*)fInputs[index].address;
	uint32 width = fParams.width, height = fParams.height;
	for (uint32 y = 0; y < fLumaRows; y++) {
		uint32 row = y < height ? y : height - 1;
		memcpy(target + (size_t)y * fStride, luma + (size_t)row * lumaStride,
			width);
	}
	uint8* targetChroma = target + (size_t)fStride * fLumaRows;
	uint32 chromaRows = height / 2;
	uint32 alignedRows = ALIGN((height + 1) >> 1, 16);
	for (uint32 y = 0; y < alignedRows; y++) {
		uint32 row = y < chromaRows ? y : chromaRows - 1;
		const uint8* cbRow = cb + (size_t)row * chromaStride;
		const uint8* crRow = cr + (size_t)row * chromaStride;
		uint8* pairs = targetChroma + (size_t)y * fStride;
		for (uint32 x = 0; x < width / 2; x++) {
			pairs[2 * x] = cbRow[x];
			pairs[2 * x + 1] = crRow[x];
		}
	}
	return _SendInput(index, pts);
}


status_t
IrisEncoder::EndOfStream()
{
	if (!fOpen || fFailed)
		return fFailed ? B_ERROR : B_NO_INIT;
	if (fEndOfStream)
		return B_OK;
	uint32 packet[16] = { 64, HFI_CMD_SESSION_EMPTY_BUFFER, fSession, 0, 0, 0,
		HFI_BUFFERFLAG_EOS, 0, 0, 0, 0, 0, 0xffff, HFI_END_OF_STREAM_ADDRESS,
		0, 0 };
	status_t status = _Send(packet);
	if (status == B_OK)
		fEndOfStream = true;
	return status;
}


status_t
IrisEncoder::NextPacket(Packet& packet, bigtime_t timeout)
{
	if (!fOpen)
		return B_NO_INIT;
	bigtime_t deadline = system_time() + timeout;
	while (fReady.empty()) {
		if (fFailed)
			return B_ERROR;
		if (fEnded)
			return B_LAST_BUFFER_ERROR;
		bigtime_t left = deadline - system_time();
		status_t status = _Pump(left > 0 ? left : 0);
		if (status == B_TIMED_OUT)
			return B_WOULD_BLOCK;
		if (status != B_OK)
			return status;
	}

	int32 index = fReady.front();
	fReady.pop_front();
	Output& output = fOutputs[index];
	output.held = true;
	packet.buffer = index;
	packet.data = (const uint8*)output.buffer.address + output.offset;
	packet.size = output.size;
	packet.pts = output.pts;
	packet.key = output.key;
	return B_OK;
}


void
IrisEncoder::ReleasePacket(const Packet& packet)
{
	if (packet.buffer < 0 || (size_t)packet.buffer >= fOutputs.size())
		return;
	Output& output = fOutputs[packet.buffer];
	if (!output.held)
		return;
	output.held = false;
	if (!fFailed && !fEnded)
		_Queue(packet.buffer);
}


//!	The VPS, SPS and PPS in front of the first IDR picture.
void
IrisEncoder::_KeepParameterSets(const uint8* data, size_t size)
{
	bool hevc = fParams.codec == HEVC;
	size_t position = 0;
	while (position + 3 < size) {
		// the next NAL unit after a start code
		size_t start = position;
		while (start + 3 <= size && !(data[start] == 0 && data[start + 1] == 0
				&& data[start + 2] == 1)) {
			start++;
		}
		if (start + 3 > size)
			break;
		size_t nal = start + 3;
		size_t end = nal;
		while (end + 3 <= size && !(data[end] == 0 && data[end + 1] == 0
				&& (data[end + 2] == 1 || data[end + 2] == 0))) {
			end++;
		}
		if (end + 3 > size)
			end = size;
		if (nal < end) {
			uint32 type = hevc ? (data[nal] >> 1) & 0x3f : data[nal] & 0x1f;
			bool parameterSet = hevc ? type >= 32 && type <= 34
				: type == 7 || type == 8;
			if (parameterSet) {
				static const uint8 kStartCode[] = { 0, 0, 0, 1 };
				fParameterSets.insert(fParameterSets.end(), kStartCode,
					kStartCode + 4);
				fParameterSets.insert(fParameterSets.end(), data + nal,
					data + end);
			}
		}
		position = end;
	}
}


status_t
IrisEncoder::_Handle(const uint32* packet, size_t size)
{
	if (size < 12)
		return B_OK;
	uint32 type = packet[1];
	if (type != HFI_MSG_EVENT_NOTIFY && packet[2] != fSession)
		return B_OK;

	switch (type) {
		case HFI_MSG_SESSION_EMPTY_BUFFER:
		{
			if (size < 36)
				return B_OK;
			uint32 error = packet[3];
			uint32 tag = packet[6];
			if (tag < fInputQueued.size())
				fInputQueued[tag] = false;
			if (error != 0)
				TRACE("picture %" B_PRIu32 ": error %#" B_PRIx32 "\n", tag, error);
			return B_OK;
		}

		case HFI_MSG_SESSION_FILL_BUFFER:
		{
			if (size < 72)
				return B_OK;
			int64 pts = ((int64)packet[3] << 32) | packet[4];
			uint32 error = packet[5];
			uint32 flags = packet[6];
			uint32 offset = packet[10];
			uint32 filled = packet[12];
			uint32 tag = packet[14] - kOutputTag;
			uint32 pictureType = packet[15];
			TRACE("stream: tag %" B_PRIu32 " flags %#" B_PRIx32 " offset %"
				B_PRIu32 " %" B_PRIu32 " bytes type %#" B_PRIx32 " error %#"
				B_PRIx32 "\n", tag, flags, offset, filled, pictureType, error);
			if (tag < fOutputs.size()) {
				Output& output = fOutputs[tag];
				output.queued = false;
				if (filled != 0 && offset + filled <= output.buffer.size) {
					output.offset = offset;
					output.size = filled;
					output.pts = pts;
					output.key = pictureType == HFI_PICTURE_IDR
						|| pictureType == HFI_PICTURE_I;
					if (output.key && fParameterSets.empty()) {
						_KeepParameterSets(
							(const uint8*)output.buffer.address + offset,
							filled);
					}
					fReady.push_back(tag);
					fEncoded++;
				} else if (!fEnded && (flags & HFI_BUFFERFLAG_EOS) == 0) {
					// nothing in it: back to the firmware
					_Queue(tag);
				}
			}
			if ((flags & HFI_BUFFERFLAG_EOS) != 0)
				fEnded = true;
			return B_OK;
		}

		case HFI_MSG_EVENT_NOTIFY:
			if (size >= 24)
				TRACE("event %#" B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32 "\n",
					packet[3], packet[4], packet[5]);
			return _HandleErrorEvent(packet, size);

		default:
			TRACE("message %#" B_PRIx32 " (%zu bytes), error %#" B_PRIx32 "\n",
				type, size, size >= 16 ? packet[3] : 0);
			return B_OK;
	}
}
