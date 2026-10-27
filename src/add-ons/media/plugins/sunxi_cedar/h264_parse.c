/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "h264_parse.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ERR(code, ...) do { snprintf(err, 160, __VA_ARGS__); return code; } while (0)


/* Table 7-3 and 7-4, in raster order */
static const uint8_t kDefault4x4Intra[16] = {
	6, 13, 20, 28, 13, 20, 28, 32, 20, 28, 32, 37, 28, 32, 37, 42
};
static const uint8_t kDefault4x4Inter[16] = {
	10, 14, 20, 24, 14, 20, 24, 27, 20, 24, 27, 30, 24, 27, 30, 34
};
static const uint8_t kDefault8x8Intra[64] = {
	6, 10, 13, 16, 18, 23, 25, 27, 10, 11, 16, 18, 23, 25, 27, 29,
	13, 16, 18, 23, 25, 27, 29, 31, 16, 18, 23, 25, 27, 29, 31, 33,
	18, 23, 25, 27, 29, 31, 33, 36, 23, 25, 27, 29, 31, 33, 36, 38,
	25, 27, 29, 31, 33, 36, 38, 40, 27, 29, 31, 33, 36, 38, 40, 42
};
static const uint8_t kDefault8x8Inter[64] = {
	9, 13, 15, 17, 19, 21, 22, 24, 13, 13, 17, 19, 21, 22, 24, 25,
	15, 17, 19, 21, 22, 24, 25, 27, 17, 19, 21, 22, 24, 25, 27, 28,
	19, 21, 22, 24, 25, 27, 28, 30, 21, 22, 24, 25, 27, 28, 30, 32,
	22, 24, 25, 27, 28, 30, 32, 33, 24, 25, 27, 28, 30, 32, 33, 35
};

/* the frame zig-zag scans: raster position of each coefficient */
static const uint8_t kZigzag4x4[16] = {
	0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};
static const uint8_t kZigzag8x8[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};


/* 7.3.2.1.1.1: a list in raster order; returns 1 for "use the default
   list", 0, or -1 on a bad value */
static int
parse_scaling_list(BitReader *br, uint8_t *list, int size)
{
	const uint8_t *scan = size == 16 ? kZigzag4x4 : kZigzag8x8;
	int last = 8, next = 8;
	for (int j = 0; j < size; j++) {
		if (next != 0) {
			int delta = br_se(br);
			if (delta < -128 || delta > 127)
				return -1;
			next = (last + delta + 256) % 256;
			if (j == 0 && next == 0)
				return 1;
		}
		list[scan[j]] = (uint8_t)(next == 0 ? last : next);
		last = list[scan[j]];
	}
	return 0;
}


/*	The scaling lists of an SPS or PPS (lists 0-5 4x4, 6-7 8x8 for 4:2:0),
	with fall-back rule A (fallback == NULL) or B (fallback: the SPS's). */
static int
parse_scaling_matrices(BitReader *br, int count, uint8_t list4x4[6][16],
	uint8_t list8x8[2][64], const uint8_t fallback4x4[6][16],
	const uint8_t fallback8x8[2][64])
{
	for (int i = 0; i < count; i++) {
		int present = br_flag(br);
		int result = 0;
		uint8_t *list = i < 6 ? list4x4[i] : list8x8[(i - 6) % 2];
		int size = i < 6 ? 16 : 64;
		if (i >= 8) {
			/* 4:4:4's chroma 8x8 lists: read past them */
			uint8_t ignored[64];
			if (present && parse_scaling_list(br, ignored, 64) < 0)
				return -1;
			continue;
		}
		if (present)
			result = parse_scaling_list(br, list, size);
		if (result < 0)
			return -1;
		const uint8_t *source = NULL;
		if (present && result == 1) {
			source = i < 3 ? kDefault4x4Intra : i < 6 ? kDefault4x4Inter
				: i == 6 ? kDefault8x8Intra : kDefault8x8Inter;
		} else if (!present) {
			if (i == 0 || i == 3 || i == 6 || i == 7) {
				if (fallback4x4 != NULL) {
					source = i < 6 ? fallback4x4[i] : fallback8x8[i - 6];
				} else {
					source = i == 0 ? kDefault4x4Intra
						: i == 3 ? kDefault4x4Inter
						: i == 6 ? kDefault8x8Intra : kDefault8x8Inter;
				}
			} else
				source = list4x4[i - 1];
		}
		if (source != NULL)
			memcpy(list, source, size);
	}
	return br->overrun ? -1 : 0;
}


static void
skip_hrd_parameters(BitReader *br)
{
	int count = br_ue(br) + 1;
	br_u(br, 8);	/* bit rate and CPB size scales */
	for (int i = 0; i < count && !br->overrun; i++) {
		br_ue(br);
		br_ue(br);
		br_flag(br);
	}
	br_u(br, 20);	/* four delay and offset lengths */
}


