/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "IrisDecoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/*	The packets are those of Linux' iris driver (iris_hfi_gen1_defines.h)
	and the order it sends them in for SM8250, whose firmware the SC8280XP
	runs; the buffer sizes are its iris_vpu_buffer.c's. */

#define HFI_PROPERTY_PARAM_WORK_ROUTE		0x1017
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

#define HFI_COLOR_FORMAT_P010				0x4003
#define HFI_COLOR_FORMAT_YUV420_TP10_UBWC	0xc002
#define HFI_BITDEPTH_8						0
#define HFI_BITDEPTH_10						2



#define HFI_EVENT_DATA_SEQUENCE_CHANGED_SUFFICIENT_BUF_RESOURCES 0x1000001
#define HFI_EVENT_DATA_SEQUENCE_CHANGED_INSUFFICIENT_BUF_RESOURCES 0x1000002
#define HFI_EVENT_SESSION_SEQUENCE_CHANGED	0x1000003



// references are tagged from here, the NV12 buffers from 0
static const uint32 kReferenceTag = 64;
static const uint32 kInputBuffers = 4;
// beyond the firmware's minimum: what the caller holds and what waits
// to be taken
static const uint32 kExtraOutputs = 4;


#define ALIGN(x, a)			((((x) + (a) - 1) / (a)) * (a))
#define DIV_ROUND_UP(x, a)	(((x) + (a) - 1) / (a))


#define TRACE IRIS_TRACE


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


//!	VP9 and MPEG-2's line buffers (venus' hfi_plat_bufs_v6.c).
static uint32
vpx_left_control(uint32 height)
{
	uint32 size = ((height + 15) >> 4) * 64;
	if (size < ((height + 31) >> 5) * 64)
		size = ((height + 31) >> 5) * 64;
	if (size < ((height + 63) >> 6) * 64)
		size = ((height + 63) >> 6) * 64;
	return size;
}


