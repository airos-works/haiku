/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "SandConvert.h"

#include <algorithm>
#include <string.h>
#include <vector>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif


static const uint32_t kColumnBytes = 128;
static const uint32_t kColumnSamples30 = 96;
// Keep both the column input and the row output local while converting.
static const uint32_t kTileRows = 8;


/*!	Split a row of interleaved chroma pairs into two rows. */
static void
split_row8(const uint8_t* rows, uint32_t columnStride, uint32_t firstPair,
	uint32_t pairs, uint8_t* cb, uint8_t* cr)
{
	while (pairs > 0) {
		uint32_t column = firstPair / (kColumnBytes / 2);
		uint32_t offset = firstPair % (kColumnBytes / 2);
		uint32_t run = std::min(kColumnBytes / 2 - offset, pairs);
		const uint8_t* in = rows + (size_t)column * columnStride + 2 * offset;

		uint32_t i = 0;
#if defined(__aarch64__)
		for (; i + 16 <= run; i += 16) {
			uint8x16x2_t both = vld2q_u8(in + 2 * i);
			vst1q_u8(cb + i, both.val[0]);
			vst1q_u8(cr + i, both.val[1]);
		}
#endif
		for (; i < run; i++) {
			cb[i] = in[2 * i];
			cr[i] = in[2 * i + 1];
		}

		cb += run;
		cr += run;
		firstPair += run;
		pairs -= run;
	}
}


static void
copy_plane8(const uint8_t* in, uint32_t columnStride, uint32_t first,
	uint32_t width, uint32_t height, uint8_t* out, uint32_t outStride)
{
	for (uint32_t row = 0; row < height; row += kTileRows) {
		uint32_t rows = std::min(kTileRows, height - row);
		for (uint32_t x = 0; x < width;) {
			uint32_t position = first + x;
			uint32_t run = std::min(kColumnBytes - position % kColumnBytes,
				width - x);
			const uint8_t* column = in
				+ (size_t)(position / kColumnBytes) * columnStride
				+ position % kColumnBytes + (size_t)row * kColumnBytes;
			uint8_t* target = out + (size_t)row * outStride + x;
			for (uint32_t y = 0; y < rows; y++) {
				memcpy(target + (size_t)y * outStride,
					column + y * kColumnBytes, run);
			}
			x += run;
		}
	}
}


static void
split_plane8(const uint8_t* in, uint32_t columnStride, uint32_t first,
	uint32_t width, uint32_t height, uint8_t* cb, uint8_t* cr, uint32_t outStride)
{
	for (uint32_t row = 0; row < height; row += kTileRows) {
		uint32_t rows = std::min(kTileRows, height - row);
		for (uint32_t x = 0; x < width;) {
			uint32_t run = std::min(
				kColumnBytes / 2 - (first + x) % (kColumnBytes / 2), width - x);
			for (uint32_t y = 0; y < rows; y++) {
				split_row8(in + (size_t)(row + y) * kColumnBytes, columnStride,
					first + x, run, cb + (size_t)(row + y) * outStride + x,
					cr + (size_t)(row + y) * outStride + x);
			}
			x += run;
		}
	}
}


void
sand8_to_i420(const SandPicture& picture, uint8_t* luma, uint32_t lumaStride,
	uint8_t* cb, uint8_t* cr, uint32_t chromaStride)
{
	copy_plane8(picture.data + (size_t)picture.top * kColumnBytes,
		picture.columnStride, picture.left, picture.width, picture.height,
		luma, lumaStride);
	split_plane8(picture.data + picture.chromaOffset
			+ (size_t)(picture.top / 2) * kColumnBytes,
		picture.columnStride, picture.left / 2, (picture.width + 1) / 2,
		(picture.height + 1) / 2, cb, cr, chromaStride);
}


void
sand8_to_nv12(const SandPicture& picture, uint8_t* luma, uint32_t lumaStride,
	uint8_t* chroma, uint32_t chromaStride)
{
	copy_plane8(picture.data + (size_t)picture.top * kColumnBytes,
		picture.columnStride, picture.left, picture.width, picture.height,
		luma, lumaStride);
	copy_plane8(picture.data + picture.chromaOffset
			+ (size_t)(picture.top / 2) * kColumnBytes,
		picture.columnStride, 2 * (picture.left / 2), 2 * ((picture.width + 1) / 2),
		(picture.height + 1) / 2, chroma, chromaStride);
}