/* E.1.1, as far as the bitstream restriction */
static void
parse_vui(BitReader *br, H264Sps *sps)
{
	if (br_flag(br)) {
		if (br_u(br, 8) == 255)
			br_u(br, 32);
	}
	if (br_flag(br))
		br_flag(br);
	if (br_flag(br)) {
		br_u(br, 4);
		if (br_flag(br))
			br_u(br, 24);
	}
	if (br_flag(br)) {
		br_ue(br);
		br_ue(br);
	}
	if (br_flag(br)) {
		br_u(br, 32);
		br_u(br, 32);
		br_flag(br);
	}
	int nalHrd = br_flag(br);
	if (nalHrd)
		skip_hrd_parameters(br);
	int vclHrd = br_flag(br);
	if (vclHrd)
		skip_hrd_parameters(br);
	if (nalHrd || vclHrd)
		br_flag(br);
	br_flag(br);	/* pic_struct_present_flag */
	if (br_flag(br) && !br->overrun) {
		br_flag(br);
		br_ue(br);
		br_ue(br);
		br_ue(br);
		br_ue(br);
		int reorder = br_ue(br);
		int buffering = br_ue(br);
		if (!br->overrun && reorder <= 16 && buffering <= 16) {
			sps->numReorderFrames = reorder;
			sps->maxDecFrameBuffering = buffering > 0 ? buffering : 1;
		}
	}
}


/* A.3.1: MaxDpbFrames from the level */
static int
max_dpb_frames(const H264Sps *sps)
{
	int mbs;
	switch (sps->levelIdc) {
		case 9: case 10: mbs = 396; break;
		case 11:
			mbs = (sps->constraintFlags & 0x10) != 0 ? 396 : 900;
			break;
		case 12: case 13: case 20: mbs = 2376; break;
		case 21: mbs = 4752; break;
		case 22: case 30: mbs = 8100; break;
		case 31: mbs = 18000; break;
		case 32: mbs = 20480; break;
		case 40: case 41: mbs = 32768; break;
		case 42: mbs = 34816; break;
		case 50: mbs = 110400; break;
		case 51: case 52: mbs = 184320; break;
		default: mbs = 696320; break;
	}
	int frames = mbs / (sps->widthMbs * sps->heightMapUnits
		* (2 - sps->frameMbsOnly));
	if (frames > 16)
		frames = 16;
	if (frames < sps->maxNumRefFrames)
		frames = sps->maxNumRefFrames;
	return frames > 0 ? frames : 1;
}


void
h264_init(H264State *st)
{
	memset(st, 0, sizeof(*st));
	st->maxLongTermFrameIdx = -1;
}


int
h264_parse_sps(H264State *st, BitReader *br, char *err)
{
	br_u(br, 8);	/* NAL header */
	int profile = br_u(br, 8);
	int constraints = br_u(br, 8);
	int level = br_u(br, 8);
	unsigned id = br_ue(br);
	if (id >= 32)
		ERR(-1, "SPS id %u", id);

	H264Sps sps;
	memset(&sps, 0, sizeof(sps));
	sps.profileIdc = profile;
	sps.constraintFlags = constraints;
	sps.levelIdc = level;
	memset(sps.scaling4x4, 16, sizeof(sps.scaling4x4));
	memset(sps.scaling8x8, 16, sizeof(sps.scaling8x8));
	sps.chromaFormatIdc = 1;
	sps.bitDepthLuma = sps.bitDepthChroma = 8;

	if (profile == 100 || profile == 110 || profile == 122 || profile == 244
		|| profile == 44 || profile == 83 || profile == 86 || profile == 118
		|| profile == 128 || profile == 138 || profile == 139
		|| profile == 134 || profile == 135) {
		sps.chromaFormatIdc = br_ue(br);
		if (sps.chromaFormatIdc == 3)
			sps.separateColourPlane = br_flag(br);
		sps.bitDepthLuma = 8 + br_ue(br);
		sps.bitDepthChroma = 8 + br_ue(br);
		br_flag(br);	/* qpprime_y_zero_transform_bypass */
		sps.scalingMatrixPresent = br_flag(br);
		if (sps.scalingMatrixPresent
			&& parse_scaling_matrices(br, sps.chromaFormatIdc != 3 ? 8 : 12,
				sps.scaling4x4, sps.scaling8x8, NULL, NULL) != 0) {
			ERR(-1, "SPS scaling list");
		}
	}
	sps.log2MaxFrameNum = br_ue(br) + 4;
	sps.pocType = br_ue(br);
	if (sps.pocType == 0) {
		sps.log2MaxPocLsb = br_ue(br) + 4;
	} else if (sps.pocType == 1) {
		sps.deltaPicOrderAlwaysZero = br_flag(br);
		sps.offsetForNonRefPic = br_se(br);
		sps.offsetForTopToBottomField = br_se(br);
		sps.numRefFramesInPocCycle = br_ue(br);
		if (sps.numRefFramesInPocCycle > 255)
			ERR(-1, "POC cycle %d", sps.numRefFramesInPocCycle);
		for (int i = 0; i < sps.numRefFramesInPocCycle; i++)
			sps.offsetForRefFrame[i] = br_se(br);
	} else if (sps.pocType != 2) {
		ERR(-1, "POC type %d", sps.pocType);
	}
	sps.maxNumRefFrames = br_ue(br);
	sps.gapsAllowed = br_flag(br);
	sps.widthMbs = br_ue(br) + 1;
	sps.heightMapUnits = br_ue(br) + 1;
	sps.frameMbsOnly = br_flag(br);
	if (!sps.frameMbsOnly)
		sps.mbaff = br_flag(br);
	sps.direct8x8 = br_flag(br);
	if (br_flag(br)) {
		/* crop units for 4:2:0 frames: 2 horizontally, 2 * (2 - frame_mbs_only) vertically */
		int cx = sps.chromaFormatIdc == 1 || sps.chromaFormatIdc == 2 ? 2 : 1;
		int cy = (sps.chromaFormatIdc == 1 ? 2 : 1) * (2 - sps.frameMbsOnly);
		sps.cropLeft = br_ue(br) * cx;
		sps.cropRight = br_ue(br) * cx;
		sps.cropTop = br_ue(br) * cy;
		sps.cropBottom = br_ue(br) * cy;
	}
	sps.numReorderFrames = -1;
	if (br_flag(br))
		parse_vui(br, &sps);
	if (sps.numReorderFrames < 0) {
		/* no bitstream restriction: as many as the level allows */
		sps.maxDecFrameBuffering = max_dpb_frames(&sps);
		sps.numReorderFrames = sps.maxDecFrameBuffering;
	}
	if (br->overrun)
		ERR(-1, "SPS truncated");

	sps.valid = 1;
	st->sps[id] = sps;
	return 0;
}


