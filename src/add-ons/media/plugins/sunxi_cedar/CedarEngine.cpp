/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "CedarEngine.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CedarRegisters.h"


// fCodec for VP9 (it runs in the HEVC engine)
static const int kCodecVp9 = 9;


static inline size_t
align(size_t value, size_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}


CedarEngine::CedarEngine(VeDevice& device)
	:
	fDevice(device),
	fCodec(0),
	fWidth(0),
	fHeight(0),
	fStride(0),
	fAlignedHeight(0),
	fLumaSize(0),
	fChromaSize(0),
	fFrameSize(0),
	fMvcolSize(0),
	fBitDepth(8),
	fTwoBitStride(0),
	fTwoBitOffset(0),
	fTwoBitChromaRow(0),
	fVp9TwoBitAlign(8),
	fDdr128(getenv("CEDAR_DDR128") != NULL),
	fEntryPointsPlus1(getenv("CEDAR_EP_PLUS1") != NULL)
{
	// CEDAR_VP9_TWOBIT_ROWS: the luma rows of a ten bit VP9 picture's two-bit
	// plane, as the alignment of the height (to find out what the engine does)
	if (const char* value = getenv("CEDAR_VP9_TWOBIT_ROWS"))
		fVp9TwoBitAlign = atoi(value) > 0 ? atoi(value) : 1;
	fError[0] = '\0';
}


CedarEngine::~CedarEngine()
{
	Unconfigure();
}


void
CedarEngine::Unconfigure()
{
	fDevice.Free(fBitstream);
	fDevice.Free(fPictureInfo);
	fDevice.Free(fNeighbour);
	fDevice.Free(fDeblocking);
	fDevice.Free(fIntraPrediction);
	fDevice.Free(fHevcNeighbour);
	fDevice.Free(fEntryPoints);
	fDevice.Free(fVp9Probs);
	fDevice.Free(fVp9Neighbour);
	fDevice.Free(fVp9ColMv);
	fCodec = 0;
	fWidth = fHeight = 0;
}


bool
CedarEngine::Matches(uint32 width, uint32 height, size_t mvcolSize) const
{
	return fWidth == width && fHeight == height && fMvcolSize == mvcolSize;
}


status_t
CedarEngine::_Configure(uint32 width, uint32 height, size_t mvcolSize,
	int codec, uint32 bitDepth)
{
	if (fCodec == codec && fBitDepth == bitDepth
		&& Matches(width, height, mvcolSize)) {
		return B_OK;
	}
	Unconfigure();

	fWidth = width;
	fHeight = height;
	fStride = align(width, 16);
	fAlignedHeight = align(height, 16);
	if (codec == kCodecVp9) {
		// The VP9 engine takes the references' line stride from their width,
		// aligned to what LAST_SCALE1 says (64), and addresses in 1 KiB units;
		// the luma plane goes on to whole superblocks.
		fStride = align(width, 64);
		fAlignedHeight = align(height, 64);
	}
	fLumaSize = (size_t)fStride * fAlignedHeight;
	fChromaSize = fLumaSize / 2;
	fFrameSize = fLumaSize + fChromaSize;
	fMvcolSize = mvcolSize;
	fBitDepth = bitDepth;
	fTwoBitChromaRow = height;
	if (bitDepth > 8 && codec == kCodecVp9) {
		// the two-bit plane right after the chroma rows of the frame, those
		// rounded up to 8x8 blocks (H.616), at the same offset from the luma
		// for every picture, the references too
		fTwoBitStride = align((width + 3) / 4, 32);
		fTwoBitOffset = fLumaSize + (size_t)fStride * align(height, 8) / 2;
		fTwoBitChromaRow = align(height, fVp9TwoBitAlign);
		fFrameSize = align(fTwoBitOffset
			+ (size_t)fTwoBitStride * fAlignedHeight * 3 / 2, 1024);
	} else if (bitDepth > 8) {
		// the low two bits of every sample behind the picture, at a 1 KiB
		// boundary (the engine's secondary output would make P010 on the
		// H616, but on the A733 it stops the engine)
		fTwoBitStride = align(width / 4, 32);
		fTwoBitOffset = align(fFrameSize, 1024);
		fFrameSize = fTwoBitOffset
			+ (size_t)fTwoBitStride * fAlignedHeight * 3 / 2;
	} else {
		fTwoBitStride = 0;
		fTwoBitOffset = 0;
	}

	status_t status = fDevice.Allocate(fBitstream, 1024 * 1024);
	if (status != B_OK)
		return status;
	if (codec == CEDAR_MODE_H264) {
		size_t pictureInfo = (size_t)H264_FRAME_SLOTS
				* (width > 2048 ? 0x4000 : 0x1000) + (size_t)height * 2 * 64;
		if (pictureInfo < 130 * 1024)
			pictureInfo = 130 * 1024;
		// the neighbour information is 32 KiB at a 16 KiB boundary; the
		// driver's buffers are only page aligned, so 48 KiB holds one
		status = fDevice.Allocate(fPictureInfo, pictureInfo);
		if (status == B_OK)
			status = fDevice.Allocate(fNeighbour, 48 * 1024);
		if (status == B_OK)
			status = fDevice.Allocate(fDeblocking, align(width, 32) * 12);
		if (status == B_OK) {
			status = fDevice.Allocate(fIntraPrediction,
				align(width, 64) * 5 * 2);
		}
	} else if (codec == kCodecVp9) {
		status = fDevice.Allocate(fVp9Probs, VP9_PROBS_BUFFER_SIZE);
		if (status == B_OK)
			status = fDevice.Allocate(fVp9Neighbour, VP9_NEIGHBOR_SIZE);
		if (status == B_OK) {
			// the collocated motion vectors, 16 bytes per 8x8 block, of the
			// frame before (read) and this one (written)
			status = fDevice.Allocate(fVp9ColMv,
				(size_t)(fStride / 8) * (fAlignedHeight / 8) * 16);
		}
		// all of them zeroed, as Linux's dma_alloc_coherent() has them: a
		// pool buffer holds what its last user left, and a stream's first
		// frame with motion vectors from the one before (tile-4x4 frame 2)
		// then ended with an error now and then
		if (status == B_OK) {
			VeBuffer* buffers[3] = { &fVp9Probs, &fVp9Neighbour, &fVp9ColMv };
			for (VeBuffer* buffer : buffers) {
				memset(buffer->address, 0, buffer->size);
				fDevice.SyncForDevice(*buffer, 0, buffer->size);
			}
		}
	} else {
		// twice Cedrus' 794 KiB, as the BSP does for 10 bit
		status = fDevice.Allocate(fHevcNeighbour, 2 * 794 * 1024);
		if (status == B_OK) {
			status = fDevice.Allocate(fEntryPoints,
				4 * HEVC_MAX_ENTRY * sizeof(uint32));
		}
	}
	if (status != B_OK) {
		Unconfigure();
		return status;
	}
	fCodec = codec;
	return B_OK;
}


status_t
CedarEngine::ConfigureH264(const H264Sps& sps)
{
	uint32 width = sps.widthMbs * 16;
	uint32 height = sps.heightMapUnits * 16 * (2 - sps.frameMbsOnly);
	size_t field = (size_t)sps.widthMbs * ((height + 15) / 16) * 16;
	if (!sps.direct8x8)
		field *= 2;
	if (!sps.frameMbsOnly)
		field *= 2;
	return _Configure(width, height, field * 2, CEDAR_MODE_H264, 8);
}


status_t
CedarEngine::ConfigureHevc(const HevcSps& sps)
{
	size_t mvcol = (size_t)sps.widthCtbs * sps.heightCtbs * 160 + 1024;
	return _Configure(sps.width, sps.height, mvcol, CEDAR_MODE_HEVC,
		sps.bitDepthLuma);
}


