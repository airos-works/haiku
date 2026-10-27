/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "SandConvert.h"

#include <algorithm>
#include <string.h>

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


/*!	Average of the 2x2 samples; \a outWidth outputs from \a inWidth inputs,
	the odd sample at the edge taken twice. Rows may be read up to 32
	samples past \a inWidth: callers pad their row buffers. */
static void
box_row_u8(const uint8_t* r0, const uint8_t* r1, uint8_t* out,
	uint32_t outWidth, uint32_t inWidth)
{
	uint32_t x = 0;
#if defined(__aarch64__)
	for (; x + 8 <= outWidth; x += 8) {
		uint16x8_t p = vpaddlq_u8(vld1q_u8(r0 + 2 * x));
		uint16x8_t q = vpaddlq_u8(vld1q_u8(r1 + 2 * x));
		uint16x8_t s = vshrq_n_u16(
			vaddq_u16(vaddq_u16(p, q), vdupq_n_u16(2)), 2);
		vst1_u8(out + x, vmovn_u16(s));
	}
#endif
	for (; x < outWidth; x++) {
		uint32_t a = 2 * x;
		uint32_t b = a + 1 < inWidth ? a + 1 : a;
		out[x] = (r0[a] + r0[b] + r1[a] + r1[b] + 2) >> 2;
	}
}


static void
box_row_u16(const uint16_t* r0, const uint16_t* r1, uint16_t* out,
	uint32_t outWidth, uint32_t inWidth)
{
	uint32_t x = 0;
#if defined(__aarch64__)
	for (; x + 4 <= outWidth; x += 4) {
		uint32x4_t p = vpaddlq_u16(vld1q_u16(r0 + 2 * x));
		uint32x4_t q = vpaddlq_u16(vld1q_u16(r1 + 2 * x));
		uint32x4_t s = vshrq_n_u32(
			vaddq_u32(vaddq_u32(p, q), vdupq_n_u32(2)), 2);
		vst1_u16(out + x, vmovn_u32(s));
	}
#endif
	for (; x < outWidth; x++) {
		uint32_t a = 2 * x;
		uint32_t b = a + 1 < inWidth ? a + 1 : a;
		out[x] = (uint16_t)(((uint32_t)r0[a] + r0[b] + r1[a] + r1[b] + 2) >> 2);
	}
}


/*!	The 2x2 average of interleaved pairs (Cb Cr Cb Cr ...), as many output
	pairs as inputs over two, rounded up. */
static void
box_pairs_u8(const uint8_t* r0, const uint8_t* r1, uint8_t* out,
	uint32_t outPairs, uint32_t inPairs)
{
	uint32_t x = 0;
#if defined(__aarch64__)
	for (; x + 8 <= outPairs; x += 8) {
		uint8x16x2_t a = vld2q_u8(r0 + 4 * x);
		uint8x16x2_t b = vld2q_u8(r1 + 4 * x);
		uint16x8_t cb = vshrq_n_u16(vaddq_u16(vaddq_u16(
			vpaddlq_u8(a.val[0]), vpaddlq_u8(b.val[0])), vdupq_n_u16(2)), 2);
		uint16x8_t cr = vshrq_n_u16(vaddq_u16(vaddq_u16(
			vpaddlq_u8(a.val[1]), vpaddlq_u8(b.val[1])), vdupq_n_u16(2)), 2);
		uint8x8x2_t packed;
		packed.val[0] = vmovn_u16(cb);
		packed.val[1] = vmovn_u16(cr);
		vst2_u8(out + 2 * x, packed);
	}
#endif
	for (; x < outPairs; x++) {
		uint32_t a = 4 * x;
		uint32_t b = 2 * x + 1 < inPairs ? a + 2 : a;
		out[2 * x] = (uint8_t)((r0[a] + r0[b] + r1[a] + r1[b] + 2) >> 2);
		a++;
		b++;
		out[2 * x + 1] = (uint8_t)((r0[a] + r0[b] + r1[a] + r1[b] + 2) >> 2);
	}
}


