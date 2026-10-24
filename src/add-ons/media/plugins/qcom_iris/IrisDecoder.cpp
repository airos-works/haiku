/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "IrisDecoder.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*	The packets are those of Linux' iris driver (iris_hfi_gen1_defines.h)
	and the order it sends them in for SM8250, whose firmware the SC8280XP
	runs; the buffer sizes are its iris_vpu_buffer.c's. */

#define HFI_CMD_SYS_SESSION_INIT			0x10007
#define HFI_CMD_SYS_SESSION_END				0x10008
#define HFI_CMD_SESSION_SET_PROPERTY		0x11001
#define HFI_CMD_SESSION_SET_BUFFERS			0x11002
#define HFI_CMD_SESSION_LOAD_RESOURCES		0x211001
#define HFI_CMD_SESSION_START				0x211002
#define HFI_CMD_SESSION_STOP				0x211003
#define HFI_CMD_SESSION_EMPTY_BUFFER		0x211004
#define HFI_CMD_SESSION_FILL_BUFFER			0x211005
#define HFI_CMD_SESSION_FLUSH				0x211008
#define HFI_CMD_SESSION_RELEASE_BUFFERS		0x21100b
#define HFI_CMD_SESSION_RELEASE_RESOURCES	0x21100c
#define HFI_CMD_SESSION_CONTINUE			0x21100d

#define HFI_MSG_SYS_SESSION_INIT			0x20006
#define HFI_MSG_SYS_SESSION_END				0x20007
#define HFI_MSG_EVENT_NOTIFY				0x21001
#define HFI_MSG_SESSION_LOAD_RESOURCES		0x221001
#define HFI_MSG_SESSION_START				0x221002
#define HFI_MSG_SESSION_STOP				0x221003
#define HFI_MSG_SESSION_FLUSH				0x221006
#define HFI_MSG_SESSION_EMPTY_BUFFER		0x221007
#define HFI_MSG_SESSION_FILL_BUFFER			0x221008
#define HFI_MSG_SESSION_RELEASE_RESOURCES	0x22100a
#define HFI_MSG_SESSION_RELEASE_BUFFERS		0x22100c

#define HFI_SESSION_TYPE_DEC				2
#define HFI_VIDEO_CODEC_H264				0x00000002
#define HFI_VIDEO_CODEC_HEVC				0x00002000

#define HFI_BUFFER_INPUT					0x1
#define HFI_BUFFER_OUTPUT					0x2		// the references (UBWC)
#define HFI_BUFFER_OUTPUT2					0x3		// the NV12 copies
#define HFI_BUFFER_INTERNAL_PERSIST_1		0x5
#define HFI_BUFFER_INTERNAL_SCRATCH			0x6		// "BIN"
#define HFI_BUFFER_INTERNAL_SCRATCH_1		0x7		// COMV, NON_COMV, LINE

#define HFI_PROPERTY_PARAM_FRAME_SIZE		0x1001
#define HFI_PROPERTY_PARAM_UNCOMPRESSED_FORMAT_SELECT 0x1003
#define HFI_PROPERTY_PARAM_PROFILE_LEVEL_CURRENT 0x1005
#define HFI_PROPERTY_PARAM_WORK_MODE		0x1015
#define HFI_PROPERTY_PARAM_WORK_ROUTE		0x1017
#define HFI_PROPERTY_CONFIG_VIDEOCORES_USAGE 0x2002
#define HFI_PROPERTY_PARAM_BUFFER_COUNT_ACTUAL 0x201001
#define HFI_PROPERTY_PARAM_UNCOMPRESSED_PLANE_ACTUAL_CONSTRAINTS_INFO 0x201002
#define HFI_PROPERTY_PARAM_BUFFER_SIZE_ACTUAL 0x20100c
#define HFI_PROPERTY_CONFIG_BUFFER_REQUIREMENTS 0x202001
#define HFI_PROPERTY_PARAM_VDEC_MULTI_STREAM 0x1003001
#define HFI_PROPERTY_PARAM_VDEC_PIXEL_BITDEPTH 0x1003007
#define HFI_PROPERTY_PARAM_VDEC_PIC_STRUCT	0x1003009
#define HFI_PROPERTY_PARAM_VDEC_COLOUR_SPACE 0x100300a
#define HFI_PROPERTY_PARAM_VDEC_DPB_COUNTS	0x120300e
#define HFI_PROPERTY_CONFIG_VDEC_ENTROPY	0x1204004
#define HFI_INDEX_EXTRADATA_INPUT_CROP		0x0700000e

#define HFI_COLOR_FORMAT_NV12				0x0002
#define HFI_COLOR_FORMAT_NV12_UBWC			0x8002

#define HFI_FLUSH_ALL						0x1000004

#define HFI_EVENT_SYS_ERROR					0x1
#define HFI_EVENT_SESSION_ERROR				0x2
#define HFI_EVENT_DATA_SEQUENCE_CHANGED_SUFFICIENT_BUF_RESOURCES 0x1000001
#define HFI_EVENT_DATA_SEQUENCE_CHANGED_INSUFFICIENT_BUF_RESOURCES 0x1000002
#define HFI_EVENT_SESSION_SEQUENCE_CHANGED	0x1000003

#define HFI_BUFFERFLAG_EOS					0x00000001
#define HFI_BUFFERFLAG_DATACORRUPT			0x00000008
#define HFI_BUFFERFLAG_DROP_FRAME			0x20000000

#define HFI_ERR_SESSION_UNSUPPORTED_SETTING	0x1008
#define HFI_ERR_SESSION_UNSUPPORTED_STREAM	0x100d
#define HFI_ERR_SESSION_UNSUPPORT_BUFFERTYPE 0x1010
#define HFI_ERR_SESSION_INVALID_SCALE_FACTOR 0x1012
#define HFI_ERR_SESSION_UPSCALE_NOT_SUPPORTED 0x1013

// the end of stream packet's buffer, which nothing is at
static const uint32 kEndOfStreamAddress = 0xdeadb000;
// references are tagged from here, the NV12 buffers from 0
static const uint32 kReferenceTag = 64;
static const uint32 kInputBuffers = 4;
// beyond the firmware's minimum: what the caller holds and what waits
// to be taken
static const uint32 kExtraOutputs = 4;
static const bigtime_t kAnswerTimeout = 1000000;
static const size_t kMessageSize = 4096;

