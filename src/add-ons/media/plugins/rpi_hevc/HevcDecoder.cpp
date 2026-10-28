/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	What the decoder block wants to be told - the registers of its first
	phase and the order they are written in for slices, tiles and wavefront
	rows, the layout of its tables - was taken from the Linux driver of
	Raspberry Pi (Trading) Ltd, drivers/staging/media/rpivid/rpivid_h265.c;
	there is no other description of the block. */


#include "HevcDecoder.h"

#include <algorithm>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "SandConvert.h"


using namespace hevc;


#define TRACE(x...) \
	do { if (fTrace) fprintf(stderr, "rpi_hevc: " x); } while (false)

static const uint32 kMaxFrames = 24;
static const size_t kCommandBufferSize = 512 * 1024;

// registers of phase one, as offsets for the command list
enum {
	REG_SPS0 = 0,
	REG_SPS1 = 4,
	REG_PPS = 8,
	REG_SLICE = 12,
	REG_TILE_START = 16,
	REG_TILE_END = 20,
	REG_SLICE_START = 24,
	REG_MODE = 28,
	REG_QP = 48,
	REG_CONTROL = 52,
	REG_STATUS = 56,
	REG_BIT_BASE = 64,
	REG_BIT_COUNT = 68,
	REG_BIT_CONTROL = 72,
	REG_SLICE_COMMANDS = 96,
	REG_BEGIN_TILE_END = 100,
	REG_TRANSFER = 104,

	REG_PROBABILITIES = 0x1000,
	REG_SCALING_FACTORS = 0x2000,
	REG_SLICE_MESSAGES = 0x4000
};

// REG_TRANSFER: keep the CABAC state aside, and take it back
static const uint32 kProbabilitiesSave = (20 << 12) | (20 << 6);
static const uint32 kProbabilitiesRestore = (20 << 12) | 20;

static const uint32 kPauseWavefront = 1;
static const uint32 kPauseTile = 0xffff;

static const uint32 kScalingFactorCount = 4064;
static const uint32 kProbabilityCount = 154;

/*	The initial values of the CABAC context variables (H.265 tables 9-5 to
	9-37) in the order the block keeps them, for I, P and B slices. */
static const uint8 kProbabilityInit[3][156] = {
	{
		153, 200, 139, 141, 157, 154, 154, 154, 154, 154, 184, 154, 154,
		154, 184, 63,  154, 154, 154, 154, 154, 154, 154, 154, 154, 154,
		154, 154, 154, 153, 138, 138, 111, 141, 94,  138, 182, 154, 154,
		154, 140, 92,  137, 138, 140, 152, 138, 139, 153, 74,  149, 92,
		139, 107, 122, 152, 140, 179, 166, 182, 140, 227, 122, 197, 110,
		110, 124, 125, 140, 153, 125, 127, 140, 109, 111, 143, 127, 111,
		79,  108, 123, 63,  110, 110, 124, 125, 140, 153, 125, 127, 140,
		109, 111, 143, 127, 111, 79,  108, 123, 63,  91,  171, 134, 141,
		138, 153, 136, 167, 152, 152, 139, 139, 111, 111, 125, 110, 110,
		94,  124, 108, 124, 107, 125, 141, 179, 153, 125, 107, 125, 141,
		179, 153, 125, 107, 125, 141, 179, 153, 125, 140, 139, 182, 182,
		152, 136, 152, 136, 153, 136, 139, 111, 136, 139, 111, 0,   0,
	},
	{
		153, 185, 107, 139, 126, 197, 185, 201, 154, 149, 154, 139, 154,
		154, 154, 152, 110, 122, 95,  79,  63,  31,  31,  153, 153, 168,
		140, 198, 79,  124, 138, 94,  153, 111, 149, 107, 167, 154, 154,
		154, 154, 196, 196, 167, 154, 152, 167, 182, 182, 134, 149, 136,
		153, 121, 136, 137, 169, 194, 166, 167, 154, 167, 137, 182, 125,
		110, 94,  110, 95,  79,  125, 111, 110, 78,  110, 111, 111, 95,
		94,  108, 123, 108, 125, 110, 94,  110, 95,  79,  125, 111, 110,
		78,  110, 111, 111, 95,  94,  108, 123, 108, 121, 140, 61,  154,
		107, 167, 91,  122, 107, 167, 139, 139, 155, 154, 139, 153, 139,
		123, 123, 63,  153, 166, 183, 140, 136, 153, 154, 166, 183, 140,
		136, 153, 154, 166, 183, 140, 136, 153, 154, 170, 153, 123, 123,
		107, 121, 107, 121, 167, 151, 183, 140, 151, 183, 140, 0,   0,
	},
	{
		153, 160, 107, 139, 126, 197, 185, 201, 154, 134, 154, 139, 154,
		154, 183, 152, 154, 137, 95,  79,  63,  31,  31,  153, 153, 168,
		169, 198, 79,  224, 167, 122, 153, 111, 149, 92,  167, 154, 154,
		154, 154, 196, 167, 167, 154, 152, 167, 182, 182, 134, 149, 136,
		153, 121, 136, 122, 169, 208, 166, 167, 154, 152, 167, 182, 125,
		110, 124, 110, 95,  94,  125, 111, 111, 79,  125, 126, 111, 111,
		79,  108, 123, 93,  125, 110, 124, 110, 95,  94,  125, 111, 111,
		79,  125, 126, 111, 111, 79,  108, 123, 93,  121, 140, 61,  154,
		107, 167, 91,  107, 107, 167, 139, 139, 170, 154, 139, 153, 139,
		123, 123, 63,  124, 166, 183, 140, 136, 153, 154, 166, 183, 140,
		136, 153, 154, 166, 183, 140, 136, 153, 154, 170, 153, 138, 138,
		122, 121, 122, 121, 167, 151, 183, 140, 151, 183, 140, 0,   0,
	},
};


static inline uint32
align_up(uint32 value, uint32 alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}


/*!	Buffer sizes are three times a power of two: the next size up is
	twice as much. */
static size_t
round_up_size(size_t size)
{
	size_t step = 256;
	while (step * 2 <= size)
		step *= 2;
	return 3 * step;
}


bool
HevcDecoder::Geometry::operator==(const Geometry& other) const
{
	return width == other.width && height == other.height
		&& bitDepth == other.bitDepth && cropLeft == other.cropLeft
		&& cropTop == other.cropTop && shownWidth == other.shownWidth
		&& shownHeight == other.shownHeight;
}


//	#pragma mark - the command list of phase one


/*!	Writes the list of register writes that takes the block through the
	slices of one picture. */
class HevcDecoder::CommandBuilder {
public:
	CommandBuilder(HevcDecoder& decoder)
		:
		fDecoder(decoder),
		fSps(*decoder.fSps),
		fPps(*decoder.fPps),
		fCommands((rpi_hevc_command*)decoder.fCommands.address),
		fMax(decoder.fCommands.size / sizeof(rpi_hevc_command)),
		fCount(0),
		fOverflow(false),
		fSliceStart(0),
		fEntryTileX(0),
		fEntryTileY(0),
		fEntryCtbX(0),
		fEntryCtbY(0),
		fEntryQp(0),
		fEntrySlice(0),
		fCollocated(-1)
	{
		fCtbWidth = fSps.ctbWidth;
		fCtbHeight = fSps.ctbHeight;
		fTileColumns = fPps.numTileColumns;
		fTileRows = fPps.numTileRows;
	}