status_t
CedarEngine::ConfigureVp9(uint32 width, uint32 height, uint32 bitDepth)
{
	return _Configure(width, height, 0, kCodecVp9, bitDepth);
}


status_t
CedarEngine::AllocateFrame(CedarFrame& frame)
{
	status_t status = fDevice.Allocate(frame.picture, fFrameSize);
	if (status == B_OK && fMvcolSize > 0)
		status = fDevice.Allocate(frame.mvcol, fMvcolSize);
	if (status != B_OK)
		FreeFrame(frame);
	return status;
}


void
CedarEngine::FreeFrame(CedarFrame& frame)
{
	fDevice.Free(frame.picture);
	fDevice.Free(frame.mvcol);
}


status_t
CedarEngine::_LoadBitstream(const uint8* nal, size_t size)
{
	// a little zero padding: the bit reader may fetch past the end
	if (size + 64 > fBitstream.size) {
		fDevice.Free(fBitstream);
		status_t status = fDevice.Allocate(fBitstream,
			align(size + 64, 256 * 1024));
		if (status != B_OK)
			return status;
	}
	memcpy(fBitstream.address, nal, size);
	memset(fBitstream.address + size, 0, 64);
	fDevice.SyncForDevice(fBitstream, 0, size + 64);
	return B_OK;
}


uint32
CedarEngine::_Mode(uint32 engine) const
{
	// 256-bit DDR mode: on the H616, mainline Cedrus' 128-bit mode corrupts
	// the chroma H.264 reads from references; CEDAR_DDR128 to compare
	uint32 mode = engine | (fDdr128 ? CEDAR_MODE_DDR_128 : CEDAR_MODE_DDR_256)
		| CEDAR_MODE_REC_WR_2MB;
	if (fWidth > 2048)
		mode |= CEDAR_MODE_WIDTH_OVER_2048;
	if (fWidth == 4096)
		mode |= CEDAR_MODE_WIDTH_IS_4096;
	return mode;
}


void
CedarEngine::_SetOutputFormat()
{
	// NV12 as the primary output and as the secondary ("ext") format: with
	// the secondary left tiled, the engine reads the references as tiled
	uint32 chromaHalf = (uint32)(fChromaSize / 2);
	fDevice.Write(CEDAR_OUT_FMT, CEDAR_PRI_FMT_NV12 | CEDAR_SEC_FMT_EXT_NV12);
	fDevice.Write(CEDAR_SEC_CHROMA_LEN, chromaHalf | CEDAR_SEC_FMT_EXT);
	fDevice.Write(CEDAR_PRI_CHROMA_LEN, chromaHalf);
	fDevice.Write(CEDAR_PRI_STRIDE, fStride | ((fStride / 2) << 16));
}


status_t
CedarEngine::_Run(uint32 trigger, uint32 statusRegister, const char* what,
	uint32 flags)
{
	uint32 status = 0;
	status_t result = fDevice.Run(trigger, 8 /* decode slice */,
		statusRegister, status, flags);
	if (result != B_OK) {
		snprintf(fError, sizeof(fError), "%s: %s (status %#" B_PRIx32 ")",
			what, strerror(result), status);
		return result;
	}
	if ((status & 6) != 0 || (status & 1) == 0) {
		snprintf(fError, sizeof(fError), "%s: status %#" B_PRIx32, what,
			status);
		return B_BAD_DATA;
	}
	return B_OK;
}


//	#pragma mark - H.264


void
CedarEngine::_H264Sram(uint32 wordOffset, const uint32* data, int words)
{
	fDevice.Write(H264_SRAM_OFFSET, wordOffset << 2);
	for (int i = 0; i < words; i++)
		fDevice.Write(H264_SRAM_DATA, data[i]);
}


void
CedarEngine::_H264SramBytes(uint32 wordOffset, const uint8* data,
	size_t size)
{
	fDevice.Write(H264_SRAM_OFFSET, wordOffset << 2);
	for (size_t i = 0; i < size; i += 4) {
		fDevice.Write(H264_SRAM_DATA, (uint32)data[i]
			| (uint32)data[i + 1] << 8 | (uint32)data[i + 2] << 16
			| (uint32)data[i + 3] << 24);
	}
}


static void
h264_frame_entry(uint32* entry, const CedarFrame& frame, size_t lumaSize,
	size_t mvcolSize, int32 topPoc, int32 bottomPoc, bool longTerm)
{
	entry[0] = (uint32)topPoc;
	entry[1] = (uint32)bottomPoc;
	// [9:8] 0: a frame, neither field nor MBAFF; [1:0] and [5:4] the top
	// and bottom field's reference type, 0 short-term and 1 long-term (2 is
	// what Allwinner's own decoder gives a non-reference picture). Spatial
	// direct prediction needs it: a long-term RefPicList1[0] has no
	// colZeroFlag (FRExt_MMCO4_Sony_B picture 21).
	entry[2] = longTerm ? 0x11 : 0;
	entry[3] = frame.picture.bus;
	entry[4] = frame.picture.bus + (uint32)lumaSize;
	entry[5] = frame.mvcol.bus;
	entry[6] = frame.mvcol.bus + (uint32)(mvcolSize / 2);
	entry[7] = 0;
}


