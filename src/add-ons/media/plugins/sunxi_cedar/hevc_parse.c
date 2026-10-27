/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include "hevc_parse.h"

#include <stdio.h>
#include <string.h>

#define ERR(code, ...) do { snprintf(err, 160, __VA_ARGS__); return code; } while (0)


static int
ceil_log2(int v)
{
	int r = 0;
	while ((1 << r) < v)
		r++;
	return r;
}


static void
skip_profile_tier_level(BitReader *br, int maxSubLayersMinus1)
{
	br_skip(br, 88);	/* general profile space .. general reserved bits */
	br_skip(br, 8);		/* general_level_idc */
	int profilePresent[8] = { 0 }, levelPresent[8] = { 0 };
	for (int i = 0; i < maxSubLayersMinus1; i++) {
		profilePresent[i] = br_flag(br);
		levelPresent[i] = br_flag(br);
	}
	if (maxSubLayersMinus1 > 0) {
		for (int i = maxSubLayersMinus1; i < 8; i++)
			br_skip(br, 2);
	}
	for (int i = 0; i < maxSubLayersMinus1; i++) {
		if (profilePresent[i])
			br_skip(br, 88);
		if (levelPresent[i])
			br_skip(br, 8);
	}
}


/* Table 7-6, in raster order */
static const uint8_t kDefaultIntra[64] = {
	16, 16, 16, 16, 17, 18, 21, 24,
	16, 16, 16, 16, 17, 19, 22, 25,
	16, 16, 17, 18, 20, 22, 25, 29,
	16, 16, 18, 21, 24, 27, 31, 36,
	17, 17, 20, 24, 30, 35, 41, 47,
	18, 19, 22, 27, 35, 44, 54, 65,
	21, 22, 25, 31, 41, 54, 70, 88,
	24, 25, 29, 36, 47, 65, 88, 115
};

static const uint8_t kDefaultInter[64] = {
	16, 16, 16, 16, 17, 18, 20, 24,
	16, 16, 16, 17, 18, 20, 24, 25,
	16, 16, 17, 18, 20, 24, 25, 28,
	16, 17, 18, 20, 24, 25, 28, 33,
	17, 18, 20, 24, 25, 28, 33, 41,
	18, 20, 24, 25, 28, 33, 41, 54,
	20, 24, 25, 28, 33, 41, 54, 71,
	24, 25, 28, 33, 41, 54, 71, 91
};


/* the up-right diagonal scan (6.5.3): raster position of each coefficient */
static void
diagonal_scan(int size, uint8_t *positions)
{
	int i = 0, x = 0, y = 0;
	while (i < size * size) {
		while (y >= 0) {
			if (x < size && y < size)
				positions[i++] = (uint8_t)(y * size + x);
			y--;
			x++;
		}
		y = x;
		x = 0;
	}
}


static void
default_scaling(HevcScaling *s)
{
	memset(s->list4x4, 16, sizeof(s->list4x4));
	for (int m = 0; m < 6; m++) {
		const uint8_t *source = m < 3 ? kDefaultIntra : kDefaultInter;
		memcpy(s->list8x8[m], source, 64);
		memcpy(s->list16x16[m], source, 64);
		s->dc16x16[m] = 16;
	}
	memcpy(s->list32x32[0], kDefaultIntra, 64);
	memcpy(s->list32x32[1], kDefaultInter, 64);
	s->dc32x32[0] = s->dc32x32[1] = 16;
}


/* 7.3.4 scaling_list_data() */
static int
parse_scaling_list_data(BitReader *br, HevcScaling *s)
{
	uint8_t scan4x4[16], scan8x8[64];
	diagonal_scan(4, scan4x4);
	diagonal_scan(8, scan8x8);
	default_scaling(s);

	for (int sizeId = 0; sizeId < 4; sizeId++) {
		int matrices = sizeId == 3 ? 2 : 6;
		for (int m = 0; m < matrices; m++) {
			uint8_t *list = sizeId == 0 ? s->list4x4[m]
				: sizeId == 1 ? s->list8x8[m]
				: sizeId == 2 ? s->list16x16[m] : s->list32x32[m];
			uint8_t *dc = sizeId == 2 ? &s->dc16x16[m]
				: sizeId == 3 ? &s->dc32x32[m] : NULL;
			int count = sizeId == 0 ? 16 : 64;

			if (!br_flag(br)) {
				/* a copy of another matrix of the size, or the default */
				uint32_t delta = br_ue(br);
				if (delta > (uint32_t)m)
					return -1;
				if (delta > 0) {
					memcpy(list, list - delta * count, count);
					if (dc != NULL)
						*dc = *(dc - delta);
				}
				continue;
			}
			int next = 8;
			if (dc != NULL) {
				next = br_se(br) + 8;
				*dc = (uint8_t)next;
			}
			for (int i = 0; i < count; i++) {
				int position = sizeId == 0 ? scan4x4[i] : scan8x8[i];
				next = (next + br_se(br) + 256) & 255;
				list[position] = (uint8_t)next;
			}
		}
	}
	return br->overrun ? -1 : 0;
}