int
h264_parse_pps(H264State *st, BitReader *br, char *err)
{
	br_u(br, 8);
	unsigned id = br_ue(br);
	unsigned spsId = br_ue(br);
	if (id >= 256 || spsId >= 32 || !st->sps[spsId].valid)
		ERR(-1, "PPS %u / SPS %u", id, spsId);
	const H264Sps *sps = &st->sps[spsId];

	H264Pps pps;
	memset(&pps, 0, sizeof(pps));
	pps.spsId = spsId;
	pps.scalingMatrixPresent = sps->scalingMatrixPresent;
	memcpy(pps.scaling4x4, sps->scaling4x4, sizeof(pps.scaling4x4));
	memcpy(pps.scaling8x8, sps->scaling8x8, sizeof(pps.scaling8x8));
	pps.entropyCodingMode = br_flag(br);
	pps.bottomFieldPicOrderPresent = br_flag(br);
	pps.numSliceGroups = br_ue(br) + 1;
	if (pps.numSliceGroups != 1)
		ERR(-2, "slice groups (FMO)");
	pps.numRefIdxDefault[0] = br_ue(br) + 1;
	pps.numRefIdxDefault[1] = br_ue(br) + 1;
	pps.weightedPred = br_flag(br);
	pps.weightedBipredIdc = br_u(br, 2);
	pps.picInitQp = 26 + br_se(br);
	br_se(br);	/* pic_init_qs_minus26 */
	pps.chromaQpIndexOffset = br_se(br);
	pps.deblockingFilterControlPresent = br_flag(br);
	pps.constrainedIntraPred = br_flag(br);
	pps.redundantPicCntPresent = br_flag(br);
	pps.secondChromaQpIndexOffset = pps.chromaQpIndexOffset;
	if (br_more_rbsp_data(br)) {
		pps.transform8x8Mode = br_flag(br);
		if (br_flag(br)) {
			/* fall-back rule B from the SPS's lists, or A without them */
			int count = 6 + (sps->chromaFormatIdc != 3 ? 2 : 6)
				* pps.transform8x8Mode;
			int fromSps = sps->scalingMatrixPresent;
			pps.scalingMatrixPresent = 1;
			if (parse_scaling_matrices(br, count, pps.scaling4x4,
					pps.scaling8x8, fromSps ? sps->scaling4x4 : NULL,
					fromSps ? sps->scaling8x8 : NULL) != 0) {
				ERR(-1, "PPS scaling list");
			}
			if (!pps.transform8x8Mode) {
				/* no 8x8 lists in the PPS: as rule A or B says */
				memcpy(pps.scaling8x8[0],
					fromSps ? sps->scaling8x8[0] : kDefault8x8Intra, 64);
				memcpy(pps.scaling8x8[1],
					fromSps ? sps->scaling8x8[1] : kDefault8x8Inter, 64);
			}
		}
		pps.secondChromaQpIndexOffset = br_se(br);
	}
	if (br->overrun)
		ERR(-1, "PPS truncated");
	pps.valid = 1;
	st->pps[id] = pps;
	return 0;
}