static uint32 sNextSession = 0x10;


#define ALIGN(x, a)			((((x) + (a) - 1) / (a)) * (a))
#define DIV_ROUND_UP(x, a)	(((x) + (a) - 1) / (a))


static bool
debug_enabled()
{
	static int enabled = -1;
	if (enabled < 0)
		enabled = getenv("IRIS_DEBUG") != NULL ? 1 : 0;
	return enabled != 0;
}


#define TRACE(...) \
	do { \
		if (debug_enabled()) \
			fprintf(stderr, "iris: " __VA_ARGS__); \
	} while (false)


// #pragma mark - buffer sizes (iris_vpu_buffer.c, iris_buffer.c)


static const uint32 kDmaAlignment = 256;
static const uint32 kVppPipes = 4;
static const uint32 kComvBuffers = 64;
static const uint32 kHwPictureBuffers = 32;


static uint32
bin_size(uint32 width, uint32 height, uint32 headerRatio, uint32 residueRatio)
{
	const uint32 threshold = 1280 * 736;
	uint32 yuv = width * height <= threshold ? (threshold * 3) >> 1
		: (width * height * 3) >> 1;
	uint32 header = ALIGN(yuv * headerRatio / kVppPipes, kDmaAlignment)
		* kVppPipes;
	uint32 residue = ALIGN(yuv * residueRatio / kVppPipes, kDmaAlignment)
		* kVppPipes;
	return header + residue;
}


static uint32
comv_h264(uint32 width, uint32 height, uint32 count)
{
	uint32 heightMbs = DIV_ROUND_UP(height, 16);
	uint32 widthMbs = DIV_ROUND_UP(width, 16);
	uint32 colZeroWidth = ALIGN(widthMbs << 2, 16);
	uint32 colMvWidth = ALIGN(widthMbs << 7, 16);
	uint32 colZero = ALIGN(colZeroWidth * ((heightMbs + 1) >> 1), 64) << 1;
	colZero = ALIGN(colZero, 512);
	uint32 colocated = ALIGN(colMvWidth * ((heightMbs + 1) >> 1), 64) << 1;
	colocated = ALIGN(colocated, 512);
	colocated += colZero + 256 * 2;
	return colocated * count + 512;
}


static uint32
comv_hevc(uint32 width, uint32 height, uint32 count)
{
	uint32 heightMbs = (height + 15) >> 4;
	uint32 widthMbs = (width + 15) >> 4;
	return ALIGN((widthMbs * heightMbs) << 8, 512) * count + 512;
}


static uint32
non_comv_h264(uint32 width, uint32 height)
{
	uint32 slices = DIV_ROUND_UP(ALIGN(height, 32), 16) * 48;
	if (slices > 1800)
		slices = 1800;
	uint32 bse = slices * 32 * 4;
	uint32 vpp = slices * 512;
	if (vpp > (1u << 20))
		vpp = 1u << 20;
	uint32 size = ALIGN(bse, kDmaAlignment) + ALIGN(vpp, kDmaAlignment)
		+ ALIGN(kHwPictureBuffers * (1u << 11), kDmaAlignment);
	return ALIGN(size, kDmaAlignment);
}


static uint32
lcu_count(uint32 width, uint32 height)
{
	return (ALIGN(width, 64) / 16) * (ALIGN(height, 64) / 16);
}


static uint32
non_comv_hevc(uint32 width, uint32 height)
{
	uint32 slices = ALIGN(lcu_count(width, height) * kHwPictureBuffers,
		kDmaAlignment);
	if (slices > 3600 + 1)
		slices = 3600 + 1;
	uint32 bse = 2 * slices * 16 * 4;
	uint32 vpp = 2 * ALIGN(slices, 4) * 256;
	if (vpp > (1u << 20))
		vpp = 1u << 20;
	uint32 size = ALIGN(bse, kDmaAlignment) + ALIGN(vpp, kDmaAlignment)
		+ ALIGN(kHwPictureBuffers * 20 * 22 * 4, kDmaAlignment)
		+ ALIGN(2 * 2 * lcu_count(width, height), kDmaAlignment)
		+ ALIGN(kHwPictureBuffers * (1u << 11), kDmaAlignment)
		+ 4 * 1024;
	return ALIGN(size, kDmaAlignment);
}


//!	The line buffer of the post processor, which split mode uses.
static uint32
vpss_line_size(uint32 width, uint32 height)
{
	uint32 luma = ALIGN(width, 32) / 32 * 256;
	luma = ALIGN(luma, kDmaAlignment) + (32 - 1) * 256;
	if (luma < 32 * ALIGN(height, 8))
		luma = 32 * ALIGN(height, 8);
	uint32 lineUV = ALIGN((ALIGN(height, 8) / 2) * 64, 32);
	uint32 lineY = ALIGN((ALIGN(height, 8) / 2) * 64, 32);
	return luma * 2 + lineUV + lineY;
}


static uint32
line_h264(uint32 width, uint32 height)
{
	uint32 size = ALIGN(640 * ALIGN(width, 16) * 3, kDmaAlignment)
		+ ALIGN(64 * DIV_ROUND_UP(width, 16), kDmaAlignment)
		+ ALIGN(64 * DIV_ROUND_UP(height, 16), kDmaAlignment) * kVppPipes
		+ ALIGN(16 * DIV_ROUND_UP(width, 16), kDmaAlignment)
		+ ALIGN(16 * DIV_ROUND_UP(height, 16), kDmaAlignment) * kVppPipes
		+ ALIGN(384 * DIV_ROUND_UP(width, 16), kDmaAlignment)
		+ ALIGN(DIV_ROUND_UP(width, 16) << 7, kDmaAlignment)
		+ ALIGN(ALIGN(height, 16) * 32, kDmaAlignment) * 2
		+ ALIGN(DIV_ROUND_UP(width, 64) * DIV_ROUND_UP(height, 64) * 128,
			kDmaAlignment);
	size = ALIGN(size, kDmaAlignment);
	return ALIGN(size + vpss_line_size(width, height), kDmaAlignment);
}