status_t
CedarEngine::DecodeH264Slice(const H264State& state, const H264Slice& slice,
	CedarFrame& current, CedarFrame* const* frames, bool firstSliceInPicture,
	const int list0[32], const int list1[32], const uint8* nal, size_t size)
{
	const H264Pps& pps = state.pps[slice.ppsId];
	const H264Sps& sps = state.sps[pps.spsId];
	if (current.position < 1 || current.position >= H264_FRAME_SLOTS)
		return B_BAD_VALUE;

	status_t status = _LoadBitstream(nal, size);
	if (status != B_OK)
		return status;

	fDevice.Begin();
	fDevice.Write(CEDAR_MODE, _Mode(CEDAR_MODE_H264));
	_SetOutputFormat();

	fDevice.Write(H264_SDROT_CTRL, 0);
	fDevice.Write(H264_EXTRA_BUF1, fPictureInfo.bus);
	fDevice.Write(H264_EXTRA_BUF2, align(fNeighbour.bus, 16 * 1024));

	if (pps.scalingMatrixPresent) {
		_H264SramBytes(H264_SRAM_SCALING_8X8_0, pps.scaling8x8[0], 64);
		_H264SramBytes(H264_SRAM_SCALING_8X8_1, pps.scaling8x8[1], 64);
		_H264SramBytes(H264_SRAM_SCALING_4X4, &pps.scaling4x4[0][0], 96);
	}

	// the frame list: every reference and the picture being decoded, each
	// at the position it keeps while it is a reference
	uint32 list[H264_FRAME_SLOTS * 8];
	memset(list, 0, sizeof(list));
	for (int i = 0; i < H264_MAX_REFS; i++) {
		const H264Ref& ref = state.refs[i];
		if (ref.ref == 0 || ref.frame < 0)
			continue;
		const CedarFrame* frame = frames[ref.frame];
		if (frame->position < 1 || frame->position >= H264_FRAME_SLOTS)
			continue;
		h264_frame_entry(&list[frame->position * 8], *frame, fLumaSize,
			fMvcolSize, ref.topPoc, ref.bottomPoc, ref.ref == 2);
	}
	h264_frame_entry(&list[current.position * 8], current, fLumaSize,
		fMvcolSize, state.curTopPoc, state.curBottomPoc, false);
	_H264Sram(H264_SRAM_FRAMES, list, H264_FRAME_SLOTS * 8);
	fDevice.Write(H264_OUTPUT_FRAME_IDX, current.position);

	// the whole NAL unit, header included; the bit reader drops the
	// emulation prevention bytes itself, the header is flushed below
	fDevice.Write(H264_VLD_LEN, (uint32)(size * 8));
	fDevice.Write(H264_VLD_OFFSET, 0);
	fDevice.Write(H264_VLD_END, fBitstream.bus + (uint32)size);
	fDevice.Write(H264_VLD_ADDR, h264_vld_addr(fBitstream.bus)
		| (1u << 30) | (1u << 29) | (1u << 28));

	// line buffers in DRAM at every width (the T527 has no SRAM for them)
	fDevice.Write(CEDAR_BUF_CTRL, CEDAR_BUF_INTRA_MIXED | CEDAR_BUF_DBLK_MIXED);
	fDevice.Write(CEDAR_DBLK_BUF, fDeblocking.bus);
	fDevice.Write(CEDAR_INTRA_BUF, fIntraPrediction.bus);

	fDevice.Write(H264_TRIGGER, H264_TRIG_INIT_SWDEC);
	for (int done = 0; done < slice.headerBits; ) {
		int count = slice.headerBits - done > 32 ? 32
			: slice.headerBits - done;
		fDevice.Write(H264_TRIGGER, H264_TRIG_FLUSH_BITS(count));
		fDevice.PollClear(H264_STATUS, H264_STATUS_VLD_BUSY);
		done += count;
	}

	if (slice.hasWeights) {
		fDevice.Write(H264_SHS_WP, (uint32)((slice.chromaLog2Denom & 7) << 4)
			| (uint32)(slice.lumaLog2Denom & 7));
		fDevice.Write(H264_SRAM_OFFSET, H264_SRAM_WEIGHTS << 2);
		for (int l = 0; l < 2; l++) {
			for (int i = 0; i < 32; i++) {
				fDevice.Write(H264_SRAM_DATA,
					((uint32)(slice.lumaOffset[l][i] & 0x1ff) << 16)
					| (uint32)(slice.lumaWeight[l][i] & 0x1ff));
			}
			for (int i = 0; i < 32; i++) {
				for (int j = 0; j < 2; j++) {
					fDevice.Write(H264_SRAM_DATA,
						((uint32)(slice.chromaOffset[l][i][j] & 0x1ff) << 16)
						| (uint32)(slice.chromaWeight[l][i][j] & 0x1ff));
				}
			}
		}
	}

	for (int l = 0; l < 2; l++) {
		bool wanted = l == 0 ? (slice.sliceType == 0 || slice.sliceType == 3
				|| slice.sliceType == 1)
			: slice.sliceType == 1;
		if (!wanted)
			continue;
		const int* refList = l == 0 ? list0 : list1;
		int count = slice.numRefIdxActive[l];
		uint8 bytes[32];
		memset(bytes, 0, sizeof(bytes));
		for (int i = 0; i < count && i < 32; i++) {
			if (refList[i] < 0)
				continue;
			const CedarFrame* frame = frames[state.refs[refList[i]].frame];
			bytes[i] = (uint8)(frame->position << 1);
		}
		_H264SramBytes(l == 0 ? H264_SRAM_LIST0 : H264_SRAM_LIST1, bytes,
			align(count, 4));
	}

	int l0 = slice.numRefIdxActive[0] > 0 ? slice.numRefIdxActive[0] - 1 : 0;
	int l1 = slice.numRefIdxActive[1] > 0 ? slice.numRefIdxActive[1] - 1 : 0;

	uint32 reg = (uint32)(l0 & 0x1f) << 10 | (uint32)(l1 & 0x1f) << 5
		| (uint32)(pps.weightedBipredIdc & 3) << 2;
	if (pps.entropyCodingMode)
		reg |= 1u << 15;
	if (pps.weightedPred)
		reg |= 1u << 4;
	if (pps.constrainedIntraPred)
		reg |= 1u << 1;
	if (pps.transform8x8Mode)
		reg |= 1u << 0;
	fDevice.Write(H264_PPS, reg);

	reg = (uint32)(sps.chromaFormatIdc & 7) << 19
		| (uint32)((sps.widthMbs - 1) & 0xff) << 8
		| (uint32)((sps.heightMapUnits - 1) & 0xff);
	if (sps.frameMbsOnly)
		reg |= 1u << 18;
	if (sps.mbaff)
		reg |= 1u << 17;
	if (sps.direct8x8)
		reg |= 1u << 16;
	fDevice.Write(H264_SPS, reg);

	reg = (uint32)((slice.firstMb % sps.widthMbs) & 0xff) << 24
		| (uint32)((slice.firstMb / sps.widthMbs) & 0xff) << 16
		| (uint32)(slice.sliceType & 0xf) << 8
		| (uint32)(slice.cabacInitIdc & 3);
	if (slice.nalRefIdc != 0)
		reg |= 1u << 12;
	if (firstSliceInPicture)
		reg |= 1u << 5;
	if (slice.directSpatialMvPred)
		reg |= 1u << 2;
	fDevice.Write(H264_SHS, reg);

	reg = 1u << 12
		| (uint32)(l0 & 0x1f) << 24 | (uint32)(l1 & 0x1f) << 16
		| (uint32)(slice.disableDeblockingFilterIdc & 3) << 8
		| (uint32)(slice.sliceAlphaC0OffsetDiv2 & 0xf) << 4
		| (uint32)(slice.sliceBetaOffsetDiv2 & 0xf);
	fDevice.Write(H264_SHS2, reg);

	int qp = pps.picInitQp + slice.sliceQpDelta;
	reg = (uint32)(pps.secondChromaQpIndexOffset & 0x3f) << 16
		| (uint32)(pps.chromaQpIndexOffset & 0x3f) << 8
		| (uint32)(qp & 0x3f);
	if (!pps.scalingMatrixPresent)
		reg |= 1u << 24;	// the flat matrices
	fDevice.Write(H264_SHS_QP, reg);

	// whatever the header flushes left in the status, then the interrupts
	fDevice.WriteBack(H264_STATUS);
	fDevice.Write(H264_CTRL, H264_IRQ_MASK);
	return _Run(H264_TRIGGER, H264_STATUS, "H.264 slice");
}


//	#pragma mark - HEVC


void
CedarEngine::_HevcFrameInfo(int slot, const CedarFrame& frame, int32 poc)
{
	uint32 chroma = frame.picture.bus + (uint32)fLumaSize;
	fDevice.Write(HEVC_SRAM_OFFSET,
		HEVC_SRAM_FRAME_INFO + HEVC_SRAM_FRAME_INFO_SIZE * (uint32)slot);
	fDevice.Write(HEVC_SRAM_DATA, (uint32)poc);
	fDevice.Write(HEVC_SRAM_DATA, (uint32)poc);
	fDevice.Write(HEVC_SRAM_DATA, frame.mvcol.bus >> 8);
	fDevice.Write(HEVC_SRAM_DATA, frame.mvcol.bus >> 8);
	fDevice.Write(HEVC_SRAM_DATA, frame.picture.bus >> 8);
	fDevice.Write(HEVC_SRAM_DATA, chroma >> 8);
}


void
CedarEngine::_HevcRefList(const HevcState& state, const int* list, int count,
	uint32 sramOffset)
{
	fDevice.Write(HEVC_SRAM_OFFSET, sramOffset);
	uint32 word = 0;
	for (int i = 0; i < count; i++) {
		uint32 value = list[i] >= 0 ? (uint32)list[i] : 0;
		if (list[i] >= 0 && state.refs[list[i]].used == 2)
			value |= 0x80;		// long term
		word |= value << ((i % 4) * 8);
		if (i % 4 == 3 || i == count - 1) {
			fDevice.Write(HEVC_SRAM_DATA, word);
			word = 0;
		}
	}
}


