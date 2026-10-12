/* Copyright 2026, air/OS. Distributed under the terms of the MIT License.
 * POC/reference rules follow H.265 8.3 and the existing nvdec_hevc.c.
 * Firmware metadata follows Mesa's radeonsi/radeon_uvd.c get_h265_msg.
 */
#include "HevcStream.h"
#include <limits.h>
#include <string.h>

HevcStream::HevcStream()
	: config(), picture(), poc(0), sequence(0), cropLeft(0), cropTop(0), width(0),
	  height(0), reorderLimit(0), discardPrior(false), fullRange(false),
	  skipPicture(false), outputPicture(false), matrixCoefficients(2),
	  fSets(), fSlice(), fSequence(0)
{
	Reset();
}

void HevcStream::Reset()
{
	memset(fRefs, 0, sizeof(fRefs)); memset(fNextRefs, 0, sizeof(fNextRefs));
	memset(&fPreviousConfig, 0, sizeof(fPreviousConfig));
	fPreviousTid0Poc = fNextTid0Poc = 0;
	fPending = fHaveIrap = fNoRaslOutput = fNextNoRaslOutput = false;
	fError = ""; bitstream.clear();
}

void HevcStream::Clear()
{
	memset(&fSets, 0, sizeof(fSets)); memset(&fSlice, 0, sizeof(fSlice));
	memset(&config, 0, sizeof(config)); memset(&picture, 0, sizeof(picture));
	sequence = fSequence = 0;
	Reset();
}

bool HevcStream::Fail(const char* reason) { fError = reason; return false; }

bool HevcStream::References(const HevcSps& sps, bool reset)
{
	memset(fNextRefs, 0, sizeof(fNextRefs));
	memset(picture.reference_slot, 0x7f, sizeof(picture.reference_slot));
	memset(picture.st_before, 0xff, sizeof(picture.st_before));
	memset(picture.st_after, 0xff, sizeof(picture.st_after));
	memset(picture.lt_current, 0xff, sizeof(picture.lt_current));
	unsigned count = 0, before = 0, after = 0, lt = 0;
	auto add = [&](int64_t target, uint32_t mask, bool longTerm, bool used,
		uint8_t* list, unsigned& listCount) {
		int slot = -1;
		if (!reset) for (int i = 0; i < 16; i++) {
			if (!fRefs[i].used || (!longTerm && fRefs[i].longTerm)) continue;
			if ((int64_t)(int32_t)(uint32_t(fRefs[i].poc) & mask) != target) continue;
			if (slot != -1) return Fail("ambiguous reference POC");
			slot = i;
		}
		if (slot < 0) return !used || Fail("missing required reference picture");
		if (fNextRefs[slot].used || count >= 15 || (used && listCount >= 8))
			return Fail("duplicate or excessive reference set");
		fNextRefs[slot] = {true, longTerm, fRefs[slot].poc};
		picture.reference_slot[count] = slot;
		picture.reference_poc[count] = fRefs[slot].poc;
		if (used) list[listCount++] = count;
		count++;
		return true;
	};
	if (!hevcIsIdr(fSlice.nalType) && !hevcIsBla(fSlice.nalType)) {
		const auto& set = fSlice.shortTerm;
		for (int i = 0; i < set.numNegative; i++)
			if (!add(int64_t(poc) + set.deltaPoc[0][i], UINT32_MAX, false,
				set.used[0][i], picture.st_before, before)) return false;
		for (int i = 0; i < set.numPositive; i++)
			if (!add(int64_t(poc) + set.deltaPoc[1][i], UINT32_MAX, false,
				set.used[1][i], picture.st_after, after)) return false;
		uint32_t maxLsb = 1u << sps.log2MaxPocLsb;
		for (int i = 0; i < fSlice.numLongTerm; i++) {
			int64_t target = fSlice.longTermPocLsb[i];
			uint32_t mask = maxLsb - 1;
			if (fSlice.longTermMsbPresent[i]) {
				target += int64_t(poc) - int64_t(fSlice.longTermMsbCycle[i]) * maxLsb
					- (uint32_t(poc) & mask);
				mask = UINT32_MAX;
			}
			if (!add(target, mask, true, fSlice.longTermUsed[i], picture.lt_current, lt)) return false;
		}
	}
	if (count > config.max_references) return Fail("reference set exceeds SPS DPB");
	for (unsigned i = 0; i < 16; i++) if (!fNextRefs[i].used) {
		picture.current_slot = i;
		fNextRefs[i] = {true, false, poc};
		return true;
	}
	return Fail("no free private DPB slot");
}