static uint32
line_hevc(uint32 width, uint32 height)
{
	uint32 feTopData = 640 * (ALIGN(width, 64) + 8) * 2;
	uint32 feTopControl = 64 * (ALIGN(width, 64) / 16);
	uint32 feLeftControl = 64 * (ALIGN(height, 64) / 16);
	uint32 seTopControl = (64 / 8 * (128 / 8)) * ((width + 15) >> 4);
	uint32 seLeftControl = ((height + 16 - 1) / 8) * 16;
	if (seLeftControl < ((height + 32 - 1) / 8) * 16)
		seLeftControl = ((height + 32 - 1) / 8) * 16;
	if (seLeftControl < ((height + 64 - 1) / 8) * 16)
		seLeftControl = ((height + 64 - 1) / 8) * 16;
	uint32 peTopData = 384 * (ALIGN(width, 16) / 16);
	uint32 vspTop = ((width + 63) >> 6) * 128;
	uint32 vspLeft = ((height + 63) >> 6) * 128;
	uint32 recon = ALIGN(height, 16) * 32;
	uint32 qp = DIV_ROUND_UP(width, 64) * DIV_ROUND_UP(height, 64) * 128;
	uint32 size = ALIGN(feTopData, kDmaAlignment)
		+ ALIGN(feTopControl, kDmaAlignment)
		+ ALIGN(feLeftControl, kDmaAlignment) * kVppPipes
		+ ALIGN(seLeftControl, kDmaAlignment) * kVppPipes
		+ ALIGN(seTopControl, kDmaAlignment)
		+ ALIGN(peTopData, kDmaAlignment)
		+ ALIGN(vspTop, kDmaAlignment)
		+ ALIGN(vspLeft, kDmaAlignment) * kVppPipes
		+ ALIGN(recon, kDmaAlignment) * 4
		+ ALIGN(qp, kDmaAlignment);
	return ALIGN(size + vpss_line_size(width, height), kDmaAlignment);
}


//!	The NV12 copies: luma rows, then rows of chroma pairs, 128 byte stride.
static uint32
nv12_size(uint32 width, uint32 height)
{
	uint32 stride = ALIGN(width, 128);
	return ALIGN(stride * ALIGN(height, 32)
		+ stride * ALIGN((height + 1) >> 1, 16), 4096);
}


//!	The references: luma meta data and samples, then chroma's (UBWC).
static uint32
ubwc_size(uint32 width, uint32 height)
{
	uint32 yMetaStride = ALIGN(DIV_ROUND_UP(width, 32), 64);
	uint32 yMeta = ALIGN(yMetaStride * ALIGN(DIV_ROUND_UP(height, 8), 16),
		4096);
	uint32 yPlane = ALIGN(ALIGN(width, 128) * ALIGN(height, 32), 4096);
	uint32 uvMetaStride = ALIGN(DIV_ROUND_UP(width / 2, 16), 64);
	uint32 uvMeta = ALIGN(uvMetaStride
		* ALIGN(DIV_ROUND_UP(height / 2, 8), 16), 4096);
	uint32 uvPlane = ALIGN(ALIGN(width, 128) * ALIGN(height / 2, 32), 4096);
	return ALIGN(yMeta + yPlane + uvMeta + uvPlane, 4096);
}


//!	A bitstream buffer: half a 4K picture's samples, up to 4K.
static uint32
input_size(uint32 width, uint32 height)
{
	uint32 mbs = DIV_ROUND_UP(height, 16) * DIV_ROUND_UP(width, 16);
	uint32 mbs4k = DIV_ROUND_UP(4096, 16) * DIV_ROUND_UP(2304, 16);
	uint32 base = mbs4k;
	uint32 divisor = 2;
	if (mbs > mbs4k) {
		divisor = 4;
		base = 138240;
	}
	return ALIGN(base * 256 * 3 / 2 / divisor, 4096);
}


// #pragma mark - IrisDecoder


IrisDecoder::IrisDecoder(codec codec)
	:
	fCodec(codec),
	fSession(0),
	fOpen(false),
	fStarted(false),
	fFailed(false),
	fWidth(0),
	fHeight(0),
	fCropLeft(0),
	fCropTop(0),
	fCropWidth(0),
	fCropHeight(0),
	fMinCount(4),
	fOutputStride(0),
	fOutputRows(0),
	fOutputSize(0),
	fReferenceSize(0),
	fSequenceChanged(false),
	fOutputStarted(false),
	fEventWidth(0),
	fEventHeight(0),
	fEventMinCount(0),
	fLastAnswer(0),
	fLastAnswerError(0),
	fEndOfStream(false),
	fEnded(false),
	fDecoded(0)
{
	memset(fEventCrop, 0, sizeof(fEventCrop));
}


IrisDecoder::~IrisDecoder()
{
	Close();
}


status_t
IrisDecoder::Open(uint32 width, uint32 height, BString* _error)
{
	status_t status = fDevice.Open();
	if (status != B_OK) {
		_Fail(status, "cannot open the video codec: %s", strerror(status));
		if (_error != NULL)
			*_error = fError;
		return status;
	}

	// what Linux assumes before the stream says otherwise
	uint32 alignment = fCodec == HEVC ? 32 : 16;
	fWidth = ALIGN(width != 0 ? width : 320, alignment);
	fHeight = ALIGN(height != 0 ? height : 240, alignment);
	fCropWidth = width != 0 ? width : fWidth;
	fCropHeight = height != 0 ? height : fHeight;
	fMinCount = 4;

	fSession = sNextSession++;
	uint32 init[5] = { 20, HFI_CMD_SYS_SESSION_INIT, fSession,
		HFI_SESSION_TYPE_DEC,
		(uint32)(fCodec == HEVC ? HFI_VIDEO_CODEC_HEVC : HFI_VIDEO_CODEC_H264) };
	status = _Send(init);
	if (status == B_OK)
		status = _WaitFor(HFI_MSG_SYS_SESSION_INIT);
	if (status != B_OK) {
		_Fail(status, "the firmware did not open a session: %s",
			strerror(status));
		fDevice.Close();
		if (_error != NULL)
			*_error = fError;
		return status;
	}
	fOpen = true;

	status = _ConfigurePorts(true);
	if (status == B_OK) {
		status = _SetInternal(HFI_BUFFER_INTERNAL_PERSIST_1, fPersist,
			_PersistSize());
	}
	if (status == B_OK) {
		status = _SetInternal(HFI_BUFFER_INTERNAL_SCRATCH, fBin,
			_BinSize(fWidth, fHeight));
	}
	if (status == B_OK) {
		status = _SetInternal(HFI_BUFFER_INTERNAL_SCRATCH_1, fScratch1,
			_Scratch1Size(fWidth, fHeight));
	}
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
		status = fDevice.Allocate(input_size(fWidth, fHeight), buffer);
		if (status == B_OK) {
			fInputs.push_back(buffer);
			fInputQueued.push_back(false);
		} else
			_Fail(status, "no memory for the bitstream");
	}

	if (status != B_OK) {
		if (fError.IsEmpty())
			_Fail(status, "cannot start decoding: %s", strerror(status));
		BString error = fError;
		Close();
		fError = error;
		if (_error != NULL)
			*_error = fError;
		return status;
	}
	TRACE("session %#" B_PRIx32 " started, %" B_PRIu32 "x%" B_PRIu32 "\n",
		fSession, fWidth, fHeight);
	return B_OK;
}