void
hevc_init(HevcState *st)
{
	memset(st, 0, sizeof(*st));
	st->firstPicture = 1;
}


/* 7.3.7 and 7.4.8. idx == sps->numShortTermRps means "in a slice header". */
static int
parse_st_rps(BitReader *br, const HevcSps *sps, int idx, HevcStRps *out,
	char *err)
{
	int inter = 0;
	if (idx != 0)
		inter = br_flag(br);
	memset(out, 0, sizeof(*out));

	if (inter) {
		int deltaIdxMinus1 = 0;
		if (idx == sps->numShortTermRps)
			deltaIdxMinus1 = br_ue(br);
		int refIdx = idx - (deltaIdxMinus1 + 1);
		if (refIdx < 0)
			ERR(-1, "RPS reference %d", refIdx);
		int sign = br_flag(br);
		int absDelta = br_ue(br) + 1;
		int deltaRps = (1 - 2 * sign) * absDelta;
		const HevcStRps *ref = &sps->stRps[refIdx];
		int numDelta = ref->numNegative + ref->numPositive;
		int usedFlag[33], useDelta[33];
		for (int j = 0; j <= numDelta; j++) {
			usedFlag[j] = br_flag(br);
			useDelta[j] = usedFlag[j] ? 1 : br_flag(br);
		}

		int i = 0;
		for (int j = ref->numPositive - 1; j >= 0; j--) {
			int d = ref->deltaPocS1[j] + deltaRps;
			if (d < 0 && useDelta[ref->numNegative + j]) {
				out->deltaPocS0[i] = d;
				out->usedS0[i++] = usedFlag[ref->numNegative + j];
			}
		}
		if (deltaRps < 0 && useDelta[numDelta]) {
			out->deltaPocS0[i] = deltaRps;
			out->usedS0[i++] = usedFlag[numDelta];
		}
		for (int j = 0; j < ref->numNegative; j++) {
			int d = ref->deltaPocS0[j] + deltaRps;
			if (d < 0 && useDelta[j]) {
				out->deltaPocS0[i] = d;
				out->usedS0[i++] = usedFlag[j];
			}
		}
		out->numNegative = i;

		i = 0;
		for (int j = ref->numNegative - 1; j >= 0; j--) {
			int d = ref->deltaPocS0[j] + deltaRps;
			if (d > 0 && useDelta[j]) {
				out->deltaPocS1[i] = d;
				out->usedS1[i++] = usedFlag[j];
			}
		}
		if (deltaRps > 0 && useDelta[numDelta]) {
			out->deltaPocS1[i] = deltaRps;
			out->usedS1[i++] = usedFlag[numDelta];
		}
		for (int j = 0; j < ref->numPositive; j++) {
			int d = ref->deltaPocS1[j] + deltaRps;
			if (d > 0 && useDelta[ref->numNegative + j]) {
				out->deltaPocS1[i] = d;
				out->usedS1[i++] = usedFlag[ref->numNegative + j];
			}
		}
		out->numPositive = i;
	} else {
		out->numNegative = br_ue(br);
		out->numPositive = br_ue(br);
		if (out->numNegative > 16 || out->numPositive > 16)
			ERR(-1, "RPS size");
		int poc = 0;
		for (int i = 0; i < out->numNegative; i++) {
			poc -= br_ue(br) + 1;
			out->deltaPocS0[i] = poc;
			out->usedS0[i] = br_flag(br);
		}
		poc = 0;
		for (int i = 0; i < out->numPositive; i++) {
			poc += br_ue(br) + 1;
			out->deltaPocS1[i] = poc;
			out->usedS1[i] = br_flag(br);
		}
	}
	return br->overrun ? -1 : 0;
}


