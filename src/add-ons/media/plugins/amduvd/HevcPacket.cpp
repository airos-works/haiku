/* Copyright 2026, air/OS. Distributed under the terms of the MIT License.
 * hvcC layout: ISO/IEC 14496-15, as used by FFmpeg hevc_mp4toannexb_bsf.c.
 */
#include "HevcPacket.h"
#include <amdgpu_video.h>

namespace {
bool AnnexB(const uint8_t* data, size_t size)
{
	return size >= 4 && !data[0] && !data[1]
		&& (data[2] == 1 || (!data[2] && data[3] == 1));
}
bool Header(const uint8_t* data, size_t size)
{
	return size >= 2 && !(data[0] & 0x81) && !(data[1] & 0xf8) && (data[1] & 7);
}
bool Append(std::vector<uint8_t>& output, const uint8_t* data, size_t size, bool prefix)
{
	size_t extra = prefix ? 4 : 0;
	if (!size || size > AMDGPU_VIDEO_MAX_BITSTREAM - extra
		|| output.size() > AMDGPU_VIDEO_MAX_BITSTREAM - extra - size) return false;
	if (prefix) output.insert(output.end(), {0, 0, 0, 1});
	output.insert(output.end(), data, data + size);
	return true;
}
}

bool HevcPacket::Configure(const uint8_t* data, size_t size)
{
	fValid = false; fLengthSize = 0; fParameterSets.clear();
	if (!size) { fValid = true; return true; }
	if (!data || size > AMDGPU_VIDEO_MAX_BITSTREAM) return false;
	if (AnnexB(data, size)) {
		fValid = Append(fParameterSets, data, size, false);
		return fValid;
	}
	if (size < 23 || data[0] != 1 || (data[13] & 0xf0) != 0xf0
		|| (data[15] & 0xfc) != 0xfc || (data[16] & 0xfc) != 0xfc
		|| (data[17] & 0xf8) != 0xf8 || (data[18] & 0xf8) != 0xf8) return false;
	std::vector<uint8_t> groups[5];
	size_t at = 23, total = 0;
	for (unsigned array = 0; array < data[22]; array++) {
		if (size - at < 3 || (data[at] & 0x40)) return false;
		unsigned type = data[at++] & 63;
		unsigned group = type >= 32 && type <= 34 ? type - 32 : type == 39 ? 3 : 4;
		if (type != 32 && type != 33 && type != 34 && type != 39 && type != 40) return false;
		unsigned count = unsigned(data[at]) * 256 + data[at + 1]; at += 2;
		for (unsigned i = 0; i < count; i++) {
			if (size - at < 2) return false;
			size_t bytes = size_t(data[at]) * 256 + data[at + 1]; at += 2;
			if (bytes > size - at || !Header(data + at, bytes) || ((data[at] >> 1) & 63) != type
				|| total > AMDGPU_VIDEO_MAX_BITSTREAM - bytes - 4) return false;
			if (!Append(groups[group], data + at, bytes, true)) return false;
			total += bytes + 4; at += bytes;
		}
	}
	if (at != size) return false;
	// Array ordering is not fixed by hvcC. Parse SPS before the PPS that uses it.
	for (const auto& group : groups) fParameterSets.insert(fParameterSets.end(), group.begin(), group.end());
	fLengthSize = (data[21] & 3) + 1;
	fValid = true;
	return true;
}

bool HevcPacket::Convert(const uint8_t* data, size_t size, bool prepend,
	std::vector<uint8_t>& output) const
{
	output.clear();
	if (!fValid || !data || !size || size > AMDGPU_VIDEO_MAX_BITSTREAM) return false;
	std::vector<uint8_t> converted;
	if (prepend) converted = fParameterSets;
	if (!fLengthSize) {
		if (!AnnexB(data, size) || !Append(converted, data, size, false)) return false;
	} else for (size_t at = 0; at < size;) {
		if (size - at < fLengthSize) return false;
		size_t bytes = 0;
		for (unsigned i = 0; i < fLengthSize; i++) bytes = (bytes << 8) | data[at++];
		if (bytes > size - at || !Header(data + at, bytes) || !Append(converted, data + at, bytes, true))
			return false;
		at += bytes;
	}
	output.swap(converted);
	return true;
}
