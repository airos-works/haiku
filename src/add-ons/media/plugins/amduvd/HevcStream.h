/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMD_UVD_HEVC_STREAM_H
#define AMD_UVD_HEVC_STREAM_H
#include <amdgpu_hevc.h>
#include <amdgpu_video.h>
#include "../nvdec/hevc_parse.h"
#include <vector>

// One Annex B access unit per Prepare. Commit only after successful decode;
// a skipped RASL picture is committed without submission. Reset after failure
// or seek. Parameter sets survive Reset; no entropy decoding happens here.
class HevcStream {
public:
	HevcStream();
	void Clear(); // new stream, including parameter sets; avoids a large stack temporary
	void Reset();
	bool Prepare(const uint8_t* data, size_t bytes);
	bool Commit();
	const char* Error() const { return fError; }
	amdgpu_hevc_config config;
	amdgpu_hevc_picture picture;
	std::vector<uint8_t> bitstream;
	int32_t poc;
	uint32_t sequence;
	int cropLeft, cropTop, width, height, reorderLimit;
	bool discardPrior, fullRange, skipPicture, outputPicture;
	int matrixCoefficients;
private:
	struct Reference { bool used, longTerm; int32_t poc; };
	HevcParamSets fSets;
	HevcSlice fSlice;
	Reference fRefs[16], fNextRefs[16];
	amdgpu_hevc_config fPreviousConfig;
	int32_t fPreviousTid0Poc, fNextTid0Poc;
	uint32_t fSequence;
	bool fPending, fHaveIrap, fNoRaslOutput, fNextNoRaslOutput;
	const char* fError;
	bool Fail(const char* reason);
	bool References(const HevcSps& sps, bool reset);
};
#endif