static void
box_pairs_u16(const uint16_t* r0, const uint16_t* r1, uint16_t* out,
	uint32_t outPairs, uint32_t inPairs)
{
	uint32_t x = 0;
#if defined(__aarch64__)
	for (; x + 4 <= outPairs; x += 4) {
		uint16x8x2_t a = vld2q_u16(r0 + 4 * x);
		uint16x8x2_t b = vld2q_u16(r1 + 4 * x);
		uint32x4_t cb = vshrq_n_u32(vaddq_u32(vaddq_u32(
			vpaddlq_u16(a.val[0]), vpaddlq_u16(b.val[0])),
			vdupq_n_u32(2)), 2);
		uint32x4_t cr = vshrq_n_u32(vaddq_u32(vaddq_u32(
			vpaddlq_u16(a.val[1]), vpaddlq_u16(b.val[1])),
			vdupq_n_u32(2)), 2);
		uint16x4x2_t packed;
		packed.val[0] = vmovn_u32(cb);
		packed.val[1] = vmovn_u32(cr);
		vst2_u16(out + 2 * x, packed);
	}
#endif
	for (; x < outPairs; x++) {
		uint32_t a = 4 * x;
		uint32_t b = 2 * x + 1 < inPairs ? a + 2 : a;
		out[2 * x] = (uint16_t)((r0[a] + r0[b] + r1[a] + r1[b] + 2) >> 2);
		a++;
		b++;
		out[2 * x + 1] = (uint16_t)((r0[a] + r0[b] + r1[a] + r1[b] + 2) >> 2);
	}
}


/*!	Duplicate the edge sample over 32 bytes, so the vectorized box reads
	past the width see the edge twice, as the scalar path takes it. */
static void
pad_row_u8(uint8_t* row, uint32_t width)
{
	memset(row + width, row[width - 1], 32);
}


static void
pad_row_u16(uint16_t* row, uint32_t width)
{
	for (uint32_t i = 0; i < 32; i++)
		row[width + i] = row[width - 1];
}


static void
half_luma8(const SandPicture& picture, uint8_t* out, uint32_t outStride,
	uint8_t* rowA, uint8_t* rowB)
{
	uint32_t outHeight = (picture.height + 1) / 2;
	const uint8_t* base = picture.data + (size_t)picture.top * kColumnBytes;
	for (uint32_t j = 0; j < outHeight; j++) {
		copy_plane8(base + (size_t)(2 * j) * kColumnBytes,
			picture.columnStride, picture.left, picture.width, 1, rowA,
			picture.width);
		if (2 * j + 1 < picture.height) {
			copy_plane8(base + (size_t)(2 * j + 1) * kColumnBytes,
				picture.columnStride, picture.left, picture.width, 1, rowB,
				picture.width);
		} else
			memcpy(rowB, rowA, picture.width);
		pad_row_u8(rowA, picture.width);
		pad_row_u8(rowB, picture.width);
		box_row_u8(rowA, rowB, out + (size_t)j * outStride,
			(picture.width + 1) / 2, picture.width);
	}
}


static void
half_chroma8_split(const SandPicture& picture, uint8_t* cb, uint8_t* cr,
	uint32_t outStride, uint8_t* cbA, uint8_t* cbB, uint8_t* crA, uint8_t* crB)
{
	uint32_t pairs = (picture.width + 1) / 2;
	uint32_t inRows = (picture.height + 1) / 2;
	uint32_t outRows = (pairs + 1) / 2;
	const uint8_t* base = picture.data + picture.chromaOffset
		+ (size_t)(picture.top / 2) * kColumnBytes;
	for (uint32_t j = 0; j < outRows; j++) {
		uint32_t row0 = std::min(2 * j, inRows - 1);
		uint32_t row1 = std::min(2 * j + 1, inRows - 1);
		split_row8(base + (size_t)row0 * kColumnBytes, picture.columnStride,
			picture.left / 2, pairs, cbA, crA);
		split_row8(base + (size_t)row1 * kColumnBytes, picture.columnStride,
			picture.left / 2, pairs, cbB, crB);
		pad_row_u8(cbA, pairs);
		pad_row_u8(cbB, pairs);
		pad_row_u8(crA, pairs);
		pad_row_u8(crB, pairs);
		box_row_u8(cbA, cbB, cb + (size_t)j * outStride, (pairs + 1) / 2, pairs);
		box_row_u8(crA, crB, cr + (size_t)j * outStride, (pairs + 1) / 2, pairs);
	}
}