int
hevc_parse_sps(HevcState *st, BitReader *br, char *err)
{
	br_skip(br, 16);	/* NAL header */
	br_skip(br, 4);		/* sps_video_parameter_set_id */
	int maxSubLayersMinus1 = br_u(br, 3);
	br_skip(br, 1);
	skip_profile_tier_level(br, maxSubLayersMinus1);
	unsigned id = br_ue(br);
	if (id >= 16)
		ERR(-1, "SPS id %u", id);

	HevcSps sps;
	memset(&sps, 0, sizeof(sps));
	sps.maxSubLayersMinus1 = maxSubLayersMinus1;
	sps.chromaFormatIdc = br_ue(br);
	if (sps.chromaFormatIdc == 3)
		sps.separateColourPlane = br_flag(br);
	sps.width = br_ue(br);
	sps.height = br_ue(br);
	if (br_flag(br)) {
		int subW = sps.chromaFormatIdc == 1 || sps.chromaFormatIdc == 2 ? 2 : 1;
		int subH = sps.chromaFormatIdc == 1 ? 2 : 1;
		sps.cropLeft = br_ue(br) * subW;
		sps.cropRight = br_ue(br) * subW;
		sps.cropTop = br_ue(br) * subH;
		sps.cropBottom = br_ue(br) * subH;
	}
	sps.bitDepthLuma = br_ue(br) + 8;
	sps.bitDepthChroma = br_ue(br) + 8;
	sps.log2MaxPocLsb = br_ue(br) + 4;
	int orderingInfo = br_flag(br);
	for (int i = orderingInfo ? 0 : maxSubLayersMinus1; i <= maxSubLayersMinus1; i++) {
		sps.maxDecPicBuffering = br_ue(br) + 1;
		sps.maxNumReorder = br_ue(br);
		sps.maxLatencyIncreasePlus1 = br_ue(br);
	}
	sps.log2MinCbSize = br_ue(br) + 3;
	sps.log2DiffMaxMinCbSize = br_ue(br);
	sps.log2MinTbSize = br_ue(br) + 2;
	sps.log2DiffMaxMinTbSize = br_ue(br);
	sps.maxTransformHierarchyDepthInter = br_ue(br);
	sps.maxTransformHierarchyDepthIntra = br_ue(br);
	sps.scalingListEnabled = br_flag(br);
	default_scaling(&sps.scaling);
	if (sps.scalingListEnabled && br_flag(br)
		&& parse_scaling_list_data(br, &sps.scaling) != 0) {
		ERR(-1, "SPS scaling lists");
	}
	sps.amp = br_flag(br);
	sps.sao = br_flag(br);
	sps.pcm = br_flag(br);
	if (sps.pcm) {
		sps.pcmBitDepthLuma = br_u(br, 4) + 1;
		sps.pcmBitDepthChroma = br_u(br, 4) + 1;
		sps.log2MinPcmCbSize = br_ue(br) + 3;
		sps.log2DiffMaxMinPcmCbSize = br_ue(br);
		sps.pcmLoopFilterDisabled = br_flag(br);
	}
	sps.numShortTermRps = br_ue(br);
	if (sps.numShortTermRps > 64)
		ERR(-1, "num_short_term_ref_pic_sets");
	for (int i = 0; i < sps.numShortTermRps; i++) {
		int r = parse_st_rps(br, &sps, i, &sps.stRps[i], err);
		if (r)
			return r;
	}
	sps.longTermRefPicsPresent = br_flag(br);
	if (sps.longTermRefPicsPresent) {
		sps.numLongTermRefPicsSps = br_ue(br);
		if (sps.numLongTermRefPicsSps > 32)
			ERR(-1, "num_long_term_ref_pics_sps");
		for (int i = 0; i < sps.numLongTermRefPicsSps; i++) {
			sps.ltRefPicPocLsbSps[i] = br_u(br, sps.log2MaxPocLsb);
			sps.usedByCurrPicLtSps[i] = br_flag(br);
		}
	}
	sps.temporalMvp = br_flag(br);
	sps.strongIntraSmoothing = br_flag(br);
	/* VUI and extensions are not needed */
	if (br->overrun)
		ERR(-1, "SPS truncated");

	sps.ctbLog2 = sps.log2MinCbSize + sps.log2DiffMaxMinCbSize;
	sps.ctbSize = 1 << sps.ctbLog2;
	sps.widthCtbs = (sps.width + sps.ctbSize - 1) / sps.ctbSize;
	sps.heightCtbs = (sps.height + sps.ctbSize - 1) / sps.ctbSize;
	sps.valid = 1;
	st->sps[id] = sps;
	return 0;
}