/*!	\a words words of three ten bit samples each to sixteen bit samples. */
static void
unpack30(const uint8_t* in, uint32_t words, uint16_t* out)
{
	uint32_t i = 0;
#if defined(__aarch64__)
	const uint32x4_t mask = vdupq_n_u32(0x3ff);
	for (; i + 8 <= words; i += 8, out += 24) {
		uint32x4_t low = vreinterpretq_u32_u8(vld1q_u8(in + 4 * i));
		uint32x4_t high = vreinterpretq_u32_u8(vld1q_u8(in + 4 * i + 16));
		uint16x8x3_t samples;
		samples.val[0] = vshlq_n_u16(vcombine_u16(
			vmovn_u32(vandq_u32(low, mask)),
			vmovn_u32(vandq_u32(high, mask))), 6);
		samples.val[1] = vshlq_n_u16(vcombine_u16(
			vmovn_u32(vandq_u32(vshrq_n_u32(low, 10), mask)),
			vmovn_u32(vandq_u32(vshrq_n_u32(high, 10), mask))), 6);
		samples.val[2] = vshlq_n_u16(vcombine_u16(
			vmovn_u32(vandq_u32(vshrq_n_u32(low, 20), mask)),
			vmovn_u32(vandq_u32(vshrq_n_u32(high, 20), mask))), 6);
		vst3q_u16(out, samples);
	}
#endif
	for (; i < words; i++, out += 3) {
		uint32_t word;
		memcpy(&word, in + 4 * i, 4);
		out[0] = (uint16_t)((word & 0x3ff) << 6);
		out[1] = (uint16_t)(((word >> 10) & 0x3ff) << 6);
		out[2] = (uint16_t)(((word >> 20) & 0x3ff) << 6);
	}
}


/*!	\a count samples of a ten bit row from its sample \a first on. */
static void
unpack_row30(const uint8_t* rows, uint32_t columnStride, uint32_t first,
	uint32_t count, uint16_t* out)
{
	while (count > 0) {
		uint32_t column = first / kColumnSamples30;
		uint32_t offset = first % kColumnSamples30;
		uint32_t run = std::min(kColumnSamples30 - offset, count);
		const uint8_t* in = rows + (size_t)column * columnStride;

		if (offset == 0 && run == kColumnSamples30)
			unpack30(in, kColumnSamples30 / 3, out);
		else {
			uint16_t samples[kColumnSamples30];
			unpack30(in, kColumnSamples30 / 3, samples);
			memcpy(out, samples + offset, run * sizeof(uint16_t));
		}

		out += run;
		first += run;
		count -= run;
	}
}


static void
unpack_plane30(const uint8_t* in, uint32_t columnStride, uint32_t first,
	uint32_t width, uint32_t height, uint8_t* out, uint32_t outStride)
{
	for (uint32_t row = 0; row < height; row += kTileRows) {
		uint32_t rows = std::min(kTileRows, height - row);
		for (uint32_t x = 0; x < width;) {
			uint32_t run = std::min(
				kColumnSamples30 - (first + x) % kColumnSamples30, width - x);
			for (uint32_t y = 0; y < rows; y++) {
				unpack_row30(in + (size_t)(row + y) * kColumnBytes, columnStride,
					first + x, run,
					(uint16_t*)(out + (size_t)(row + y) * outStride) + x);
			}
			x += run;
		}
	}
}


void
sand30_to_p010(const SandPicture& picture, uint8_t* luma, uint32_t lumaStride,
	uint8_t* chroma, uint32_t chromaStride)
{
	unpack_plane30(picture.data + (size_t)picture.top * kColumnBytes,
		picture.columnStride, picture.left, picture.width, picture.height,
		luma, lumaStride);
	unpack_plane30(picture.data + picture.chromaOffset
			+ (size_t)(picture.top / 2) * kColumnBytes,
		picture.columnStride, 2 * (picture.left / 2), 2 * ((picture.width + 1) / 2),
		(picture.height + 1) / 2, chroma, chromaStride);
}


//	#pragma mark - half size


/*!	Half a row: each sample the mean of a 2x2 block of \a top and \a bottom;
	\a count samples out. */