static void
half_chroma8_pairs(const SandPicture& picture, uint8_t* out, uint32_t outStride,
	uint8_t* rowA, uint8_t* rowB)
{
	uint32_t samples = 2 * ((picture.width + 1) / 2);
	uint32_t inRows = (picture.height + 1) / 2;
	uint32_t outRows = (inRows + 1) / 2;
	const uint8_t* base = picture.data + picture.chromaOffset
		+ (size_t)(picture.top / 2) * kColumnBytes;
	for (uint32_t j = 0; j < outRows; j++) {
		uint32_t row0 = std::min(2 * j, inRows - 1);
		uint32_t row1 = std::min(2 * j + 1, inRows - 1);
		copy_plane8(base + (size_t)row0 * kColumnBytes, picture.columnStride,
			2 * (picture.left / 2), samples, 1, rowA, samples);
		copy_plane8(base + (size_t)row1 * kColumnBytes, picture.columnStride,
			2 * (picture.left / 2), samples, 1, rowB, samples);
		pad_row_u8(rowA, samples);
		pad_row_u8(rowB, samples);
		box_pairs_u8(rowA, rowB, out + (size_t)j * outStride,
			(picture.width + 3) / 4, samples / 2);
	}
}


void
sand8_to_i420_half(const SandPicture& picture, uint8_t* luma,
	uint32_t lumaStride, uint8_t* cb, uint8_t* cr, uint32_t chromaStride,
	uint8_t* scratch, size_t scratchSize)
{
	uint32_t rows = picture.width + 64;
	uint32_t chromaRows = (picture.width + 1) / 2 + 64;
	if (scratchSize < 2 * rows + 4 * chromaRows)
		return;
	half_luma8(picture, luma, lumaStride, scratch, scratch + rows);
	half_chroma8_split(picture, cb, cr, chromaStride, scratch + 2 * rows,
		scratch + 2 * rows + chromaRows, scratch + 2 * rows + 2 * chromaRows,
		scratch + 2 * rows + 3 * chromaRows);
}


void
sand8_to_nv12_half(const SandPicture& picture, uint8_t* luma,
	uint32_t lumaStride, uint8_t* chroma, uint32_t chromaStride,
	uint8_t* scratch, size_t scratchSize)
{
	uint32_t rows = picture.width + 64;
	uint32_t chromaRowBytes = 2 * ((picture.width + 1) / 2) + 64;
	if (scratchSize < 2 * rows + 2 * chromaRowBytes)
		return;
	half_luma8(picture, luma, lumaStride, scratch, scratch + rows);
	half_chroma8_pairs(picture, chroma, chromaStride, scratch + 2 * rows,
		scratch + 2 * rows + chromaRowBytes);
}


static void
half_luma30(const SandPicture& picture, uint8_t* out, uint32_t outStride,
	uint16_t* rowA, uint16_t* rowB)
{
	uint32_t outHeight = (picture.height + 1) / 2;
	const uint8_t* base = picture.data + (size_t)picture.top * kColumnBytes;
	for (uint32_t j = 0; j < outHeight; j++) {
		unpack_row30(base + (size_t)(2 * j) * kColumnBytes,
			picture.columnStride, picture.left, picture.width, rowA);
		if (2 * j + 1 < picture.height) {
			unpack_row30(base + (size_t)(2 * j + 1) * kColumnBytes,
				picture.columnStride, picture.left, picture.width, rowB);
		} else
			memcpy(rowB, rowA, picture.width * sizeof(uint16_t));
		pad_row_u16(rowA, picture.width);
		pad_row_u16(rowB, picture.width);
		box_row_u16(rowA, rowB, (uint16_t*)(out + (size_t)j * outStride),
			(picture.width + 1) / 2, picture.width);
	}
}


/*!	The fused fast path: each output row unpacks its two input rows one
	column run at a time into L1-sized tiles and boxes them straight into
	the output, no full-width row buffers. Runs then never leave the
	cache. Requires \a first on a 4 sample grid, so the 2x2 boxes never
	cross a run; odd crops take the row buffer path above. */
static void
unpack_box_plane30(const uint8_t* in, uint32_t columnStride, uint32_t first,
	uint32_t width, uint32_t height, uint8_t* out, uint32_t outStride)
{
	uint32_t outHeight = (height + 1) / 2;
	for (uint32_t j = 0; j < outHeight; j++) {
		uint32_t row1 = std::min(2 * j + 1, height - 1);
		const uint8_t* in0 = in + (size_t)(2 * j) * kColumnBytes;
		const uint8_t* in1 = in + (size_t)row1 * kColumnBytes;
		uint16_t* outRow = (uint16_t*)(out + (size_t)j * outStride);
		for (uint32_t x = 0; x < width;) {
			uint32_t position = first + x;
			uint32_t run = std::min(
				kColumnSamples30 - position % kColumnSamples30, width - x);
			uint16_t t0[kColumnSamples30 + 32];
			uint16_t t1[kColumnSamples30 + 32];
			unpack_row30(in0, columnStride, position, run, t0);
			unpack_row30(in1, columnStride, position, run, t1);
			pad_row_u16(t0, run);
			pad_row_u16(t1, run);
			box_row_u16(t0, t1, outRow + x / 2, (run + 1) / 2, run);
			x += run;
		}
	}
}