void
IrisDecoder::Close()
{
	if (fOpen) {
		if (fStarted) {
			uint32 flush[4] = { 16, HFI_CMD_SESSION_FLUSH, fSession,
				HFI_FLUSH_ALL };
			if (!fFailed && _Send(flush) == B_OK)
				_WaitFor(HFI_MSG_SESSION_FLUSH);
			if (!fFailed)
				_Simple(HFI_CMD_SESSION_STOP, HFI_MSG_SESSION_STOP);
			if (!fFailed) {
				_Simple(HFI_CMD_SESSION_RELEASE_RESOURCES,
					HFI_MSG_SESSION_RELEASE_RESOURCES);
			}
		}
		_Simple(HFI_CMD_SYS_SESSION_END, HFI_MSG_SYS_SESSION_END);
	}

	// the firmware is done with them now (and the driver frees what an
	// open leaves behind when it closes)
	_FreeOutputs();
	for (size_t i = 0; i < fInputs.size(); i++)
		fDevice.Free(fInputs[i]);
	fInputs.clear();
	fInputQueued.clear();
	fDevice.Free(fPersist);
	fDevice.Free(fBin);
	fDevice.Free(fScratch1);
	fDevice.Close();

	fOpen = false;
	fStarted = false;
	fOutputStarted = false;
	fSequenceChanged = false;
	fEndOfStream = false;
	fEnded = false;
	fFailed = false;
	fReady.clear();
	fReadyPts.clear();
	fReadyCorrupt.clear();
}


status_t
IrisDecoder::PutAccessUnit(const uint8* data, size_t size, int64 pts,
	bigtime_t timeout)
{
	status_t status = _Service();
	if (status != B_OK)
		return status;
	if (fEndOfStream)
		return B_NOT_ALLOWED;
	if (size == 0)
		return B_OK;

	bigtime_t deadline = system_time() + timeout;
	int32 index = -1;
	while (true) {
		for (size_t i = 0; i < fInputs.size(); i++) {
			if (!fInputQueued[i]) {
				index = i;
				break;
			}
		}
		if (index >= 0)
			break;
		bigtime_t left = deadline - system_time();
		if (left <= 0)
			return B_WOULD_BLOCK;
		status = _Pump(left);
		if (status == B_OK)
			status = _Service();
		if (status == B_TIMED_OUT)
			return B_WOULD_BLOCK;
		if (status != B_OK)
			return status;
	}

	IrisDevice::Buffer& buffer = fInputs[index];
	if (size > buffer.size)
		return _Fail(B_BAD_VALUE, "an access unit of %zu bytes", size);
	memcpy(buffer.address, data, size);

	uint32 packet[15] = { 60, HFI_CMD_SESSION_EMPTY_BUFFER, fSession,
		(uint32)((uint64)pts >> 32), (uint32)pts, 0, 0, 0, 0,
		(uint32)buffer.size, (uint32)size, (uint32)index,
		buffer.deviceAddress, 0, 0 };
	status = _Send(packet);
	if (status != B_OK)
		return status;
	fInputQueued[index] = true;
	return B_OK;
}


status_t
IrisDecoder::EndOfStream()
{
	status_t status = _Service();
	if (status != B_OK)
		return status;
	if (fEndOfStream)
		return B_OK;

	uint32 packet[15] = { 60, HFI_CMD_SESSION_EMPTY_BUFFER, fSession,
		0, 0, HFI_BUFFERFLAG_EOS, 0, 0, 0, 0, 0, 0xffff, kEndOfStreamAddress,
		0, 0 };
	status = _Send(packet);
	if (status == B_OK)
		fEndOfStream = true;
	return status;
}


status_t
IrisDecoder::Flush()
{
	if (!fOpen || fFailed)
		return B_NO_INIT;
	uint32 flush[4] = { 16, HFI_CMD_SESSION_FLUSH, fSession, HFI_FLUSH_ALL };
	status_t status = _Send(flush);
	if (status == B_OK)
		status = _WaitFor(HFI_MSG_SESSION_FLUSH);
	if (status != B_OK)
		return status;

	// what was not taken yet goes back to the firmware with the rest
	while (!fReady.empty()) {
		fOutputs[fReady.front()].ready = false;
		fReady.pop_front();
	}
	fReadyPts.clear();
	fReadyCorrupt.clear();
	fEndOfStream = false;
	fEnded = false;
	if (!fOutputStarted)
		return B_OK;

	for (size_t i = 0; status == B_OK && i < fReferences.size(); i++) {
		if (!fReferenceQueued[i])
			status = _QueueReference(i);
	}
	for (size_t i = 0; status == B_OK && i < fOutputs.size(); i++) {
		if (!fOutputs[i].queued && !fOutputs[i].held)
			status = _QueueOutput(i);
	}
	return status;
}


status_t
IrisDecoder::NextPicture(Picture& picture, bigtime_t timeout)
{
	status_t status = _Service();
	if (status != B_OK)
		return status;

	bigtime_t deadline = system_time() + timeout;
	while (fReady.empty()) {
		if (fEnded)
			return B_LAST_BUFFER_ERROR;
		bigtime_t left = deadline - system_time();
		status = _Pump(left > 0 ? left : 0);
		if (status == B_OK)
			status = _Service();
		if (status != B_OK)
			return status == B_TIMED_OUT ? B_WOULD_BLOCK : status;
	}

	int32 index = fReady.front();
	Output& output = fOutputs[index];
	output.ready = false;
	output.held = true;
	picture.buffer = index;
	picture.pts = fReadyPts.front();
	picture.corrupt = fReadyCorrupt.front();
	fReady.pop_front();
	fReadyPts.pop_front();
	fReadyCorrupt.pop_front();

	picture.width = fCropWidth;
	picture.height = fCropHeight;
	picture.cropLeft = fCropLeft;
	picture.cropTop = fCropTop;
	picture.stride = fOutputStride;
	picture.lumaRows = fOutputRows;
	picture.data = (const uint8*)output.buffer.address;
	return B_OK;
}


