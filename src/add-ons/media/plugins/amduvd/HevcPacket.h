/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMD_UVD_HEVC_PACKET_H
#define AMD_UVD_HEVC_PACKET_H
#include <stddef.h>
#include <stdint.h>
#include <vector>

// hvcC/Annex B configuration and complete packets, bounded by the kernel
// input limit. Failed configuration disables conversion until reconfigured.
class HevcPacket {
public:
	HevcPacket() : fLengthSize(0), fValid(false) {}
	bool Configure(const uint8_t* data, size_t size);
	bool Convert(const uint8_t* data, size_t size, bool prepend,
		std::vector<uint8_t>& output) const;
private:
	unsigned fLengthSize;
	bool fValid;
	std::vector<uint8_t> fParameterSets;
};
#endif
