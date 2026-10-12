/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMD_UVD_H264_STREAM_H
#define AMD_UVD_H264_STREAM_H
#include <amdgpu_video.h>
#include "../nvdec/h264_parse.h"
#include <vector>

// Userspace parameter-set, reference-picture and POC bookkeeping. Prepare
// consumes exactly one Annex B access unit. Commit only after GPU completion;
// Reset after a failed submission or seek. No entropy decoding happens here.
class H264Stream {
public:
	H264Stream();
	void Reset();
	bool Prepare(const uint8_t* data, size_t bytes);
	bool Commit();
	const char* Error() const { return fError; }
	amdgpu_h264_config config;
	amdgpu_h264_picture picture;
	std::vector<uint8_t> bitstream;
	int32_t poc;
	uint32_t sequence;
	int cropLeft, cropTop, width, height, reorderLimit;
	bool discardPrior, fullRange;
	int matrixCoefficients;
private:
	struct Reference {
		bool used, longTerm;
		int frameNum, wrap, longTermIndex;
		int32_t top, bottom;
	};
	H264ParamSets fSets;
	H264Slice fSlice;
	Reference fRefs[16];
	int64_t fPreviousPocMsb, fPreviousPocLsb, fFrameOffset;
	int fPreviousFrame;
	bool fPending, fHaveIDR;
	const char* fError;
	bool Fail(const char* reason);
	bool PictureOrder(const H264Sps& sps);
};
#endif