void
IrisDecoder::ReleasePicture(const Picture& picture)
{
	if (picture.buffer < 0 || (size_t)picture.buffer >= fOutputs.size())
		return;
	Output& output = fOutputs[picture.buffer];
	if (!output.held)
		return;
	output.held = false;
	if (!fFailed && !fEnded && fOutputStarted
		&& output.buffer.size == fOutputSize) {
		_QueueOutput(picture.buffer);
	}
}


void
IrisDecoder::CopyPlanes(const Picture& picture, uint8* target, uint32 stride,
	bool interleaved) const
{
	const uint8* luma = picture.data + (size_t)picture.cropTop * picture.stride
		+ picture.cropLeft;
	for (uint32 y = 0; y < picture.height; y++) {
		memcpy(target + (size_t)y * stride, luma + (size_t)y * picture.stride,
			picture.width);
	}

	const uint8* chroma = picture.data
		+ (size_t)picture.lumaRows * picture.stride
		+ (size_t)(picture.cropTop / 2) * picture.stride
		+ (picture.cropLeft & ~1u);
	uint8* targetChroma = target + (size_t)picture.height * stride;
	uint32 chromaWidth = (picture.width + 1) / 2;
	uint32 chromaHeight = (picture.height + 1) / 2;
	if (interleaved) {
		for (uint32 y = 0; y < chromaHeight; y++) {
			memcpy(targetChroma + (size_t)y * stride,
				chroma + (size_t)y * picture.stride, chromaWidth * 2);
		}
		return;
	}

	uint32 chromaStride = stride / 2;
	uint8* cb = targetChroma;
	uint8* cr = targetChroma + (size_t)chromaHeight * chromaStride;
	for (uint32 y = 0; y < chromaHeight; y++) {
		const uint8* source = chroma + (size_t)y * picture.stride;
		uint8* cbRow = cb + (size_t)y * chromaStride;
		uint8* crRow = cr + (size_t)y * chromaStride;
		for (uint32 x = 0; x < chromaWidth; x++) {
			cbRow[x] = source[2 * x];
			crRow[x] = source[2 * x + 1];
		}
	}
}


// #pragma mark - private


status_t
IrisDecoder::_Fail(status_t status, const char* format, ...)
{
	char message[256];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	if (fError.IsEmpty())
		fError = message;
	TRACE("%s\n", message);
	return status;
}


status_t
IrisDecoder::_Send(const uint32* packet)
{
	if (debug_enabled() && packet[1] != HFI_CMD_SESSION_EMPTY_BUFFER
		&& packet[1] != HFI_CMD_SESSION_FILL_BUFFER) {
		TRACE("send %#" B_PRIx32 " (%" B_PRIu32 " bytes)", packet[1],
			packet[0]);
		for (uint32 i = 3; i < packet[0] / 4 && i < 16; i++)
			fprintf(stderr, " %#" B_PRIx32, packet[i]);
		fprintf(stderr, "\n");
	}
	status_t status = fDevice.Send(packet);
	if (status != B_OK)
		return _Fail(status, "cannot send to the firmware: %s",
			strerror(status));
	return B_OK;
}


status_t
IrisDecoder::_SetProperty(uint32 property, const uint32* payload,
	uint32 words)
{
	uint32 packet[32] = { 20 + words * 4, HFI_CMD_SESSION_SET_PROPERTY,
		fSession, 1, property };
	memcpy(packet + 5, payload, words * 4);
	return _Send(packet);
}


status_t
IrisDecoder::_SetProperty(uint32 property, uint32 a)
{
	return _SetProperty(property, &a, 1);
}


status_t
IrisDecoder::_SetProperty(uint32 property, uint32 a, uint32 b)
{
	uint32 payload[2] = { a, b };
	return _SetProperty(property, payload, 2);
}


status_t
IrisDecoder::_SetProperty(uint32 property, uint32 a, uint32 b, uint32 c)
{
	uint32 payload[3] = { a, b, c };
	return _SetProperty(property, payload, 3);
}


status_t
IrisDecoder::_Simple(uint32 command, uint32 answer)
{
	uint32 packet[3] = { 12, command, fSession };
	status_t status = _Send(packet);
	if (status == B_OK)
		status = _WaitFor(answer);
	return status;
}


/*!	Handles messages until one of \a type came, which is copied to \a packet
	(kMessageSize bytes). Sequence events are only noted here, and acted on
	by _Service().
*/
status_t
IrisDecoder::_WaitFor(uint32 type, uint32* packet)
{
	uint32 message[kMessageSize / 4];
	bigtime_t deadline = system_time() + kAnswerTimeout;
	while (true) {
		bigtime_t left = deadline - system_time();
		if (left <= 0)
			break;
		size_t size = 0;
		status_t status = fDevice.Receive(message, sizeof(message), left,
			&size);
		if (status == B_TIMED_OUT || status == B_WOULD_BLOCK)
			break;
		if (status != B_OK)
			return _Fail(status, "cannot receive: %s", strerror(status));
		status = _Handle(message, size);
		if (message[1] == type) {
			if (packet != NULL)
				memcpy(packet, message, size);
			if (size >= 16 && message[3] != 0) {
				return _Fail(B_ERROR, "the firmware answered %#" B_PRIx32
					" with error %#" B_PRIx32, type, message[3]);
			}
			return status;
		}
		if (status != B_OK)
			return status;
	}
	return _Fail(B_TIMED_OUT, "no answer %#" B_PRIx32 " from the firmware",
		type);
}


status_t
IrisDecoder::_Pump(bigtime_t timeout)
{
	uint32 message[kMessageSize / 4];
	size_t size = 0;
	status_t status = fDevice.Receive(message, sizeof(message), timeout,
		&size);
	if (status == B_WOULD_BLOCK)
		return B_TIMED_OUT;
	if (status != B_OK)
		return status;
	return _Handle(message, size);
}


