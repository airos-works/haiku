/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "HevcOutput.h"
#include <cmath>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <utility>
#include <string.h>

#define CHECK(value) do { if (!(value)) { \
	fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #value); abort(); } } while (0)

static unsigned Read(const HevcFrame& f, size_t sample)
{
	return (unsigned(f.pixels[sample * 2]) | unsigned(f.pixels[sample * 2 + 1]) << 8) >> (16 - f.bitDepth);
}
static int Clip(double value)
{
	return value <= 0 ? 0 : value >= 255 ? 255 : int(std::floor(value + 0.5));
}
static void Pixels()
{
	HevcFrame f = {};
	f.p010 = true; f.pitch = 32; f.codedHeight = 16;
	f.width = 8; f.height = 6; f.cropLeft = 2; f.cropTop = 4;
	f.pixels.resize(32 * 16 * 3 / 2);
	unsigned maxError = 0;
	for (unsigned bits : {8u, 10u}) for (bool full : {false, true}) {
		f.bitDepth = bits; f.fullRange = full;
		for (unsigned pattern = 0; pattern < 128; pattern++) {
			for (size_t i = 0; i < f.pixels.size() / 2; i++) {
				unsigned sample = (i * 127 + pattern * 13) & ((1u << bits) - 1);
				unsigned word = sample << (16 - bits);
				f.pixels[2 * i] = word; f.pixels[2 * i + 1] = word >> 8;
			}
			for (int matrix : {1, 5, 6}) for (auto format : {HevcOutput::P010,
				HevcOutput::NV12, HevcOutput::I420, HevcOutput::YCbCr422, HevcOutput::RGB32}) {
				f.matrix = matrix;
				size_t size = HevcOutput::Bytes(f.width, f.height, format);
				std::vector<uint8_t> out(size + 32, 0xa5);
				CHECK(!HevcOutput::Copy(f, format, out.data() + 16, size - 1));
				for (auto byte : out) CHECK(byte == 0xa5);
				CHECK(HevcOutput::Copy(f, format, out.data() + 16, size));
				for (unsigned i = 0; i < 16; i++) CHECK(out[i] == 0xa5 && out[16 + size + i] == 0xa5);
				const uint8_t* p = out.data() + 16;
				for (int y = 0; y < f.height; y++) for (int x = 0; x < f.width; x++) {
					unsigned yy = Read(f, (y + 4) * 16 + x + 2);
					unsigned uv = Read(f, 256 + (y / 2 + 2) * 16 + x + 2);
					auto down = [&](unsigned value) { return bits == 8 ? value : std::min(255u, (value + 2) / 4); };
					if (format == HevcOutput::P010) {
						unsigned luma = unsigned(p[2 * (y * 8 + x)]) | unsigned(p[2 * (y * 8 + x) + 1]) << 8;
						unsigned chroma = unsigned(p[96 + 2 * (y / 2 * 8 + x)]) | unsigned(p[97 + 2 * (y / 2 * 8 + x)]) << 8;
						CHECK(luma == yy << (16 - bits) && chroma == uv << (16 - bits));
					} else if (format == HevcOutput::YCbCr422) {
						CHECK(p[y * 16 + x * 2] == down(yy) && p[y * 16 + x * 2 + 1] == down(uv));
					} else if (format == HevcOutput::NV12 || format == HevcOutput::I420) {
						CHECK(p[y * 8 + x] == down(yy));
						size_t index = format == HevcOutput::NV12 ? y / 2 * 8 + x : (x & 1) * 12 + y / 2 * 4 + x / 2;
						CHECK(p[48 + index] == down(uv));
					} else {
						// Independent floating-point colour equations, not the implementation's integer constants.
						double kr = matrix == 1 ? .2126 : .299, kb = matrix == 1 ? .0722 : .114;
						double scale = 1u << (bits - 8), maximum = (1u << bits) - 1;
						double l = (yy - (full ? 0. : 16. * scale)) / (full ? maximum : 219. * scale);
						double u = (Read(f, 256 + (y / 2 + 2) * 16 + (x & ~1) + 2) - 128. * scale) / (full ? maximum : 224. * scale);
						double v = (Read(f, 256 + (y / 2 + 2) * 16 + (x & ~1) + 3) - 128. * scale) / (full ? maximum : 224. * scale);
						int expected[] = {Clip((l + (2 - 2 * kb) * u) * 255),
							Clip((l - 2 * kb * (1 - kb) / (1 - kr - kb) * u - 2 * kr * (1 - kr) / (1 - kr - kb) * v) * 255),
							Clip((l + (2 - 2 * kr) * v) * 255), 255};
						for (unsigned c = 0; c < 4; c++) {
							unsigned error = std::abs(int(p[4 * (y * 8 + x) + c]) - expected[c]);
							maxError = std::max(maxError, error); CHECK(error <= 1);
						}
					}
				}
			}
		}
	}
	std::vector<uint8_t> out(256, 0xa5);
	f.matrix = 9; CHECK(!HevcOutput::Copy(f, HevcOutput::RGB32, out.data(), out.size()));
	f.cropLeft = INT_MAX - 1; CHECK(!HevcOutput::Copy(f, HevcOutput::P010, out.data(), out.size()));
	for (auto byte : out) CHECK(byte == 0xa5);
	// Eight-bit NV12 storage can be preserved as P010 without dropping codes.
	f = {}; f.width = f.height = f.codedHeight = 2; f.pitch = 2;
	f.pixels = {0, 16, 235, 255, 128, 240}; f.p010 = false;
	CHECK(HevcOutput::Copy(f, HevcOutput::P010, out.data(), out.size()));
	for (size_t i = 0; i < f.pixels.size(); i++) CHECK(out[2 * i] == 0 && out[2 * i + 1] == f.pixels[i]);
	printf("PASS: cropped P010 exact, 8/10-bit YUV, 601/709 full/limited RGB max error %u, capacity and guards\n", maxError);
}