static void
halve_samples8(const uint8_t* top, const uint8_t* bottom, uint32_t count,
	uint8_t* out)
{
	uint32_t i = 0;
#if defined(__aarch64__)
	for (; i + 8 <= count; i += 8) {
		vst1_u8(out + i, vrshrn_n_u16(vaddq_u16(
			vpaddlq_u8(vld1q_u8(top + 2 * i)),
			vpaddlq_u8(vld1q_u8(bottom + 2 * i))), 2));
	}
#endif
	for (; i < count; i++) {
		out[i] = (uint8_t)(((uint32_t)top[2 * i] + top[2 * i + 1]
			+ bottom[2 * i] + bottom[2 * i + 1] + 2) >> 2);
	}
}


/*!	As halve_samples8(), for rows of Cb Cr pairs, into a Cb and a Cr row:
	\a count samples of each out. */
static void
halve_pairs8(const uint8_t* top, const uint8_t* bottom, uint32_t count,
	uint8_t* cb, uint8_t* cr)
{
	uint32_t i = 0;
#if defined(__aarch64__)
	for (; i + 8 <= count; i += 8) {
		// Cb, Cr of the even pairs, then of the odd ones
		uint8x8x4_t a = vld4_u8(top + 4 * i);
		uint8x8x4_t b = vld4_u8(bottom + 4 * i);
		vst1_u8(cb + i, vrshrn_n_u16(vaddq_u16(vaddl_u8(a.val[0], a.val[2]),
			vaddl_u8(b.val[0], b.val[2])), 2));
		vst1_u8(cr + i, vrshrn_n_u16(vaddq_u16(vaddl_u8(a.val[1], a.val[3]),
			vaddl_u8(b.val[1], b.val[3])), 2));
	}
#endif
	for (; i < count; i++) {
		cb[i] = (uint8_t)(((uint32_t)top[4 * i] + top[4 * i + 2]
			+ bottom[4 * i] + bottom[4 * i + 2] + 2) >> 2);
		cr[i] = (uint8_t)(((uint32_t)top[4 * i + 1] + top[4 * i + 3]
			+ bottom[4 * i + 1] + bottom[4 * i + 3] + 2) >> 2);
	}
}


/*!	As halve_samples8(), for sixteen bit samples. */
static void
halve_samples16(const uint16_t* top, const uint16_t* bottom, uint32_t count,
	uint16_t* out)
{
	uint32_t i = 0;
#if defined(__aarch64__)
	for (; i + 8 <= count; i += 8) {
		uint32x4_t low = vaddq_u32(vpaddlq_u16(vld1q_u16(top + 2 * i)),
			vpaddlq_u16(vld1q_u16(bottom + 2 * i)));
		uint32x4_t high = vaddq_u32(vpaddlq_u16(vld1q_u16(top + 2 * i + 8)),
			vpaddlq_u16(vld1q_u16(bottom + 2 * i + 8)));
		vst1q_u16(out + i, vcombine_u16(vrshrn_n_u32(low, 2),
			vrshrn_n_u32(high, 2)));
	}
#endif
	for (; i < count; i++) {
		out[i] = (uint16_t)(((uint32_t)top[2 * i] + top[2 * i + 1]
			+ bottom[2 * i] + bottom[2 * i + 1] + 2) >> 2);
	}
}


/*!	As halve_samples16(), for rows of Cb Cr pairs: \a count pairs out. */
static void
halve_pairs16(const uint16_t* top, const uint16_t* bottom, uint32_t count,
	uint16_t* out)
{
	uint32_t i = 0;
#if defined(__aarch64__)
	for (; i + 4 <= count; i += 4) {
		uint16x8x2_t a = vld2q_u16(top + 4 * i);
		uint16x8x2_t b = vld2q_u16(bottom + 4 * i);
		uint16x4x2_t pairs;
		pairs.val[0] = vrshrn_n_u32(vaddq_u32(vpaddlq_u16(a.val[0]),
			vpaddlq_u16(b.val[0])), 2);
		pairs.val[1] = vrshrn_n_u32(vaddq_u32(vpaddlq_u16(a.val[1]),
			vpaddlq_u16(b.val[1])), 2);
		vst2_u16(out + 2 * i, pairs);
	}
#endif
	for (; i < count; i++) {
		for (uint32_t c = 0; c < 2; c++) {
			out[2 * i + c] = (uint16_t)(((uint32_t)top[4 * i + c]
				+ top[4 * i + 2 + c] + bottom[4 * i + c]
				+ bottom[4 * i + 2 + c] + 2) >> 2);
		}
	}
}