int
hevc_parse_pps(HevcState *st, BitReader *br, char *err)
{
	br_skip(br, 16);
	unsigned id = br_ue(br);
	unsigned spsId = br_ue(br);
	if (id >= 64 || spsId >= 16)
		ERR(-1, "PPS %u / SPS %u", id, spsId);

	HevcPps pps;
	memset(&pps, 0, sizeof(pps));
	pps.spsId = spsId;
	pps.dependentSliceSegmentsEnabled = br_flag(br);
	pps.outputFlagPresent = br_flag(br);
	pps.numExtraSliceHeaderBits = br_u(br, 3);
	pps.signDataHiding = br_flag(br);
	pps.cabacInitPresent = br_flag(br);
	pps.numRefIdxDefault[0] = br_ue(br) + 1;
	pps.numRefIdxDefault[1] = br_ue(br) + 1;
	pps.initQpMinus26 = br_se(br);
	pps.constrainedIntraPred = br_flag(br);
	pps.transformSkip = br_flag(br);
	pps.cuQpDeltaEnabled = br_flag(br);
	if (pps.cuQpDeltaEnabled)
		pps.diffCuQpDeltaDepth = br_ue(br);
	pps.cbQpOffset = br_se(br);
	pps.crQpOffset = br_se(br);
	pps.sliceChromaQpOffsetsPresent = br_flag(br);
	pps.weightedPred = br_flag(br);
	pps.weightedBipred = br_flag(br);
	pps.transquantBypass = br_flag(br);
	pps.tilesEnabled = br_flag(br);
	pps.entropyCodingSync = br_flag(br);
	pps.numTileColumns = pps.numTileRows = 1;
	pps.uniformSpacing = 1;
	if (pps.tilesEnabled) {
		pps.numTileColumns = br_ue(br) + 1;
		pps.numTileRows = br_ue(br) + 1;
		if (pps.numTileColumns > HEVC_MAX_TILE_COLUMNS
			|| pps.numTileRows > HEVC_MAX_TILE_ROWS)
			ERR(-1, "%d x %d tiles", pps.numTileColumns, pps.numTileRows);
		pps.uniformSpacing = br_flag(br);
		if (!pps.uniformSpacing) {
			for (int i = 0; i < pps.numTileColumns - 1; i++)
				pps.columnWidth[i] = br_ue(br) + 1;
			for (int i = 0; i < pps.numTileRows - 1; i++)
				pps.rowHeight[i] = br_ue(br) + 1;
		}
		pps.loopFilterAcrossTiles = br_flag(br);
	}
	pps.loopFilterAcrossSlices = br_flag(br);
	if (br_flag(br)) {
		pps.deblockingOverrideEnabled = br_flag(br);
		pps.deblockingDisabled = br_flag(br);
		if (!pps.deblockingDisabled) {
			pps.betaOffsetDiv2 = br_se(br);
			pps.tcOffsetDiv2 = br_se(br);
		}
	}
	pps.scalingListDataPresent = br_flag(br);
	if (pps.scalingListDataPresent
		&& parse_scaling_list_data(br, &pps.scaling) != 0) {
		ERR(-1, "PPS scaling lists");
	}
	pps.listsModificationPresent = br_flag(br);
	pps.log2ParallelMergeLevelMinus2 = br_ue(br);
	pps.sliceHeaderExtensionPresent = br_flag(br);
	if (br->overrun)
		ERR(-1, "PPS truncated");
	pps.valid = 1;
	st->pps[id] = pps;
	return 0;
}


static void
parse_pred_weights(BitReader *br, HevcSlice *sh, int bitDepthChroma)
{
	sh->lumaLog2Denom = br_ue(br);
	sh->chromaLog2Denom = sh->lumaLog2Denom + br_se(br);
	int lists = sh->sliceType == 0 ? 2 : 1;
	int halfRange = 1 << (bitDepthChroma - 1);	/* no high precision offsets */
	(void)halfRange;
	for (int l = 0; l < lists; l++) {
		int n = sh->numRefIdxActive[l];
		int lumaFlag[16], chromaFlag[16];
		for (int i = 0; i < n; i++)
			lumaFlag[i] = br_flag(br);
		for (int i = 0; i < n; i++)
			chromaFlag[i] = br_flag(br);
		for (int i = 0; i < n; i++) {
			sh->deltaLumaWeight[l][i] = 0;
			sh->lumaOffset[l][i] = 0;
			if (lumaFlag[i]) {
				sh->deltaLumaWeight[l][i] = br_se(br);
				sh->lumaOffset[l][i] = br_se(br);
			}
			for (int j = 0; j < 2; j++) {
				sh->deltaChromaWeight[l][i][j] = 0;
				sh->chromaOffset[l][i][j] = 0;
			}
			if (chromaFlag[i]) {
				for (int j = 0; j < 2; j++) {
					int dw = br_se(br);
					int doff = br_se(br);
					int w = (1 << sh->chromaLog2Denom) + dw;
					/* 7-56, wpOffsetHalfRangeC = 128 for 8 bit */
					int hr = 128;
					int off = hr + doff - ((hr * w) >> sh->chromaLog2Denom);
					if (off < -hr)
						off = -hr;
					if (off > hr - 1)
						off = hr - 1;
					sh->deltaChromaWeight[l][i][j] = dw;
					sh->chromaOffset[l][i][j] = off;
				}
			}
		}
	}
}


