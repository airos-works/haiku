/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CEDAR_HEVC_PARSE_H
#define CEDAR_HEVC_PARSE_H

/*	HEVC parsing and reference handling for a decoder whose hardware only
	does the slice data (what the Cedar engine needs: the full slice segment
	header, POC, RPS, final lists, weights, where the slice data begins).
	Section numbers: ITU-T H.265 (V8, 08/2021).

	Not handled (reported as unsupported): other than 4:2:0, more than 10
	bit, range and SCC extensions. */

#include <stdint.h>

#include "bits.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HEVC_MAX_DPB	16
#define HEVC_MAX_ENTRY	512
#define HEVC_MAX_TILE_COLUMNS	20
#define HEVC_MAX_TILE_ROWS	22

/* scaling lists in raster order (7.3.4), with the DC values */
typedef struct {
	uint8_t	list4x4[6][16];
	uint8_t	list8x8[6][64];
	uint8_t	list16x16[6][64];
	uint8_t	list32x32[2][64];
	uint8_t	dc16x16[6];
	uint8_t	dc32x32[2];
} HevcScaling;

typedef struct {
	int	numNegative, numPositive;
	int	deltaPocS0[16], usedS0[16];
	int	deltaPocS1[16], usedS1[16];
} HevcStRps;

typedef struct {
	int	valid;
	int	maxSubLayersMinus1;
	int	chromaFormatIdc, separateColourPlane;
	int	width, height;
	int	cropLeft, cropRight, cropTop, cropBottom;	/* luma samples */
	int	bitDepthLuma, bitDepthChroma;
	int	log2MaxPocLsb;
	int	maxDecPicBuffering;	/* of the highest sub-layer */
	int	maxNumReorder;
	int	maxLatencyIncreasePlus1;
	int	log2MinCbSize, log2DiffMaxMinCbSize;
	int	log2MinTbSize, log2DiffMaxMinTbSize;
	int	maxTransformHierarchyDepthInter, maxTransformHierarchyDepthIntra;
	int	scalingListEnabled;
	HevcScaling scaling;		/* default lists without any in the stream */
	int	amp, sao;
	int	pcm, pcmBitDepthLuma, pcmBitDepthChroma;
	int	log2MinPcmCbSize, log2DiffMaxMinPcmCbSize, pcmLoopFilterDisabled;
	int	numShortTermRps;
	HevcStRps stRps[65];
	int	longTermRefPicsPresent, numLongTermRefPicsSps;
	int	ltRefPicPocLsbSps[33], usedByCurrPicLtSps[33];
	int	temporalMvp, strongIntraSmoothing;
	/* derived */
	int	ctbLog2, ctbSize, widthCtbs, heightCtbs;
} HevcSps;

typedef struct {
	int	valid, spsId;
	int	dependentSliceSegmentsEnabled, outputFlagPresent;
	int	numExtraSliceHeaderBits;
	int	signDataHiding, cabacInitPresent;
	int	numRefIdxDefault[2];
	int	initQpMinus26;
	int	constrainedIntraPred, transformSkip;
	int	cuQpDeltaEnabled, diffCuQpDeltaDepth;
	int	cbQpOffset, crQpOffset, sliceChromaQpOffsetsPresent;
	int	weightedPred, weightedBipred, transquantBypass;
	int	tilesEnabled, entropyCodingSync;
	int	numTileColumns, numTileRows, uniformSpacing;
	int	columnWidth[HEVC_MAX_TILE_COLUMNS];	/* in CTBs, when not uniform */
	int	rowHeight[HEVC_MAX_TILE_ROWS];
	int	loopFilterAcrossTiles, loopFilterAcrossSlices;
	int	deblockingOverrideEnabled, deblockingDisabled;
	int	betaOffsetDiv2, tcOffsetDiv2;
	int	scalingListDataPresent;
	HevcScaling scaling;
	int	listsModificationPresent;
	int	log2ParallelMergeLevelMinus2;
	int	sliceHeaderExtensionPresent;
} HevcPps;

