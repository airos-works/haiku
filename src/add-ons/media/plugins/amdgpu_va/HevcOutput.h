/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMD_UVD_HEVC_OUTPUT_H
#define AMD_UVD_HEVC_OUTPUT_H
#include "H264Output.h"

struct HevcFrame : H264Frame {
	bool p010 = false;
	unsigned bitDepth = 8;
};

class HevcOutput : public VideoOutputQueue<HevcFrame> {
public:
	enum Format { NV12, I420, YCbCr422, RGB32, P010 };
	static size_t Bytes(int width, int height, Format format);
	// P010 retains every sample; RGB uses the coded precision and VUI range.
	// Explicit eight-bit YUV output rounds ten-bit samples to the nearest code.
	static bool Copy(const HevcFrame& frame, Format format, uint8_t* output, size_t capacity);
};
#endif