void
CedarEngine::_HevcWeights(const HevcSlice& slice, int l)
{
	int count = slice.numRefIdxActive[l];
	fDevice.Write(HEVC_SRAM_OFFSET,
		l == 0 ? HEVC_SRAM_WEIGHTS_L0_LUMA : HEVC_SRAM_WEIGHTS_L1_LUMA);
	for (int i = 0; i < count; i += 2) {
		uint32 word = (uint8)slice.deltaLumaWeight[l][i]
			| (uint32)(uint8)slice.lumaOffset[l][i] << 8;
		if (i + 1 < count) {
			word |= (uint32)(uint8)slice.deltaLumaWeight[l][i + 1] << 16
				| (uint32)(uint8)slice.lumaOffset[l][i + 1] << 24;
		}
		fDevice.Write(HEVC_SRAM_DATA, word);
	}
	fDevice.Write(HEVC_SRAM_OFFSET,
		l == 0 ? HEVC_SRAM_WEIGHTS_L0_CHROMA : HEVC_SRAM_WEIGHTS_L1_CHROMA);
	for (int i = 0; i < count; i++) {
		fDevice.Write(HEVC_SRAM_DATA,
			(uint8)slice.deltaChromaWeight[l][i][0]
			| (uint32)(uint8)slice.chromaOffset[l][i][0] << 8
			| (uint32)(uint8)slice.deltaChromaWeight[l][i][1] << 16
			| (uint32)(uint8)slice.chromaOffset[l][i][1] << 24);
	}
}


/*!	Four rows of one column to a word: the lists go in column by column. */
static void
write_scaling_matrix(VeDevice& device, const uint8* list, int size)
{
	if (size == 4) {
		for (int j = 0; j < 4; j++) {
			device.Write(HEVC_SRAM_DATA, (uint32)list[j + 12] << 24
				| (uint32)list[j + 8] << 16 | (uint32)list[j + 4] << 8
				| list[j]);
		}
		return;
	}
	for (int j = 0; j < 8; j++) {
		for (int k = 0; k < 8; k += 4) {
			device.Write(HEVC_SRAM_DATA, (uint32)list[j + (k + 3) * 8] << 24
				| (uint32)list[j + (k + 2) * 8] << 16
				| (uint32)list[j + (k + 1) * 8] << 8 | list[j + k * 8]);
		}
	}
}


void
CedarEngine::_HevcScaling(const HevcScaling& scaling)
{
	fDevice.Write(HEVC_SCALING_DC0, (uint32)scaling.dc32x32[1] << 24
		| (uint32)scaling.dc32x32[0] << 16 | (uint32)scaling.dc16x16[1] << 8
		| scaling.dc16x16[0]);
	fDevice.Write(HEVC_SCALING_DC1, (uint32)scaling.dc16x16[5] << 24
		| (uint32)scaling.dc16x16[4] << 16 | (uint32)scaling.dc16x16[3] << 8
		| scaling.dc16x16[2]);
	fDevice.Write(HEVC_SRAM_OFFSET, HEVC_SRAM_SCALING_LISTS);
	for (int m = 0; m < 6; m++)
		write_scaling_matrix(fDevice, scaling.list8x8[m], 8);
	for (int m = 0; m < 2; m++)
		write_scaling_matrix(fDevice, scaling.list32x32[m], 8);
	for (int m = 0; m < 6; m++)
		write_scaling_matrix(fDevice, scaling.list16x16[m], 8);
	for (int m = 0; m < 6; m++)
		write_scaling_matrix(fDevice, scaling.list4x4[m], 4);
}


/*!	The tile the slice starts in, and the entry points: for wavefronts the
	offsets, for tiles each with the next tile's bounds.
*/
void
CedarEngine::_HevcTiles(const HevcState& state, const HevcSlice& slice,
	int ctbX, int ctbY)
{
	const HevcPps& pps = state.pps[slice.ppsId];
	int columnWidth[HEVC_MAX_TILE_COLUMNS], rowHeight[HEVC_MAX_TILE_ROWS];
	hevc_tiles(&state, &slice, columnWidth, rowHeight);

	int x = 0, tx = 0;
	while (tx < pps.numTileColumns - 1 && x + columnWidth[tx] <= ctbX)
		x += columnWidth[tx++];
	int y = 0, ty = 0;
	while (ty < pps.numTileRows - 1 && y + rowHeight[ty] <= ctbY)
		y += rowHeight[ty++];

	fDevice.Write(HEVC_TILE_START, (uint32)y << 16 | (uint32)x);
	fDevice.Write(HEVC_TILE_END, (uint32)(y + rowHeight[ty] - 1) << 16
		| (uint32)(x + columnWidth[tx] - 1));

	uint32* entries = (uint32*)fEntryPoints.address;
	uint32 plus = fEntryPointsPlus1 ? 1 : 0;
	if (pps.entropyCodingSync) {
		for (int i = 0; i < slice.numEntryPoints; i++)
			entries[i] = slice.entryPointMinus1[i] + plus;
		fDevice.SyncForDevice(fEntryPoints, 0,
			slice.numEntryPoints * sizeof(uint32));
		return;
	}
	for (int i = 0; i < slice.numEntryPoints; i++) {
		if (tx + 1 >= pps.numTileColumns) {
			x = 0;
			tx = 0;
			y += rowHeight[ty++];
		} else
			x += columnWidth[tx++];
		entries[i * 4 + 0] = slice.entryPointMinus1[i] + plus;
		entries[i * 4 + 1] = 0;
		entries[i * 4 + 2] = (uint32)y << 16 | (uint32)x;
		entries[i * 4 + 3] = (uint32)(y + rowHeight[ty] - 1) << 16
			| (uint32)(x + columnWidth[tx] - 1);
	}
	fDevice.SyncForDevice(fEntryPoints, 0,
		slice.numEntryPoints * 4 * sizeof(uint32));
}