	/*!	\a lists: for each reference of the slice's two lists, its place
		among the picture's references. */
	void AddSlice(const Slice& slice, uint32 index,
		const uint8 lists[2][kMaxReferences], bool last)
	{
		const SliceHeader& header = slice.header;
		fHeader = &header;
		fLists = lists;

		fSliceQp = 26 + fPps.initQpMinus26 + header.qpDelta;
		fMaxNumMergeCand = header.type == SLICE_I ? 0 : header.maxNumMergeCand;
		fReferences[0] = header.type == SLICE_I ? 0 : header.numRefIdx[0];
		fReferences[1] = header.type == SLICE_B ? header.numRefIdx[1] : 0;

		fStartTs = fDecoder.fRsToTs[header.segmentAddress];
		fStartCtbX = header.segmentAddress % fCtbWidth;
		fStartCtbY = header.segmentAddress / fCtbWidth;
		uint32 previous = fStartTs == 0 ? 0 : fDecoder.fTsToRs[fStartTs - 1];
		fPreviousCtbX = previous % fCtbWidth;
		fPreviousCtbY = previous / fCtbWidth;

		if (fPps.entropyCodingSyncEnabled)
			_WavefrontSlice(slice, index, last);
		else
			_TileSlice(slice, index, last);
	}

	uint32 Count() const { return fCount; }
	bool Overflow() const { return fOverflow; }
	int32 Collocated() const { return fCollocated; }

private:
	void _Write(uint32 address, uint32 data)
	{
		if (fCount >= fMax) {
			fOverflow = true;
			return;
		}
		fCommands[fCount].address = address;
		fCommands[fCount].data = data;
		fCount++;
	}

	uint32 _TileX(uint32 ctbX) const
	{
		uint32 i = 1;
		while (ctbX >= fDecoder.fColumnBoundary[i])
			i++;
		return i - 1;
	}

	uint32 _TileY(uint32 ctbY) const
	{
		uint32 i = 1;
		while (ctbY >= fDecoder.fRowBoundary[i])
			i++;
		return i - 1;
	}

	/*!	The context variables as the slice starts with them (9.3.2.2). */
	void _WriteProbabilities()
	{
		const SliceHeader& header = *fHeader;
		uint32 kind = header.cabacInit && header.type != SLICE_I
			? header.type + 1 : 2 - header.type;
		const uint8* init = kProbabilityInit[kind];
		int qp = std::max(0, std::min(51, fSliceQp));

		uint8 values[156];
		for (uint32 i = 0; i < kProbabilityCount; i++) {
			int slope = (init[i] >> 4) * 5 - 45;
			int offset = ((init[i] & 15) << 3) - 16;
			int state = 2 * (((slope * qp) >> 4) + offset) - 127;
			state ^= state >> 31;
			if (state > 124)
				state = 124 + (state & 1);
			values[i] = (uint8)state;
		}
		values[154] = values[155] = 0;

		for (uint32 i = 0; i < 156; i += 4) {
			_Write(REG_PROBABILITIES + i, values[i] | (values[i + 1] << 8)
				| (values[i + 2] << 16) | ((uint32)values[i + 3] << 24));
		}
		// kept aside at once: a later tile or row may start from them
		_Write(REG_TRANSFER, kProbabilitiesSave);
	}

	void _WriteScalingFactors()
	{
		const ScalingList& list = fPps.scalingListPresent
			? fPps.scalingList : fSps.scalingList;

		uint8 factors[kScalingFactorCount];
		memset(factors, 0, sizeof(factors));
		uint8* out = factors;
		for (int matrix = 0; matrix < 6; matrix++, out += 16)
			memcpy(out, list.list4x4[matrix], 16);
		for (int matrix = 0; matrix < 6; matrix++, out += 64)
			memcpy(out, list.list8x8[matrix], 64);
		// each value of the larger ones for a square of 2 or 4
		for (int matrix = 0; matrix < 6; matrix++, out += 256) {
			for (int y = 0; y < 16; y++) {
				for (int x = 0; x < 16; x++) {
					out[y * 16 + x]
						= list.list16x16[matrix][(y >> 1) * 8 + (x >> 1)];
				}
			}
			out[0] = list.dc16x16[matrix];
		}
		for (int matrix = 0; matrix < 2; matrix++, out += 1024) {
			for (int y = 0; y < 32; y++) {
				for (int x = 0; x < 32; x++) {
					out[y * 32 + x]
						= list.list32x32[matrix][(y >> 2) * 8 + (x >> 2)];
				}
			}
			out[0] = list.dc32x32[matrix];
		}

		for (uint32 i = 0; i < kScalingFactorCount; i += 4) {
			_Write(REG_SCALING_FACTORS + i, factors[i] | (factors[i + 1] << 8)
				| (factors[i + 2] << 16) | ((uint32)factors[i + 3] << 24));
		}
	}

	void _WriteBitstream(const Slice& slice)
	{
		uint64 address = fDecoder.fBitstream.physical + slice.dataOffset;
		uint32 offset = address & 63;
		_Write(REG_BIT_BASE, (uint32)(address >> 6));
		_Write(REG_BIT_COUNT, slice.dataSize);
		_Write(REG_BIT_CONTROL, offset | (1 << 7));
			// stop
		_Write(REG_BIT_CONTROL, offset | (1 << 6));
			// and go, taking the emulation prevention bytes out
	}

	/*!	What is the same for every tile and row of the slice. */
	uint32 _SliceRegister() const
	{
		const SliceHeader& header = *fHeader;
		uint32 value = fMaxNumMergeCand | (fReferences[0] << 4)
			| (fReferences[1] << 8) | ((uint32)header.type << 12);
		if (header.saoLuma)
			value |= 1 << 14;
		if (header.saoChroma)
			value |= 1 << 15;
		if (header.type == SLICE_B && header.mvdL1Zero)
			value |= 1 << 16;
		return value;
	}

	void _NewSliceSegment()
	{
		const SliceHeader& header = *fHeader;

		_Write(REG_SPS0, fSps.log2MinCbSize | (fSps.log2CtbSize << 4)
			| (fSps.log2MinTbSize << 8)
			| ((fSps.log2MinTbSize + fSps.log2DiffMaxMinTbSize) << 12)
			| (fSps.bitDepthLuma << 16) | (fSps.bitDepthChroma << 20)
			| (fSps.maxTransformHierarchyDepthIntra << 24)
			| ((uint32)fSps.maxTransformHierarchyDepthInter << 28));

		_Write(REG_SPS1, fSps.pcmBitDepthLuma | (fSps.pcmBitDepthChroma << 4)
			| (fSps.log2MinPcmCbSize << 8)
			| ((fSps.log2MinPcmCbSize + fSps.log2DiffMaxMinPcmCbSize) << 12)
			| ((fSps.separateColourPlane ? 0 : fSps.chromaFormat) << 16)
			| (fSps.ampEnabled ? 1 << 18 : 0)
			| (fSps.pcmEnabled ? 1 << 19 : 0)
			| (fSps.scalingListEnabled ? 1 << 20 : 0)
			| (fSps.strongIntraSmoothing ? 1 << 21 : 0));

		_Write(REG_PPS, (fSps.log2CtbSize - fPps.diffCuQpDeltaDepth)
			| (fPps.cuQpDeltaEnabled ? 1 << 4 : 0)
			| (fPps.transquantBypassEnabled ? 1 << 5 : 0)
			| (fPps.transformSkipEnabled ? 1 << 6 : 0)
			| (fPps.signDataHiding ? 1 << 7 : 0)
			| (((fPps.cbQpOffset + header.cbQpOffset) & 255) << 8)
			| (((fPps.crQpOffset + header.crQpOffset) & 255) << 16)
			| (fPps.constrainedIntraPred ? 1 << 24 : 0));

		if (fStartTs == 0 && fSps.scalingListEnabled)
			_WriteScalingFactors();

		if (!header.dependentSliceSegment)
			fSliceStart = fStartCtbX | (fStartCtbY << 16);
		_Write(REG_SLICE_START, fSliceStart);
	}

