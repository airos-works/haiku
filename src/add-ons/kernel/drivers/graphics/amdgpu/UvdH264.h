/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_UVD_H264_H
#define AMDGPU_UVD_H264_H
#include <amdgpu_video.h>
#include <stddef.h>
#include "UvdLayout.h"

namespace amdgpu {
// Firmware layout from Mesa ac_uvd_dec.h and radeonsi/radeon_uvd.c (MIT).
// All extents/offsets are derived from checked metadata, never client addresses.
typedef UvdVideoLayout UvdH264Layout;
bool UvdH264Size(const amdgpu_h264_config& config, UvdH264Layout& layout);
bool UvdH264Validate(const amdgpu_h264_config& config,
	const amdgpu_h264_picture& picture, uint32_t bitstreamBytes);
bool UvdH264Bitstream(const uint8_t* data, uint32_t bytes, bool idr);
void UvdH264Message(uint8_t* message, uint32_t type, uint32_t handle,
	const amdgpu_h264_config& config, const UvdH264Layout& layout,
	const amdgpu_h264_picture* picture, uint32_t bitstreamBytes, uint32_t frame);
}
#endif
