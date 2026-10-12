/* Copyright 2026, air/OS. Distributed under the terms of the MIT License.
 * H.264 reference/POC rules follow the existing nvdec_h264.c and H.264 8.2.
 */
#include "H264Stream.h"
#include <limits.h>
#include <string.h>

H264Stream::H264Stream()
	: config(), picture(), poc(0), sequence(0), cropLeft(0), cropTop(0), width(0),
	  height(0), reorderLimit(0), discardPrior(false), fullRange(false),
	  matrixCoefficients(2), fSets(), fSlice()
{
	Reset();
}

void H264Stream::Reset()
{
	memset(fRefs, 0, sizeof(fRefs));
	fPreviousPocMsb = fPreviousPocLsb = fFrameOffset = 0;
	fPreviousFrame = 0; fPending = fHaveIDR = false;
	fError = ""; bitstream.clear();
}

bool H264Stream::Fail(const char* reason) { fError = reason; return false; }

bool H264Stream::PictureOrder(const H264Sps& sps)
{
	const H264Slice& slice = fSlice;
	int64_t top, bottom;
	if (sps.picOrderCntType == 0) {
		int maxLsb = 1 << (sps.log2MaxPocLsbMinus4 + 4);
		int64_t msb = fPreviousPocMsb;
		if (slice.pocLsb < fPreviousPocLsb && fPreviousPocLsb - slice.pocLsb >= maxLsb / 2)
			msb += maxLsb;
		else if (slice.pocLsb > fPreviousPocLsb && slice.pocLsb - fPreviousPocLsb > maxLsb / 2)
			msb -= maxLsb;
		top = msb + slice.pocLsb; bottom = top + slice.deltaPocBottom;
		if (slice.nalRefIdc != 0) { fPreviousPocMsb = msb; fPreviousPocLsb = slice.pocLsb; }
	} else {
		int maxFrame = 1 << (sps.log2MaxFrameNumMinus4 + 4);
		if (fPreviousFrame > slice.frameNum) fFrameOffset += maxFrame;
		if (sps.picOrderCntType == 1) {
			int64_t absolute = sps.numRefFramesInPocCycle != 0 ? fFrameOffset + slice.frameNum : 0;
			if (slice.nalRefIdc == 0 && absolute > 0) absolute--;
			int64_t expected = 0;
			if (absolute > 0) {
				int64_t cycleSum = 0;
				for (int i = 0; i < sps.numRefFramesInPocCycle; i++) cycleSum += sps.offsetForRefFrame[i];
				// Keep the intermediate multiplication bounded for hostile long-running streams.
				int64_t cycles = (absolute - 1) / sps.numRefFramesInPocCycle;
				if (cycles > INT_MAX || cycleSum < INT_MIN || cycleSum > INT_MAX)
					return Fail("POC cycle exceeds supported range");
				expected = cycles * cycleSum;
				for (int i = 0; i <= (absolute - 1) % sps.numRefFramesInPocCycle; i++)
					expected += sps.offsetForRefFrame[i];
			}
			if (slice.nalRefIdc == 0) expected += sps.offsetForNonRefPic;
			top = expected + slice.deltaPoc[0];
			bottom = top + sps.offsetForTopToBottomField + slice.deltaPoc[1];
		} else {
			top = 2 * (fFrameOffset + slice.frameNum) - (slice.nalRefIdc == 0 ? 1 : 0);
			bottom = top;
		}
		fPreviousFrame = slice.frameNum;
	}
	if (top < INT_MIN || top > INT_MAX || bottom < INT_MIN || bottom > INT_MAX)
		return Fail("picture order exceeds firmware range");
	picture.field_order_count[0] = top; picture.field_order_count[1] = bottom;
	poc = top < bottom ? top : bottom;
	return true;
}