status_t
IrisDecoder::_Handle(const uint32* packet, size_t size)
{
	if (size < 12)
		return B_OK;
	uint32 type = packet[1];
	if (type != HFI_MSG_EVENT_NOTIFY && packet[2] != fSession) {
		TRACE("message %#" B_PRIx32 " of session %#" B_PRIx32 "\n", type,
			packet[2]);
		return B_OK;
	}

	switch (type) {
		case HFI_MSG_SESSION_EMPTY_BUFFER:
		{
			if (size < 36)
				return B_OK;
			uint32 error = packet[3];
			uint32 tag = packet[6];
			if (tag < fInputQueued.size())
				fInputQueued[tag] = false;
			if (error == HFI_ERR_SESSION_UNSUPPORTED_STREAM) {
				fFailed = true;
				return _Fail(B_NOT_SUPPORTED,
					"the firmware does not support the stream");
			}
			if (error != 0)
				TRACE("input %" B_PRIu32 ": error %#" B_PRIx32 "\n", tag, error);
			return B_OK;
		}

		case HFI_MSG_SESSION_FILL_BUFFER:
		{
			if (size < 100)
				return B_OK;
			uint32 stream = packet[3];
			uint32 error = packet[5];
			int64 pts = ((int64)packet[6] << 32) | packet[7];
			uint32 flags = packet[8];
			uint32 filled = packet[13];
			uint32 tag = packet[21];
			TRACE("picture: stream %" B_PRIu32 " tag %" B_PRIu32 " flags %#"
				B_PRIx32 " filled %" B_PRIu32 " offset %" B_PRIu32 " %"
				B_PRIu32 "x%" B_PRIu32 " type %#" B_PRIx32 " error %#" B_PRIx32
				"\n", stream, tag, flags, filled, packet[14], packet[15],
				packet[16], packet[22], error);
			if (stream == 0) {
				if (tag >= kReferenceTag
					&& tag - kReferenceTag < fReferenceQueued.size()) {
					fReferenceQueued[tag - kReferenceTag] = false;
				}
			} else if (tag < fOutputs.size()) {
				Output& output = fOutputs[tag];
				output.queued = false;
				if (filled != 0) {
					output.ready = true;
					fReady.push_back(tag);
					fReadyPts.push_back(pts);
					fReadyCorrupt.push_back((flags & (HFI_BUFFERFLAG_DATACORRUPT
						| HFI_BUFFERFLAG_DROP_FRAME)) != 0 || error != 0);
					fDecoded++;
				}
			}
			if ((flags & HFI_BUFFERFLAG_EOS) != 0)
				fEnded = true;
			return B_OK;
		}

		case HFI_MSG_EVENT_NOTIFY:
			return _HandleEvent(packet, size);

		default:
			TRACE("message %#" B_PRIx32 " (%zu bytes), error %#" B_PRIx32 "\n",
				type, size, size >= 16 ? packet[3] : 0);
			return B_OK;
	}
}


status_t
IrisDecoder::_HandleEvent(const uint32* packet, size_t size)
{
	if (size < 24)
		return B_OK;
	uint32 event = packet[3];
	uint32 data1 = packet[4];
	uint32 data2 = packet[5];
	TRACE("event %#" B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32 "\n", event,
		data1, data2);

	switch (event) {
		case HFI_EVENT_SYS_ERROR:
			fFailed = true;
			return _Fail(B_DEV_NOT_READY, "the codec's firmware failed");

		case HFI_EVENT_SESSION_ERROR:
			switch (data1) {
				case HFI_ERR_SESSION_UNSUPPORTED_SETTING:
				case HFI_ERR_SESSION_UNSUPPORT_BUFFERTYPE:
				case HFI_ERR_SESSION_INVALID_SCALE_FACTOR:
				case HFI_ERR_SESSION_UPSCALE_NOT_SUPPORTED:
					TRACE("session error %#" B_PRIx32 " for command %#"
						B_PRIx32 "\n", data1, data2);
					return B_OK;
			}
			fFailed = true;
			return _Fail(B_ERROR, "session error %#" B_PRIx32
				" (command %#" B_PRIx32 ")", data1, data2);

		case HFI_EVENT_SESSION_SEQUENCE_CHANGED:
			break;

		default:
			return B_OK;
	}

	if (data1 != HFI_EVENT_DATA_SEQUENCE_CHANGED_SUFFICIENT_BUF_RESOURCES
		&& data1 != HFI_EVENT_DATA_SEQUENCE_CHANGED_INSUFFICIENT_BUF_RESOURCES) {
		fFailed = true;
		return _Fail(B_ERROR, "a sequence event %#" B_PRIx32, data1);
	}

	uint32 width = 0, height = 0, bitDepth = 0, progressive = 1;
	uint32 crop[4] = {};
	uint32 minCount = 0;
	const uint8* record = (const uint8*)(packet + 6);
	const uint8* end = (const uint8*)packet + size;
	for (uint32 i = 0; i < data2 && record + 4 <= end; i++) {
		uint32 property;
		memcpy(&property, record, 4);
		const uint32* data = (const uint32*)(record + 4);
		size_t length;
		switch (property) {
			case HFI_PROPERTY_PARAM_FRAME_SIZE:
				length = 12;
				break;
			case HFI_PROPERTY_PARAM_PROFILE_LEVEL_CURRENT:
			case HFI_PROPERTY_PARAM_VDEC_PIXEL_BITDEPTH:
				length = 8;
				break;
			case HFI_PROPERTY_PARAM_VDEC_PIC_STRUCT:
			case HFI_PROPERTY_PARAM_VDEC_COLOUR_SPACE:
			case HFI_PROPERTY_CONFIG_VDEC_ENTROPY:
				length = 4;
				break;
			case HFI_PROPERTY_CONFIG_BUFFER_REQUIREMENTS:
				length = 32;
				break;
			case HFI_INDEX_EXTRADATA_INPUT_CROP:
				length = 28;
				break;
			case HFI_PROPERTY_PARAM_VDEC_DPB_COUNTS:
				length = 20;
				break;
			default:
				TRACE("sequence: unknown record %#" B_PRIx32 "\n", property);
				length = 0;
				break;
		}
		if (length == 0 || record + 4 + length > end)
			break;
		switch (property) {
			case HFI_PROPERTY_PARAM_FRAME_SIZE:
				width = data[1];
				height = data[2];
				break;
			case HFI_PROPERTY_PARAM_VDEC_PIXEL_BITDEPTH:
				bitDepth = data[1];
				break;
			case HFI_PROPERTY_PARAM_VDEC_PIC_STRUCT:
				progressive = data[0];
				break;
			case HFI_PROPERTY_CONFIG_BUFFER_REQUIREMENTS:
			case HFI_PROPERTY_PARAM_VDEC_DPB_COUNTS:
				minCount = data[4];
				break;
			case HFI_INDEX_EXTRADATA_INPUT_CROP:
				memcpy(crop, data + 3, sizeof(crop));
				break;
		}
		if (debug_enabled()) {
			TRACE("sequence: %#" B_PRIx32, property);
			for (size_t word = 0; word < length / 4; word++)
				fprintf(stderr, " %" B_PRIu32, data[word]);
			fprintf(stderr, "\n");
		}
		record += 4 + length;
	}

	if (bitDepth != 0 || progressive == 0) {
		fFailed = true;
		return _Fail(B_NOT_SUPPORTED, "%s pictures are not supported",
			bitDepth != 0 ? "ten bit" : "interlaced");
	}
	if (width == 0 || height == 0) {
		fFailed = true;
		return _Fail(B_ERROR, "a sequence event without a picture size");
	}

	fEventWidth = width;
	fEventHeight = height;
	if (crop[2] != 0 && crop[3] != 0)
		memcpy(fEventCrop, crop, sizeof(crop));
	else {
		fEventCrop[0] = fEventCrop[1] = 0;
		fEventCrop[2] = width;
		fEventCrop[3] = height;
	}
	fEventMinCount = minCount != 0 ? minCount : 4;
	fSequenceChanged = true;
	return B_OK;
}