status_t
CedarEngine::DecodeHevcSlice(const HevcState& state, const HevcSlice& slice,
	CedarFrame& current, CedarFrame* const* frames, const int list0[16],
	const int list1[16], const uint8* nal, size_t size)
{
	const HevcPps& pps = state.pps[slice.ppsId];
	const HevcSps& sps = state.sps[pps.spsId];

	status_t status = _LoadBitstream(nal, size);
	if (status != B_OK)
		return status;

	fDevice.Begin();
	fDevice.Write(CEDAR_MODE, _Mode(CEDAR_MODE_HEVC));
	_SetOutputFormat();

	fDevice.Write(HEVC_BITS_OFFSET, 0);
	fDevice.Write(HEVC_BITS_LEN, (uint32)(size * 8));
	fDevice.Write(HEVC_BITS_ADDR, ((fBitstream.bus >> 8) & 0x0fffffff)
		| (1u << 30) | (1u << 29) | (1u << 28));
	fDevice.Write(HEVC_BITS_END, (fBitstream.bus + (uint32)size) >> 8);

	int ctbX = slice.segmentAddress % sps.widthCtbs;
	int ctbY = slice.segmentAddress / sps.widthCtbs;
	fDevice.Write(HEVC_CTB_ADDR, (uint32)ctbY << 16 | (uint32)ctbX);

	if (pps.tilesEnabled || pps.entropyCodingSync)
		_HevcTiles(state, slice, ctbX, ctbY);
	else {
		fDevice.Write(HEVC_TILE_START, 0);
		fDevice.Write(HEVC_TILE_END, 0);
	}

	if (slice.firstSliceSegmentInPic)
		fDevice.Write(HEVC_CTB_NUM, 0);

	// leave the bit reader on byte_alignment(): flush the header proper
	fDevice.Write(HEVC_TRIGGER, HEVC_TRIG_INIT_SWDEC);
	for (int done = 0; done < slice.alignBitPos; ) {
		int count = slice.alignBitPos - done > 32 ? 32
			: slice.alignBitPos - done;
		fDevice.Write(HEVC_TRIGGER, HEVC_TRIG_FLUSH_BITS(count));
		fDevice.PollClear(HEVC_STATUS, HEVC_STATUS_VLD_BUSY);
		done += count;
	}

	fDevice.Write(HEVC_NAL_HDR, (uint32)(slice.temporalId + 1) << 6
		| (uint32)(slice.nalType & 0x3f));

	uint32 reg = (uint32)(sps.maxTransformHierarchyDepthIntra & 7) << 20
		| (uint32)(sps.maxTransformHierarchyDepthInter & 7) << 17
		| (uint32)(sps.log2DiffMaxMinTbSize & 3) << 15
		| (uint32)((sps.log2MinTbSize - 2) & 3) << 13
		| (uint32)(sps.log2DiffMaxMinCbSize & 3) << 11
		| (uint32)((sps.log2MinCbSize - 3) & 3) << 9
		| (uint32)((sps.bitDepthChroma - 8) & 7) << 6
		| (uint32)((sps.bitDepthLuma - 8) & 7) << 3
		| (uint32)(sps.chromaFormatIdc & 3);
	if (sps.strongIntraSmoothing)
		reg |= 1u << 26;
	if (sps.temporalMvp)
		reg |= 1u << 25;
	if (sps.sao)
		reg |= 1u << 24;
	if (sps.amp)
		reg |= 1u << 23;
	fDevice.Write(HEVC_SPS, reg);

	reg = 0;
	if (sps.pcm) {
		reg = (uint32)(sps.log2DiffMaxMinPcmCbSize & 3) << 10
			| (uint32)((sps.log2MinPcmCbSize - 3) & 3) << 8
			| (uint32)((sps.pcmBitDepthChroma - 1) & 0xf) << 4
			| (uint32)((sps.pcmBitDepthLuma - 1) & 0xf)
			| 1u << 15;
		if (sps.pcmLoopFilterDisabled)
			reg |= 1u << 14;
	}
	fDevice.Write(HEVC_PCM_CTRL, reg);

	reg = (uint32)(pps.crQpOffset & 0x3f) << 24
		| (uint32)(pps.cbQpOffset & 0x3f) << 16
		| (uint32)(pps.initQpMinus26 & 0x7f) << 8
		| (uint32)(pps.diffCuQpDeltaDepth & 3) << 4;
	if (pps.cuQpDeltaEnabled)
		reg |= 1u << 3;
	if (pps.transformSkip)
		reg |= 1u << 2;
	if (pps.constrainedIntraPred)
		reg |= 1u << 1;
	if (pps.signDataHiding)
		reg |= 1u << 0;
	fDevice.Write(HEVC_PPS0, reg);

	reg = (uint32)(pps.log2ParallelMergeLevelMinus2 & 7) << 8;
	if (pps.loopFilterAcrossSlices)
		reg |= 1u << 6;
	if (pps.loopFilterAcrossTiles)
		reg |= 1u << 5;
	if (pps.entropyCodingSync)
		reg |= 1u << 4;
	if (pps.tilesEnabled)
		reg |= 1u << 3;
	if (pps.transquantBypass)
		reg |= 1u << 2;
	if (pps.weightedBipred)
		reg |= 1u << 1;
	if (pps.weightedPred)
		reg |= 1u << 0;
	fDevice.Write(HEVC_PPS1, reg);

	int l0 = slice.numRefIdxActive[0] > 0 ? slice.numRefIdxActive[0] - 1 : 0;
	int l1 = slice.numRefIdxActive[1] > 0 ? slice.numRefIdxActive[1] - 1 : 0;
	reg = (uint32)(slice.fiveMinusMaxNumMergeCand & 7) << 24
		| (uint32)(l1 & 0xf) << 20 | (uint32)(l0 & 0xf) << 16
		| (uint32)(slice.collocatedRefIdx & 0xf) << 12
		| (uint32)(slice.sliceType & 3) << 2;	// picture structure 0: frame
	if (slice.collocatedFromL0)
		reg |= 1u << 11;
	if (slice.cabacInit)
		reg |= 1u << 10;
	if (slice.mvdL1Zero)
		reg |= 1u << 9;
	if (slice.saoChroma)
		reg |= 1u << 8;
	if (slice.saoLuma)
		reg |= 1u << 7;
	if (slice.temporalMvp)
		reg |= 1u << 6;
	if (slice.dependentSliceSegment)
		reg |= 1u << 1;
	if (slice.firstSliceSegmentInPic)
		reg |= 1u << 0;
	fDevice.Write(HEVC_SLICE0, reg);

	reg = (uint32)(slice.tcOffsetDiv2 & 0xf) << 28
		| (uint32)(slice.betaOffsetDiv2 & 0xf) << 24
		| (uint32)(slice.crQpOffset & 0x1f) << 16
		| (uint32)(slice.cbQpOffset & 0x1f) << 8
		| (uint32)(slice.sliceQpDelta & 0x7f);
	if (slice.deblockingDisabled)
		reg |= 1u << 23;
	if (slice.loopFilterAcrossSlices)
		reg |= 1u << 22;
	if (slice.sliceType != 2) {
		// NoBackwardPredFlag: no reference after the picture
		bool backward = false;
		for (int i = 0; i < slice.numRefIdxActive[0]; i++) {
			if (list0[i] >= 0 && state.refs[list0[i]].poc > state.curPoc)
				backward = true;
		}
		if (slice.sliceType == 0) {
			for (int i = 0; i < slice.numRefIdxActive[1]; i++) {
				if (list1[i] >= 0 && state.refs[list1[i]].poc > state.curPoc)
					backward = true;
			}
		}
		if (!backward)
			reg |= 1u << 21;
	}
	fDevice.Write(HEVC_SLICE1, reg);

	reg = (uint32)(slice.numEntryPoints & 0x3fff) << 8
		| (uint32)(slice.chromaLog2Denom & 7) << 4
		| (uint32)(slice.lumaLog2Denom & 7);
	fDevice.Write(HEVC_SLICE2, reg);
	fDevice.Write(HEVC_ENTRY_POINT_ADDR, fEntryPoints.bus >> 8);

	fDevice.Write(HEVC_PIC_SIZE, (uint32)sps.width | (uint32)sps.height << 16);
	if (sps.scalingListEnabled) {
		_HevcScaling(*hevc_scaling(&state, &slice));
		fDevice.Write(HEVC_SCALING_CTRL, HEVC_SCALING_ENABLED);
	} else
		fDevice.Write(HEVC_SCALING_CTRL, HEVC_SCALING_DEFAULT);
	fDevice.Write(HEVC_NEIGHBOR_ADDR, fHevcNeighbour.bus >> 8);

	for (int i = 0; i < HEVC_MAX_DPB; i++) {
		if (state.refs[i].used)
			_HevcFrameInfo(i, *frames[state.refs[i].frame], state.refs[i].poc);
	}
	_HevcFrameInfo(HEVC_OUTPUT_SLOT, current, state.curPoc);
	fDevice.Write(HEVC_OUTPUT_FRAME_IDX, HEVC_OUTPUT_SLOT);

	if (slice.sliceType != 2) {
		_HevcRefList(state, list0, slice.numRefIdxActive[0],
			HEVC_SRAM_LIST0);
		if (pps.weightedPred || pps.weightedBipred)
			_HevcWeights(slice, 0);
	}
	if (slice.sliceType == 0) {
		_HevcRefList(state, list1, slice.numRefIdxActive[1],
			HEVC_SRAM_LIST1);
		if (pps.weightedBipred)
			_HevcWeights(slice, 1);
	}

	if (fBitDepth > 8) {
		// the low two bits behind the picture
		fDevice.Write(HEVC_OFFSET_FIRST_OUT, (uint32)fTwoBitOffset);
		fDevice.Write(HEVC_10BIT_CONFIG, fTwoBitStride & 0x7ff);
	}
	uint32 control = HEVC_IRQ_MASK;
	fDevice.Write(HEVC_CTRL, control);
	return _Run(HEVC_TRIGGER, HEVC_STATUS, "HEVC slice");
}