	/*!	The slice's references and weights, the filter settings: a list of
		words of its own. */
	void _SliceMessages(uint32 index)
	{
		const SliceHeader& header = *fHeader;
		uint16 messages[2 * kMaxReferences * 8 + 3];
		uint32 count = 0;

		uint32 command = header.type == SLICE_I ? 1
			: header.type == SLICE_P ? 2 : 3;
		command |= (fReferences[0] << 2) | (fReferences[1] << 6)
			| (fMaxNumMergeCand << 11);
		bool collocatedFromL0 = !header.temporalMvpEnabled
			|| header.type != SLICE_B || header.collocatedFromL0;
		if (collocatedFromL0)
			command |= 1 << 14;

		if (header.type != SLICE_I) {
			// are all the references pictures from before?
			bool noBackward = true;
			for (int list = 0; list < 2; list++) {
				for (uint32 i = 0; i < fReferences[list]; i++) {
					if (fDecoder.fDpb[fLists[list][i]]->poc > fDecoder.fPoc)
						noBackward = false;
				}
			}
			if (noBackward)
				command |= 1 << 10;
			messages[count++] = (uint16)command;

			if (header.temporalMvpEnabled) {
				fCollocated = fLists[collocatedFromL0 ? 0 : 1][
					header.collocatedRefIdx];
			}

			bool weighted = header.type == SLICE_P ? fPps.weightedPred
				: fPps.weightedBipred;
			const PredWeightTable& table = header.weights;
			for (int list = 0; list < 2; list++) {
				for (uint32 i = 0; i < fReferences[list]; i++) {
					uint32 place = fLists[list][i];
					const Frame* frame = fDecoder.fDpb[place];
					messages[count++] = (uint16)(place
						| (frame->longTerm ? 1 << 4 : 0)
						| (weighted ? 3 << 5 : 0));
					messages[count++] = (uint16)(frame->poc & 0xffff);
					if (!weighted)
						continue;
					messages[count++] = (uint16)(table.lumaLog2Denom
						| ((table.lumaWeight[list][i] & 0x1ff) << 3));
					messages[count++]
						= (uint16)(table.lumaOffset[list][i] & 0xff);
					for (int j = 0; j < 2; j++) {
						messages[count++] = (uint16)(table.chromaLog2Denom
							| ((table.chromaWeight[list][i][j] & 0x1ff) << 3));
						messages[count++]
							= (uint16)(table.chromaOffset[list][i][j] & 0xff);
					}
				}
			}
		} else
			messages[count++] = (uint16)command;

		messages[count++] = (uint16)((header.betaOffsetDiv2 & 15)
			| ((header.tcOffsetDiv2 & 15) << 4)
			| (header.deblockingDisabled ? 1 << 8 : 0)
			| (header.loopFilterAcrossSlices ? 1 << 9 : 0)
			| (fPps.loopFilterAcrossTiles ? 1 << 10 : 0));
		messages[count++] = (uint16)(((header.crQpOffset & 31) << 5)
			| (header.cbQpOffset & 31));

		_Write(REG_SLICE_COMMANDS, count | (index << 8));
		for (uint32 i = 0; i < count; i++)
			_Write(REG_SLICE_MESSAGES + 4 * i, messages[i]);
	}

	/*!	Starts the block at a coding tree block: of a slice, of a tile, or
		of a row in wavefront mode. Uses nothing of the current slice but
		what is passed in, since it also fills in for the slice before. */
	void _EntryPoint(bool beginTileEnd, bool resetQp, uint32 pauseMode,
		uint32 tileX, uint32 tileY, uint32 ctbX, uint32 ctbY, uint32 qp,
		uint32 sliceRegister)
	{
		uint32 endX = fDecoder.fColumnBoundary[tileX + 1] - 1;
		uint32 endY = pauseMode == kPauseWavefront
			? ctbY : fDecoder.fRowBoundary[tileY + 1] - 1;

		_Write(REG_TILE_START, fDecoder.fColumnBoundary[tileX]
			| (fDecoder.fRowBoundary[tileY] << 16));
		_Write(REG_TILE_END, endX | (endY << 16));
		if (beginTileEnd)
			_Write(REG_BEGIN_TILE_END, endX | (endY << 16));

		// the size of the last block in each direction
		uint32 ctbSize = 1u << fSps.log2CtbSize;
		uint32 lastWidth = fSps.width & (ctbSize - 1);
		uint32 lastHeight = fSps.height & (ctbSize - 1);
		_Write(REG_SLICE, sliceRegister
			| ((endX + 1 < fCtbWidth || lastWidth == 0
				? ctbSize : lastWidth) << 17)
			| ((endY + 1 < fCtbHeight || lastHeight == 0
				? ctbSize : lastHeight) << 24));

		if (resetQp)
			_Write(REG_QP, 6 * (fSps.bitDepthLuma - 8) + qp);

		_Write(REG_MODE, pauseMode | (endX == fCtbWidth - 1 ? 1 << 17 : 0)
			| (endY == fCtbHeight - 1 ? 1 << 18 : 0));
		_Write(REG_CONTROL, ctbX | (ctbY << 16));

		fEntryTileX = tileX;
		fEntryTileY = tileY;
		fEntryCtbX = ctbX;
		fEntryCtbY = ctbY;
		fEntryQp = qp;
		fEntrySlice = sliceRegister;
	}

	// wavefront rows

	void _WavefrontPause(uint32 ctbY)
	{
		// after the second block of the row the state is kept for the next
		_Write(REG_STATUS, (ctbY << 18) | 0x25);
		_Write(REG_TRANSFER, kProbabilitiesSave);
		_Write(REG_MODE, ctbY == fCtbHeight - 1 ? 0x70000 : 0x30000);
		_Write(REG_CONTROL, (ctbY << 16) + 2);
	}

	/*!	The rows from the last entry point up to row \a lastY. */
	void _WavefrontFill(uint32 lastY)
	{
		uint32 lastX = fCtbWidth - 1;
		while (fEntryCtbY < lastY && !fOverflow) {
			if (fCtbWidth > 2)
				_WavefrontPause(fEntryCtbY);
			_Write(REG_STATUS, (fEntryCtbY << 18) | (lastX << 5) | 2);

			// one block wide: the state kept is the initial one
			_Write(REG_TRANSFER, fCtbWidth == 2
				? kProbabilitiesSave : kProbabilitiesRestore);

			_EntryPoint(false, true, kPauseWavefront, 0, 0, 0,
				fEntryCtbY + 1, fEntryQp, fEntrySlice);
		}
	}

	void _WavefrontSlice(const Slice& slice, uint32 index, bool last)
	{
		const SliceHeader& header = *fHeader;
		bool independent = !header.dependentSliceSegment;
		bool resetQp = true;

		if (fStartTs != 0) {
			// the slice before ends where this one starts
			_WavefrontFill(fPreviousCtbY);
			if (fEntryCtbX < 2
				&& (fEntryCtbY < fStartCtbY || fStartCtbX > 2)
				&& fCtbWidth > 2) {
				_WavefrontPause(fPreviousCtbY);
			}
			_Write(REG_STATUS, 1 | (fPreviousCtbX << 5)
				| (fPreviousCtbY << 18));
			if (fStartCtbX == 2
				|| (fCtbWidth == 2 && fEntryCtbY < fStartCtbY)) {
				_Write(REG_TRANSFER, kProbabilitiesSave);
			}
		}

		_WriteBitstream(slice);

		if (fStartTs == 0 || independent || fCtbWidth == 1)
			_WriteProbabilities();
		else if (fStartCtbX == 0)
			_Write(REG_TRANSFER, kProbabilitiesRestore);
		else
			resetQp = false;

		_SliceMessages(index);
		_NewSliceSegment();
		_EntryPoint(independent, resetQp, kPauseWavefront, 0, 0, fStartCtbX,
			fStartCtbY, fSliceQp, _SliceRegister());

		if (last) {
			_WavefrontFill(fCtbHeight - 1);
			if (fEntryCtbX < 2 && fCtbWidth > 2)
				_WavefrontPause(fCtbHeight - 1);
			_Write(REG_STATUS, 1 | ((fCtbWidth - 1) << 5)
				| ((fCtbHeight - 1) << 18));
		}
	}

	// tiles (a picture without tiles is one tile)