static int
parse_list_mods(BitReader *br, H264Slice *sh, int list, char *err)
{
	sh->modCount[list] = 0;
	if (!br_flag(br))
		return 0;
	for (;;) {
		int idc = br_ue(br);
		if (idc == 3)
			break;
		if (idc > 5 || sh->modCount[list] >= H264_MAX_MODS)
			ERR(-1, "ref list modification %d", idc);
		if (idc > 2)
			ERR(-2, "MVC list modification");
		H264Mod *m = &sh->mods[list][sh->modCount[list]++];
		m->idc = idc;
		m->value = br_ue(br);
		if (br->overrun)
			ERR(-1, "list modification truncated");
	}
	return 0;
}


static void
parse_weights(BitReader *br, H264Slice *sh, int list, int chroma)
{
	for (int i = 0; i < 32; i++) {
		sh->lumaWeight[list][i] = 1 << sh->lumaLog2Denom;
		sh->lumaOffset[list][i] = 0;
		for (int j = 0; j < 2; j++) {
			sh->chromaWeight[list][i][j] = 1 << sh->chromaLog2Denom;
			sh->chromaOffset[list][i][j] = 0;
		}
	}
	for (int i = 0; i < sh->numRefIdxActive[list]; i++) {
		if (br_flag(br)) {
			sh->lumaWeight[list][i] = br_se(br);
			sh->lumaOffset[list][i] = br_se(br);
		}
		if (chroma && br_flag(br)) {
			for (int j = 0; j < 2; j++) {
				sh->chromaWeight[list][i][j] = br_se(br);
				sh->chromaOffset[list][i][j] = br_se(br);
			}
		}
	}
}


int
h264_parse_slice(H264State *st, BitReader *br, H264Slice *sh, char *err)
{
	memset(sh, 0, sizeof(*sh));
	br_u(br, 1);
	sh->nalRefIdc = br_u(br, 2);
	sh->nalType = br_u(br, 5);
	sh->idr = sh->nalType == 5;

	sh->firstMb = br_ue(br);
	sh->sliceType = br_ue(br) % 5;
	sh->ppsId = br_ue(br);
	if (sh->ppsId >= 256 || !st->pps[sh->ppsId].valid)
		ERR(-1, "slice refers to missing PPS %d", sh->ppsId);
	const H264Pps *pps = &st->pps[sh->ppsId];
	const H264Sps *sps = &st->sps[pps->spsId];

	if (sps->separateColourPlane)
		ERR(-2, "separate colour planes");
	if (sps->chromaFormatIdc != 1 || sps->bitDepthLuma != 8
		|| sps->bitDepthChroma != 8)
		ERR(-2, "only 4:2:0 8-bit");
	/* the engine has no SP and SI slices (Extended profile switching) */
	if (sh->sliceType == 3 || sh->sliceType == 4)
		ERR(-2, "SP and SI slices");

	sh->frameNum = br_u(br, sps->log2MaxFrameNum);
	if (!sps->frameMbsOnly) {
		sh->fieldPic = br_flag(br);
		if (sh->fieldPic)
			sh->bottomField = br_flag(br);
	}
	if (sh->fieldPic || sps->mbaff)
		ERR(-2, "field or MBAFF pictures");
	if (sh->idr)
		sh->idrPicId = br_ue(br);
	if (sps->pocType == 0) {
		sh->pocLsb = br_u(br, sps->log2MaxPocLsb);
		if (pps->bottomFieldPicOrderPresent && !sh->fieldPic)
			sh->deltaPocBottom = br_se(br);
	}
	if (sps->pocType == 1 && !sps->deltaPicOrderAlwaysZero) {
		sh->deltaPoc[0] = br_se(br);
		if (pps->bottomFieldPicOrderPresent && !sh->fieldPic)
			sh->deltaPoc[1] = br_se(br);
	}
	if (pps->redundantPicCntPresent && br_ue(br) != 0)
		ERR(-2, "redundant slices");
	if (sh->sliceType == 1)
		sh->directSpatialMvPred = br_flag(br);

	sh->numRefIdxActive[0] = pps->numRefIdxDefault[0];
	sh->numRefIdxActive[1] = pps->numRefIdxDefault[1];
	if (sh->sliceType == 0 || sh->sliceType == 3 || sh->sliceType == 1) {
		if (br_flag(br)) {
			sh->numRefIdxActive[0] = br_ue(br) + 1;
			if (sh->sliceType == 1)
				sh->numRefIdxActive[1] = br_ue(br) + 1;
		}
	}
	if (sh->sliceType != 1)
		sh->numRefIdxActive[1] = 0;
	if (sh->sliceType == 2 || sh->sliceType == 4)
		sh->numRefIdxActive[0] = 0;
	if (sh->numRefIdxActive[0] > 32 || sh->numRefIdxActive[1] > 32)
		ERR(-1, "num_ref_idx_active");

	if (sh->sliceType != 2 && sh->sliceType != 4) {
		int r = parse_list_mods(br, sh, 0, err);
		if (r)
			return r;
		if (sh->sliceType == 1) {
			r = parse_list_mods(br, sh, 1, err);
			if (r)
				return r;
		}
	}

	if ((pps->weightedPred && (sh->sliceType == 0 || sh->sliceType == 3))
		|| (pps->weightedBipredIdc == 1 && sh->sliceType == 1)) {
		sh->hasWeights = 1;
		sh->lumaLog2Denom = br_ue(br);
		sh->chromaLog2Denom = br_ue(br);	/* ChromaArrayType 1 */
		parse_weights(br, sh, 0, 1);
		if (sh->sliceType == 1)
			parse_weights(br, sh, 1, 1);
	}

	if (sh->nalRefIdc != 0) {
		if (sh->idr) {
			sh->noOutputOfPriorPics = br_flag(br);
			sh->longTermReference = br_flag(br);
		} else {
			sh->adaptiveMarking = br_flag(br);
			if (sh->adaptiveMarking) {
				for (;;) {
					int op = br_ue(br);
					if (op == 0)
						break;
					if (op > 6 || sh->mmcoCount >= H264_MAX_MMCO)
						ERR(-1, "MMCO %d", op);
					H264Mmco *m = &sh->mmco[sh->mmcoCount++];
					memset(m, 0, sizeof(*m));
					m->op = op;
					if (op == 1 || op == 3)
						m->diffPicNumsMinus1 = br_ue(br);
					if (op == 2)
						m->longTermPicNum = br_ue(br);
					if (op == 3 || op == 6)
						m->longTermFrameIdx = br_ue(br);
					if (op == 4)
						m->maxLongTermFrameIdxPlus1 = br_ue(br);
					if (br->overrun)
						ERR(-1, "MMCO truncated");
				}
			}
		}
	}

	if (pps->entropyCodingMode && sh->sliceType != 2 && sh->sliceType != 4)
		sh->cabacInitIdc = br_ue(br);
	sh->sliceQpDelta = br_se(br);
	if (sh->sliceType == 3 || sh->sliceType == 4) {
		if (sh->sliceType == 3)
			br_flag(br);
		br_se(br);
	}
	if (pps->deblockingFilterControlPresent) {
		sh->disableDeblockingFilterIdc = br_ue(br);
		if (sh->disableDeblockingFilterIdc != 1) {
			sh->sliceAlphaC0OffsetDiv2 = br_se(br);
			sh->sliceBetaOffsetDiv2 = br_se(br);
		}
	}
	if (br->overrun)
		ERR(-1, "slice header truncated");
	sh->headerBits = (int)br_pos(br);
	return 0;
}