//	#pragma mark - VP9


/*!	The tiles after the first (whose bounds go to TILE_START and TILE_END)
	as a list at the start of the probability buffer: per tile a zero and
	its bounds in superblocks (as TILE_START and TILE_END, in one 64 bit
	word), bit 31 for an empty one. The engine finds the tiles' data
	itself, from the sizes in front of them.
*/
void
CedarEngine::_Vp9Tiles(const Vp9Frame& frame)
{
	int rows = 1 << frame.log2TileRows;
	int columns = 1 << frame.log2TileCols;
	uint32* list = (uint32*)(fVp9Probs.address + VP9_PROBS_TILES);
	int entries = 0;
	for (int r = 0; r < rows; r++) {
		int rowStart = vp9_tile_offset(r, frame.miRows, frame.log2TileRows);
		int rowEnd = vp9_tile_offset(r + 1, frame.miRows, frame.log2TileRows);
		for (int c = 0; c < columns; c++) {
			int columnStart = vp9_tile_offset(c, frame.miCols,
				frame.log2TileCols);
			int columnEnd = vp9_tile_offset(c + 1, frame.miCols,
				frame.log2TileCols);
			uint32 start = ((uint32)(columnStart >> 3) & 0x1ff)
				| ((uint32)(rowStart >> 3) & 0x1ff) << 16;
			uint32 end = ((uint32)(columnEnd - 1) >> 3 & 0x1ff)
				| ((uint32)(rowEnd - 1) >> 3 & 0x1ff) << 16;
			bool empty = columnStart == columnEnd || rowStart == rowEnd;
			if (r == 0 && c == 0) {
				fDevice.Write(VP9_TILE_START, empty ? VP9_TILE_INVALID : 0);
				fDevice.Write(VP9_TILE_END, end);
				continue;
			}
			list[entries * 4 + 0] = 0;
			list[entries * 4 + 1] = 0;
			list[entries * 4 + 2] = start | (empty ? VP9_TILE_INVALID : 0);
			list[entries * 4 + 3] = end;
			entries++;
		}
	}
	if (entries > 0)
		fDevice.SyncForDevice(fVp9Probs, VP9_PROBS_TILES, entries * 16);
}


static void
pack_mv_component(uint8* target, const Vp9MvComponentProbs& mv)
{
	target[0x00] = mv.sign;
	target[0x01] = mv.class0[0];
	target[0x02] = mv.class0Hp;
	target[0x03] = mv.hp;
	memcpy(target + 0x04, mv.class0Fp[0], 3);
	memcpy(target + 0x08, mv.class0Fp[1], 3);
	memcpy(target + 0x0c, mv.classes, 10);
	memcpy(target + 0x1c, mv.bits, 10);
	memcpy(target + 0x28, mv.fp, 3);
}


/*!	The probabilities the frame is decoded with, packed as the engine reads
	them (mostly a 32 bit word per context, up to four probabilities in
	it).
*/
void
CedarEngine::_Vp9Probabilities(const Vp9Frame& frame)
{
	const Vp9Probs& p = frame.probs;
	uint8* table = fVp9Probs.address + VP9_PROBS_TABLE;
	memset(table, 0, VP9_PROBS_TABLE_SIZE);

	table[0x000] = p.tx8[0][0];
	table[0x001] = p.tx8[1][0];
	memcpy(table + 0x004, p.tx16[0], 2);
	memcpy(table + 0x008, p.tx16[1], 2);
	memcpy(table + 0x00c, p.tx32[0], 3);
	memcpy(table + 0x010, p.tx32[1], 3);
	uint8* coef = table + 0x014;
	for (int t = 0; t < 4; t++) {
		for (int i = 0; i < 2; i++) {
			for (int j = 0; j < 2; j++) {
				for (int k = 0; k < 6; k++) {
					for (int l = 0; l < 6; l++) {
						memcpy(coef, p.coef[t][i][j][k][l], 3);
						coef += 4;
					}
				}
			}
		}
	}
	memcpy(table + 0x914, p.skip, 3);
	for (int i = 0; i < 7; i++)
		memcpy(table + 0x918 + i * 4, p.interMode[i], 3);
	for (int i = 0; i < 4; i++)
		memcpy(table + 0x934 + i * 4, p.interp[i], 2);
	memcpy(table + 0x944, p.intraInter, 4);
	memcpy(table + 0x948, p.compInter, 5);
	memcpy(table + 0x950, p.compRef, 5);
	for (int i = 0; i < 4; i++)
		memcpy(table + 0x958 + i * 4, p.singleRef[i], 2);
	memcpy(table + 0x968, p.singleRef[4], 2);
	for (int i = 0; i < 4; i++)
		memcpy(table + 0x970 + i * 0x10, p.yMode[i], 9);
	for (int i = 0; i < 10; i++)
		memcpy(table + 0x9b0 + i * 0x10, p.uvMode[i], 9);
	for (int i = 0; i < 16; i++)
		memcpy(table + 0xa50 + i * 4, p.partition[i], 3);
	memcpy(table + 0xa90, p.mvJoints, 3);
	// the columns' component first
	pack_mv_component(table + 0xa94, p.mv[1]);
	pack_mv_component(table + 0xac4, p.mv[0]);
	if (frame.seg.enabled) {
		memcpy(table + 0xaf0, frame.seg.treeProbs, 7);
		memcpy(table + 0xaf8, frame.seg.predProbs, 3);
	}
	fDevice.SyncForDevice(fVp9Probs, VP9_PROBS_TABLE, VP9_PROBS_TABLE_SIZE);
}


/*!	A reference's scale factors: [4:0] the step in 1/16 samples minus one,
	[20:5] the ratio of the sizes in Q14 (libvpx's x_scale_fp).
*/
static void
vp9_scale(uint32 reference, uint32 current, uint32& value)
{
	uint32 factor = (reference << 14) / current;
	value = (((factor >> 10) - 1) & 0x1f) | (factor & 0xffff) << 5;
}


void
CedarEngine::_Vp9References(const Vp9Frame& frame,
	CedarFrame* const references[3])
{
	static const uint32 kSize[3] = { VP9_LAST_SIZE, VP9_GOLDEN_SIZE,
		VP9_ALTREF_SIZE };
	static const uint32 kLuma[3] = { VP9_LAST_LUMA, VP9_GOLDEN_LUMA,
		VP9_ALTREF_LUMA };
	static const uint32 kChroma[3] = { VP9_LAST_CHROMA, VP9_GOLDEN_CHROMA,
		VP9_ALTREF_CHROMA };

	for (int i = 0; i < 3; i++) {
		const CedarFrame& reference = *references[i];
		fDevice.Write(kSize[i], (reference.width & 0x3fff)
			| (reference.height & 0x3fff) << 16);
		uint32 x, y;
		vp9_scale(reference.width, frame.width, x);
		vp9_scale(reference.height, frame.height, y);
		switch (i) {
			case 0:
				fDevice.Write(VP9_LAST_SCALE0, x);
				fDevice.Write(VP9_LAST_SCALE1, y | VP9_STRIDE_ALIGN_64);
				break;
			case 1:
				// the vertical factor in [31:16] here
				fDevice.Write(VP9_GOLDEN_SCALE0, x);
				fDevice.Write(VP9_GOLDEN_SCALE1, ((y >> 5) & 0xffff) << 16
					| (y & 0x1f));
				break;
			case 2:
				fDevice.Write(VP9_ALTREF_SCALE0, x);
				fDevice.Write(VP9_ALTREF_SCALE1, y);
				break;
		}
		fDevice.Write(kLuma[i], reference.picture.bus >> 8);
		fDevice.Write(kChroma[i],
			(reference.picture.bus + (uint32)reference.lumaSize) >> 8);
	}
}