	/*!	The tiles from the last entry point up to the given one. */
	void _TileFill(uint32 lastTileX, uint32 lastTileY)
	{
		while ((fEntryTileY < lastTileY
				|| (fEntryTileY == lastTileY && fEntryTileX < lastTileX))
			&& !fOverflow) {
			uint32 tileX = fEntryTileX;
			uint32 tileY = fEntryTileY;
			uint32 lastX = fDecoder.fColumnBoundary[tileX + 1] - 1;
			uint32 lastY = fDecoder.fRowBoundary[tileY + 1] - 1;

			_Write(REG_STATUS, 2 | (lastX << 5) | (lastY << 18));
			_Write(REG_TRANSFER, kProbabilitiesRestore);

			if (++tileX >= fTileColumns) {
				tileX = 0;
				tileY++;
			}
			_EntryPoint(false, true, kPauseTile, tileX, tileY,
				fDecoder.fColumnBoundary[tileX], fDecoder.fRowBoundary[tileY],
				fEntryQp, fEntrySlice);
		}
	}

	void _TileSlice(const Slice& slice, uint32 index, bool last)
	{
		const SliceHeader& header = *fHeader;
		uint32 tileX = _TileX(fStartCtbX);
		uint32 tileY = _TileY(fStartCtbY);

		if (fStartTs != 0) {
			// the slice before ends where this one starts
			_TileFill(_TileX(fPreviousCtbX), _TileY(fPreviousCtbY));
			_Write(REG_STATUS, 1 | (fPreviousCtbX << 5)
				| (fPreviousCtbY << 18));
		}

		_WriteBitstream(slice);

		bool resetQp = fStartTs == 0 || !header.dependentSliceSegment
			|| tileX != _TileX(fPreviousCtbX)
			|| tileY != _TileY(fPreviousCtbY);
		if (resetQp)
			_WriteProbabilities();

		_SliceMessages(index);
		_NewSliceSegment();
		_EntryPoint(!header.dependentSliceSegment, resetQp, kPauseTile, tileX,
			tileY, fStartCtbX, fStartCtbY, fSliceQp, _SliceRegister());

		if (last) {
			_TileFill(fTileColumns - 1, fTileRows - 1);
			_Write(REG_STATUS, 1 | ((fCtbWidth - 1) << 5)
				| ((fCtbHeight - 1) << 18));
		}
	}

	HevcDecoder&		fDecoder;
	const SPS&			fSps;
	const PPS&			fPps;
	rpi_hevc_command*	fCommands;
	uint32				fMax;
	uint32				fCount;
	bool				fOverflow;

	uint32				fCtbWidth;
	uint32				fCtbHeight;
	uint32				fTileColumns;
	uint32				fTileRows;

	// of the slice being added
	const SliceHeader*	fHeader;
	const uint8			(*fLists)[kMaxReferences];
	int					fSliceQp;
	uint32				fMaxNumMergeCand;
	uint32				fReferences[2];
	uint32				fStartTs;
	uint32				fStartCtbX;
	uint32				fStartCtbY;
	uint32				fPreviousCtbX;
	uint32				fPreviousCtbY;

	// carried from slice to slice
	uint32				fSliceStart;
	uint32				fEntryTileX;
	uint32				fEntryTileY;
	uint32				fEntryCtbX;
	uint32				fEntryCtbY;
	uint32				fEntryQp;
	uint32				fEntrySlice;
	int32				fCollocated;
};


//	#pragma mark - HevcDecoder


HevcDecoder::HevcDecoder()
	:
	fDevice(-1),
	fTrace(getenv("RPI_HEVC_TRACE") != NULL),
	fSps(NULL),
	fPps(NULL),
	fConfigured(false),
	fCommandCount(0),
	fBitstreamUsed(0),
	fPicturePts(0),
	fSkipBefore(INT64_MIN),
	fAwaitRandomAccess(true),
	fFirstPicture(true),
	fNoRaslOutput(false),
	fPrevTid0Poc(0),
	fPoc(0),
	fCurrent(NULL),
	fDpbCount(0),
	fNumStCurrBefore(0),
	fNumStCurrAfter(0),
	fNumLtCurr(0),
	fDecoded(0)
{
	memset(fSpsList, 0, sizeof(fSpsList));
	memset(fPpsList, 0, sizeof(fPpsList));
	memset(&fGeometry, 0, sizeof(fGeometry));
	memset(fDpb, 0, sizeof(fDpb));
}


HevcDecoder::~HevcDecoder()
{
	Close();
}


status_t
HevcDecoder::_Fail(const char* format, ...)
{
	char text[256];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	fError = text;
	TRACE("%s\n", text);
	return B_ERROR;
}


status_t
HevcDecoder::Open(BString* _error)
{
	fDevice = open(RPI_HEVC_DEVICE_PATH, O_RDWR);
	if (fDevice < 0) {
		status_t status = errno;
		fError.SetToFormat("%s: %s", RPI_HEVC_DEVICE_PATH, strerror(status));
		if (_error != NULL)
			*_error = fError;
		return status;
	}

	status_t status = _Allocate(fCommands, kCommandBufferSize, true);
	if (status != B_OK) {
		fError.SetToFormat("no memory for the decoder: %s", strerror(status));
		if (_error != NULL)
			*_error = fError;
		Close();
		return status;
	}
	return B_OK;
}


void
HevcDecoder::Close()
{
	for (Frame* frame : fFrames) {
		if (frame->buffer.clone >= 0)
			delete_area(frame->buffer.clone);
		delete frame;
	}
	fFrames.clear();
	fOutput.clear();
	fCurrent = NULL;

	for (Buffer* buffer : {&fCommands, &fBitstream, &fPu, &fCoeff}) {
		if (buffer->clone >= 0)
			delete_area(buffer->clone);
		*buffer = Buffer();
	}

	// the driver frees what was allocated with the file
	if (fDevice >= 0)
		close(fDevice);
	fDevice = -1;
	fConfigured = false;
}


status_t
HevcDecoder::_Allocate(Buffer& buffer, size_t size, bool map)
{
	rpi_hevc_allocate request = {};
	request.size = size;
	if (ioctl(fDevice, RPI_HEVC_ALLOCATE, &request, sizeof(request)) != 0)
		return errno;

	buffer.id = request.buffer;
	buffer.size = request.size;
	buffer.physical = request.physical;
	buffer.clone = -1;
	buffer.address = NULL;
	if (!map)
		return B_OK;

	void* address;
	buffer.clone = clone_area("rpi_hevc buffer", &address, B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA, request.area);
	if (buffer.clone < 0) {
		status_t status = buffer.clone;
		_Free(buffer);
		return status;
	}
	buffer.address = (uint8*)address;
	return B_OK;
}


void
HevcDecoder::_Free(Buffer& buffer)
{
	if (buffer.clone >= 0)
		delete_area(buffer.clone);
	if (buffer.id != RPI_HEVC_NO_BUFFER)
		ioctl(fDevice, RPI_HEVC_FREE, &buffer.id, sizeof(buffer.id));
	buffer = Buffer();
}


//	#pragma mark - pictures in and out


status_t
HevcDecoder::PutNal(const uint8* nal, size_t size, int64 pts)
{
	if (size < 2)
		return B_OK;
	if (fDevice < 0)
		return B_NO_INIT;

	uint8 type = (nal[0] >> 1) & 0x3f;
	uint8 layer = ((nal[0] & 1) << 5) | (nal[1] >> 3);
	if (layer != 0)
		return B_OK;

	if (type <= NAL_VCL_LAST) {
		if (type > NAL_IRAP_LAST || (type > NAL_RASL_R && type < NAL_BLA_W_LP))
			return B_OK;	// reserved
		// The first slice segment of a picture ends the one before.
		if (size > 2 && (nal[2] & 0x80) != 0) {
			status_t status = _DecodePicture();
			if (status != B_OK)
				return status;
		}
		return _AddSlice(nal, size, pts);
	}

	// Anything else of these ends the picture too.
	if (type == NAL_VPS || type == NAL_SPS || type == NAL_PPS
		|| type == NAL_AUD || type == NAL_EOS || type == NAL_EOB
		|| type == NAL_PREFIX_SEI) {
		status_t status = _DecodePicture();
		if (status != B_OK)
			return status;
	}

	switch (type) {
		case NAL_SPS:
		{
			SPS sps;
			if (!sps.Parse(nal, size)) {
				TRACE("a sequence parameter set that cannot be read\n");
				break;
			}
			fSpsList[sps.id] = sps;
			break;
		}

		case NAL_PPS:
		{
			PPS pps;
			if (!pps.Parse(nal, size)) {
				TRACE("a picture parameter set that cannot be read\n");
				break;
			}
			fPpsList[pps.id] = pps;
			break;
		}

		case NAL_EOS:
		case NAL_EOB:
			// what follows starts like a new stream
			fFirstPicture = true;
			break;
	}
	return B_OK;
}