// #pragma mark - POC, 8.2.1


static int free_slot(H264State *st);


/* 8.2.5.3 for a frame with frame_num \a frameNum: the oldest short-term
   reference goes when the references are as many as the SPS allows. */
static void
sliding_window(H264State *st, const H264Sps *sps, int frameNum, int maxFrameNum)
{
	int numShort = 0, numLong = 0, oldest = -1, oldestWrap = 0;
	for (int i = 0; i < H264_MAX_REFS; i++) {
		if (st->refs[i].ref == 1) {
			int wrap = st->refs[i].frameNum > frameNum
				? st->refs[i].frameNum - maxFrameNum : st->refs[i].frameNum;
			numShort++;
			if (oldest < 0 || wrap < oldestWrap) {
				oldest = i;
				oldestWrap = wrap;
			}
		} else if (st->refs[i].ref == 2)
			numLong++;
	}
	int max = sps->maxNumRefFrames > 0 ? sps->maxNumRefFrames : 1;
	if (numShort + numLong >= max && numShort > 0)
		st->refs[oldest].ref = 0;
}


/* 8.2.5.2: the frame_num values a stream skipped become "non-existing"
   short-term references, marked as decoded frames would be. */
static void
fill_frame_num_gap(H264State *st, const H264Slice *sh, const H264Sps *sps,
	int maxFrameNum)
{
	if (sh->idr)
		return;
	int unused = (st->prevRefFrameNum + 1) % maxFrameNum;
	if (sh->frameNum == st->prevRefFrameNum || sh->frameNum == unused)
		return;
	for (int n = 0; unused != sh->frameNum && n < maxFrameNum; n++) {
		sliding_window(st, sps, unused, maxFrameNum);
		int slot = free_slot(st);
		if (slot < 0)
			break;
		H264Ref *r = &st->refs[slot];
		memset(r, 0, sizeof(*r));
		r->ref = 1;
		r->frameNum = unused;
		r->frameNumWrap = unused;
		r->frame = -1;
		r->nonExisting = 1;
		/* for POC types 1 and 2 they count as decoded frames */
		if (st->prevFrameNum > unused)
			st->prevFrameNumOffset += maxFrameNum;
		st->prevFrameNum = unused;
		st->prevRefFrameNum = unused;
		unused = (unused + 1) % maxFrameNum;
	}
}