/*!	The dequantisers and the loop filter levels of the segments. */
void
CedarEngine::_Vp9Sram(const Vp9Frame& frame)
{
	fDevice.Write(VP9_SRAM_OFFSET, VP9_SRAM_DEQUANT);
	for (int i = 0; i < VP9_MAX_SEGMENTS; i++) {
		fDevice.Write(VP9_SRAM_DATA, (uint32)frame.yDequant[i][0]
			| (uint32)frame.yDequant[i][1] << 16);
	}
	for (int i = 0; i < VP9_MAX_SEGMENTS; i++) {
		fDevice.Write(VP9_SRAM_DATA, (uint32)frame.uvDequant[i][0]
			| (uint32)frame.uvDequant[i][1] << 16);
	}

	// intra; last's two modes / golden's two, altref's two (6 bits each)
	fDevice.Write(VP9_SRAM_OFFSET, VP9_SRAM_LOOP_FILTER);
	for (int i = 0; i < VP9_MAX_SEGMENTS; i++) {
		const uint8 (*level)[2] = frame.filterLevels[i];
		fDevice.Write(VP9_SRAM_DATA, (uint32)(level[VP9_INTRA][0] & 0x3f)
			| (uint32)(level[VP9_LAST][0] & 0x3f) << 16
			| (uint32)(level[VP9_LAST][1] & 0x3f) << 24);
		fDevice.Write(VP9_SRAM_DATA, (uint32)(level[VP9_GOLDEN][0] & 0x3f)
			| (uint32)(level[VP9_GOLDEN][1] & 0x3f) << 8
			| (uint32)(level[VP9_ALTREF][0] & 0x3f) << 16
			| (uint32)(level[VP9_ALTREF][1] & 0x3f) << 24);
	}
}


/*	The counts the engine writes back, at VP9_PROBS_COUNTS (32 bit words):
	tx 8x8 [2][2], 16x16 [2][3], 32x32 [2][4]; EOB branches [4][2][2][6][6];
	coefficients [4][2][2][6][6][4]; skip [3][2]; inter modes [7][4];
	filters [4][3]; intra/inter [4][2]; compound/single [5][2]; compound
	references [5][2]; single references [5][2][2]; y modes [4][10]; uv
	modes [10][10]; partitions [16][4]; joints [4]; then per motion vector
	component (columns first) sign [2], class0 [2], class0_hp [2], hp [2],
	classes [11], bits [10][2], class0_fp [2][4], fp [4]. */
struct Vp9HardwareMvCounts {
	uint32	sign[2];
	uint32	class0[2];
	uint32	class0Hp[2];
	uint32	hp[2];
	uint32	classes[11];
	uint32	bits[10][2];
	uint32	class0Fp[2][4];
	uint32	fp[4];
};

struct Vp9HardwareCounts {
	uint32	tx8[2][2];
	uint32	tx16[2][3];
	uint32	tx32[2][4];
	uint32	eobBranch[4][2][2][6][6];
	uint32	coef[4][2][2][6][6][4];
	uint32	skip[3][2];
	uint32	interMode[7][4];
	uint32	interp[4][3];
	uint32	intraInter[4][2];
	uint32	compInter[5][2];
	uint32	compRef[5][2];
	uint32	singleRef[5][2][2];
	uint32	yMode[4][10];
	uint32	uvMode[10][10];
	uint32	partition[16][4];
	uint32	mvJoints[4];
	Vp9HardwareMvCounts mv[2];
};

static_assert(sizeof(Vp9HardwareCounts) == VP9_PROBS_COUNTS_SIZE,
	"the engine's counts");
static_assert(offsetof(Vp9HardwareCounts, coef) == 0x948, "coefficients");
static_assert(offsetof(Vp9HardwareCounts, mvJoints) == 0x31f0, "joints");


void
CedarEngine::_Vp9Counts(Vp9Counts& counts) const
{
	const Vp9HardwareCounts& c = *(const Vp9HardwareCounts*)
		(fVp9Probs.address + VP9_PROBS_COUNTS);
	memset(&counts, 0, sizeof(counts));
	memcpy(counts.tx8, c.tx8, sizeof(c.tx8));
	memcpy(counts.tx16, c.tx16, sizeof(c.tx16));
	memcpy(counts.tx32, c.tx32, sizeof(c.tx32));
	memcpy(counts.eobBranch, c.eobBranch, sizeof(c.eobBranch));
	memcpy(counts.coef, c.coef, sizeof(c.coef));
	memcpy(counts.skip, c.skip, sizeof(c.skip));
	memcpy(counts.interMode, c.interMode, sizeof(c.interMode));
	memcpy(counts.interp, c.interp, sizeof(c.interp));
	memcpy(counts.intraInter, c.intraInter, sizeof(c.intraInter));
	memcpy(counts.compInter, c.compInter, sizeof(c.compInter));
	memcpy(counts.compRef, c.compRef, sizeof(c.compRef));
	memcpy(counts.singleRef, c.singleRef, sizeof(c.singleRef));
	memcpy(counts.yMode, c.yMode, sizeof(c.yMode));
	memcpy(counts.uvMode, c.uvMode, sizeof(c.uvMode));
	memcpy(counts.partition, c.partition, sizeof(c.partition));
	memcpy(counts.mvJoints, c.mvJoints, sizeof(c.mvJoints));
	for (int i = 0; i < 2; i++) {
		// Vp9Counts has the rows' component first
		const Vp9HardwareMvCounts& from = c.mv[1 - i];
		Vp9MvComponentCounts& to = counts.mv[i];
		memcpy(to.sign, from.sign, sizeof(to.sign));
		memcpy(to.class0, from.class0, sizeof(to.class0));
		memcpy(to.class0Hp, from.class0Hp, sizeof(to.class0Hp));
		memcpy(to.hp, from.hp, sizeof(to.hp));
		memcpy(to.classes, from.classes, sizeof(to.classes));
		memcpy(to.bits, from.bits, sizeof(to.bits));
		memcpy(to.class0Fp, from.class0Fp, sizeof(to.class0Fp));
		memcpy(to.fp, from.fp, sizeof(to.fp));
	}
}