/*	Each band of kTileRows rows is first made into plain rows, as the full
	size conversion makes them (which keeps the columns' reads local), and
	then halved from there while it is still in the cache. */


void
sand8_to_i420_half(const SandPicture& picture, uint8_t* luma,
	uint32_t lumaStride, uint8_t* cb, uint8_t* cr, uint32_t chromaStride)
{
	uint32_t width = picture.width;
	static thread_local std::vector<uint8_t> band;
	band.resize((size_t)width * kTileRows);

	const uint8_t* in = picture.data + (size_t)picture.top * kColumnBytes;
	for (uint32_t row = 0; row < picture.height; row += kTileRows) {
		uint32_t rows = std::min(kTileRows, picture.height - row);
		copy_plane8(in + (size_t)row * kColumnBytes, picture.columnStride,
			picture.left, width, rows, band.data(), width);
		for (uint32_t y = 0; y + 1 < rows; y += 2) {
			halve_samples8(band.data() + (size_t)y * width,
				band.data() + (size_t)(y + 1) * width, width / 2,
				luma + (size_t)((row + y) / 2) * lumaStride);
		}
	}

	// rows of width / 2 pairs, half as many as of luma
	in = picture.data + picture.chromaOffset
		+ (size_t)(picture.top / 2) * kColumnBytes;
	uint32_t chromaHeight = picture.height / 2;
	for (uint32_t row = 0; row < chromaHeight; row += kTileRows) {
		uint32_t rows = std::min(kTileRows, chromaHeight - row);
		copy_plane8(in + (size_t)row * kColumnBytes, picture.columnStride,
			2 * (picture.left / 2), width, rows, band.data(), width);
		for (uint32_t y = 0; y + 1 < rows; y += 2) {
			size_t target = (size_t)((row + y) / 2) * chromaStride;
			halve_pairs8(band.data() + (size_t)y * width,
				band.data() + (size_t)(y + 1) * width, width / 4,
				cb + target, cr + target);
		}
	}
}


void
sand30_to_p010_half(const SandPicture& picture, uint8_t* luma,
	uint32_t lumaStride, uint8_t* chroma, uint32_t chromaStride)
{
	uint32_t width = picture.width;
	static thread_local std::vector<uint16_t> band;
	band.resize((size_t)width * kTileRows);
	uint16_t* rows16 = band.data();

	const uint8_t* in = picture.data + (size_t)picture.top * kColumnBytes;
	for (uint32_t row = 0; row < picture.height; row += kTileRows) {
		uint32_t rows = std::min(kTileRows, picture.height - row);
		unpack_plane30(in + (size_t)row * kColumnBytes, picture.columnStride,
			picture.left, width, rows, (uint8_t*)rows16, width * 2);
		for (uint32_t y = 0; y + 1 < rows; y += 2) {
			halve_samples16(rows16 + (size_t)y * width,
				rows16 + (size_t)(y + 1) * width, width / 2,
				(uint16_t*)(luma + (size_t)((row + y) / 2) * lumaStride));
		}
	}

	// rows of width / 2 pairs, half as many as of luma
	in = picture.data + picture.chromaOffset
		+ (size_t)(picture.top / 2) * kColumnBytes;
	uint32_t chromaHeight = picture.height / 2;
	for (uint32_t row = 0; row < chromaHeight; row += kTileRows) {
		uint32_t rows = std::min(kTileRows, chromaHeight - row);
		unpack_plane30(in + (size_t)row * kColumnBytes, picture.columnStride,
			2 * (picture.left / 2), width, rows, (uint8_t*)rows16, width * 2);
		for (uint32_t y = 0; y + 1 < rows; y += 2) {
			halve_pairs16(rows16 + (size_t)y * width,
				rows16 + (size_t)(y + 1) * width, width / 4,
				(uint16_t*)(chroma + (size_t)((row + y) / 2) * chromaStride));
		}
	}
}