void
h264_start_picture(H264State *st, const H264Slice *sh)
{
	const H264Pps *pps = &st->pps[sh->ppsId];
	const H264Sps *sps = &st->sps[pps->spsId];
	int maxFrameNum = 1 << sps->log2MaxFrameNum;

	if (sh->idr) {
		/* 8.2.5.1: an IDR empties the reference list */
		for (int i = 0; i < H264_MAX_REFS; i++)
			st->refs[i].ref = 0;
		st->prevRefFrameNum = 0;
	} else
		fill_frame_num_gap(st, sh, sps, maxFrameNum);

	if (sps->pocType == 0) {
		if (sh->idr) {
			st->prevPocMsb = 0;
			st->prevPocLsb = 0;
		}
		int maxLsb = 1 << sps->log2MaxPocLsb;
		int msb;
		if (sh->pocLsb < st->prevPocLsb
			&& st->prevPocLsb - sh->pocLsb >= maxLsb / 2)
			msb = st->prevPocMsb + maxLsb;
		else if (sh->pocLsb > st->prevPocLsb
			&& sh->pocLsb - st->prevPocLsb > maxLsb / 2)
			msb = st->prevPocMsb - maxLsb;
		else
			msb = st->prevPocMsb;
		st->curPocMsb = msb;
		st->curTopPoc = msb + sh->pocLsb;
		st->curBottomPoc = st->curTopPoc + sh->deltaPocBottom;
	} else {
		int offset;
		if (sh->idr)
			offset = 0;
		else if (st->prevFrameNum > sh->frameNum)
			offset = st->prevFrameNumOffset + maxFrameNum;
		else
			offset = st->prevFrameNumOffset;
		st->curFrameNumOffset = offset;

		if (sps->pocType == 1) {
			int n = sps->numRefFramesInPocCycle;
			int abs = n != 0 ? offset + sh->frameNum : 0;
			if (sh->nalRefIdc == 0 && abs > 0)
				abs--;
			int expected = 0;
			if (abs > 0) {
				int perCycle = 0;
				for (int i = 0; i < n; i++)
					perCycle += sps->offsetForRefFrame[i];
				int cycle = (abs - 1) / n;
				int inCycle = (abs - 1) % n;
				expected = cycle * perCycle;
				for (int i = 0; i <= inCycle; i++)
					expected += sps->offsetForRefFrame[i];
			}
			if (sh->nalRefIdc == 0)
				expected += sps->offsetForNonRefPic;
			st->curTopPoc = expected + sh->deltaPoc[0];
			st->curBottomPoc = st->curTopPoc
				+ sps->offsetForTopToBottomField + sh->deltaPoc[1];
		} else {
			int poc;
			if (sh->idr)
				poc = 0;
			else if (sh->nalRefIdc == 0)
				poc = 2 * (offset + sh->frameNum) - 1;
			else
				poc = 2 * (offset + sh->frameNum);
			st->curTopPoc = st->curBottomPoc = poc;
		}
	}
	st->curPoc = st->curTopPoc < st->curBottomPoc
		? st->curTopPoc : st->curBottomPoc;

	/* 8.2.4.1: picture numbers of the short-term references */
	for (int i = 0; i < H264_MAX_REFS; i++) {
		H264Ref *r = &st->refs[i];
		if (r->ref == 1) {
			r->frameNumWrap = r->frameNum > sh->frameNum
				? r->frameNum - maxFrameNum : r->frameNum;
		}
	}
}


// #pragma mark - reference lists, 8.2.4


/* refs of one kind, sorted by key: an insertion sort of at most 16 */
static int
collect(H264State *st, int kind, int (*keyOf)(const H264Ref *), int desc,
	int (*filter)(const H264State *, const H264Ref *), int *out)
{
	int keys[H264_MAX_REFS];
	int count = 0;
	for (int i = 0; i < H264_MAX_REFS; i++) {
		const H264Ref *r = &st->refs[i];
		if (r->ref != kind || (filter && !filter(st, r)))
			continue;
		int key = keyOf(r);
		int j = count++;
		while (j > 0 && (desc ? keys[j - 1] < key : keys[j - 1] > key)) {
			keys[j] = keys[j - 1];
			out[j] = out[j - 1];
			j--;
		}
		keys[j] = key;
		out[j] = i;
	}
	return count;
}

static int key_picnum(const H264Ref *r) { return r->frameNumWrap; }
static int key_ltnum(const H264Ref *r) { return r->longTermFrameIdx; }
static int key_poc(const H264Ref *r) { return r->poc; }
static int before(const H264State *st, const H264Ref *r) { return r->poc < st->curPoc; }
static int after(const H264State *st, const H264Ref *r) { return r->poc > st->curPoc; }