bool H264Stream::Prepare(const uint8_t* data, size_t bytes)
{
	if (fPending) return Fail("previous picture has not completed");
	if (data == NULL || bytes == 0 || bytes > AMDGPU_VIDEO_MAX_BITSTREAM)
		return Fail("access unit extent");
	bitstream.clear(); memset(&picture, 0, sizeof(picture));
	std::vector<uint8_t> rbsp(bytes);
	bool first = true;
	unsigned slices = 0;
	for (size_t pos = 0; pos + 3 < bytes;) {
		if (data[pos] != 0 || data[pos + 1] != 0 || data[pos + 2] != 1) { pos++; continue; }
		size_t start = pos + 3, next = start;
		while (next + 3 <= bytes && !(data[next] == 0 && data[next + 1] == 0 && data[next + 2] == 1)) next++;
		if (next + 3 > bytes) next = bytes;
		pos = next;
		size_t end = next;
		while (end > start && data[end - 1] == 0) end--;
		if (end <= start || (data[start] & 0x80) != 0) return Fail("invalid NAL unit");
		unsigned type = data[start] & 31, ref = data[start] >> 5 & 3;
		if (type != 7 && type != 8 && type != 1 && type != 5) continue;
		size_t size = h264ToRbsp(data + start + 1, end - start - 1, rbsp.data());
		if (type == 7) {
			H264Sps sps;
			if (!h264ParseSps(rbsp.data(), size, &sps)) return Fail("invalid SPS");
			if (!first) return Fail("parameter set after picture slices");
			fSets.sps[sps.id] = sps;
		} else if (type == 8) {
			H264Pps pps;
			if (!h264ParsePps(rbsp.data(), size, &fSets, &pps)) return Fail("invalid PPS");
			if (!first) return Fail("parameter set after picture slices");
			fSets.pps[pps.id] = pps;
		} else {
			H264Slice slice;
			if (++slices > 4096 || !h264ParseSliceHeader(rbsp.data(), size, &fSets, type, ref, &slice))
				return Fail("invalid slice header");
			const H264Sps& active = fSets.sps[fSets.pps[slice.ppsId].spsId];
			if (slice.firstMbInSlice >= active.picWidthInMbs * active.picHeightInMapUnits)
				return Fail("slice starts outside the picture");
			if (first) {
				if (slice.firstMbInSlice != 0) return Fail("missing first slice");
				fSlice = slice; first = false;
			} else if (slice.firstMbInSlice == 0 || slice.ppsId != fSlice.ppsId
				|| slice.frameNum != fSlice.frameNum || slice.idr != fSlice.idr
				|| slice.pocLsb != fSlice.pocLsb || slice.deltaPocBottom != fSlice.deltaPocBottom
				|| slice.deltaPoc[0] != fSlice.deltaPoc[0] || slice.deltaPoc[1] != fSlice.deltaPoc[1]
				|| slice.idrPicId != fSlice.idrPicId
				|| slice.nalRefIdc != fSlice.nalRefIdc || slice.fieldPic != fSlice.fieldPic)
				return Fail("access unit contains inconsistent pictures");
			const uint8_t prefix[] = {0, 0, 1};
			bitstream.insert(bitstream.end(), prefix, prefix + 3);
			bitstream.insert(bitstream.end(), data + start, data + end);
		}
	}
	if (first) return Fail("access unit has no picture");
	const H264Pps& pps = fSets.pps[fSlice.ppsId];
	const H264Sps& sps = fSets.sps[pps.spsId];
	if (!sps.frameMbsOnly || fSlice.fieldPic || sps.chromaFormatIdc != 1
		|| sps.bitDepthLuma != 8 || sps.bitDepthChroma != 8 || sps.qpprimeYZeroTransformBypass)
		return Fail("only progressive 8-bit 4:2:0 is supported");
	if (sps.profileIdc != 66 && sps.profileIdc != 77 && sps.profileIdc != 100)
		return Fail("unsupported H264 profile");
	if (pps.picInitQpMinus26 < -26 || pps.picInitQpMinus26 > 25
		|| pps.chromaQpIndexOffset < -12 || pps.chromaQpIndexOffset > 12
		|| pps.secondChromaQpIndexOffset < -12 || pps.secondChromaQpIndexOffset > 12
		|| pps.numRefIdxL0Minus1 > 15 || pps.numRefIdxL1Minus1 > 15
		|| fSlice.sliceType > 2) return Fail("unsupported picture parameters");
	config = {(uint32_t)sps.picWidthInMbs * 16, (uint32_t)sps.picHeightInMapUnits * 16,
		(uint32_t)sps.profileIdc, (uint32_t)sps.levelIdc, (uint32_t)sps.maxNumRefFrames, 0};
	cropLeft = sps.cropLeft * 2; cropTop = sps.cropTop * 2;
	width = config.width - cropLeft - sps.cropRight * 2;
	height = config.height - cropTop - sps.cropBottom * 2;
	if (width <= 0 || height <= 0) return Fail("invalid frame cropping");
	reorderLimit = sps.hasReorderFrames ? sps.maxNumReorderFrames : h264MaxDpbFrames(&sps);
	if (reorderLimit < 0 || reorderLimit > 16) return Fail("unsupported reorder limit");
	fullRange = sps.fullRange != 0;
	matrixCoefficients = sps.matrixCoefficients;
	discardPrior = fSlice.idr && fSlice.noOutputOfPriorPics;
	if (fSlice.idr) {
		std::vector<uint8_t> saved;
		saved.swap(bitstream);
		Reset();
		bitstream.swap(saved);
		if (sequence == UINT32_MAX) return Fail("coded sequence counter exhausted");
		sequence++; fHaveIDR = true;
	} else if (!fHaveIDR) return Fail("IDR required after reset");
	picture.flags = fSlice.idr ? AMDGPU_H264_IDR : 0;
	picture.sps_flags = sps.direct8x8Inference | 4 | sps.deltaPicOrderAlwaysZero << 3
		| sps.gapsInFrameNumAllowed << 5;
	picture.pps_flags = pps.transform8x8Mode | pps.redundantPicCntPresent << 1
		| pps.constrainedIntraPred << 2 | pps.deblockingFilterControlPresent << 3
		| pps.weightedBipredIdc << 4 | pps.weightedPred << 6 | pps.picOrderPresent << 7
		| pps.entropyCodingMode << 8;
	picture.log2_frame_num_minus4 = sps.log2MaxFrameNumMinus4;
	picture.poc_type = sps.picOrderCntType; picture.log2_poc_lsb_minus4 = sps.log2MaxPocLsbMinus4;
	picture.ref_l0_minus1 = pps.numRefIdxL0Minus1; picture.ref_l1_minus1 = pps.numRefIdxL1Minus1;
	picture.initial_qp_minus26 = pps.picInitQpMinus26;
	picture.chroma_qp_offset = pps.chromaQpIndexOffset;
	picture.second_chroma_qp_offset = pps.secondChromaQpIndexOffset;
	picture.frame_num = fSlice.frameNum;
	memcpy(picture.scaling4x4, pps.scaling4x4, sizeof(picture.scaling4x4));
	memcpy(picture.scaling8x8, pps.scaling8x8, sizeof(picture.scaling8x8));
	if (!PictureOrder(sps)) return false;
	// MMCO 5 starts a new output epoch and renumbers this picture to zero.
	// Keep the original POCs in the firmware descriptor; Commit applies the
	// same renumbering to the retained reference and the next picture's state.
	for (int i = 0; i < fSlice.mmcoCount; i++) {
		if (fSlice.mmco[i].op != 5) continue;
		if (sequence == UINT32_MAX) return Fail("coded sequence counter exhausted");
		sequence++; poc = 0;
		break;
	}
	unsigned index = 0;
	int maxFrame = 1 << (sps.log2MaxFrameNumMinus4 + 4);
	for (auto& ref : fRefs) {
		if (!ref.used) continue;
		ref.wrap = ref.frameNum > fSlice.frameNum ? ref.frameNum - maxFrame : ref.frameNum;
		picture.reference_frame_num[index] = ref.longTerm ? ref.longTermIndex : ref.frameNum;
		picture.reference_field_order_count[index][0] = ref.top;
		picture.reference_field_order_count[index][1] = ref.bottom;
		index++;
	}
	fPending = true;
	return true;
}

