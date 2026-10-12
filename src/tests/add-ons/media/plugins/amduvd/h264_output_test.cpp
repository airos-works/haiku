/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "H264Output.h"
#include "H264Packet.h"
#include <assert.h>
#include <cmath>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <utility>

static void Packet()
{
	const uint8_t config[] = {1,100,0,31,255,225,0,2,0x67,0x80,1,0,2,0x68,0x80};
	H264Packet packet; std::vector<uint8_t> out;
	for (size_t n = 1; n < sizeof(config); n++) assert(!packet.Configure(config, n));
	assert(!packet.Configure(NULL, 12));
	for (unsigned length : {1,2,4}) {
		std::vector<uint8_t> c(config, config + sizeof(config)); c[4] = 0xfc | (length - 1);
		assert(packet.Configure(c.data(), c.size()));
		std::vector<uint8_t> input(length, 0); input.back() = 2;
		input.push_back(0x65); input.push_back(0x80);
		assert(packet.Convert(input.data(), input.size(), true, out));
		const uint8_t expected[] = {0,0,0,1,0x67,0x80,0,0,0,1,0x68,0x80,0,0,0,1,0x65,0x80};
		assert(out.size() == sizeof(expected) && memcmp(out.data(), expected, sizeof(expected)) == 0);
		assert(packet.Convert(input.data(), input.size(), false, out) && out.size() == 6);
		for (size_t n = 0; n < input.size(); n++) assert(!packet.Convert(input.data(), n, false, out));
		input.push_back(0); assert(!packet.Convert(input.data(), input.size(), false, out));
	}
	uint32_t random = 71;
	for (unsigned i = 0; i < 25000; i++) {
		std::vector<uint8_t> data(i % 512 + 1);
		for (auto& byte : data) { random = random * 1664525 + 1013904223; byte = random >> 24; }
		packet.Configure(data.data(), data.size());
		packet.Convert(data.data(), data.size(), true, out);
	}
}

static H264Frame Frame(int seq, int poc)
{
	H264Frame f = {}; f.sequence = seq; f.poc = poc; f.time = poc * 1000 - 5000;
	return f;
}

static void Queue()
{
	H264Output q;
	for (int seq = 1; seq <= 64; seq++) {
		for (int poc : {0,8,4,2,6,16,12,10,14}) {
			assert(q.Push(Frame(seq, poc), 3, false));
			while (q.Ready(false)) {
				const auto& f = q.Front(); assert(f.time == f.poc * 1000 - 5000); q.Pop();
			}
		}
	}
	int count = 0;
	while (q.Ready(true)) { count++; q.Pop(); }
	assert(count == 3);
	assert(!q.Push(Frame(64, 2), 3, false));
	q.Reset();
	assert(q.Push(Frame(1,0), 16, false));
	assert(!q.Push(Frame(1,0), 16, false));
	assert(q.Push(Frame(2,0), 16, true));
	assert(q.Front().sequence == 2 && !q.Ready(false));
	q.Reset();
	for (int i = 0; i < 17; i++) assert(q.Push(Frame(1,i), 16, false));
	assert(q.Ready(false)); assert(!q.Push(Frame(1,17), 16, false));
	assert(!q.Push(Frame(1,18), 17, false));
}

static void Pixels()
{
	H264Frame f = {}; f.pitch = 16; f.codedHeight = 16; f.width = 8; f.height = 6;
	f.cropLeft = 2; f.cropTop = 4; f.matrix = 6; f.pixels.resize(384);
	for (size_t i = 0; i < f.pixels.size(); i++) f.pixels[i] = i * 31;
	for (auto format : {H264Output::NV12, H264Output::I420, H264Output::YCbCr422}) {
		size_t bytes = H264Output::Bytes(f.width, f.height, format);
		std::vector<uint8_t> output(bytes + 32, 0x7d);
		assert(H264Output::Copy(f, format, output.data() + 16));
		for (int i = 0; i < 16; i++) assert(output[i] == 0x7d && output[16 + bytes + i] == 0x7d);
		const uint8_t* p = output.data() + 16;
		for (int y = 0; y < f.height; y++) for (int x = 0; x < f.width; x++) {
			uint8_t yy = f.pixels[(y + 4) * 16 + x + 2];
			uint8_t uv = f.pixels[256 + (y / 2 + 2) * 16 + x + 2];
			if (format == H264Output::YCbCr422) {
				assert(p[y * 16 + x * 2] == yy); assert(p[y * 16 + x * 2 + 1] == uv);
			} else {
				assert(p[y * 8 + x] == yy);
				if (format == H264Output::NV12) assert(p[48 + y / 2 * 8 + x] == uv);
				else assert(p[48 + (x % 2) * 12 + y / 2 * 4 + x / 2] == uv);
			}
		}
	}
	std::vector<uint8_t> rgb(8 * 6 * 4);
	for (bool full : {false, true}) {
		f.fullRange = full;
		for (bool white : {false, true}) {
			memset(f.pixels.data(), white ? (full ? 255 : 235) : (full ? 0 : 16), 256);
			memset(f.pixels.data() + 256, 128, 128);
			assert(H264Output::Copy(f, H264Output::RGB32, rgb.data()));
			for (size_t i = 0; i < rgb.size(); i++) assert(rgb[i] == ((i % 4 == 3 || white) ? 255 : 0));
		}
	}
	f.matrix = 9; assert(!H264Output::Copy(f, H264Output::RGB32, rgb.data()));
	f.cropTop = 15; assert(!H264Output::Copy(f, H264Output::NV12, rgb.data()));
	f.cropTop = 0; f.cropLeft = INT_MAX - 1;
	assert(!H264Output::Copy(f, H264Output::NV12, rgb.data()));
}