static void
modify_list(H264State *st, const H264Slice *sh, int list, int *refList)
{
	const H264Sps *sps = &st->sps[st->pps[sh->ppsId].spsId];
	int maxPicNum = 1 << sps->log2MaxFrameNum;
	int currPicNum = sh->frameNum;
	int picNumPred = currPicNum;
	int n = sh->numRefIdxActive[list];
	int refIdx = 0;

	for (int m = 0; m < sh->modCount[list]; m++) {
		const H264Mod *mod = &sh->mods[list][m];
		int pic = -1;
		int isLong = mod->idc == 2;
		int target;
		if (!isLong) {
			int absDiff = mod->value + 1;
			int noWrap;
			if (mod->idc == 0) {
				noWrap = picNumPred - absDiff;
				if (noWrap < 0)
					noWrap += maxPicNum;
			} else {
				noWrap = picNumPred + absDiff;
				if (noWrap >= maxPicNum)
					noWrap -= maxPicNum;
			}
			picNumPred = noWrap;
			target = noWrap > currPicNum ? noWrap - maxPicNum : noWrap;
			for (int i = 0; i < H264_MAX_REFS; i++) {
				if (st->refs[i].ref == 1 && st->refs[i].frameNumWrap == target)
					pic = i;
			}
		} else {
			target = mod->value;
			for (int i = 0; i < H264_MAX_REFS; i++) {
				if (st->refs[i].ref == 2 && st->refs[i].longTermFrameIdx == target)
					pic = i;
			}
		}

		/* 8.2.4.3.1 / 8.2.4.3.2: insert at refIdx, drop the later duplicate */
		for (int c = n; c > refIdx; c--)
			refList[c] = refList[c - 1];
		refList[refIdx++] = pic;
		int nIdx = refIdx;
		for (int c = refIdx; c <= n; c++) {
			int e = refList[c];
			int same = e >= 0 && e == pic;
			(void)isLong;
			if (!same)
				refList[nIdx++] = e;
		}
	}
}


void
h264_ref_lists(H264State *st, const H264Slice *sh, int list0[32], int list1[32])
{
	int tmp[2][H264_MAX_REFS * 2 + 2];
	int count[2] = { 0, 0 };

	for (int i = 0; i < 32; i++)
		list0[i] = list1[i] = -1;

	if (sh->sliceType == 0 || sh->sliceType == 3) {
		count[0] = collect(st, 1, key_picnum, 1, NULL, tmp[0]);
		count[0] += collect(st, 2, key_ltnum, 0, NULL, tmp[0] + count[0]);
	} else if (sh->sliceType == 1) {
		int n = collect(st, 1, key_poc, 1, before, tmp[0]);
		n += collect(st, 1, key_poc, 0, after, tmp[0] + n);
		n += collect(st, 2, key_ltnum, 0, NULL, tmp[0] + n);
		count[0] = n;
		n = collect(st, 1, key_poc, 0, after, tmp[1]);
		n += collect(st, 1, key_poc, 1, before, tmp[1] + n);
		n += collect(st, 2, key_ltnum, 0, NULL, tmp[1] + n);
		count[1] = n;
		if (count[1] > 1 && count[0] == count[1]
			&& memcmp(tmp[0], tmp[1], count[0] * sizeof(int)) == 0) {
			int t = tmp[1][0];
			tmp[1][0] = tmp[1][1];
			tmp[1][1] = t;
		}
	} else {
		return;
	}

	for (int l = 0; l < 2; l++) {
		int *out = l == 0 ? list0 : list1;
		int n = sh->numRefIdxActive[l];
		if (n == 0)
			continue;
		int work[34];
		for (int i = 0; i < 34; i++)
			work[i] = i < count[l] && i < n ? tmp[l][i] : -1;
		modify_list(st, sh, l, work);
		/* an entry left at -1 is "no reference picture"; the engine
		   gets position 0 for it, as Cedrus does */
		for (int i = 0; i < n; i++)
			out[i] = work[i];
	}
}


// #pragma mark - marking, 8.2.5


static int
free_slot(H264State *st)
{
	for (int i = 0; i < H264_MAX_REFS; i++) {
		if (st->refs[i].ref == 0)
			return i;
	}
	return -1;
}