bool H264Stream::Commit()
{
	if (!fPending) return Fail("no pending picture");
	fPending = false;
	if (fSlice.nalRefIdc == 0 || config.max_references == 0) return true;
	bool longTerm = fSlice.idr && fSlice.longTermReference, mmco5 = false;
	int longIndex = 0;
	auto removeLong = [&](int index) {
		for (auto& r : fRefs) if (r.used && r.longTerm && r.longTermIndex == index) r.used = false;
	};
	if (!fSlice.idr && fSlice.adaptiveRefPicMarking) {
		for (int i = 0; i < fSlice.mmcoCount; i++) {
			const H264Mmco& m = fSlice.mmco[i];
			if (m.op == 1 || m.op == 3) {
				if (m.op == 3) removeLong(m.longTermFrameIdx);
				for (auto& r : fRefs) {
					if (!r.used || r.longTerm || r.wrap != fSlice.frameNum - m.differenceOfPicNumsMinus1 - 1) continue;
					if (m.op == 1) r.used = false;
					else { r.longTerm = true; r.longTermIndex = m.longTermFrameIdx; }
				}
			} else if (m.op == 2) removeLong(m.longTermPicNum);
			else if (m.op == 4) {
				for (auto& r : fRefs) if (r.used && r.longTerm && r.longTermIndex >= m.maxLongTermFrameIdxPlus1) r.used = false;
			} else if (m.op == 5) { memset(fRefs, 0, sizeof(fRefs)); mmco5 = true; }
			else if (m.op == 6) { removeLong(m.longTermFrameIdx); longTerm = true; longIndex = m.longTermFrameIdx; }
		}
	} else if (!fSlice.idr) {
		unsigned count = 0; Reference* oldest = NULL;
		for (auto& r : fRefs) {
			if (!r.used) continue;
			count++;
			if (!r.longTerm && (oldest == NULL || r.wrap < oldest->wrap)) oldest = &r;
		}
		if (count >= config.max_references && oldest != NULL) oldest->used = false;
	}
	int64_t top = picture.field_order_count[0], bottom = picture.field_order_count[1];
	if (mmco5) {
		int64_t minimum = top < bottom ? top : bottom;
		top -= minimum; bottom -= minimum;
		if (top > INT_MAX || bottom > INT_MAX) return Fail("MMCO picture order range");
		fPreviousFrame = 0; fFrameOffset = 0; fPreviousPocMsb = 0; fPreviousPocLsb = top;
	}
	for (auto& ref : fRefs) {
		if (ref.used) continue;
		ref = {true, longTerm, mmco5 ? 0 : fSlice.frameNum, mmco5 ? 0 : fSlice.frameNum,
			longIndex, (int32_t)top, (int32_t)bottom};
		return true;
	}
	return Fail("reference picture table full");
}