static uint32
vpx_lines(uint32 width, uint32 height, bool vp9)
{
	uint32 alignedWidth = ALIGN(ALIGN(width, 16), 64);
	uint32 seLeftControl = ((height + 15) >> 4) * 16;
	uint32 vspTop = vp9 ? (alignedWidth >> 6) * 64 * 8 + 256
		: (ALIGN(width, 16) >> 4) * 64 / 2 + 256;
	uint32 peTopData = vp9 ? (alignedWidth >> 6) * 176
		: (ALIGN(width, 16) >> 4) * 64;
	uint32 feTopData = vp9 ? (alignedWidth + 8) * 10 * 2
		: (ALIGN(width, 16) + 8) * 10 * 2;
	return ALIGN(vpx_left_control(height), kDmaAlignment) * kVppPipes
		+ ALIGN(seLeftControl, kDmaAlignment) * kVppPipes
		+ ALIGN(vspTop, kDmaAlignment)
		+ ALIGN((ALIGN(width, 64) + 8) * 10 * 2, kDmaAlignment)
		+ 2 * ALIGN(ALIGN((ALIGN(height, 16) / 2) * 64, 32), kDmaAlignment)
		+ ALIGN(((width + 15) >> 4) * 64, kDmaAlignment)
		+ ALIGN(peTopData, kDmaAlignment)
		+ ALIGN(feTopData, kDmaAlignment);
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


//!	The P010 copies: as NV12's, with samples of two bytes (venus' helpers.c).
static uint32
p010_size(uint32 width, uint32 height)
{
	uint32 stride = ALIGN(width * 2, 128);
	return ALIGN(stride * ALIGN(height, 32)
		+ stride * ALIGN((height + 1) >> 1, 16), 4096);
}


//!	Ten bit references: three samples in four bytes (TP10 UBWC, venus').
static uint32
tp10_ubwc_size(uint32 width, uint32 height)
{
	uint32 stride = ALIGN(width * 4 / 3, 256);
	uint32 yPlane = ALIGN(stride * ALIGN(height, 16), 4096);
	uint32 uvPlane = ALIGN(stride * ALIGN((height + 1) >> 1, 16), 4096);
	uint32 yMeta = ALIGN(ALIGN(DIV_ROUND_UP(width, 48), 64)
		* ALIGN(DIV_ROUND_UP(height, 4), 16), 4096);
	uint32 uvMeta = ALIGN(ALIGN(DIV_ROUND_UP((width + 1) >> 1, 24), 64)
		* ALIGN(DIV_ROUND_UP((height + 1) >> 1, 4), 16), 4096);
	uint32 extradata = 16 * 1024 + 8 * 1024;
	if (extradata < stride * 48)
		extradata = stride * 48;
	return ALIGN(yPlane + uvPlane + yMeta + uvMeta + extradata, 4096);
}


//!	The references: luma meta data and samples, then chroma's (UBWC).
static uint32
ubwc_size(uint32 width, uint32 height, bool interlaced = false)
{
	// Fields are two of those, each with its own meta data (Qualcomm's
	// msm_media_info.h), somewhat more than a frame of the same size; a
	// reference too small for them took the SoC down.
	if (interlaced)
		return 2 * ubwc_size(width, (height + 1) / 2) + 64 * 1024;

	uint32 yMetaStride = ALIGN(DIV_ROUND_UP(width, 32), 64);
	uint32 yMeta = ALIGN(yMetaStride * ALIGN(DIV_ROUND_UP(height, 8), 16),
		4096);
	uint32 yPlane = ALIGN(ALIGN(width, 128) * ALIGN(height, 32), 4096);
	uint32 uvMetaStride = ALIGN(DIV_ROUND_UP(width / 2, 16), 64);
	uint32 uvMeta = ALIGN(uvMetaStride
		* ALIGN(DIV_ROUND_UP(height / 2, 8), 16), 4096);
	uint32 uvPlane = ALIGN(ALIGN(width, 128) * ALIGN(height / 2, 32), 4096);
	// and the firmware's extra data behind them (venus' helpers.c; 4K VP9
	// hung the firmware after the first picture without it)
	uint32 extradata = 16 * 1024;
	if (extradata < ALIGN(width, 128) * 48)
		extradata = ALIGN(width, 128) * 48;
	return ALIGN(yMeta + yPlane + uvMeta + uvPlane + extradata, 4096);
}


//!	A bitstream buffer: half a 4K picture's samples (VP9: all), up to 4K.
static uint32
input_size(uint32 width, uint32 height, bool vp9)
{
	uint32 mbs = DIV_ROUND_UP(height, 16) * DIV_ROUND_UP(width, 16);
	uint32 mbs4k = DIV_ROUND_UP(4096, 16) * DIV_ROUND_UP(2304, 16);
	uint32 base = mbs4k;
	uint32 divisor = vp9 ? 1 : 2;
	if (mbs > mbs4k) {
		divisor = 4;
		base = 138240;
	}
	return ALIGN(base * 256 * 3 / 2 / divisor, 4096);
}


/*!	Writes the lines of \a size bytes at \a address back to memory (to the
	point of coherency), for the codec to read.
*/
static void
clean_cache(const void* address, size_t size)
{
#if defined(__aarch64__)
	const size_t kLine = 64;
	uintptr_t line = (uintptr_t)address & ~(uintptr_t)(kLine - 1);
	for (uintptr_t end = (uintptr_t)address + size; line < end; line += kLine)
		__asm__ __volatile__("dc cvac, %0" : : "r"(line) : "memory");
	__asm__ __volatile__("dsb sy" : : : "memory");
#else
	(void)address;
	(void)size;
#endif
}


// #pragma mark - IrisDecoder


IrisDecoder::IrisDecoder(codec codec)
	:
	fCodec(codec),
	fWidth(0),
	fHeight(0),
	fCropLeft(0),
	fCropTop(0),
	fCropWidth(0),
	fCropHeight(0),
	fMinCount(4),
	fTenBit(false),
	fInterlaced(false),
	fOutputStride(0),
	fOutputRows(0),
	fOutputSize(0),
	fReferenceSize(0),
	fSequenceChanged(false),
	fOutputStarted(false),
	fEventWidth(0),
	fEventHeight(0),
	fEventMinCount(0),
	fEventTenBit(false),
	fEventInterlaced(false),
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
	// What Linux assumes before the stream says otherwise, whatever the
	// container says: then the stream's sequence event always finds too
	// little room ("insufficient resources") and the firmware waits for
	// the output side. Opened at the stream's own size, it said
	// "sufficient" and VP9 went on decoding while its scratch buffers were
	// replaced: it stopped for good after a few pictures, now and then.
	(void)width;
	(void)height;
	uint32 alignment = fCodec == HEVC ? 32 : 16;
	static const uint32 kSessionCodecs[] = { HFI_VIDEO_CODEC_H264,
		HFI_VIDEO_CODEC_HEVC, HFI_VIDEO_CODEC_VP9, HFI_VIDEO_CODEC_MPEG2 };
	fWidth = ALIGN(320, alignment);
	fHeight = ALIGN(240, alignment);
	fCropWidth = fWidth;
	fCropHeight = fHeight;
	fMinCount = 4;
	fTenBit = false;
	fInterlaced = false;

	status_t status = _OpenSession(HFI_SESSION_TYPE_DEC,
		kSessionCodecs[fCodec]);
	if (status != B_OK) {
		if (_error != NULL)
			*_error = fError;
		return status;
	}

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
		status = fDevice.Allocate(input_size(fWidth, fHeight, fCodec == VP9),
			buffer);
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
	if (fOpen && fStarted && !fFailed)
		_Flush(HFI_FLUSH_ALL);
	_EndSession();

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
		if (!fReady.empty())
			return B_WOULD_BLOCK;
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
	// The buffers are mapped write-back as the codec is said to be
	// coherent, yet through the Media Kit VP9 stopped for good after the
	// same few pictures every time, where a lab tool with a larger working
	// set (that evicts the lines) never did.
	clean_cache(buffer.address, size);
	TRACE("input %" B_PRId32 ": %zu bytes, pts %" B_PRId64 ", %02x%02x..%02x\n",
		index, size, pts, data[0], size > 1 ? data[1] : 0, data[size - 1]);

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
		0, 0, HFI_BUFFERFLAG_EOS, 0, 0, 0, 0, 0, 0xffff, HFI_END_OF_STREAM_ADDRESS,
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
	status_t status = _Flush(HFI_FLUSH_ALL);
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
	picture.tenBit = fTenBit;
	picture.interlaced = fInterlaced;
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


/*!	A sample as eight bits: P010's sixteen bit samples have the ten in the
	top, so that is their high byte. */
static inline uint8
sample8(const uint8* row, uint32 x)
{
	return row[x];
}


static inline uint8
sample8(const uint16* row, uint32 x)
{
	return (uint8)(row[x] >> 8);
}


template<typename Sample>
static void
copy_planes(const IrisDecoder::Picture& picture, uint8* target, uint32 stride,
	bool interleaved)
{
	const uint8* luma = picture.data + (size_t)picture.cropTop * picture.stride
		+ picture.cropLeft * sizeof(Sample);
	for (uint32 y = 0; y < picture.height; y++) {
		const Sample* source
			= (const Sample*)(luma + (size_t)y * picture.stride);
		uint8* row = target + (size_t)y * stride;
		if (sizeof(Sample) == 1)
			memcpy(row, source, picture.width);
		else {
			for (uint32 x = 0; x < picture.width; x++)
				row[x] = sample8(source, x);
		}
	}

	const uint8* chroma = picture.data
		+ (size_t)picture.lumaRows * picture.stride
		+ (size_t)(picture.cropTop / 2) * picture.stride
		+ (picture.cropLeft & ~1u) * sizeof(Sample);
	uint8* targetChroma = target + (size_t)picture.height * stride;
	uint32 chromaWidth = (picture.width + 1) / 2;
	uint32 chromaHeight = (picture.height + 1) / 2;
	if (interleaved) {
		for (uint32 y = 0; y < chromaHeight; y++) {
			const Sample* source
				= (const Sample*)(chroma + (size_t)y * picture.stride);
			uint8* row = targetChroma + (size_t)y * stride;
			if (sizeof(Sample) == 1)
				memcpy(row, source, chromaWidth * 2);
			else {
				for (uint32 x = 0; x < chromaWidth * 2; x++)
					row[x] = sample8(source, x);
			}
		}
		return;
	}

	uint32 chromaStride = stride / 2;
	uint8* cb = targetChroma;
	uint8* cr = targetChroma + (size_t)chromaHeight * chromaStride;
	for (uint32 y = 0; y < chromaHeight; y++) {
		const Sample* source
			= (const Sample*)(chroma + (size_t)y * picture.stride);
		uint8* cbRow = cb + (size_t)y * chromaStride;
		uint8* crRow = cr + (size_t)y * chromaStride;
		for (uint32 x = 0; x < chromaWidth; x++) {
			cbRow[x] = sample8(source, 2 * x);
			crRow[x] = sample8(source, 2 * x + 1);
		}
	}
}


void
IrisDecoder::CopyPlanes(const Picture& picture, uint8* target, uint32 stride,
	bool interleaved) const
{
	if (picture.tenBit)
		copy_planes<uint16>(picture, target, stride, interleaved);
	else
		copy_planes<uint8>(picture, target, stride, interleaved);
}


template<typename Sample>
static void
copy_half_planes(const IrisDecoder::Picture& picture, uint8* target,
	uint32 stride)
{
	// the sum of four samples, as eight bits
	const uint32 shift = sizeof(Sample) == 1 ? 2 : 10;
	const uint32 round = 1u << (shift - 1);

	uint32 width = picture.width / 2;
	uint32 height = picture.height / 2;
	const uint8* luma = picture.data + (size_t)picture.cropTop * picture.stride
		+ picture.cropLeft * sizeof(Sample);
	for (uint32 y = 0; y < height; y++) {
		const Sample* top
			= (const Sample*)(luma + (size_t)(2 * y) * picture.stride);
		const Sample* bottom
			= (const Sample*)((const uint8*)top + picture.stride);
		uint8* row = target + (size_t)y * stride;
		for (uint32 x = 0; x < width; x++) {
			row[x] = (uint8)(((uint32)top[2 * x] + top[2 * x + 1]
				+ bottom[2 * x] + bottom[2 * x + 1] + round) >> shift);
		}
	}

	const uint8* chroma = picture.data
		+ (size_t)picture.lumaRows * picture.stride
		+ (size_t)(picture.cropTop / 2) * picture.stride
		+ (picture.cropLeft & ~1u) * sizeof(Sample);
	uint32 chromaStride = stride / 2;
	uint8* cb = target + (size_t)height * stride;
	uint8* cr = cb + (size_t)(height / 2) * chromaStride;
	for (uint32 y = 0; y < height / 2; y++) {
		const Sample* top
			= (const Sample*)(chroma + (size_t)(2 * y) * picture.stride);
		const Sample* bottom
			= (const Sample*)((const uint8*)top + picture.stride);
		uint8* cbRow = cb + (size_t)y * chromaStride;
		uint8* crRow = cr + (size_t)y * chromaStride;
		for (uint32 x = 0; x < width / 2; x++) {
			cbRow[x] = (uint8)(((uint32)top[4 * x] + top[4 * x + 2]
				+ bottom[4 * x] + bottom[4 * x + 2] + round) >> shift);
			crRow[x] = (uint8)(((uint32)top[4 * x + 1] + top[4 * x + 3]
				+ bottom[4 * x + 1] + bottom[4 * x + 3] + round) >> shift);
		}
	}
}


void
IrisDecoder::CopyHalfPlanes(const Picture& picture, uint8* target,
	uint32 stride) const
{
	if (picture.tenBit)
		copy_half_planes<uint16>(picture, target, stride);
	else
		copy_half_planes<uint8>(picture, target, stride);
}


void
IrisDecoder::CopyP010(const Picture& picture, uint8* target, uint32 stride,
	bool half) const
{
	if (!picture.tenBit)
		return;

	const uint8* luma = picture.data + (size_t)picture.cropTop * picture.stride
		+ picture.cropLeft * 2;
	const uint8* chroma = picture.data
		+ (size_t)picture.lumaRows * picture.stride
		+ (size_t)(picture.cropTop / 2) * picture.stride
		+ (picture.cropLeft & ~1u) * 2;

	if (!half) {
		for (uint32 y = 0; y < picture.height; y++) {
			memcpy(target + (size_t)y * stride,
				luma + (size_t)y * picture.stride, picture.width * 2);
		}
		uint8* targetChroma = target + (size_t)picture.height * stride;
		uint32 chromaHeight = (picture.height + 1) / 2;
		uint32 chromaBytes = ((picture.width + 1) / 2) * 4;
		for (uint32 y = 0; y < chromaHeight; y++) {
			memcpy(targetChroma + (size_t)y * stride,
				chroma + (size_t)y * picture.stride, chromaBytes);
		}
		return;
	}

	uint32 width = picture.width / 2;
	uint32 height = picture.height / 2;
	for (uint32 y = 0; y < height; y++) {
		const uint16* top
			= (const uint16*)(luma + (size_t)(2 * y) * picture.stride);
		const uint16* bottom
			= (const uint16*)((const uint8*)top + picture.stride);
		uint16* row = (uint16*)(target + (size_t)y * stride);
		for (uint32 x = 0; x < width; x++) {
			row[x] = (uint16)(((uint32)top[2 * x] + top[2 * x + 1]
				+ bottom[2 * x] + bottom[2 * x + 1] + 2) >> 2);
		}
	}
	uint8* targetChroma = target + (size_t)height * stride;
	for (uint32 y = 0; y < height / 2; y++) {
		const uint16* top
			= (const uint16*)(chroma + (size_t)(2 * y) * picture.stride);
		const uint16* bottom
			= (const uint16*)((const uint8*)top + picture.stride);
		uint16* row = (uint16*)(targetChroma + (size_t)y * stride);
		for (uint32 x = 0; x < width / 2; x++) {
			// Cb and Cr of a 2x2 block of pairs
			row[2 * x] = (uint16)(((uint32)top[4 * x] + top[4 * x + 2]
				+ bottom[4 * x] + bottom[4 * x + 2] + 2) >> 2);
			row[2 * x + 1] = (uint16)(((uint32)top[4 * x + 1]
				+ top[4 * x + 3] + bottom[4 * x + 1] + bottom[4 * x + 3]
				+ 2) >> 2);
		}
	}
}


static inline uint8
clamp_sample(int32 value)
{
	return value < 0 ? 0 : value > 255 ? 255 : (uint8)value;
}


template<typename Sample>
static void
copy_packed(const IrisDecoder::Picture& picture, uint8* target, uint32 stride,
	color_space space)
{
	// luma scale (255/219), then Cr to R, Cb to G, Cr to G, Cb to B with
	// studio range chroma (255/224), all in 1/64 (as sunxi_cedar)
	static const int16 kBt601[] = { 75, 102, 25, 52, 129 };
	static const int16 kBt709[] = { 75, 115, 14, 34, 135 };
	const int16* matrix = picture.height >= 720 ? kBt709 : kBt601;

	const uint8* luma = picture.data + (size_t)picture.cropTop * picture.stride
		+ picture.cropLeft * sizeof(Sample);
	const uint8* chroma = picture.data
		+ (size_t)picture.lumaRows * picture.stride
		+ (size_t)(picture.cropTop / 2) * picture.stride
		+ (picture.cropLeft & ~1u) * sizeof(Sample);
	for (uint32 y = 0; y < picture.height; y++) {
		const Sample* lumaRow
			= (const Sample*)(luma + (size_t)y * picture.stride);
		const Sample* chromaRow
			= (const Sample*)(chroma + (size_t)(y / 2) * picture.stride);
		uint8* row = target + (size_t)y * stride;
		if (space == B_YCbCr422) {
			for (uint32 x = 0; x < picture.width / 2; x++) {
				row[4 * x] = sample8(lumaRow, 2 * x);
				row[4 * x + 1] = sample8(chromaRow, 2 * x);
				row[4 * x + 2] = sample8(lumaRow, 2 * x + 1);
				row[4 * x + 3] = sample8(chromaRow, 2 * x + 1);
			}
			continue;
		}
		for (uint32 x = 0; x < picture.width; x++) {
			int32 value = ((int32)sample8(lumaRow, x) - 16) * matrix[0];
			int32 cb = (int32)sample8(chromaRow, x & ~1u) - 128;
			int32 cr = (int32)sample8(chromaRow, (x & ~1u) + 1) - 128;
			row[4 * x + 2] = clamp_sample((value + matrix[1] * cr + 32) >> 6);
			row[4 * x + 1] = clamp_sample((value - matrix[2] * cb
				- matrix[3] * cr + 32) >> 6);
			row[4 * x] = clamp_sample((value + matrix[4] * cb + 32) >> 6);
			row[4 * x + 3] = 255;
		}
	}
}


void
IrisDecoder::CopyPacked(const Picture& picture, uint8* target, uint32 stride,
	color_space space) const
{
	if (picture.tenBit)
		copy_packed<uint16>(picture, target, stride, space);
	else
		copy_packed<uint8>(picture, target, stride, space);
}


// #pragma mark - private


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
				if (filled != 0 && (flags & HFI_BUFFERFLAG_DROP_FRAME) != 0
					&& (flags & HFI_BUFFERFLAG_EOS) == 0) {
					// not to be shown (VP9's hidden frames, the parts of a
					// superframe before the last), as venus has it
					if (fOutputStarted && !fFailed)
						_QueueOutput(tag);
				} else if (filled != 0) {
					output.ready = true;
					fReady.push_back(tag);
					fReadyPts.push_back(pts);
					fReadyCorrupt.push_back(
						(flags & HFI_BUFFERFLAG_DATACORRUPT) != 0 || error != 0);
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

	if (event != HFI_EVENT_SESSION_SEQUENCE_CHANGED)
		return _HandleErrorEvent(packet, size);

	if (data1 != HFI_EVENT_DATA_SEQUENCE_CHANGED_SUFFICIENT_BUF_RESOURCES
		&& data1 != HFI_EVENT_DATA_SEQUENCE_CHANGED_INSUFFICIENT_BUF_RESOURCES) {
		fFailed = true;
		return _Fail(B_ERROR, "a sequence event %#" B_PRIx32, data1);
	}

	uint32 width = 0, height = 0, bitDepth = 0, progressive = 1;
	uint32 crop[4] = {};
	uint32 minCount = 0;
	uint32 maxDpbCount = 0;
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
				minCount = data[4];
				break;
			case HFI_PROPERTY_PARAM_VDEC_DPB_COUNTS:
				// max_dpb_count, max_ref_frames, max_dec_buffering,
				// max_reorder_frames, fw_min_count
				maxDpbCount = data[0];
				minCount = data[4];
				break;
			case HFI_INDEX_EXTRADATA_INPUT_CROP:
				memcpy(crop, data + 3, sizeof(crop));
				break;
		}
		if (iris_debug_enabled()) {
			TRACE("sequence: %#" B_PRIx32, property);
			for (size_t word = 0; word < length / 4; word++)
				fprintf(stderr, " %" B_PRIu32, data[word]);
			fprintf(stderr, "\n");
		}
		record += 4 + length;
	}

	// luma's depth in the low half, chroma's in the high one
	if (bitDepth == (HFI_BITDEPTH_10 << 16 | HFI_BITDEPTH_10))
		bitDepth = HFI_BITDEPTH_10;
	if (bitDepth != HFI_BITDEPTH_8 && bitDepth != HFI_BITDEPTH_10) {
		fFailed = true;
		return _Fail(B_NOT_SUPPORTED,
			"mixed or nine bit pictures are not supported");
	}
	// The firmware stopped answering (a system error, for good) when it
	// went on with fields, even with references of the size Qualcomm gives
	// fields; IRIS_INTERLACED lets the lab try again.
	if (progressive == 0 && getenv("IRIS_INTERLACED") == NULL) {
		fFailed = true;
		return _Fail(B_NOT_SUPPORTED, "interlaced pictures are not supported");
	}
	if (width == 0 || height == 0) {
		fFailed = true;
		return _Fail(B_ERROR, "a sequence event without a picture size");
	}
	// Larger VP9 kept references without end: with 12 the firmware stopped
	// after one 4K picture, with 24 after four, and then answered nothing
	// any more (asked for 64, 850 MB). 1080p decodes with 12.
	if (fCodec == VP9 && (width > 1920 || height > 1088)) {
		fFailed = true;
		return _Fail(B_NOT_SUPPORTED, "VP9 larger than 1080p is not supported");
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
	// VP9 asks for 64 (850 MB of references at 4K); its eight reference
	// slots and the picture being decoded need a few more than eight
	if (fCodec == VP9 && maxDpbCount != 0 && fEventMinCount > maxDpbCount + 4)
		fEventMinCount = maxDpbCount + 4;

	fEventTenBit = bitDepth == HFI_BITDEPTH_10;
	// (HFI_INTERLACE_FRAME_PROGRESSIVE, or 0: fields may come)
	fEventInterlaced = progressive == 0;
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
			HFI_BUFFER_OUTPUT, fTenBit ? HFI_COLOR_FORMAT_YUV420_TP10_UBWC
				: HFI_COLOR_FORMAT_NV12_UBWC);
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_UNCOMPRESSED_FORMAT_SELECT,
			HFI_BUFFER_OUTPUT2,
			fTenBit ? HFI_COLOR_FORMAT_P010 : HFI_COLOR_FORMAT_NV12);
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
			HFI_BUFFER_OUTPUT, fTenBit ? tp10_ubwc_size(outputWidth, outputHeight)
				: ubwc_size(outputWidth, outputHeight, fInterlaced));
	}
	if (status == B_OK) {
		status = _SetProperty(HFI_PROPERTY_PARAM_BUFFER_SIZE_ACTUAL,
			HFI_BUFFER_OUTPUT2, fTenBit ? p010_size(outputWidth, outputHeight)
				: nv12_size(outputWidth, outputHeight));
	}
	if (status == B_OK && input) {
		uint32 mbs = DIV_ROUND_UP(fHeight, 16) * DIV_ROUND_UP(fWidth, 16);
		bool small = mbs < 3600 && fWidth < 1280 && fHeight < 1280;
		status = _SetProperty(HFI_PROPERTY_PARAM_WORK_MODE,
			small || fCodec == MPEG2 ? 1 : 2);
		if (status == B_OK)
			status = _SetProperty(HFI_PROPERTY_PARAM_WORK_ROUTE, kVppPipes);
	}
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
			&& fEventMinCount <= fMinCount && fEventTenBit == fTenBit
			&& fEventInterlaced == fInterlaced) {
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
	fTenBit = fEventTenBit;
	fInterlaced = fEventInterlaced;

	uint32 outputWidth = ALIGN(width, 128);
	uint32 outputHeight = ALIGN(height, 32);
	fOutputStride = fTenBit ? ALIGN(outputWidth * 2, 128) : outputWidth;
	fOutputRows = outputHeight;
	fOutputSize = fTenBit ? p010_size(outputWidth, outputHeight)
		: nv12_size(outputWidth, outputHeight);
	fReferenceSize = fTenBit ? tp10_ubwc_size(outputWidth, outputHeight)
		: ubwc_size(outputWidth, outputHeight, fInterlaced);

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
		// fields only in the single stage mode (venus' helpers.c)
		uint32 mbs = DIV_ROUND_UP(height, 16) * DIV_ROUND_UP(width, 16);
		bool small = mbs < 3600 && width < 1280 && height < 1280;
		status = _SetProperty(HFI_PROPERTY_PARAM_WORK_MODE,
			small || fInterlaced || fCodec == MPEG2 ? 1 : 2);
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
	TRACE("output %" B_PRId32 " queued\n", index);
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
	// fields are decoded in one stage, without (venus' *_scratch_size())
	if (fInterlaced)
		return 0;
	if (fCodec == HEVC)
		return bin_size(ALIGN(width, 16), ALIGN(height, 16), 2, 2);
	if (fCodec == VP9) {
		// venus' vpxd_scratch_size(): two pictures' worth, the header
		// budget half of one and the residue's one and a half
		const uint32 threshold = 1280 * 736;
		uint32 yuv = ALIGN(width, 16) * ALIGN(height, 16) * 3 / 2;
		if (yuv < threshold * 3 / 2)
			yuv = threshold * 3 / 2;
		return ALIGN(yuv * 2 / 2 + yuv * 2 * 3 / 2, kDmaAlignment);
	}
	if (fCodec == MPEG2)
		return 0;
	return bin_size(ALIGN(width, 16), ALIGN(height, 16), 1, 3);
}