status_t
CedarEngine::DecodeVp9Frame(const Vp9Frame& frame, CedarFrame& current,
	CedarFrame* const references[3], const uint8* data, size_t size,
	Vp9Counts* counts)
{
	if (fCodec != kCodecVp9 || !fVp9Probs.IsValid())
		return B_NO_INIT;
	bool intra = frame.frameType == VP9_KEY_FRAME || frame.intraOnly;
	if (!intra) {
		for (int i = 0; i < 3; i++) {
			if (references[i] == NULL || !references[i]->picture.IsValid())
				return B_BAD_VALUE;
		}
	}
	size_t headers = frame.uncompressedHeaderSize + frame.compressedHeaderSize;
	if (headers >= size)
		return B_BAD_DATA;

	status_t status = _LoadBitstream(data, size);
	if (status != B_OK)
		return status;

	// the probability buffer
	_Vp9Probabilities(frame);
	if (frame.refreshProbs) {
		memset(fVp9Probs.address + VP9_PROBS_COUNTS, 0, VP9_PROBS_COUNTS_SIZE);
		fDevice.SyncForDevice(fVp9Probs, VP9_PROBS_COUNTS,
			VP9_PROBS_COUNTS_SIZE);
	}
	if (frame.resetSegmentMap) {
		// segment IDs are predicted from the map earlier frames left
		memset(fVp9Probs.address + VP9_PROBS_SEGMENT_MAP, 0,
			fVp9Probs.size - VP9_PROBS_SEGMENT_MAP);
		fDevice.SyncForDevice(fVp9Probs, VP9_PROBS_SEGMENT_MAP,
			fVp9Probs.size - VP9_PROBS_SEGMENT_MAP);
	}

	fDevice.Begin();
	// CEDAR_VP9_MODE: other bits 16-31 (hex) than the H.616's, to find out
	// what the A733 needs
	static const char* sModeBits = getenv("CEDAR_VP9_MODE");
	uint32 mode = CEDAR_MODE_HEVC | (sModeBits != NULL
		? (uint32)strtoul(sModeBits, NULL, 16) & 0xffff0000u
		: CEDAR_MODE_DDR_256 | CEDAR_MODE_VP9_RAMPD | CEDAR_MODE_VP9_BIT30);
	if (frame.width > 2048)
		mode |= CEDAR_MODE_WIDTH_OVER_2048;
	if (frame.width == 4096)
		mode |= CEDAR_MODE_WIDTH_IS_4096;
	fDevice.Write(CEDAR_MODE, mode);
	_SetOutputFormat();

	uint32 syntax = VP9_SYN_DECODE | VP9_SYN_ENABLE2
		| VP9_SYN_DEPTH(frame.bitDepth - 8)
		| VP9_SYN_TX_MODE(frame.txMode)
		| VP9_SYN_REF_MODE(frame.referenceMode);
	if (frame.log2TileCols != 0 || frame.log2TileRows != 0)
		syntax |= VP9_SYN_TILES;
	if (!intra)
		syntax |= VP9_SYN_INTER;
	if (frame.signBias[VP9_LAST])
		syntax |= VP9_SYN_BIAS_LAST;
	if (frame.signBias[VP9_GOLDEN])
		syntax |= VP9_SYN_BIAS_GOLDEN;
	if (frame.signBias[VP9_ALTREF])
		syntax |= VP9_SYN_BIAS_ALTREF;
	if (frame.allowHighPrecisionMv)
		syntax |= VP9_SYN_HIGH_PRECISION_MV;
	if (frame.frameType != VP9_KEY_FRAME)
		syntax |= VP9_SYN_INTERP(frame.interpFilter);
	if (frame.filterLevel != 0) {
		syntax |= VP9_SYN_LOOP_FILTER
			| VP9_SYN_SHARPNESS(frame.sharpness);
	}
	if (frame.lossless)
		syntax |= VP9_SYN_LOSSLESS;
	if (frame.seg.enabled) {
		syntax |= VP9_SYN_SEGMENTATION;
		if (frame.seg.updateMap) {
			syntax |= VP9_SYN_SEG_UPDATE_MAP;
			if (frame.seg.temporalUpdate)
				syntax |= VP9_SYN_SEG_TEMPORAL;
		}
	}
	if (frame.usePrevFrameMvs)
		syntax |= VP9_SYN_PREV_MVS;
	if (frame.refreshProbs)
		syntax |= VP9_SYN_COUNTS;
	fDevice.Write(VP9_HDR_SYN, syntax);

	// the tiles, after the headers; the end at a 1 KiB boundary
	fDevice.Write(VP9_BITS_OFFSET, (uint32)(headers * 8));
	fDevice.Write(VP9_BITS_END, (fBitstream.bus + (uint32)fBitstream.size)
		>> 8);
	fDevice.Write(VP9_BITS_LEN, (uint32)((size - headers) * 8));
	fDevice.Write(VP9_BITS_ADDR, ((fBitstream.bus >> 8) & 0x0fffffff)
		| (1u << 30) | (1u << 29) | (1u << 28));

	fDevice.Write(VP9_PIC_SIZE, ((uint32)frame.width & 0x3fff)
		| ((uint32)frame.height & 0x3fff) << 16);
	if (intra)
		fDevice.Write(VP9_LAST_SCALE1, VP9_STRIDE_ALIGN_64);
	else
		_Vp9References(frame, references);

	fDevice.Write(VP9_CUR_LUMA, current.picture.bus >> 8);
	fDevice.Write(VP9_CUR_CHROMA,
		(current.picture.bus + (uint32)current.lumaSize) >> 8);
	fDevice.Write(VP9_SDRT_CTRL, 0);
	fDevice.Write(VP9_SDRT_LUMA, 0);
	fDevice.Write(VP9_SDRT_CHROMA, 0);
	if (frame.bitDepth > 8) {
		fDevice.Write(VP9_TWO_BIT_OFFSET, (uint32)fTwoBitOffset & 0x0fffffff);
		fDevice.Write(VP9_TEN_BIT_CONFIG, fTwoBitStride & 0x7ff);
	}

	fDevice.Write(VP9_NEIGHBOR_ADDR, fVp9Neighbour.bus >> 8);
	fDevice.Write(VP9_PROBS_ADDR, fVp9Probs.bus >> 8);
	fDevice.Write(VP9_COL_MV_ADDR, fVp9ColMv.bus >> 8);

	uint32 features = 0;
	if (frame.seg.enabled) {
		for (int i = 0; i < VP9_MAX_SEGMENTS; i++) {
			uint8 enabled = frame.seg.featureEnabled[i];
			if ((enabled & (1 << VP9_SEG_REF_FRAME)) != 0) {
				features |= 1u << i;
				features |= ((uint32)frame.seg.featureData[i][VP9_SEG_REF_FRAME]
					& 3) << (16 + i * 2);
			}
			if ((enabled & (1 << VP9_SEG_SKIP)) != 0)
				features |= 1u << (8 + i);
		}
	}
	fDevice.Write(VP9_SEG_FEATURES, features);

	_Vp9Tiles(frame);
	_Vp9Sram(frame);

	uint32 control = VP9_IRQ_MASK;
	// Allwinner's library does so for widths of 129 to 192
	uint32 alignedWidth = align(frame.width, 16);
	if (alignedWidth > 128 && alignedWidth <= 192
		&& align(frame.height, 16) > 64) {
		control |= VP9_DDR_CONSISTENCY;
	}
	fDevice.Write(VP9_FUNC_CTRL, control);
	fDevice.Write(VP9_CTB_NUM, 0);

	fDevice.Write(VP9_STATUS, 0xffffffff);
	fDevice.Write(VP9_TRIGGER, VP9_TRIG_INIT_SWDEC);
	fDevice.Write(VP9_CTB_NUM, 0);
	fDevice.Write(CEDAR_DEC_COUNTER, 0);
	fDevice.Write(CEDAR_DEC_COUNTER, CEDAR_DEC_COUNTER_ENABLE);
	status = _Run(VP9_TRIGGER, VP9_STATUS, "VP9 frame", SUNXI_VE_RUN_VP9);
	if (status != B_OK)
		return status;

	if (counts != NULL && frame.refreshProbs) {
		fDevice.SyncForCpu(fVp9Probs, VP9_PROBS_COUNTS, VP9_PROBS_COUNTS_SIZE);
		_Vp9Counts(*counts);
	}
	return B_OK;
}