status_t
HevcDecoder::EndAccessUnit()
{
	return _DecodePicture();
}


void
HevcDecoder::Drain()
{
	_DecodePicture();
	_BumpAll();
	fFirstPicture = true;
}


void
HevcDecoder::Reset()
{
	fSlices.clear();
	fBitstreamUsed = 0;
	fCurrent = NULL;
	fOutput.clear();
	for (Frame* frame : fFrames) {
		frame->shortTerm = frame->longTerm = false;
		frame->neededForOutput = false;
		frame->queued = false;
		frame->current = false;
	}
	_ReleaseUnused();
	fAwaitRandomAccess = true;
	fFirstPicture = true;
	fNoRaslOutput = false;
}


bool
HevcDecoder::NextPicture(Picture& picture)
{
	if (fOutput.empty())
		return false;
	Frame* frame = fOutput.front();
	fOutput.pop_front();

	picture.frame = -1;
	for (size_t i = 0; i < fFrames.size(); i++) {
		if (fFrames[i] == frame)
			picture.frame = (int32)i;
	}
	picture.pts = frame->pts;
	picture.poc = frame->poc;
	picture.width = frame->geometry.shownWidth;
	picture.height = frame->geometry.shownHeight;
	picture.bitDepth = frame->geometry.bitDepth;
	picture.corrupt = frame->corrupt;
	return true;
}


void
HevcDecoder::ReleasePicture(const Picture& picture)
{
	if (picture.frame < 0 || (size_t)picture.frame >= fFrames.size())
		return;
	fFrames[picture.frame]->queued = false;
}


void
HevcDecoder::CopyPlanes(const Picture& picture, uint8* target, uint32 stride,
	bool interleaved, bool half)
{
	if (picture.frame < 0 || (size_t)picture.frame >= fFrames.size())
		return;
	const Frame* frame = fFrames[picture.frame];
	const Geometry& geometry = frame->geometry;

	SandPicture sand;
	sand.data = frame->buffer.address;
	sand.columnStride = geometry.columnStride;
	sand.chromaOffset = geometry.chromaOffset;
	sand.left = geometry.cropLeft;
	sand.top = geometry.cropTop;
	sand.width = geometry.shownWidth;
	sand.height = geometry.shownHeight;

	if (half) {
		uint32 rows = geometry.shownHeight / 2;
		uint8* chroma = target + (size_t)stride * rows;
		if (geometry.bitDepth > 8)
			sand30_to_p010_half(sand, target, stride, chroma, stride);
		else {
			sand8_to_i420_half(sand, target, stride, chroma,
				chroma + (size_t)(stride / 2) * ((rows + 1) / 2), stride / 2);
		}
		return;
	}

	uint8* chroma = target + (size_t)stride * geometry.shownHeight;
	uint32 chromaRows = (geometry.shownHeight + 1) / 2;
	if (geometry.bitDepth > 8)
		sand30_to_p010(sand, target, stride, chroma, stride);
	else if (interleaved)
		sand8_to_nv12(sand, target, stride, chroma, stride);
	else {
		sand8_to_i420(sand, target, stride, chroma,
			chroma + (size_t)(stride / 2) * chromaRows, stride / 2);
	}
}


//	#pragma mark - a picture's slices


status_t
HevcDecoder::_AddSlice(const uint8* nal, size_t size, int64 pts)
{
	Slice slice;
	const SliceHeader* previous = fSlices.empty()
		? NULL : &fSlices.back().header;
	if (!parse_slice_header(nal, size, fSpsList, fPpsList, previous,
			slice.header)) {
		TRACE("a slice header that cannot be read (NAL type %d)\n",
			(nal[0] >> 1) & 0x3f);
		return B_OK;
	}
	const SliceHeader& header = slice.header;

	if (fSlices.empty()) {
		if (!header.firstSliceSegmentInPic)
			return B_OK;	// the start of the picture is missing
		bool randomAccess = header.nalType >= NAL_BLA_W_LP
			&& header.nalType <= NAL_IRAP_LAST;
		if (fAwaitRandomAccess && !randomAccess)
			return B_OK;
		// pictures that lead up to a random access point decoding starts at
		if ((header.nalType == NAL_RASL_N || header.nalType == NAL_RASL_R)
			&& fNoRaslOutput) {
			return B_OK;
		}
		// A picture nobody wants to see and nothing refers to: one of the
		// highest sub-layer that says so in its type.
		if (pts < fSkipBefore && header.nalType < NAL_BLA_W_LP
			&& (header.nalType & 1) == 0) {
			const SPS& sps = fSpsList[fPpsList[header.ppsId].spsId];
			if (header.temporalId >= sps.maxSubLayersMinus1)
				return B_OK;
		}
		fPicturePts = pts;
		fBitstreamUsed = 0;
	} else if (header.ppsId != fSlices[0].header.ppsId)
		return B_OK;

	// the slice data, where the block can read it
	slice.dataSize = (uint32)(size - header.dataOffset);
	slice.dataOffset = fBitstreamUsed;
	size_t needed = (size_t)fBitstreamUsed + align_up(slice.dataSize, 64) + 64;
	if (needed > fBitstream.size) {
		Buffer larger;
		size_t newSize = std::max(round_up_size(needed * 2),
			(size_t)1024 * 1024);
		status_t status = _Allocate(larger, newSize, true);
		if (status != B_OK)
			return _Fail("no memory for the stream: %s", strerror(status));
		if (fBitstreamUsed > 0)
			memcpy(larger.address, fBitstream.address, fBitstreamUsed);
		_Free(fBitstream);
		fBitstream = larger;
	}
	memcpy(fBitstream.address + slice.dataOffset, nal + header.dataOffset,
		slice.dataSize);
	fBitstreamUsed += align_up(slice.dataSize, 64);

	fSlices.push_back(slice);
	return B_OK;
}


/*!	The tile boundaries and the two orders of the coding tree blocks: by
	row over the picture and as the stream has them, tile by tile.
*/
void
HevcDecoder::_UpdateTiles()
{
	const SPS& sps = *fSps;
	const PPS& pps = *fPps;
	uint32 columns = pps.numTileColumns;
	uint32 rows = pps.numTileRows;

	fColumnBoundary.assign(columns + 1, 0);
	fRowBoundary.assign(rows + 1, 0);
	for (uint32 i = 1; i < columns; i++) {
		fColumnBoundary[i] = pps.uniformSpacing
			? (i * sps.ctbWidth) / columns
			: fColumnBoundary[i - 1] + pps.columnWidth[i - 1];
		fColumnBoundary[i] = std::min(fColumnBoundary[i], sps.ctbWidth);
	}
	fColumnBoundary[columns] = sps.ctbWidth;
	for (uint32 i = 1; i < rows; i++) {
		fRowBoundary[i] = pps.uniformSpacing
			? (i * sps.ctbHeight) / rows
			: fRowBoundary[i - 1] + pps.rowHeight[i - 1];
		fRowBoundary[i] = std::min(fRowBoundary[i], sps.ctbHeight);
	}
	fRowBoundary[rows] = sps.ctbHeight;

	uint32 count = sps.ctbWidth * sps.ctbHeight;
	fRsToTs.assign(count, 0);
	fTsToRs.assign(count, 0);
	uint32 ts = 0;
	for (uint32 tileY = 0; tileY < rows; tileY++) {
		for (uint32 tileX = 0; tileX < columns; tileX++) {
			for (uint32 y = fRowBoundary[tileY]; y < fRowBoundary[tileY + 1];
					y++) {
				for (uint32 x = fColumnBoundary[tileX];
						x < fColumnBoundary[tileX + 1]; x++) {
					uint32 rs = y * sps.ctbWidth + x;
					fRsToTs[rs] = ts;
					fTsToRs[ts] = rs;
					ts++;
				}
			}
		}
	}
}