static void ColourRows()
{
	unsigned maxError = 0;
	uint32_t random = 101;
	for (int width : {2,4,6,8,10,14,16,18,30,32,34}) for (int height : {6,580}) {
		HevcFrame f = {}; f.width = width; f.height = height; f.cropLeft = 2; f.cropTop = 4;
		f.pitch = (2 * (width + 2) + 15) & ~15; f.codedHeight = height + 4;
		f.p010 = true; f.pixels.resize(f.pitch * f.codedHeight * 3 / 2);
		for (unsigned pattern = 0; pattern < 8; pattern++) {
			for (auto& byte : f.pixels) { random = random * 1664525 + 1013904223; byte = random >> 24; }
			for (unsigned bits : {8u,10u}) for (int matrix : {1,2,5,6}) for (bool full : {false,true}) {
				f.bitDepth = bits; f.matrix = matrix; f.fullRange = full;
				size_t size = width * height * 4;
				std::vector<uint8_t> out(size + 34, 0xa5);
				CHECK(HevcOutput::Copy(f, HevcOutput::RGB32, out.data() + 17, size));
				for (unsigned i = 0; i < 17; i++) CHECK(out[i] == 0xa5 && out[size + 17 + i] == 0xa5);
				// Two-pixel tiles always take the scalar tail, giving a complete
				// bit-exact comparison independent of the vector load/shuffle.
				HevcFrame pair = f; pair.width = 2;
				std::vector<uint8_t> reference(2 * height * 4);
				for (int x = 0; x < width; x += 2) {
					pair.cropLeft = f.cropLeft + x;
					CHECK(HevcOutput::Copy(pair, HevcOutput::RGB32, reference.data(), reference.size()));
					for (int y = 0; y < height; y++)
						CHECK(!memcmp(out.data() + 17 + (y * width + x) * 4, reference.data() + y * 8, 8));
				}
				bool bt709 = matrix == 1 || (matrix == 2 && height > 576);
				double kr = bt709 ? .2126 : .299, kb = bt709 ? .0722 : .114;
				double scale = 1u << (bits - 8), maximum = (1u << bits) - 1;
				for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
					size_t uv = (f.codedHeight + (f.cropTop + y) / 2) * (f.pitch / 2) + f.cropLeft + (x & ~1);
					double l = (Read(f, (f.cropTop + y) * (f.pitch / 2) + f.cropLeft + x) - (full ? 0. : 16. * scale)) / (full ? maximum : 219. * scale);
					double u = (Read(f, uv) - 128. * scale) / (full ? maximum : 224. * scale);
					double v = (Read(f, uv + 1) - 128. * scale) / (full ? maximum : 224. * scale);
					int expected[] = {Clip((l + 2 * (1 - kb) * u) * 255),
						Clip((l - 2 * kb * (1 - kb) / (1 - kr - kb) * u - 2 * kr * (1 - kr) / (1 - kr - kb) * v) * 255),
						Clip((l + 2 * (1 - kr) * v) * 255), 255};
					for (unsigned c = 0; c < 4; c++) {
						unsigned error = std::abs(int(out[17 + (y * width + x) * 4 + c]) - expected[c]);
						maxError = std::max(maxError, error); CHECK(error <= 1);
					}
				}
			}
		}
	}
	// The last chroma pair ends at the allocation boundary; vector loads
	// cannot consume padding past either this scalar tail or a four-pixel row.
	for (int width : {2,4,6}) {
		HevcFrame f = {}; f.width = width; f.height = f.codedHeight = 2;
		f.pitch = width * 2; f.p010 = true; f.bitDepth = 10; f.matrix = 1;
		f.pixels.resize(width * 6, 0xff); std::vector<uint8_t> out(width * 8);
		CHECK(HevcOutput::Copy(f, HevcOutput::RGB32, out.data(), out.size()));
	}
	printf("PASS: P010 RGB widths 2..34, 8/10 bits, matrices/ranges, crop/alignment/tails and guards; scalar exact, float max error %u\n", maxError);
}

