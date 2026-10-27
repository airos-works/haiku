/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef CEDAR_H264_PARSE_H
#define CEDAR_H264_PARSE_H

/*	H.264 parsing and reference handling for a decoder whose hardware only
	does the slice data: everything the Cedar engine needs in its registers
	and SRAM, which is the full slice header, POC, the final reference lists
	and the prediction weights. Section numbers: ITU-T H.264 (08/2021).

	Not handled (reported as unsupported): field and MBAFF pictures, slice
	groups, SP and SI slices, data partitioning, other than 4:2:0 8 bit.
	Gaps in frame_num get "non-existing" reference frames (frame -1), which a
	conforming stream never predicts from. */

#include <stdint.h>

#include "bits.h"

#ifdef __cplusplus
extern "C" {
#endif

#define H264_MAX_REFS	16
#define H264_MAX_MODS	33
#define H264_MAX_MMCO	66

typedef struct {
	int	valid;
	int	profileIdc, levelIdc, constraintFlags;
	int	chromaFormatIdc, separateColourPlane;
	int	bitDepthLuma, bitDepthChroma;
	int	scalingMatrixPresent;
	uint8_t	scaling4x4[6][16];		/* raster order, after fall-back rule A */
	uint8_t	scaling8x8[2][64];
	int	log2MaxFrameNum;
	int	pocType, log2MaxPocLsb;
	int	deltaPicOrderAlwaysZero;
	int	offsetForNonRefPic, offsetForTopToBottomField;
	int	numRefFramesInPocCycle;
	int	offsetForRefFrame[256];
	int	maxNumRefFrames;
	int	gapsAllowed;
	int	widthMbs, heightMapUnits;
	int	frameMbsOnly, mbaff, direct8x8;
	int	cropLeft, cropRight, cropTop, cropBottom;	/* in luma samples */
	/* output: from the VUI's bitstream restriction, or the level */
	int	numReorderFrames;
	int	maxDecFrameBuffering;
} H264Sps;

typedef struct {
	int	valid;
	int	spsId;
	int	entropyCodingMode;
	int	bottomFieldPicOrderPresent;
	int	numSliceGroups;
	int	numRefIdxDefault[2];		/* minus1 + 1 */
	int	weightedPred, weightedBipredIdc;
	int	picInitQp;			/* 26 + pic_init_qp_minus26 */
	int	chromaQpIndexOffset, secondChromaQpIndexOffset;
	int	deblockingFilterControlPresent;
	int	constrainedIntraPred;
	int	redundantPicCntPresent;
	int	transform8x8Mode;
	/* the matrices in effect for its pictures (SPS or PPS level) */
	int	scalingMatrixPresent;
	uint8_t	scaling4x4[6][16];
	uint8_t	scaling8x8[2][64];
} H264Pps;

typedef struct {
	int	idc;		/* modification_of_pic_nums_idc */
	int	value;		/* abs_diff_pic_num_minus1 or long_term_pic_num */
} H264Mod;

typedef struct {
	int	op;
	int	diffPicNumsMinus1, longTermPicNum, longTermFrameIdx,
		maxLongTermFrameIdxPlus1;
} H264Mmco;

typedef struct {
	/* NAL */
	int	nalRefIdc, nalType, idr;
	/* slice header */
	int	firstMb, sliceType;	/* sliceType % 5: 0 P, 1 B, 2 I, 3 SP, 4 SI */
	int	ppsId;
	int	frameNum;
	int	fieldPic, bottomField;
	int	idrPicId;
	int	pocLsb, deltaPocBottom, deltaPoc[2];
	int	directSpatialMvPred;
	int	numRefIdxActive[2];
	int	modCount[2];
	H264Mod	mods[2][H264_MAX_MODS];
	/* prediction weights, defaults filled in for absent entries */
	int	hasWeights;
	int	lumaLog2Denom, chromaLog2Denom;
	int	lumaWeight[2][32], lumaOffset[2][32];
	int	chromaWeight[2][32][2], chromaOffset[2][32][2];
	/* dec_ref_pic_marking */
	int	noOutputOfPriorPics, longTermReference;
	int	adaptiveMarking, mmcoCount;
	H264Mmco mmco[H264_MAX_MMCO];
	int	cabacInitIdc;
	int	sliceQpDelta;
	int	disableDeblockingFilterIdc;
	int	sliceAlphaC0OffsetDiv2, sliceBetaOffsetDiv2;
	/* RBSP bits from the first bit of the NAL header to slice_data() */
	int	headerBits;
} H264Slice;

/* A decoded frame kept for reference. */
typedef struct {
	int	ref;			/* 0 none, 1 short term, 2 long term */
	int	frameNum, frameNumWrap, longTermFrameIdx;
	int	topPoc, bottomPoc, poc;
	int	frame;			/* the caller's picture, -1 for none */
	int	nonExisting;		/* inferred for a gap in frame_num (8.2.5.2) */
} H264Ref;

typedef struct {
	H264Sps	sps[32];
	H264Pps	pps[256];

	/* POC state, 8.2.1 */
	int	prevPocMsb, prevPocLsb;
	int	prevFrameNumOffset, prevFrameNum;
	int	prevRefFrameNum;	/* 7.4.3, for gaps in frame_num */

	/* current picture */
	int	curFrameNumOffset, curPocMsb;
	int	curTopPoc, curBottomPoc, curPoc;
	int	curHadMmco5;

	/* references, 8.2.5 */
	H264Ref	refs[H264_MAX_REFS];
	int	maxLongTermFrameIdx;	/* -1 = "no long-term frame indices" */
} H264State;

void h264_init(H264State *st);

/* Each returns 0, or -1 malformed, or -2 unsupported (message in err, 160
   bytes). The units are RBSP: emulation prevention removed, header byte
   included. */
int h264_parse_sps(H264State *st, BitReader *br, char *err);
int h264_parse_pps(H264State *st, BitReader *br, char *err);
int h264_parse_slice(H264State *st, BitReader *br, H264Slice *sh, char *err);

/* A slice that starts a new picture, compared with the one before
   (7.4.1.2.4). */
int h264_new_picture(const H264Slice *previous, const H264Slice *slice);

/* Picture-level steps, in order, for the first slice of every picture
   (frame_num gaps are filled here). */
void h264_start_picture(H264State *st, const H264Slice *sh);
/* Final reference lists for one slice; entries are indices into st->refs,
   -1 for "no reference picture". */
void h264_ref_lists(H264State *st, const H264Slice *sh, int list0[32],
	int list1[32]);
/* Marks the decoded picture; returns the refs[] slot it went to, or -1 if it
   is not a reference. Frees nothing: the caller sees which pictures are
   still references through refs[].ref. */
int h264_finish_picture(H264State *st, const H264Slice *sh, int frame);

#ifdef __cplusplus
}
#endif

#endif	/* CEDAR_H264_PARSE_H */