/*!	Sizes everything for the pictures of \a sps. */
status_t
HevcDecoder::_Configure(const SPS& sps)
{
	Geometry geometry = {};
	geometry.width = sps.width;
	geometry.height = sps.height;
	geometry.bitDepth = sps.bitDepthLuma;
	// Columns of 128 bytes: 128 samples of eight bits, or 96 of ten.
	uint32 alignedHeight = align_up(sps.height, 16);
	if (sps.bitDepthLuma == 8)
		geometry.columns = align_up(sps.width, 128) / 128;
	else
		geometry.columns = align_up((sps.width + 2) / 3, 32) / 32;
	geometry.columnStride = alignedHeight * 3 / 2 * 128;
	geometry.chromaOffset = alignedHeight * 128;
	geometry.frameSize = geometry.columnStride * geometry.columns;
	geometry.mvStride = align_up(sps.width, 64);
	geometry.mvSize = geometry.mvStride * (align_up(sps.height, 64) >> 4);
	geometry.cropLeft = sps.cropLeft;
	geometry.cropTop = sps.cropTop;
	geometry.shownWidth = sps.width - sps.cropLeft - sps.cropRight;
	geometry.shownHeight = sps.height - sps.cropTop - sps.cropBottom;

	if (fConfigured && geometry == fGeometry)
		return B_OK;

	TRACE("%" B_PRIu32 "x%" B_PRIu32 " (%" B_PRIu32 "x%" B_PRIu32 " shown), %"
		B_PRIu32 " bit, %" B_PRIu32 " columns of %" B_PRIu32 " bytes\n",
		geometry.width, geometry.height, geometry.shownWidth,
		geometry.shownHeight, geometry.bitDepth, geometry.columns,
		geometry.columnStride);

	// pictures of another size: out with all there is
	_BumpAll();
	for (Frame* frame : fFrames)
		frame->shortTerm = frame->longTerm = false;
	fGeometry = geometry;
	_ReleaseUnused();

	// what phase one leaves for phase two; they grow when a picture needs
	// more
	size_t pixels = (size_t)sps.width * sps.height;
	size_t coeffSize = round_up_size(pixels);
	size_t puSize = round_up_size(pixels / 4);
	if (fCoeff.size < coeffSize) {
		_Free(fCoeff);
		status_t status = _Allocate(fCoeff, coeffSize, false);
		if (status != B_OK)
			return _Fail("no memory for the decoder: %s", strerror(status));
	}
	if (fPu.size < puSize) {
		_Free(fPu);
		status_t status = _Allocate(fPu, puSize, false);
		if (status != B_OK)
			return _Fail("no memory for the decoder: %s", strerror(status));
	}

	fConfigured = true;
	return B_OK;
}


//	#pragma mark - picture order and references


void
HevcDecoder::_ComputePoc(const SliceHeader& header)
{
	bool randomAccess = header.nalType >= NAL_BLA_W_LP
		&& header.nalType <= NAL_IRAP_LAST;
	int32 maxLsb = 1 << fSps->log2MaxPocLsb;
	int32 lsb = (int32)header.pocLsb;
	int32 msb;
	if (randomAccess && fNoRaslOutput)
		msb = 0;
	else {
		int32 previousLsb = fPrevTid0Poc & (maxLsb - 1);
		int32 previousMsb = fPrevTid0Poc - previousLsb;
		if (lsb < previousLsb && previousLsb - lsb >= maxLsb / 2)
			msb = previousMsb + maxLsb;
		else if (lsb > previousLsb && lsb - previousLsb > maxLsb / 2)
			msb = previousMsb - maxLsb;
		else
			msb = previousMsb;
	}
	fPoc = msb + lsb;

	// not a leading picture, not one nothing refers to
	bool subLayerNonReference = header.nalType < NAL_BLA_W_LP
		&& (header.nalType & 1) == 0;
	bool leading = header.nalType >= NAL_RADL_N
		&& header.nalType <= NAL_RASL_R;
	if (header.temporalId == 0 && !subLayerNonReference && !leading)
		fPrevTid0Poc = fPoc;
}


HevcDecoder::Frame*
HevcDecoder::_FindReference(int32 poc, bool longTerm, bool fullPoc)
{
	int32 mask = fullPoc ? -1 : (1 << fSps->log2MaxPocLsb) - 1;
	for (Frame* frame : fFrames) {
		if (frame->current || !(frame->shortTerm || frame->longTerm))
			continue;
		if (!longTerm && !frame->shortTerm)
			continue;
		if ((frame->poc & mask) == (poc & mask))
			return frame;
	}
	return NULL;
}


/*!	A reference the stream names that is not there (the stream was cut, or
	a picture did not decode): any picture stands in for it.
*/
HevcDecoder::Frame*
HevcDecoder::_MissingReference(int32 poc)
{
	TRACE("picture %" B_PRId32 " refers to %" B_PRId32 ", which is missing\n",
		fPoc, poc);

	Frame* frame = _NewFrame();
	if (frame == NULL)
		return NULL;
	frame->poc = poc;
	frame->pts = 0;
	frame->corrupt = true;
	return frame;
}


/*!	Which pictures the current one and those after it may refer to (8.3.2);
	the others are references no more.
*/
void
HevcDecoder::_ApplyReferenceSet(const SliceHeader& header)
{
	bool randomAccess = header.nalType >= NAL_BLA_W_LP
		&& header.nalType <= NAL_IRAP_LAST;
	if (randomAccess && fNoRaslOutput) {
		for (Frame* frame : fFrames) {
			if (!frame->current)
				frame->shortTerm = frame->longTerm = false;
		}
	}

	std::vector<Frame*> keep;
	fNumStCurrBefore = fNumStCurrAfter = fNumLtCurr = 0;
	int32 maxLsb = 1 << fSps->log2MaxPocLsb;

	bool idr = header.nalType == NAL_IDR_W_RADL
		|| header.nalType == NAL_IDR_N_LP;
	if (!idr) {
		// long term first: such a picture may still be marked short term
		Frame* longTerm[kMaxReferences];
		for (int i = 0; i < header.numLongTerm; i++) {
			int32 poc = (int32)header.longTermPocLsb[i];
			if (header.longTermMsbPresent[i]) {
				poc += fPoc - (int32)header.longTermMsbCycle[i] * maxLsb
					- (fPoc & (maxLsb - 1));
			}
			Frame* frame = _FindReference(poc, true,
				header.longTermMsbPresent[i]);
			if (frame == NULL && header.longTermUsed[i]) {
				frame = _MissingReference(poc);
				if (frame != NULL)
					frame->longTerm = true;
			}
			longTerm[i] = frame;
			if (frame == NULL)
				continue;
			keep.push_back(frame);
			if (header.longTermUsed[i])
				fLtCurr[fNumLtCurr++] = frame;
		}

		const ShortTermSet& set = header.shortTermSet;
		for (int i = 0; i < set.numNegative + set.numPositive; i++) {
			bool before = i < set.numNegative;
			int32 poc = fPoc + (before ? set.deltaPocS0[i]
				: set.deltaPocS1[i - set.numNegative]);
			bool used = before ? set.usedS0[i]
				: set.usedS1[i - set.numNegative];

			Frame* frame = NULL;
			for (Frame* candidate : fFrames) {
				if (candidate->current || !candidate->shortTerm
					|| candidate->poc != poc) {
					continue;
				}
				if (std::find(longTerm, longTerm + header.numLongTerm,
						candidate) != longTerm + header.numLongTerm) {
					continue;
				}
				frame = candidate;
				break;
			}
			if (frame == NULL && used) {
				frame = _MissingReference(poc);
				if (frame != NULL)
					frame->shortTerm = true;
			}
			if (frame == NULL)
				continue;
			keep.push_back(frame);
			if (!used)
				continue;
			if (before)
				fStCurrBefore[fNumStCurrBefore++] = frame;
			else
				fStCurrAfter[fNumStCurrAfter++] = frame;
		}

		for (int i = 0; i < header.numLongTerm; i++) {
			if (longTerm[i] == NULL)
				continue;
			longTerm[i]->shortTerm = false;
			longTerm[i]->longTerm = true;
		}
	}

	for (Frame* frame : fFrames) {
		if (frame->current)
			continue;
		if (std::find(keep.begin(), keep.end(), frame) == keep.end())
			frame->shortTerm = frame->longTerm = false;
	}

	// the block knows the references by their place in this list
	fDpbCount = 0;
	for (Frame* frame : fFrames) {
		if (!frame->current && (frame->shortTerm || frame->longTerm)
			&& fDpbCount < kMaxReferences) {
			fDpb[fDpbCount++] = frame;
		}
	}
}