/*!	Pair-interleaved variant of unpack_box_plane30: \a width is in
	samples (twice the pair count). */
static void
unpack_box_pairs30(const uint8_t* in, uint32_t columnStride, uint32_t first,
	uint32_t width, uint32_t height, uint8_t* out, uint32_t outStride)
{
	uint32_t outHeight = (height + 1) / 2;
	for (uint32_t j = 0; j < outHeight; j++) {
		uint32_t row1 = std::min(2 * j + 1, height - 1);
		const uint8_t* in0 = in + (size_t)(2 * j) * kColumnBytes;
		const uint8_t* in1 = in + (size_t)row1 * kColumnBytes;
		uint16_t* outRow = (uint16_t*)(out + (size_t)j * outStride);
		for (uint32_t x = 0; x < width;) {
			uint32_t position = first + x;
			uint32_t run = std::min(
				kColumnSamples30 - position % kColumnSamples30, width - x);
			uint16_t t0[kColumnSamples30 + 32];
			uint16_t t1[kColumnSamples30 + 32];
			unpack_row30(in0, columnStride, position, run, t0);
			unpack_row30(in1, columnStride, position, run, t1);
			pad_row_u16(t0, run);
			pad_row_u16(t1, run);
			box_pairs_u16(t0, t1, outRow + x / 2, (run + 2) / 4, run / 2);
			x += run;
		}
	}
}


void
sand30_to_p010_half(const SandPicture& picture, uint8_t* luma,
	uint32_t lumaStride, uint8_t* chroma, uint32_t chromaStride,
	uint8_t* scratch, size_t scratchSize)
{
	if (picture.left % 4 == 0) {
		// the 2x2 boxes never cross a column run: no row buffers at all
		unpack_box_plane30(picture.data + (size_t)picture.top * kColumnBytes,
			picture.columnStride, picture.left, picture.width,
			picture.height, luma, lumaStride);
		unpack_box_pairs30(picture.data + picture.chromaOffset
				+ (size_t)(picture.top / 2) * kColumnBytes,
			picture.columnStride, 2 * (picture.left / 2),
			2 * ((picture.width + 1) / 2), (picture.height + 1) / 2, chroma,
			chromaStride);
		return;
	}

	uint32_t lumaBytes = (picture.width + 32) * 2;
	uint32_t samples = 2 * ((picture.width + 1) / 2);
	uint32_t chromaBytes = (samples + 32) * 2;
	if (scratchSize < 2 * lumaBytes + 2 * chromaBytes)
		return;

	half_luma30(picture, luma, lumaStride, (uint16_t*)scratch,
		(uint16_t*)(scratch + lumaBytes));

	uint32_t inRows = (picture.height + 1) / 2;
	uint32_t outRows = (inRows + 1) / 2;
	const uint8_t* base = picture.data + picture.chromaOffset
		+ (size_t)(picture.top / 2) * kColumnBytes;
	uint16_t* rowA = (uint16_t*)(scratch + 2 * lumaBytes);
	uint16_t* rowB = (uint16_t*)(scratch + 2 * lumaBytes + chromaBytes);
	for (uint32_t j = 0; j < outRows; j++) {
		uint32_t row0 = std::min(2 * j, inRows - 1);
		uint32_t row1 = std::min(2 * j + 1, inRows - 1);
		unpack_row30(base + (size_t)row0 * kColumnBytes, picture.columnStride,
			2 * (picture.left / 2), samples, rowA);
		unpack_row30(base + (size_t)row1 * kColumnBytes, picture.columnStride,
			2 * (picture.left / 2), samples, rowB);
		pad_row_u16(rowA, samples);
		pad_row_u16(rowB, samples);
		box_pairs_u16(rowA, rowB, (uint16_t*)(chroma + (size_t)j * chromaStride),
			(picture.width + 3) / 4, samples / 2);
	}
}