static void ColourRows()
{
	unsigned maxError = 0;
	uint32_t random = 101;
	for (int width : {2,4,6,8,10,14,16,18,30,32,34}) {
		H264Frame f = {}; f.width = width; f.height = 6; f.cropLeft = 2; f.cropTop = 4;
		f.pitch = (width + 17) & ~15; f.codedHeight = 16;
		f.pixels.resize(f.pitch * f.codedHeight * 3 / 2);
		for (unsigned pattern = 0; pattern < 64; pattern++) {
			for (auto& byte : f.pixels) { random = random * 1664525 + 1013904223; byte = random >> 24; }
			for (int matrix : {1,2,5,6}) for (bool full : {false, true}) {
				f.matrix = matrix; f.fullRange = full;
				size_t size = width * f.height * 4;
				std::vector<uint8_t> out(size + 34, 0xa5);
				assert(H264Output::Copy(f, H264Output::RGB32, out.data() + 17));
				for (unsigned i = 0; i < 17; i++) assert(out[i] == 0xa5 && out[size + 17 + i] == 0xa5);
				// Tiling into two-pixel crops exercises the scalar tail: the
				// vector path must preserve every original fixed-point result.
				H264Frame pair = f; pair.width = 2;
				std::vector<uint8_t> reference(2 * f.height * 4);
				for (int x = 0; x < width; x += 2) {
					pair.cropLeft = f.cropLeft + x;
					assert(H264Output::Copy(pair, H264Output::RGB32, reference.data()));
					for (int y = 0; y < f.height; y++)
						assert(!memcmp(out.data() + 17 + (y * width + x) * 4, reference.data() + y * 8, 8));
				}
				// Independent floating-point YUV equations check the colour
				// math as well as equivalence between the two code paths.
				double kr = matrix == 1 ? .2126 : .299, kb = matrix == 1 ? .0722 : .114;
				auto clip = [](double v) { return v <= 0 ? 0 : v >= 255 ? 255 : int(std::floor(v + .5)); };
				for (int y = 0; y < f.height; y++) for (int x = 0; x < width; x++) {
					size_t uv = f.pitch * f.codedHeight + (f.cropTop + y) / 2 * f.pitch + f.cropLeft + (x & ~1);
					double l = (f.pixels[(f.cropTop + y) * f.pitch + f.cropLeft + x] - (full ? 0. : 16.)) / (full ? 255. : 219.);
					double u = (f.pixels[uv] - 128.) / (full ? 255. : 224.);
					double v = (f.pixels[uv + 1] - 128.) / (full ? 255. : 224.);
					int expected[] = {clip((l + 2 * (1 - kb) * u) * 255),
						clip((l - 2 * kb * (1 - kb) / (1 - kr - kb) * u - 2 * kr * (1 - kr) / (1 - kr - kb) * v) * 255),
						clip((l + 2 * (1 - kr) * v) * 255), 255};
					for (unsigned c = 0; c < 4; c++) {
						unsigned error = std::abs(int(out[17 + (y * width + x) * 4 + c]) - expected[c]);
						maxError = std::max(maxError, error); assert(error <= 1);
					}
				}
			}
		}
	}
	// No row padding: ASan checks the last two-byte chroma pair's allocation.
	H264Frame f = {}; f.width = f.height = f.pitch = f.codedHeight = 2; f.matrix = 1;
	f.pixels = {0,16,235,255,128,240}; uint8_t out[16];
	assert(H264Output::Copy(f, H264Output::RGB32, out));
	printf("PASS: RGB widths 2..34, crop/alignment/tails and guards; scalar tiles exact, floating-point max error %u\n", maxError);
}
int main()
{
	Packet(); Queue(); Pixels(); ColourRows();
	puts("PASS: bounded AVCC packets, 25000 malformed inputs, output epochs/order, crop/planes/packed YUV/RGB and guards");
}