/*!	The two reference lists of a slice (8.3.4), as places in fDpb. */
void
HevcDecoder::_BuildLists(const SliceHeader& header,
	uint8 lists[2][kMaxReferences])
{
	memset(lists, 0, 2 * kMaxReferences);
	if (header.type == SLICE_I)
		return;

	for (int list = 0; list < (header.type == SLICE_B ? 2 : 1); list++) {
		// before, after, long term; for the second list after comes first
		Frame* candidates[3 * kMaxReferences];
		int total = 0;
		Frame** first = list == 0 ? fStCurrBefore : fStCurrAfter;
		int firstCount = list == 0 ? fNumStCurrBefore : fNumStCurrAfter;
		Frame** second = list == 0 ? fStCurrAfter : fStCurrBefore;
		int secondCount = list == 0 ? fNumStCurrAfter : fNumStCurrBefore;
		for (int i = 0; i < firstCount; i++)
			candidates[total++] = first[i];
		for (int i = 0; i < secondCount; i++)
			candidates[total++] = second[i];
		for (int i = 0; i < fNumLtCurr; i++)
			candidates[total++] = fLtCurr[i];

		for (int i = 0; i < header.numRefIdx[list]; i++) {
			Frame* frame = NULL;
			if (total > 0) {
				int entry = header.listModification[list]
					? header.listEntry[list][i] : i;
				// the candidates repeat as often as the list is long
				frame = candidates[entry % total];
			}
			uint8 place = 0;
			for (int32 j = 0; j < fDpbCount; j++) {
				if (fDpb[j] == frame)
					place = (uint8)j;
			}
			lists[list][i] = place;
		}
	}
}


bool
HevcDecoder::_FrameIsFree(const Frame* frame) const
{
	return !frame->shortTerm && !frame->longTerm && !frame->neededForOutput
		&& !frame->queued && !frame->current;
}


/*!	Frames of another geometry than the current one go when nobody needs
	them any more. */
void
HevcDecoder::_ReleaseUnused()
{
	for (size_t i = 0; i < fFrames.size(); i++) {
		Frame* frame = fFrames[i];
		if (frame->buffer.id == RPI_HEVC_NO_BUFFER || !_FrameIsFree(frame)
			|| (frame->geometry == fGeometry
				&& frame->buffer.size >= fGeometry.frameSize)) {
			continue;
		}
		_Free(frame->buffer);
		_Free(frame->mv);
	}
}


HevcDecoder::Frame*
HevcDecoder::_NewFrame()
{
	_ReleaseUnused();

	Frame* frame = NULL;
	for (Frame* candidate : fFrames) {
		if (!_FrameIsFree(candidate))
			continue;
		// one with its memory, else an empty place
		if (candidate->buffer.id != RPI_HEVC_NO_BUFFER) {
			frame = candidate;
			break;
		}
		if (frame == NULL)
			frame = candidate;
	}
	if (frame == NULL) {
		if (fFrames.size() >= kMaxFrames) {
			_Fail("all %" B_PRIu32 " pictures are in use", kMaxFrames);
			return NULL;
		}
		frame = new Frame();
		fFrames.push_back(frame);
	}

	if (frame->buffer.id == RPI_HEVC_NO_BUFFER) {
		status_t status = _Allocate(frame->buffer, fGeometry.frameSize, true);
		if (status == B_OK && fSps->temporalMvpEnabled)
			status = _Allocate(frame->mv, fGeometry.mvSize, false);
		if (status != B_OK) {
			_Free(frame->buffer);
			_Fail("no memory for a picture: %s", strerror(status));
			return NULL;
		}
	} else if (frame->mv.id == RPI_HEVC_NO_BUFFER
		&& fSps->temporalMvpEnabled) {
		if (_Allocate(frame->mv, fGeometry.mvSize, false) != B_OK) {
			_Fail("no memory for a picture");
			return NULL;
		}
	}

	frame->geometry = fGeometry;
	frame->poc = 0;
	frame->pts = 0;
	frame->shortTerm = frame->longTerm = false;
	frame->neededForOutput = false;
	frame->queued = false;
	frame->current = false;
	frame->corrupt = false;
	frame->latency = 0;
	return frame;
}


/*!	The first picture in output order of those waiting goes out. */
bool
HevcDecoder::_Bump()
{
	Frame* first = NULL;
	for (Frame* frame : fFrames) {
		if (frame->neededForOutput
			&& (first == NULL || frame->poc < first->poc)) {
			first = frame;
		}
	}
	if (first == NULL)
		return false;
	first->neededForOutput = false;
	first->queued = true;
	fOutput.push_back(first);
	return true;
}


void
HevcDecoder::_BumpAll()
{
	while (_Bump()) {
	}
}


/*!	Before the current picture is decoded (C.5.2.2). */
void
HevcDecoder::_BumpBeforeDecoding()
{
	while (true) {
		uint32 waiting = 0;
		uint32 held = 0;
		for (Frame* frame : fFrames) {
			if (frame->current)
				continue;
			if (frame->neededForOutput)
				waiting++;
			if (frame->neededForOutput || frame->shortTerm || frame->longTerm)
				held++;
		}
		if (waiting == 0)
			break;
		if (waiting <= fSps->maxNumReorder && held < fSps->maxDecPicBuffering)
			break;
		_Bump();
	}
}


/*!	With the current picture decoded (C.5.2.3). */
void
HevcDecoder::_BumpAfterDecoding()
{
	uint32 maxLatency = fSps->maxLatencyIncreasePlus1 != 0
		? fSps->maxNumReorder + fSps->maxLatencyIncreasePlus1 - 1 : 0;

	while (true) {
		uint32 waiting = 0;
		bool late = false;
		for (Frame* frame : fFrames) {
			if (!frame->neededForOutput)
				continue;
			waiting++;
			if (maxLatency != 0 && frame->latency >= maxLatency)
				late = true;
		}
		if (waiting == 0 || (waiting <= fSps->maxNumReorder && !late))
			break;
		_Bump();
	}
}