int
hevc_parse_slice(HevcState *st, BitReader *br, const Rbsp *rbsp, HevcSlice *sh,
	char *err)
{
	memset(sh, 0, sizeof(*sh));
	br_skip(br, 1);
	sh->nalType = br_u(br, 6);
	br_skip(br, 6);
	sh->temporalId = br_u(br, 3) - 1;

	sh->firstSliceSegmentInPic = br_flag(br);
	if (hevc_is_irap(sh->nalType))
		sh->noOutputOfPriorPics = br_flag(br);
	sh->ppsId = br_ue(br);
	if (sh->ppsId >= 64 || !st->pps[sh->ppsId].valid)
		ERR(-1, "missing PPS %d", sh->ppsId);
	const HevcPps *pps = &st->pps[sh->ppsId];
	const HevcSps *sps = &st->sps[pps->spsId];
	if (!sps->valid)
		ERR(-1, "missing SPS %d", pps->spsId);
	if (sps->chromaFormatIdc != 1 || sps->separateColourPlane)
		ERR(-2, "only 4:2:0");
	if (sps->bitDepthLuma > 10 || sps->bitDepthLuma != sps->bitDepthChroma)
		ERR(-2, "bit depth %d/%d", sps->bitDepthLuma, sps->bitDepthChroma);

	if (!sh->firstSliceSegmentInPic) {
		if (pps->dependentSliceSegmentsEnabled)
			sh->dependentSliceSegment = br_flag(br);
		sh->segmentAddress = br_u(br,
			ceil_log2(sps->widthCtbs * sps->heightCtbs));
	}
	if (sh->dependentSliceSegment) {
		/* 7.4.7.1: the header is the last independent segment's, but for
		   where this one starts and its entry points */
		if (!st->haveIndependent)
			ERR(-1, "dependent slice segment without its slice");
		HevcSlice own = *sh;
		*sh = st->lastIndependent;
		sh->nalType = own.nalType;
		sh->temporalId = own.temporalId;
		sh->firstSliceSegmentInPic = 0;
		sh->noOutputOfPriorPics = own.noOutputOfPriorPics;
		sh->dependentSliceSegment = 1;
		sh->segmentAddress = own.segmentAddress;
		sh->numEntryPoints = 0;
		goto entry_points;
	}

	br_skip(br, pps->numExtraSliceHeaderBits);
	sh->sliceType = br_ue(br);
	sh->picOutputFlag = 1;
	if (pps->outputFlagPresent)
		sh->picOutputFlag = br_flag(br);

	int numPicTotalCurr = 0;
	if (!hevc_is_idr(sh->nalType)) {
		sh->pocLsb = br_u(br, sps->log2MaxPocLsb);
		if (!br_flag(br)) {
			int r = parse_st_rps(br, sps, sps->numShortTermRps, &sh->rps, err);
			if (r)
				return r;
		} else {
			int idx = 0;
			if (sps->numShortTermRps > 1)
				idx = br_u(br, ceil_log2(sps->numShortTermRps));
			sh->rps = sps->stRps[idx];
		}
		for (int i = 0; i < sh->rps.numNegative; i++)
			numPicTotalCurr += sh->rps.usedS0[i];
		for (int i = 0; i < sh->rps.numPositive; i++)
			numPicTotalCurr += sh->rps.usedS1[i];

		if (sps->longTermRefPicsPresent) {
			int numLtSps = 0;
			if (sps->numLongTermRefPicsSps > 0)
				numLtSps = br_ue(br);
			int numLtPics = br_ue(br);
			sh->numLongTerm = numLtSps + numLtPics;
			if (sh->numLongTerm > 32)
				ERR(-1, "long-term count");
			int maxLsb = 1 << sps->log2MaxPocLsb;
			int msbCycle = 0;
			for (int i = 0; i < sh->numLongTerm; i++) {
				int lsb, used;
				if (i < numLtSps) {
					int ltIdx = 0;
					if (sps->numLongTermRefPicsSps > 1)
						ltIdx = br_u(br, ceil_log2(sps->numLongTermRefPicsSps));
					lsb = sps->ltRefPicPocLsbSps[ltIdx];
					used = sps->usedByCurrPicLtSps[ltIdx];
				} else {
					lsb = br_u(br, sps->log2MaxPocLsb);
					used = br_flag(br);
				}
				sh->ltMsbPresent[i] = br_flag(br);
				if (i == 0 || i == numLtSps)
					msbCycle = 0;
				if (sh->ltMsbPresent[i])
					msbCycle += br_ue(br);
				/* full POC relative to the current one, or only the LSBs */
				sh->ltPoc[i] = sh->ltMsbPresent[i]
					? -(msbCycle * maxLsb) - sh->pocLsb + lsb : lsb;
				sh->ltUsed[i] = used;
				numPicTotalCurr += used;
			}
		}
		if (sps->temporalMvp)
			sh->temporalMvp = br_flag(br);
	}

	if (sps->sao) {
		sh->saoLuma = br_flag(br);
		sh->saoChroma = br_flag(br);
	}

	sh->collocatedFromL0 = 1;
	if (sh->sliceType == 0 || sh->sliceType == 1) {
		sh->numRefIdxActive[0] = pps->numRefIdxDefault[0];
		sh->numRefIdxActive[1] = sh->sliceType == 0 ? pps->numRefIdxDefault[1] : 0;
		if (br_flag(br)) {
			sh->numRefIdxActive[0] = br_ue(br) + 1;
			if (sh->sliceType == 0)
				sh->numRefIdxActive[1] = br_ue(br) + 1;
		}
		if (sh->numRefIdxActive[0] > 15 || sh->numRefIdxActive[1] > 15)
			ERR(-1, "num_ref_idx_active");
		if (pps->listsModificationPresent && numPicTotalCurr > 1) {
			int bits = ceil_log2(numPicTotalCurr);
			for (int l = 0; l < (sh->sliceType == 0 ? 2 : 1); l++) {
				sh->listModified[l] = br_flag(br);
				if (sh->listModified[l]) {
					for (int i = 0; i < sh->numRefIdxActive[l]; i++)
						sh->listEntry[l][i] = br_u(br, bits);
				}
			}
		}
		if (sh->sliceType == 0)
			sh->mvdL1Zero = br_flag(br);
		if (pps->cabacInitPresent)
			sh->cabacInit = br_flag(br);
		if (sh->temporalMvp) {
			if (sh->sliceType == 0)
				sh->collocatedFromL0 = br_flag(br);
			if ((sh->collocatedFromL0 && sh->numRefIdxActive[0] > 1)
				|| (!sh->collocatedFromL0 && sh->numRefIdxActive[1] > 1))
				sh->collocatedRefIdx = br_ue(br);
		}
		if ((pps->weightedPred && sh->sliceType == 1)
			|| (pps->weightedBipred && sh->sliceType == 0))
			parse_pred_weights(br, sh, sps->bitDepthChroma);
		sh->fiveMinusMaxNumMergeCand = br_ue(br);
	}
	sh->sliceQpDelta = br_se(br);
	if (pps->sliceChromaQpOffsetsPresent) {
		sh->cbQpOffset = br_se(br);
		sh->crQpOffset = br_se(br);
	}
	int override = 0;
	if (pps->deblockingOverrideEnabled)
		override = br_flag(br);
	sh->deblockingDisabled = pps->deblockingDisabled;
	sh->betaOffsetDiv2 = pps->betaOffsetDiv2;
	sh->tcOffsetDiv2 = pps->tcOffsetDiv2;
	if (override) {
		sh->deblockingDisabled = br_flag(br);
		if (!sh->deblockingDisabled) {
			sh->betaOffsetDiv2 = br_se(br);
			sh->tcOffsetDiv2 = br_se(br);
		}
	}
	sh->loopFilterAcrossSlices = pps->loopFilterAcrossSlices;
	if (pps->loopFilterAcrossSlices
		&& (sh->saoLuma || sh->saoChroma || !sh->deblockingDisabled))
		sh->loopFilterAcrossSlices = br_flag(br);

entry_points:
	if (pps->tilesEnabled || pps->entropyCodingSync) {
		sh->numEntryPoints = br_ue(br);
		if (sh->numEntryPoints > HEVC_MAX_ENTRY)
			ERR(-1, "entry points %d", sh->numEntryPoints);
		if (sh->numEntryPoints > 0) {
			int bits = br_ue(br) + 1;
			if (bits > 32)
				ERR(-1, "offset_len");
			for (int i = 0; i < sh->numEntryPoints; i++)
				sh->entryPointMinus1[i] = br_u(br, bits);
		}
	}
	if (pps->sliceHeaderExtensionPresent) {
		int len = br_ue(br);
		br_skip(br, (size_t)len * 8);
	}
	/* byte_alignment(): a 1, then 0s */
	sh->alignBitPos = (int)br_pos(br);
	if (br_flag(br) != 1)
		ERR(-1, "alignment bit");
	while (!br_byte_aligned(br))
		br_flag(br);
	if (br->overrun)
		ERR(-1, "slice header truncated");
	sh->dataOffset = (int)(br_pos(br) / 8);
	sh->dataOffsetRaw = (int)rbsp_to_raw(rbsp, sh->dataOffset);
	if (!sh->dependentSliceSegment) {
		st->lastIndependent = *sh;
		st->haveIndependent = 1;
	}
	return 0;
}