typedef struct {
	int	nalType, temporalId;
	int	firstSliceSegmentInPic, noOutputOfPriorPics;
	int	ppsId, dependentSliceSegment, segmentAddress;
	int	sliceType;		/* 0 B, 1 P, 2 I */
	int	picOutputFlag;
	int	pocLsb;
	HevcStRps rps;			/* the one in effect */
	int	numLongTerm;
	int	ltPoc[32], ltUsed[32], ltMsbPresent[32];
	int	temporalMvp;
	int	saoLuma, saoChroma;
	int	numRefIdxActive[2];
	int	listModified[2], listEntry[2][16];
	int	mvdL1Zero, cabacInit, collocatedFromL0, collocatedRefIdx;
	int	lumaLog2Denom, chromaLog2Denom;
	int	deltaLumaWeight[2][16], lumaOffset[2][16];
	int	deltaChromaWeight[2][16][2], chromaOffset[2][16][2];
	int	fiveMinusMaxNumMergeCand;
	int	sliceQpDelta, cbQpOffset, crQpOffset;
	int	deblockingDisabled, betaOffsetDiv2, tcOffsetDiv2;
	int	loopFilterAcrossSlices;
	int	numEntryPoints;
	uint32_t entryPointMinus1[HEVC_MAX_ENTRY];
	/* first slice-data byte: RBSP offset from the NAL start, and raw */
	int	dataOffset, dataOffsetRaw;
	/* RBSP bit position of alignment_bit_equal_to_one (end of the header
	   proper); where Cedar wants the bit reader before decoding */
	int	alignBitPos;
} HevcSlice;

typedef struct {
	int	used;			/* 0 unused, 1 short term, 2 long term */
	int	poc;
	int	frame;			/* the caller's picture */
} HevcRef;

typedef struct {
	HevcSps	sps[16];
	HevcPps	pps[64];
	int	firstPicture;		/* next IRAP gets NoRaslOutputFlag */
	int	prevTid0PocLsb, prevTid0PocMsb;
	int	curPoc, curPocMsb;
	int	noRaslOutput;		/* of the current picture */
	HevcRef	refs[HEVC_MAX_DPB];
	/* RPS of the current picture, as refs[] indices */
	int	stCurrBefore[16], numStCurrBefore;
	int	stCurrAfter[16], numStCurrAfter;
	int	ltCurr[16], numLtCurr;
	/* the RPS names a picture that is not there (after a broken stream);
	   the lists are made of what there is */
	int	missingReference;
	/* the last independent slice segment: a dependent one takes its header */
	HevcSlice lastIndependent;
	int	haveIndependent;
} HevcState;

void hevc_init(HevcState *st);
int hevc_parse_sps(HevcState *st, BitReader *br, char *err);
int hevc_parse_pps(HevcState *st, BitReader *br, char *err);
int hevc_parse_slice(HevcState *st, BitReader *br, const Rbsp *rbsp,
	HevcSlice *sh, char *err);

/* First slice of a picture: POC and RPS (marks unused refs). A reference
   that is missing sets missingReference, it is no error. */
int hevc_start_picture(HevcState *st, const HevcSlice *sh, char *err);
/* Final lists for one slice, as refs[] indices. */
int hevc_ref_lists(HevcState *st, const HevcSlice *sh, int list0[16],
	int list1[16], char *err);
/* After the picture: it becomes a short-term reference; returns its slot. */
int hevc_finish_picture(HevcState *st, const HevcSlice *sh, int frame);
/* The scaling lists the slices of a picture use (when enabled). */
const HevcScaling *hevc_scaling(const HevcState *st, const HevcSlice *sh);
/* Width and height of each tile column and row, in CTBs. */
void hevc_tiles(const HevcState *st, const HevcSlice *sh,
	int columnWidth[HEVC_MAX_TILE_COLUMNS], int rowHeight[HEVC_MAX_TILE_ROWS]);

static inline int hevc_is_irap(int nalType) { return nalType >= 16 && nalType <= 23; }
static inline int hevc_is_idr(int nalType) { return nalType == 19 || nalType == 20; }
/* a sub-layer non-reference picture (TRAIL_N, TSA_N, ...) */
static inline int hevc_is_sub_layer_non_reference(int nalType)
	{ return nalType <= 14 && (nalType % 2) == 0; }

#ifdef __cplusplus
}
#endif

#endif	/* CEDAR_HEVC_PARSE_H */