static void Queue()
{
	HevcOutput q;
	HevcFrame f = {}; f.sequence = 1; f.poc = 2; f.time = 0; f.p010 = true; f.bitDepth = 10;
	CHECK(q.Push(std::move(f), 1, false));
	f = {}; f.sequence = 1; f.poc = 0; f.time = -2000;
	CHECK(q.Push(std::move(f), 1, false));
	CHECK(q.Ready(false) && !q.Front().p010 && q.Front().time == -2000); q.Pop();
	CHECK(!q.Ready(false));
	f = {}; f.sequence = 2; f.poc = 0;
	CHECK(q.Push(std::move(f), 1, false));
	CHECK(q.Ready(false) && q.Front().p010 && q.Front().bitDepth == 10 && q.Front().time == 0); q.Pop();
	CHECK(q.Ready(true) && q.Front().sequence == 2); q.Pop();
	CHECK(!q.Ready(true));
	// Taking storage back after consumption must retain the queue's order
	// watermark, including duplicate rejection and a following epoch.
	q.Reset(); f = {}; f.sequence = 7; f.poc = 2; f.time = 1234;
	f.pixels.resize(1024, 0x5a); const uint8_t* allocation = f.pixels.data();
	CHECK(q.Push(std::move(f), 0, false) && q.Ready(false));
	std::vector<uint8_t> spare; q.Front().pixels.swap(spare); q.Pop();
	CHECK(spare.data() == allocation && spare.size() == 1024 && spare[1023] == 0x5a);
	f = {}; f.sequence = 7; f.poc = 2; CHECK(!q.Push(std::move(f), 0, false));
	f = {}; f.sequence = 7; f.poc = 3; f.pixels.swap(spare);
	CHECK(q.Push(std::move(f), 0, false) && q.Front().pixels.data() == allocation);
	q.Reset(); CHECK(!q.Ready(true));
	puts("PASS: mixed storage/depth and timestamps survive shared output ordering and epoch changes");
}

int main() { Pixels(); ColourRows(); Queue(); }