size_t
IrisDecoder::_Scratch1Size(uint32 width, uint32 height) const
{
	if (fCodec == HEVC) {
		return comv_hevc(width, height, kComvBuffers)
			+ non_comv_hevc(width, height) + line_hevc(width, height);
	}
	if (fCodec == VP9 || fCodec == MPEG2) {
		// (VP9's motion vectors are in the persistent buffer.) Iris'
		// sizes add a QP line (without it 4K VP9 hung the firmware after
		// the first picture); the MiB is room to spare.
		uint32 qp = DIV_ROUND_UP(width, 64) * DIV_ROUND_UP(height, 64) * 128;
		return vpx_lines(width, height, fCodec == VP9)
			+ ALIGN(qp, kDmaAlignment) + vpss_line_size(width, height) + 4096
			+ 1024 * 1024;
	}
	return comv_h264(width, height, kComvBuffers)
		+ non_comv_h264(width, height) + line_h264(width, height);
}


size_t
IrisDecoder::_PersistSize() const
{
	if (fCodec == VP9) {
		// probability tables, motion vectors for 8K, the superframe
		// index, headers, tile offsets, HDR10 histograms
		const uint32 comv = ((8192 + 63) >> 6) * ((4320 + 63) >> 6) * 8 * 8
			* 2 * 8;
		return ALIGN(36 * 3840, kDmaAlignment) + ALIGN(comv, kDmaAlignment)
			+ ALIGN(34, kDmaAlignment) + ALIGN(3 * 128, kDmaAlignment)
			+ ALIGN(32 * ALIGN(32 * 4 * 4, 32), kDmaAlignment) + 4096;
	}
	if (fCodec == MPEG2) {
		// the quantisation matrices and a QP dump
		return ALIGN(4 * 128 + 256 + 115200, kDmaAlignment);
	}
	if (fCodec == HEVC) {
		const uint32 tiles = 128 * 32 + 1;
		return ALIGN((1u << 10) * (80 + 20) + 48 * 3072 + tiles * 4
			+ kHwPictureBuffers * 4096, kDmaAlignment);
	}
	return ALIGN(512 * (256 + 32) + 3328 * 66 + kHwPictureBuffers * 4096,
		kDmaAlignment);
}