int
h264_finish_picture(H264State *st, const H264Slice *sh, int frame)
{
	const H264Sps *sps = &st->sps[st->pps[sh->ppsId].spsId];
	int maxFrameNum = 1 << sps->log2MaxFrameNum;
	int mmco5 = 0;
	int markedLong = 0;
	int slot = -1;

	if (sh->nalRefIdc != 0) {
		if (sh->idr) {
			st->maxLongTermFrameIdx = sh->longTermReference ? 0 : -1;
			slot = free_slot(st);
			if (slot < 0)
				return -1;
			st->refs[slot].ref = sh->longTermReference ? 2 : 1;
			st->refs[slot].longTermFrameIdx = 0;
			markedLong = 1;	/* placed */
		} else if (sh->adaptiveMarking) {
			int currPicNum = sh->frameNum;
			for (int m = 0; m < sh->mmcoCount; m++) {
				const H264Mmco *op = &sh->mmco[m];
				int picNumX = currPicNum - (op->diffPicNumsMinus1 + 1);
				switch (op->op) {
				case 1:
					for (int i = 0; i < H264_MAX_REFS; i++) {
						if (st->refs[i].ref == 1
							&& st->refs[i].frameNumWrap == picNumX)
							st->refs[i].ref = 0;
					}
					break;
				case 2:
					for (int i = 0; i < H264_MAX_REFS; i++) {
						if (st->refs[i].ref == 2 && st->refs[i].longTermFrameIdx
								== op->longTermPicNum)
							st->refs[i].ref = 0;
					}
					break;
				case 3:
					for (int i = 0; i < H264_MAX_REFS; i++) {
						if (st->refs[i].ref == 2 && st->refs[i].longTermFrameIdx
								== op->longTermFrameIdx)
							st->refs[i].ref = 0;
					}
					for (int i = 0; i < H264_MAX_REFS; i++) {
						if (st->refs[i].ref == 1
							&& st->refs[i].frameNumWrap == picNumX) {
							st->refs[i].ref = 2;
							st->refs[i].longTermFrameIdx = op->longTermFrameIdx;
						}
					}
					break;
				case 4:
					st->maxLongTermFrameIdx = op->maxLongTermFrameIdxPlus1 - 1;
					for (int i = 0; i < H264_MAX_REFS; i++) {
						if (st->refs[i].ref == 2 && st->refs[i].longTermFrameIdx
								> st->maxLongTermFrameIdx)
							st->refs[i].ref = 0;
					}
					break;
				case 5:
					for (int i = 0; i < H264_MAX_REFS; i++)
						st->refs[i].ref = 0;
					st->maxLongTermFrameIdx = -1;
					mmco5 = 1;
					break;
				case 6:
					for (int i = 0; i < H264_MAX_REFS; i++) {
						if (st->refs[i].ref == 2 && st->refs[i].longTermFrameIdx
								== op->longTermFrameIdx)
							st->refs[i].ref = 0;
					}
					slot = free_slot(st);
					if (slot < 0)
						return -1;
					st->refs[slot].ref = 2;
					st->refs[slot].longTermFrameIdx = op->longTermFrameIdx;
					markedLong = 1;
					break;
				}
			}
		} else {
			/* 8.2.5.3 sliding window */
			sliding_window(st, sps, sh->frameNum, 1 << sps->log2MaxFrameNum);
		}
		if (!markedLong) {
			slot = free_slot(st);
			if (slot < 0)
				return -1;
			st->refs[slot].ref = 1;
		}
		H264Ref *r = &st->refs[slot];
		r->frameNum = mmco5 ? 0 : sh->frameNum;
		r->frameNumWrap = r->frameNum;
		r->topPoc = st->curTopPoc;
		r->bottomPoc = st->curBottomPoc;
		r->frame = frame;
		r->nonExisting = 0;
		if (mmco5) {
			int temp = st->curPoc;
			r->topPoc -= temp;
			r->bottomPoc -= temp;
		}
		r->poc = r->topPoc < r->bottomPoc ? r->topPoc : r->bottomPoc;
	}

	/* state for the next picture's POC */
	if (sps->pocType == 0) {
		if (sh->nalRefIdc != 0) {
			if (mmco5) {
				st->prevPocMsb = 0;
				st->prevPocLsb = st->curTopPoc - st->curPoc;
			} else {
				st->prevPocMsb = st->curPocMsb;
				st->prevPocLsb = sh->pocLsb;
			}
		}
	} else {
		st->prevFrameNumOffset = mmco5 ? 0 : st->curFrameNumOffset;
	}
	st->prevFrameNum = mmco5 ? 0 : sh->frameNum;
	if (sh->nalRefIdc != 0)
		st->prevRefFrameNum = mmco5 ? 0 : sh->frameNum;
	st->curHadMmco5 = mmco5;
	(void)maxFrameNum;
	return slot;
}


int
h264_new_picture(const H264Slice *previous, const H264Slice *slice)
{
	if (previous == NULL || slice->firstMb == 0)
		return 1;
	return slice->frameNum != previous->frameNum
		|| slice->ppsId != previous->ppsId
		|| slice->fieldPic != previous->fieldPic
		|| slice->bottomField != previous->bottomField
		|| (slice->nalRefIdc == 0) != (previous->nalRefIdc == 0)
		|| slice->pocLsb != previous->pocLsb
		|| slice->deltaPocBottom != previous->deltaPocBottom
		|| slice->deltaPoc[0] != previous->deltaPoc[0]
		|| slice->deltaPoc[1] != previous->deltaPoc[1]
		|| slice->idr != previous->idr
		|| (slice->idr && slice->idrPicId != previous->idrPicId);
}