status_t
HevcDecoder::_StartPicture(const SliceHeader& header)
{
	fPps = &fPpsList[header.ppsId];
	fSps = &fSpsList[fPps->spsId];
	const SPS& sps = *fSps;

	if (sps.chromaFormat != 1 || sps.bitDepthLuma != sps.bitDepthChroma
		|| (sps.bitDepthLuma != 8 && sps.bitDepthLuma != 10)
		|| sps.width > 4096 || sps.height > 4096) {
		return _Fail("the decoder does 4:2:0 of 8 or 10 bits up to 4096x4096;"
			" this is %ux%u, format %u, %u bits", (unsigned)sps.width,
			(unsigned)sps.height, sps.chromaFormat, sps.bitDepthLuma);
	}
	if (fPps->entropyCodingSyncEnabled && fPps->tilesEnabled
		&& (fPps->numTileColumns > 1 || fPps->numTileRows > 1)) {
		return _Fail("tiles together with wavefronts are not supported");
	}

	bool randomAccess = header.nalType >= NAL_BLA_W_LP
		&& header.nalType <= NAL_IRAP_LAST;
	if (randomAccess) {
		// one decoding starts at (or starts anew at) shows nothing of
		// what led up to it
		fNoRaslOutput = header.nalType != NAL_CRA || fFirstPicture;
	}
	fAwaitRandomAccess = false;
	fFirstPicture = false;

	if (randomAccess && fNoRaslOutput) {
		if (header.noOutputOfPriorPics) {
			for (Frame* frame : fFrames)
				frame->neededForOutput = false;
		} else
			_BumpAll();
	}

	status_t status = _Configure(sps);
	if (status != B_OK)
		return status;
	_UpdateTiles();

	_ComputePoc(header);
	_ApplyReferenceSet(header);
	if (!(randomAccess && fNoRaslOutput))
		_BumpBeforeDecoding();

	fCurrent = _NewFrame();
	if (fCurrent == NULL)
		return B_NO_MEMORY;
	fCurrent->current = true;
	fCurrent->poc = fPoc;
	fCurrent->pts = fPicturePts;
	return B_OK;
}


/*!	Decodes the picture whose slices have been collected. */
status_t
HevcDecoder::_DecodePicture()
{
	if (fSlices.empty())
		return B_OK;

	const SliceHeader& first = fSlices[0].header;
	status_t status = _StartPicture(first);
	if (status != B_OK) {
		fSlices.clear();
		fCurrent = NULL;
		return status;
	}

	bool corrupt = false;
	status = _RunHardware(corrupt);

	Frame* frame = fCurrent;
	fCurrent = NULL;
	frame->current = false;
	frame->corrupt = corrupt;
	frame->shortTerm = true;
	frame->longTerm = false;
	// a picture that did not decode is not shown, but stays to be
	// referred to: the ones after it are less wrong with it than without
	frame->neededForOutput = first.picOutput && !corrupt;
	frame->latency = 0;
	if (frame->neededForOutput) {
		for (Frame* other : fFrames) {
			if (other != frame && other->neededForOutput)
				other->latency++;
		}
	}
	fDecoded++;
	fSlices.clear();
	fBitstreamUsed = 0;

	_BumpAfterDecoding();
	return status;
}


/*!	The two phases of the block for the current picture. */
status_t
HevcDecoder::_RunHardware(bool& corrupt)
{
	const SPS& sps = *fSps;
	const PPS& pps = *fPps;
	const SliceHeader& first = fSlices[0].header;

	for (size_t i = 0; i < fSlices.size(); i++) {
		if (fSlices[i].header.type != SLICE_I && fDpbCount == 0) {
			TRACE("picture %" B_PRId32 ": no picture to refer to\n", fPoc);
			corrupt = true;
			return B_OK;
		}
	}

	CommandBuilder builder(*this);
	for (size_t i = 0; i < fSlices.size(); i++) {
		uint8 lists[2][kMaxReferences];
		_BuildLists(fSlices[i].header, lists);
		builder.AddSlice(fSlices[i], (uint32)i, lists,
			i + 1 == fSlices.size());
	}
	if (builder.Overflow()) {
		TRACE("picture %" B_PRId32 ": too many commands\n", fPoc);
		corrupt = true;
		return B_OK;
	}

	// phase one, again with larger buffers when one was too small
	while (true) {
		rpi_hevc_phase1 phase1 = {};
		phase1.commands = fCommands.id;
		phase1.command_count = builder.Count();
		phase1.bitstream = fBitstream.id;
		phase1.bitstream_size = fBitstreamUsed;
		phase1.pu = fPu.id;
		phase1.pu_stride = (uint32)(fPu.size / sps.ctbHeight) & ~63u;
		phase1.coeff = fCoeff.id;
		phase1.coeff_stride = (uint32)(fCoeff.size / sps.ctbHeight) & ~63u;
		if (ioctl(fDevice, RPI_HEVC_PHASE1, &phase1, sizeof(phase1)) != 0)
			return _Fail("phase 1: %s", strerror(errno));

		if (phase1.result == RPI_HEVC_PHASE1_OK)
			break;
		if (phase1.result == RPI_HEVC_PHASE1_ERROR) {
			TRACE("picture %" B_PRId32 ": phase 1 failed, status %#" B_PRIx32
				"\n", fPoc, phase1.status);
			corrupt = true;
			return B_OK;
		}

		for (int which = 0; which < 2; which++) {
			Buffer& buffer = which == 0 ? fCoeff : fPu;
			if ((phase1.result & (which == 0 ? RPI_HEVC_PHASE1_COEFF_FULL
					: RPI_HEVC_PHASE1_PU_FULL)) == 0) {
				continue;
			}
			size_t size = round_up_size(buffer.size + 1);
			TRACE("a larger %s buffer: %" B_PRIuSIZE "\n",
				which == 0 ? "coefficient" : "prediction unit", size);
			_Free(buffer);
			status_t status = _Allocate(buffer, size, false);
			if (status != B_OK) {
				return _Fail("no memory for the decoder: %s",
					strerror(status));
			}
		}
	}

	rpi_hevc_phase2 phase2 = {};
	phase2.pu = fPu.id;
	phase2.pu_stride = (uint32)(fPu.size / sps.ctbHeight) & ~63u;
	phase2.coeff = fCoeff.id;
	phase2.coeff_stride = (uint32)(fCoeff.size / sps.ctbHeight) & ~63u;
	phase2.frame = fCurrent->buffer.id;
	phase2.chroma_offset = fGeometry.chromaOffset;
	phase2.frame_stride = fGeometry.columnStride;
	for (int32 i = 0; i < 16; i++) {
		phase2.references[i] = i < fDpbCount
			? fDpb[i]->buffer.id : RPI_HEVC_NO_BUFFER;
	}

	// Motion vectors are kept for a picture later ones may take theirs
	// from: one that is referred to.
	bool keepMotion = sps.temporalMvpEnabled
		&& (sps.maxSubLayersMinus1 >= first.temporalId + 1
			|| first.nalType >= NAL_BLA_W_LP || (first.nalType & 1) != 0);
	uint32 config = sps.bitDepthLuma | (sps.bitDepthChroma << 4);
	if (sps.bitDepthLuma > 8)
		config |= 1 << 8;
	if (sps.bitDepthChroma > 8)
		config |= 1 << 9;
	config |= sps.log2CtbSize << 10;
	if (pps.constrainedIntraPred)
		config |= 1 << 13;
	if (sps.strongIntraSmoothing)
		config |= 1 << 14;
	if (keepMotion)
		config |= 1 << 15;
	config |= pps.log2ParallelMergeLevel << 16;
	if (first.temporalMvpEnabled)
		config |= 1 << 19;
	if (sps.pcmLoopFilterDisabled)
		config |= 1 << 20;
	config |= (pps.cbQpOffset & 31) << 21;
	config |= (uint32)(pps.crQpOffset & 31) << 26;
	phase2.config = config;
	phase2.frame_size = (sps.height << 16) | sps.width;
	phase2.current_poc = (uint32)fPoc;
	phase2.rows = sps.ctbHeight;
	phase2.mv_stride = fGeometry.mvStride;
	phase2.mv = keepMotion ? fCurrent->mv.id : RPI_HEVC_NO_BUFFER;
	phase2.collocated = RPI_HEVC_NO_BUFFER;
	if (builder.Collocated() >= 0 && builder.Collocated() < fDpbCount)
		phase2.collocated = fDpb[builder.Collocated()]->mv.id;

	if (ioctl(fDevice, RPI_HEVC_PHASE2, &phase2, sizeof(phase2)) != 0) {
		if (errno == B_TIMED_OUT) {
			TRACE("picture %" B_PRId32 ": phase 2 did not end\n", fPoc);
			corrupt = true;
			return B_OK;
		}
		return _Fail("phase 2: %s", strerror(errno));
	}
	return B_OK;
}
