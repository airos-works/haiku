/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_UVD_LAYOUT_H
#define AMDGPU_UVD_LAYOUT_H
#include <stdint.h>
#include <stddef.h>
namespace amdgpu {
struct UvdVideoLayout {
	uint32_t bytes, pitch, outputHeight, outputBytes, dpbBytes, contextBytes;
	uint32_t message, feedback, scaling, session, bitstream, dpb, context, target;
	uint32_t guards[4];
};
const size_t kUvdMessageBytes = 3556;
}
#endif