// #pragma mark - POC and RPS, 8.3.1 / 8.3.2


int
hevc_start_picture(HevcState *st, const HevcSlice *sh, char *err)
{
	const HevcSps *sps = &st->sps[st->pps[sh->ppsId].spsId];
	int maxLsb = 1 << sps->log2MaxPocLsb;
	int noRaslOutput = hevc_is_irap(sh->nalType)
		&& (hevc_is_idr(sh->nalType) || sh->nalType <= 18 || st->firstPicture);

	int msb;
	if (hevc_is_irap(sh->nalType) && noRaslOutput) {
		msb = 0;
	} else {
		int prevLsb = st->prevTid0PocLsb, prevMsb = st->prevTid0PocMsb;
		if (sh->pocLsb < prevLsb && prevLsb - sh->pocLsb >= maxLsb / 2)
			msb = prevMsb + maxLsb;
		else if (sh->pocLsb > prevLsb && sh->pocLsb - prevLsb > maxLsb / 2)
			msb = prevMsb - maxLsb;
		else
			msb = prevMsb;
	}
	st->curPocMsb = msb;
	st->curPoc = msb + sh->pocLsb;
	st->noRaslOutput = noRaslOutput;
	st->firstPicture = 0;

	/* prevTid0Pic: TemporalId 0 and not RASL, RADL or a sub-layer non-reference */
	int nt = sh->nalType;
	int subLayerNonRef = nt <= 14 && (nt % 2) == 0;
	if (sh->temporalId == 0 && !(nt >= 6 && nt <= 9) && !subLayerNonRef) {
		st->prevTid0PocLsb = sh->pocLsb;
		st->prevTid0PocMsb = msb;
	}

	st->numStCurrBefore = st->numStCurrAfter = st->numLtCurr = 0;
	st->missingReference = 0;
	if (hevc_is_idr(sh->nalType)) {
		for (int i = 0; i < HEVC_MAX_DPB; i++)
			st->refs[i].used = 0;
		return 0;
	}
	if (hevc_is_irap(sh->nalType) && noRaslOutput) {
		for (int i = 0; i < HEVC_MAX_DPB; i++)
			st->refs[i].used = 0;
	}

	int keep[HEVC_MAX_DPB] = { 0 };
	/* long-term first, so a short-term match cannot take its picture */
	for (int i = 0; i < sh->numLongTerm; i++) {
		int found = -1;
		for (int j = 0; j < HEVC_MAX_DPB; j++) {
			if (!st->refs[j].used)
				continue;
			int poc = st->refs[j].poc;
			int match = sh->ltMsbPresent[i]
				? poc == st->curPoc + sh->ltPoc[i]
				: (poc & (maxLsb - 1)) == sh->ltPoc[i];
			if (match)
				found = j;
		}
		if (found < 0) {
			if (sh->ltUsed[i])
				st->missingReference = 1;
			continue;
		}
		keep[found] = 2;
		if (sh->ltUsed[i])
			st->ltCurr[st->numLtCurr++] = found;
	}
	for (int k = 0; k < 2; k++) {
		int n = k == 0 ? sh->rps.numNegative : sh->rps.numPositive;
		for (int i = 0; i < n; i++) {
			int delta = k == 0 ? sh->rps.deltaPocS0[i] : sh->rps.deltaPocS1[i];
			int used = k == 0 ? sh->rps.usedS0[i] : sh->rps.usedS1[i];
			int found = -1;
			for (int j = 0; j < HEVC_MAX_DPB; j++) {
				if (st->refs[j].used == 1 && keep[j] != 2
					&& st->refs[j].poc == st->curPoc + delta)
					found = j;
			}
			if (found < 0) {
				if (used)
					st->missingReference = 1;
				continue;
			}
			keep[found] = 1;
			if (!used)
				continue;
			if (k == 0)
				st->stCurrBefore[st->numStCurrBefore++] = found;
			else
				st->stCurrAfter[st->numStCurrAfter++] = found;
		}
	}
	for (int j = 0; j < HEVC_MAX_DPB; j++)
		st->refs[j].used = keep[j];
	return 0;
}