//!	Acts on a sequence event that came while waiting.
status_t
IrisDecoder::_Service()
{
	if (!fOpen)
		return B_NO_INIT;
	if (fFailed)
		return B_ERROR;
	if (!fSequenceChanged)
		return B_OK;
	fSequenceChanged = false;
	return _StartOutput();
}


status_t
IrisDecoder::_ConfigurePorts(bool input)
{
	uint32 outputWidth = ALIGN(fWidth, 128);
	uint32 outputHeight = ALIGN(fHeight, 32);
	uint32 constraints[10] = { HFI_BUFFER_OUTPUT2, 2, 128, 8192, 32, 256,
		128, 8192, 16, 256 };

	status_t status = B_OK;
	if (input)
		status = _SetProperty(HFI_PROPERTY_CONFIG_VIDEOCORES_USAGE, 1);
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_UNCOMPRESSED_FORMAT_SELECT,
			HFI_BUFFER_OUTPUT, HFI_COLOR_FORMAT_NV12_UBWC);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_UNCOMPRESSED_FORMAT_SELECT,
			HFI_BUFFER_OUTPUT2, HFI_COLOR_FORMAT_NV12);
	}
	if (status == B_OK) {
		status = _SetProperty(
			HFI_PROPERTY_PARAM_UNCOMPRESSED_PLANE_ACTUAL_CONSTRAINTS_INFO,
			constraints, 10);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_BUFFER_COUNT_ACTUAL,
			HFI_BUFFER_INPUT, 64, 64);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_BUFFER_COUNT_ACTUAL,
			HFI_BUFFER_OUTPUT, 64, 64);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_BUFFER_COUNT_ACTUAL,
			HFI_BUFFER_OUTPUT2, fMinCount, fMinCount);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_VDEC_MULTI_STREAM,
			HFI_BUFFER_OUTPUT, 0);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_VDEC_MULTI_STREAM,
			HFI_BUFFER_OUTPUT2, 1);
	}
	if (status == B_OK && input) {
		status = _SetProperty(HFI_PROPERTY_PARAM_FRAME_SIZE, HFI_BUFFER_INPUT,
			fWidth, fHeight);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_FRAME_SIZE,
			HFI_BUFFER_OUTPUT2, outputWidth, outputHeight);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_BUFFER_SIZE_ACTUAL,
			HFI_BUFFER_OUTPUT, ubwc_size(outputWidth, outputHeight));
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_BUFFER_SIZE_ACTUAL,
			HFI_BUFFER_OUTPUT2, nv12_size(outputWidth, outputHeight));
	}
	if (status == B_OK && input) {
		uint32 mbs = DIV_ROUND_UP(fHeight, 16) * DIV_ROUND_UP(fWidth, 16);
		bool small = mbs < 3600 && fWidth < 1280 && fHeight < 1280;
		status = _SetProperty(HFI_PROPERTY_PARAM_WORK_MODE, small ? 1 : 2);
		if (status == B_OK)
			status = _SetProperty(HFI_PROPERTY_PARAM_WORK_ROUTE, kVppPipes);
	}
	return status;
}


status_t
IrisDecoder::_SetInternal(uint32 type, IrisDevice::Buffer& buffer,
	size_t size)
{
	status_t status = fDevice.Allocate(size, buffer);
	if (status != B_OK) {
		return _Fail(status, "no memory for a %zu byte codec buffer: %s",
			size, strerror(status));
	}
	uint32 packet[9] = { 36, HFI_CMD_SESSION_SET_BUFFERS, fSession, type,
		(uint32)buffer.size, 0, (uint32)buffer.size, 1,
		buffer.deviceAddress };
	return _Send(packet);
}


status_t
IrisDecoder::_ReleaseInternal(uint32 type, IrisDevice::Buffer& buffer)
{
	if (buffer.deviceAddress == 0)
		return B_OK;
	uint32 packet[9] = { 36, HFI_CMD_SESSION_RELEASE_BUFFERS, fSession, type,
		(uint32)buffer.size, 0, 1, 1, buffer.deviceAddress };
	status_t status = _Send(packet);
	if (status == B_OK)
		status = _WaitFor(HFI_MSG_SESSION_RELEASE_BUFFERS);
	if (status == B_OK)
		fDevice.Free(buffer);
	return status;
}


