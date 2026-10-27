/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "CedarEngine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "CedarRegisters.h"


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
	fDdr128(getenv("CEDAR_DDR128") != NULL),
	fEntryPointsPlus1(getenv("CEDAR_EP_PLUS1") != NULL)
{

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
	fLumaSize = (size_t)fStride * fAlignedHeight;
	fChromaSize = fLumaSize / 2;
	fFrameSize = fLumaSize + fChromaSize;
	fMvcolSize = mvcolSize;
	fBitDepth = bitDepth;
	if (bitDepth > 8) {
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
CedarEngine::AllocateFrame(CedarFrame& frame)
{
	status_t status = fDevice.Allocate(frame.picture, fFrameSize);
	if (status == B_OK)
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
CedarEngine::_Run(uint32 trigger, uint32 statusRegister, const char* what)
{
	uint32 status = 0;
	status_t result = fDevice.Run(trigger, 8 /* decode slice */,
		statusRegister, status);
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
