/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMD_UVD_H264_OUTPUT_H
#define AMD_UVD_H264_OUTPUT_H
#include <stddef.h>
#include <stdint.h>
#include <vector>
#include "VideoOutputQueue.h"

struct H264Frame {
	std::vector<uint8_t> pixels;
	uint32_t sequence, pitch, codedHeight;
	int32_t poc;
	int64_t time;
	int width, height, cropLeft, cropTop, matrix;
	bool fullRange;
};

class H264Output : public VideoOutputQueue<H264Frame> {
public:
	enum Format { NV12, I420, YCbCr422, RGB32 };
	static size_t Bytes(int width, int height, Format format);
	static bool Copy(const H264Frame& frame, Format format, uint8_t* output);
};
#endif
