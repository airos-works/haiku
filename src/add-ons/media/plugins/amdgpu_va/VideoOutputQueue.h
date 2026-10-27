/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMD_UVD_VIDEO_OUTPUT_QUEUE_H
#define AMD_UVD_VIDEO_OUTPUT_QUEUE_H
#include <algorithm>
#include <stdint.h>
#include <utility>
#include <vector>

// CPU output pictures ordered by coded sequence and POC. A frame retains its
// own timestamp and storage format across in-band configuration changes.
template<typename Frame> class VideoOutputQueue {
public:
	VideoOutputQueue() { Reset(); }
	void Reset() {
		fFrames.clear(); fSequence = fLastSequence = 0; fLastPoc = 0;
		fReorderLimit = 0; fHaveOutput = false;
	}
	bool Push(Frame&& frame, unsigned reorderLimit, bool discardPrior) {
		if (reorderLimit > 16 || frame.sequence < fSequence
			|| (fHaveOutput && frame.sequence == fLastSequence && frame.poc <= fLastPoc)) return false;
		if (discardPrior) fFrames.clear();
		if (fFrames.size() >= 17) return false;
		for (const auto& f : fFrames)
			if (f.sequence == frame.sequence && f.poc == frame.poc) return false;
		fSequence = frame.sequence; fReorderLimit = reorderLimit;
		auto position = std::lower_bound(fFrames.begin(), fFrames.end(), frame,
			[](const Frame& a, const Frame& b) {
				return a.sequence != b.sequence ? a.sequence < b.sequence : a.poc < b.poc;
			});
		fFrames.insert(position, std::move(frame));
		return true;
	}
	bool Ready(bool drain) const {
		return !fFrames.empty() && (drain || fFrames.front().sequence < fSequence
			|| fFrames.size() > fReorderLimit);
	}
	Frame& Front() { return fFrames.front(); }
	const Frame& Front() const { return fFrames.front(); }
	void Pop() {
		fHaveOutput = true; fLastSequence = fFrames.front().sequence;
		fLastPoc = fFrames.front().poc; fFrames.erase(fFrames.begin());
	}
private:
	std::vector<Frame> fFrames;
	uint32_t fSequence, fLastSequence;
	int32_t fLastPoc;
	unsigned fReorderLimit;
	bool fHaveOutput;
};
#endif