/*!	After the firmware's sequence event: the output side for the stream's
	real geometry, the scratch buffers again, and the firmware continues
	(Linux' iris_vdec_streamon_output()).
*/
status_t
IrisDecoder::_StartOutput()
{
	uint32 width = fEventWidth;
	uint32 height = fEventHeight;
	TRACE("sequence: %" B_PRIu32 "x%" B_PRIu32 ", crop %" B_PRIu32 ",%"
		B_PRIu32 " %" B_PRIu32 "x%" B_PRIu32 ", %" B_PRIu32 " buffers\n",
		width, height, fEventCrop[0], fEventCrop[1], fEventCrop[2],
		fEventCrop[3], fEventMinCount);

	if (fOutputStarted) {
		// the same geometry again (a new sequence header): go on
		if (width == fWidth && height == fHeight
			&& fEventMinCount <= fMinCount) {
			uint32 packet[3] = { 12, HFI_CMD_SESSION_CONTINUE, fSession };
			return _Send(packet);
		}
		fFailed = true;
		return _Fail(B_NOT_SUPPORTED,
			"the picture size changes within the stream");
	}

	fWidth = width;
	fHeight = height;
	fCropLeft = fEventCrop[0];
	fCropTop = fEventCrop[1];
	fCropWidth = fEventCrop[2];
	fCropHeight = fEventCrop[3];
	fMinCount = fEventMinCount;

	uint32 outputWidth = ALIGN(width, 128);
	uint32 outputHeight = ALIGN(height, 32);
	fOutputStride = outputWidth;
	fOutputRows = outputHeight;
	fOutputSize = nv12_size(outputWidth, outputHeight);
	fReferenceSize = ubwc_size(outputWidth, outputHeight);

	status_t status = _ConfigurePorts(false);

	for (uint32 i = 0; status == B_OK && i < fMinCount; i++) {
		IrisDevice::Buffer buffer;
		status = fDevice.Allocate(fReferenceSize, buffer);
		if (status == B_OK) {
			fReferences.push_back(buffer);
			fReferenceQueued.push_back(false);
		} else
			_Fail(status, "no memory for the reference pictures");
	}

	if (status == B_OK)
		status = _ReleaseInternal(HFI_BUFFER_INTERNAL_SCRATCH, fBin);
	if (status == B_OK)
		status = _ReleaseInternal(HFI_BUFFER_INTERNAL_SCRATCH_1, fScratch1);
	if (status == B_OK) {
		status = _SetInternal(HFI_BUFFER_INTERNAL_SCRATCH, fBin,
			_BinSize(width, height));
	}
	if (status == B_OK) {
		status = _SetInternal(HFI_BUFFER_INTERNAL_SCRATCH_1, fScratch1,
			_Scratch1Size(width, height));
	}
	if (status == B_OK) {
		uint32 mbs = DIV_ROUND_UP(height, 16) * DIV_ROUND_UP(width, 16);
		bool small = mbs < 3600 && width < 1280 && height < 1280;
		status = _SetProperty(HFI_PROPERTY_PARAM_WORK_MODE, small ? 1 : 2);
	}
	if (status == B_OK)
		status = _SetProperty(HFI_PROPERTY_PARAM_WORK_ROUTE, kVppPipes);
	if (status == B_OK) {
		uint32 packet[3] = { 12, HFI_CMD_SESSION_CONTINUE, fSession };
		status = _Send(packet);
	}

	for (size_t i = 0; status == B_OK && i < fReferences.size(); i++)
		status = _QueueReference(i);

	for (uint32 i = 0; status == B_OK && i < fMinCount + kExtraOutputs; i++) {
		Output output = {};
		status = fDevice.Allocate(fOutputSize, output.buffer);
		if (status != B_OK) {
			_Fail(status, "no memory for the pictures");
			break;
		}
		fOutputs.push_back(output);
		status = _QueueOutput(fOutputs.size() - 1);
	}

	if (status != B_OK) {
		fFailed = true;
		return status;
	}
	fOutputStarted = true;
	return B_OK;
}


status_t
IrisDecoder::_QueueOutput(int32 index)
{
	Output& output = fOutputs[index];
	uint32 packet[11] = { 44, HFI_CMD_SESSION_FILL_BUFFER, fSession, 1, 0,
		(uint32)output.buffer.size, 0, (uint32)index,
		output.buffer.deviceAddress, 0, 0 };
	status_t status = _Send(packet);
	if (status == B_OK)
		output.queued = true;
	return status;
}


status_t
IrisDecoder::_QueueReference(int32 index)
{
	IrisDevice::Buffer& buffer = fReferences[index];
	uint32 packet[11] = { 44, HFI_CMD_SESSION_FILL_BUFFER, fSession, 0, 0,
		(uint32)buffer.size, 0, kReferenceTag + index, buffer.deviceAddress,
		0, 0 };
	status_t status = _Send(packet);
	if (status == B_OK)
		fReferenceQueued[index] = true;
	return status;
}


void
IrisDecoder::_FreeOutputs()
{
	for (size_t i = 0; i < fOutputs.size(); i++)
		fDevice.Free(fOutputs[i].buffer);
	fOutputs.clear();
	for (size_t i = 0; i < fReferences.size(); i++)
		fDevice.Free(fReferences[i]);
	fReferences.clear();
	fReferenceQueued.clear();
}


size_t
IrisDecoder::_BinSize(uint32 width, uint32 height) const
{
	if (fCodec == HEVC)
		return bin_size(ALIGN(width, 16), ALIGN(height, 16), 2, 2);
	return bin_size(ALIGN(width, 16), ALIGN(height, 16), 1, 3);
}


size_t
IrisDecoder::_Scratch1Size(uint32 width, uint32 height) const
{
	if (fCodec == HEVC) {
		return comv_hevc(width, height, kComvBuffers)
			+ non_comv_hevc(width, height) + line_hevc(width, height);
	}
	return comv_h264(width, height, kComvBuffers)
		+ non_comv_h264(width, height) + line_h264(width, height);
}


size_t
IrisDecoder::_PersistSize() const
{
	if (fCodec == HEVC) {
		const uint32 tiles = 128 * 32 + 1;
		return ALIGN((1u << 10) * (80 + 20) + 48 * 3072 + tiles * 4
			+ kHwPictureBuffers * 4096, kDmaAlignment);
	}
	return ALIGN(512 * (256 + 32) + 3328 * 66 + kHwPictureBuffers * 4096,
		kDmaAlignment);
}