int
hevc_ref_lists(HevcState *st, const HevcSlice *sh, int list0[16], int list1[16],
	char *err)
{
	for (int i = 0; i < 16; i++)
		list0[i] = list1[i] = -1;
	if (sh->sliceType == 2)
		return 0;

	int total = st->numStCurrBefore + st->numStCurrAfter + st->numLtCurr;
	if (total == 0)
		ERR(-1, "P/B slice without references");

	for (int l = 0; l < (sh->sliceType == 0 ? 2 : 1); l++) {
		int n = sh->numRefIdxActive[l];
		int numTemp = n > total ? n : total;
		int temp[32];
		int r = 0;
		const int *first = l == 0 ? st->stCurrBefore : st->stCurrAfter;
		int nFirst = l == 0 ? st->numStCurrBefore : st->numStCurrAfter;
		const int *second = l == 0 ? st->stCurrAfter : st->stCurrBefore;
		int nSecond = l == 0 ? st->numStCurrAfter : st->numStCurrBefore;
		while (r < numTemp) {
			for (int i = 0; i < nFirst && r < numTemp; i++)
				temp[r++] = first[i];
			for (int i = 0; i < nSecond && r < numTemp; i++)
				temp[r++] = second[i];
			for (int i = 0; i < st->numLtCurr && r < numTemp; i++)
				temp[r++] = st->ltCurr[i];
		}
		int *out = l == 0 ? list0 : list1;
		for (int i = 0; i < n; i++)
			out[i] = sh->listModified[l] ? temp[sh->listEntry[l][i]] : temp[i];
	}
	return 0;
}