bool HevcStream::Prepare(const uint8_t* data, size_t bytes)
{
	if (fPending) return Fail("previous picture has not completed");
	if (data == NULL || bytes == 0 || bytes > AMDGPU_VIDEO_MAX_BITSTREAM)
		return Fail("access unit extent");
	bitstream.clear(); memset(&picture, 0, sizeof(picture));
	std::vector<uint8_t> rbsp(bytes);
	bool first = true;
	unsigned slices = 0;
	for (size_t pos = 0; pos + 3 < bytes;) {
		if (data[pos] || data[pos + 1] || data[pos + 2] != 1) { pos++; continue; }
		size_t start = pos + 3, next = start;
		while (next + 3 <= bytes && !(data[next] == 0 && data[next + 1] == 0 && data[next + 2] == 1)) next++;
		if (next + 3 > bytes) next = bytes;
		pos = next;
		size_t end = next;
		while (end > start && data[end - 1] == 0) end--;
		if (end - start < 3 || (data[start] & 0x81) || (data[start + 1] & 0xf8)
			|| !(data[start + 1] & 7)) return Fail("invalid NAL unit");
		unsigned type = data[start] >> 1 & 63;
		if (type != 33 && type != 34 && type >= 32) continue;
		size_t size = h264ToRbsp(data + start, end - start, rbsp.data());
		if (type == 33) {
			HevcSps sps;
			if (!first || !hevcParseSps(rbsp.data(), size, &sps)) return Fail("invalid or late SPS");
			fSets.sps[sps.id] = sps;
		} else if (type == 34) {
			HevcPps pps;
			if (!first || !hevcParsePps(rbsp.data(), size, &fSets, &pps)) return Fail("invalid or late PPS");
			fSets.pps[pps.id] = pps;
		} else {
			HevcSlice slice;
			if (++slices > 4096 || (type > 9 && (type < 16 || type > 21))
				|| !hevcParseSliceHeader(rbsp.data(), size, &fSets, &slice)) return Fail("invalid slice header");
			if (first) {
				if (!slice.firstSliceInPicture) return Fail("missing first slice");
				fSlice = slice; first = false;
			} else if (slice.firstSliceInPicture || slice.ppsId != fSlice.ppsId
				|| slice.nalType != fSlice.nalType || slice.temporalId != fSlice.temporalId
				|| (!slice.dependentSliceSegment && (slice.pocLsb != fSlice.pocLsb
					|| slice.picOutputFlag != fSlice.picOutputFlag
					|| memcmp(&slice.shortTerm, &fSlice.shortTerm, sizeof(slice.shortTerm))
					|| slice.numLongTerm != fSlice.numLongTerm
					|| memcmp(slice.longTermPocLsb, fSlice.longTermPocLsb, sizeof(slice.longTermPocLsb))
					|| memcmp(slice.longTermUsed, fSlice.longTermUsed, sizeof(slice.longTermUsed))
					|| memcmp(slice.longTermMsbPresent, fSlice.longTermMsbPresent, sizeof(slice.longTermMsbPresent))
					|| memcmp(slice.longTermMsbCycle, fSlice.longTermMsbCycle, sizeof(slice.longTermMsbCycle)))))
				return Fail("access unit contains inconsistent pictures");
			bitstream.insert(bitstream.end(), {0, 0, 1});
			bitstream.insert(bitstream.end(), data + start, data + end);
		}
	}
	if (first) return Fail("access unit has no picture");
	const HevcPps& p = fSets.pps[fSlice.ppsId];
	const HevcSps& s = fSets.sps[p.spsId];
	if (s.transformSkipRotation || s.transformSkipContext || s.implicitRdpcm || s.explicitRdpcm
		|| s.extendedPrecision || s.intraSmoothingDisabled || s.highPrecisionOffsets
		|| s.persistentRiceAdaptation || s.cabacBypassAlignment || p.log2MaxTransformSkipSizeMinus2
		|| p.crossComponentPrediction || p.chromaQpOffsetListEnabled
		|| p.log2SaoOffsetScaleLuma || p.log2SaoOffsetScaleChroma)
		return Fail("range extensions are not represented by the UVD Main profile");
	if ((s.profileIdc != 1 && s.profileIdc != 2) || s.chromaFormatIdc != 1 || s.fieldSeq
		|| s.separateColourPlane || s.bitDepthLuma != s.bitDepthChroma
		|| (s.bitDepthLuma != 8 && s.bitDepthLuma != 10) || (s.profileIdc == 1 && s.bitDepthLuma != 8)
		|| s.width < 16 || s.height < 16 || s.width > 4096 || s.height > 4096
		|| ((s.width | s.height) & 7) || s.maxDecPicBuffering < 1 || s.maxDecPicBuffering > 16)
		return Fail("only progressive Main/Main 10 4:2:0 is supported");
	config = {(uint32_t)s.width, (uint32_t)s.height, (uint32_t)s.profileIdc,
		(uint32_t)s.bitDepthLuma, (uint32_t)s.maxDecPicBuffering - 1, (uint32_t)s.log2CtbSize, {0, 0}};
	bool irap = hevcIsIrap(fSlice.nalType);
	if (fSlice.temporalId > s.maxSubLayersMinus1 || (irap && fSlice.temporalId != 0))
		return Fail("unsupported temporal layer");
	if (!fHaveIrap && !irap) return Fail("random access picture required after reset");
	bool reset = hevcIsIdr(fSlice.nalType) || hevcIsBla(fSlice.nalType) || !fHaveIrap;
	if (fHaveIrap && !reset && memcmp(&config, &fPreviousConfig, sizeof(config)))
		return Fail("coded geometry changed without a DPB reset");
	fNextNoRaslOutput = irap ? reset : fNoRaslOutput;
	skipPicture = hevcIsRasl(fSlice.nalType) && fNextNoRaslOutput;
	outputPicture = !skipPicture && fSlice.picOutputFlag;
	discardPrior = irap && fSlice.noOutputOfPriorPics;
	cropLeft = s.confLeft; cropTop = s.confTop;
	width = s.width - s.confLeft - s.confRight; height = s.height - s.confTop - s.confBottom;
	if (width <= 0 || height <= 0) return Fail("invalid crop window");
	reorderLimit = s.maxNumReorderPics;
	fullRange = s.fullRange != 0; matrixCoefficients = s.matrixCoefficients;
	sequence = fSequence;
	if (reset) {
		if (sequence == UINT32_MAX) return Fail("coded sequence counter exhausted");
		sequence++;
	}
	int64_t maxLsb = 1u << s.log2MaxPocLsb;
	int64_t previousLsb = uint32_t(fPreviousTid0Poc) & (maxLsb - 1);
	int64_t msb = int64_t(fPreviousTid0Poc) - previousLsb;
	if (reset) msb = 0;
	else if (fSlice.pocLsb < previousLsb && previousLsb - fSlice.pocLsb >= maxLsb / 2) msb += maxLsb;
	else if (fSlice.pocLsb > previousLsb && fSlice.pocLsb - previousLsb > maxLsb / 2) msb -= maxLsb;
	int64_t order = hevcIsIdr(fSlice.nalType) ? 0 : msb + fSlice.pocLsb;
	if (order < INT_MIN || order > INT_MAX) return Fail("POC exceeds firmware range");
	poc = order;
	fNextTid0Poc = reset ? 0 : fPreviousTid0Poc;
	if (!fSlice.temporalId && !hevcIsRasl(fSlice.nalType) && !hevcIsRadl(fSlice.nalType)
		&& !hevcIsSubLayerNonReference(fSlice.nalType)) fNextTid0Poc = poc;
	if (skipPicture) { fPending = true; return true; }
	picture.nal_type = fSlice.nalType; picture.current_poc = poc;
	picture.sps_flags = s.scalingListEnabled | s.ampEnabled << 1 | s.saoEnabled << 2
		| s.pcmEnabled << 3 | s.pcmLoopFilterDisabled << 4 | s.longTermRefsPresent << 5
		| s.temporalMvpEnabled << 6 | s.strongIntraSmoothing << 7;
	picture.pps_flags = p.dependentSliceSegmentsEnabled | p.outputFlagPresent << 1
		| p.signDataHiding << 2 | p.cabacInitPresent << 3 | p.constrainedIntraPred << 4
		| p.transformSkipEnabled << 5 | p.cuQpDeltaEnabled << 6 | p.sliceChromaQpOffsetsPresent << 7
		| p.weightedPred << 8 | p.weightedBipred << 9 | p.transquantBypassEnabled << 10
		| p.tilesEnabled << 11 | p.entropyCodingSync << 12 | p.uniformSpacing << 13
		| p.loopFilterAcrossTiles << 14 | p.loopFilterAcrossSlices << 15
		| p.deblockingOverrideEnabled << 16 | p.deblockingDisabled << 17
		| p.listsModificationPresent << 18 | p.sliceHeaderExtensionPresent << 19;
	picture.log2_max_poc_lsb_minus4 = s.log2MaxPocLsb - 4;
	picture.max_dec_pic_buffering_minus1 = s.maxDecPicBuffering - 1;
	picture.log2_min_cb_size_minus3 = s.log2MinCbSize - 3;
	picture.log2_diff_max_min_cb_size = s.log2CtbSize - s.log2MinCbSize;
	picture.log2_min_tb_size_minus2 = s.log2MinTbSize - 2;
	picture.log2_diff_max_min_tb_size = s.log2MaxTbSize - s.log2MinTbSize;
	picture.max_transform_hierarchy_depth_inter = s.maxTransformHierarchyDepthInter;
	picture.max_transform_hierarchy_depth_intra = s.maxTransformHierarchyDepthIntra;
	if (s.pcmEnabled) {
		picture.pcm_bit_depth_luma_minus1 = s.pcmBitDepthLuma - 1;
		picture.pcm_bit_depth_chroma_minus1 = s.pcmBitDepthChroma - 1;
		picture.log2_min_pcm_cb_size_minus3 = s.log2MinPcmCbSize - 3;
		picture.log2_diff_max_min_pcm_cb_size = s.log2MaxPcmCbSize - s.log2MinPcmCbSize;
	}
	picture.num_extra_slice_header_bits = p.numExtraSliceHeaderBits;
	picture.num_short_term_ref_pic_sets = s.numShortTermSets;
	picture.num_long_term_ref_pics_sps = s.numLongTermRefsSps;
	picture.ref_l0_minus1 = p.numRefIdxL0DefaultActive - 1;
	picture.ref_l1_minus1 = p.numRefIdxL1DefaultActive - 1;
	picture.cb_qp_offset = p.cbQpOffset; picture.cr_qp_offset = p.crQpOffset;
	picture.beta_offset_div2 = p.betaOffsetDiv2; picture.tc_offset_div2 = p.tcOffsetDiv2;
	picture.diff_cu_qp_delta_depth = p.diffCuQpDeltaDepth;
	picture.tile_columns_minus1 = p.numTileColumns - 1;
	picture.tile_rows_minus1 = p.numTileRows - 1;
	picture.log2_parallel_merge_level_minus2 = p.log2ParallelMergeLevel - 2;
	picture.initial_qp_minus26 = p.initQpMinus26;
	picture.num_delta_pocs_ref_rps_idx = fSlice.shortTermSetSpsFlag ? 0
		: fSlice.shortTerm.numDeltaPocsOfRefRpsIdx;
	int columns[HEVC_MAX_TILE_COLUMNS], rows[HEVC_MAX_TILE_ROWS];
	hevcTileSizes(&s, &p, columns, rows);
	for (int i = 0; i < p.numTileColumns - 1; i++) picture.column_width_minus1[i] = columns[i] - 1;
	for (int i = 0; i < p.numTileRows - 1; i++) picture.row_height_minus1[i] = rows[i] - 1;
	const auto& scaling = p.scalingListPresent ? p.scaling : s.scaling;
	memcpy(picture.scaling4x4, scaling.list4x4, sizeof(picture.scaling4x4));
	memcpy(picture.scaling8x8, scaling.list8x8, sizeof(picture.scaling8x8));
	memcpy(picture.scaling16x16, scaling.list16x16, sizeof(picture.scaling16x16));
	memcpy(picture.dc16x16, scaling.dc16x16, sizeof(picture.dc16x16));
	for (unsigned i = 0; i < 2; i++) {
		memcpy(picture.scaling32x32[i], scaling.list32x32[i * 3], 64);
		picture.dc32x32[i] = scaling.dc32x32[i * 3];
	}
	if (!References(s, reset)) return false;
	fPending = true;
	return true;
}

bool HevcStream::Commit()
{
	if (!fPending) return Fail("no pending picture");
	fPending = false;
	if (skipPicture) return true;
	memcpy(fRefs, fNextRefs, sizeof(fRefs));
	fPreviousTid0Poc = fNextTid0Poc; fNoRaslOutput = fNextNoRaslOutput;
	fPreviousConfig = config; fSequence = sequence; fHaveIrap = true;
	return true;
}
