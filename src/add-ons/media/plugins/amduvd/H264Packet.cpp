/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "H264Packet.h"
#include <amdgpu_video.h>

static bool Append(std::vector<uint8_t>& output, const uint8_t* data, size_t size,
	bool startCode)
{
	size_t prefix = startCode ? 4 : 0;
	if (size == 0 || size > AMDGPU_VIDEO_MAX_BITSTREAM - prefix
		|| output.size() > AMDGPU_VIDEO_MAX_BITSTREAM - prefix - size) return false;
	if (startCode) {
		const uint8_t start[] = {0, 0, 0, 1};
		output.insert(output.end(), start, start + 4);
	}
	output.insert(output.end(), data, data + size);
	return true;
}

bool H264Packet::Configure(const uint8_t* data, size_t size)
{
	fLengthSize = 0; fParameterSets.clear();
	if (size == 0) return true;
	if (data == NULL || size > AMDGPU_VIDEO_MAX_BITSTREAM) return false;
	if (data[0] != 1) {
		if (size < 4 || data[0] != 0 || data[1] != 0
			|| (data[2] != 1 && (data[2] != 0 || data[3] != 1))) return false;
		return Append(fParameterSets, data, size, false);
	}
	if (size < 7 || (data[4] & 0xfc) != 0xfc || (data[5] & 0xe0) != 0xe0)
		return false;
	unsigned lengthSize = (data[4] & 3) + 1;
	if (lengthSize == 3) return false; // reserved by AVCDecoderConfigurationRecord
	std::vector<uint8_t> sets;
	size_t at = 5;
	for (unsigned kind = 0; kind < 2; kind++) {
		if (at >= size) return false;
		unsigned count = data[at++] & (kind == 0 ? 31 : 255);
		if (count == 0) return false;
		for (unsigned i = 0; i < count; i++) {
			if (size - at < 2) return false;
			size_t bytes = (size_t)data[at] * 256 + data[at + 1]; at += 2;
			if (bytes == 0 || bytes > size - at || (data[at] & 0x80)
				|| (data[at] & 31) != (kind == 0 ? 7 : 8)
				|| !Append(sets, data + at, bytes, true)) return false;
			at += bytes;
		}
	}
	// Optional high-profile extension: validate its extents even though
	// 8-bit 4:2:0 does not use sequence parameter set extensions.
	if (at < size) {
		if (data[1] != 100 && data[1] != 110 && data[1] != 122 && data[1] != 144)
			return false;
		if (size - at < 4 || data[at] != 0xfd || data[at + 1] != 0xf8
			|| data[at + 2] != 0xf8) return false;
		unsigned count = data[at + 3]; at += 4;
		for (unsigned i = 0; i < count; i++) {
			if (size - at < 2) return false;
			size_t bytes = (size_t)data[at] * 256 + data[at + 1]; at += 2;
			if (bytes == 0 || bytes > size - at || (data[at] & 0x80)
				|| (data[at] & 31) != 13) return false;
			at += bytes;
		}
	}
	if (at != size) return false;
	fLengthSize = lengthSize; fParameterSets.swap(sets);
	return true;
}

bool H264Packet::Convert(const uint8_t* data, size_t size, bool prepend,
	std::vector<uint8_t>& output) const
{
	output.clear();
	if (data == NULL || size == 0 || size > AMDGPU_VIDEO_MAX_BITSTREAM) return false;
	if (prepend) output = fParameterSets;
	if (fLengthSize == 0) return Append(output, data, size, false);
	for (size_t at = 0; at < size;) {
		if (size - at < fLengthSize) return false;
		size_t bytes = 0;
		for (unsigned i = 0; i < fLengthSize; i++) bytes = (bytes << 8) | data[at++];
		if (bytes == 0 || bytes > size - at || !Append(output, data + at, bytes, true))
			return false;
		at += bytes;
	}
	return true;
}