int
hevc_finish_picture(HevcState *st, const HevcSlice *sh, int frame)
{
	(void)sh;
	for (int i = 0; i < HEVC_MAX_DPB; i++) {
		if (!st->refs[i].used) {
			st->refs[i].used = 1;
			st->refs[i].poc = st->curPoc;
			st->refs[i].frame = frame;
			return i;
		}
	}
	return -1;
}


const HevcScaling *
hevc_scaling(const HevcState *st, const HevcSlice *sh)
{
	const HevcPps *pps = &st->pps[sh->ppsId];
	if (pps->scalingListDataPresent)
		return &pps->scaling;
	return &st->sps[pps->spsId].scaling;
}


void
hevc_tiles(const HevcState *st, const HevcSlice *sh,
	int columnWidth[HEVC_MAX_TILE_COLUMNS], int rowHeight[HEVC_MAX_TILE_ROWS])
{
	const HevcPps *pps = &st->pps[sh->ppsId];
	const HevcSps *sps = &st->sps[pps->spsId];
	int columns = pps->numTileColumns, rows = pps->numTileRows;
	if (pps->uniformSpacing) {
		/* 6-3, 6-4 */
		for (int i = 0; i < columns; i++) {
			columnWidth[i] = ((i + 1) * sps->widthCtbs) / columns
				- (i * sps->widthCtbs) / columns;
		}
		for (int j = 0; j < rows; j++) {
			rowHeight[j] = ((j + 1) * sps->heightCtbs) / rows
				- (j * sps->heightCtbs) / rows;
		}
		return;
	}
	int used = 0;
	for (int i = 0; i < columns - 1; i++) {
		columnWidth[i] = pps->columnWidth[i];
		used += columnWidth[i];
	}
	columnWidth[columns - 1] = sps->widthCtbs - used;
	used = 0;
	for (int j = 0; j < rows - 1; j++) {
		rowHeight[j] = pps->rowHeight[j];
		used += rowHeight[j];
	}
	rowHeight[rows - 1] = sps->heightCtbs - used;
}
