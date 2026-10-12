/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMD_UVD_H264_PACKET_H
#define AMD_UVD_H264_PACKET_H
#include <stddef.h>
#include <stdint.h>
#include <vector>

// Container configuration and complete access units. No partial/truncated
// length prefix is accepted, and expansion is bounded by the kernel limit.
class H264Packet {
public:
	H264Packet() : fLengthSize(0) {}
	bool Configure(const uint8_t* data, size_t size);
	bool Convert(const uint8_t* data, size_t size, bool prepend,
		std::vector<uint8_t>& output) const;
private:
	unsigned fLengthSize;
	std::vector<uint8_t> fParameterSets;
};
#endif
