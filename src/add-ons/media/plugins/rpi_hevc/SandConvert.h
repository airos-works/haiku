/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SAND_CONVERT_H
#define SAND_CONVERT_H


#include <stddef.h>
#include <stdint.h>


/*	Pictures as the Raspberry Pi 4's HEVC block makes them: columns 128
	bytes wide, one after the other in memory, each with all its luma rows
	and then its chroma rows (Cb and Cr in pairs). Eight bit samples are
	bytes; ten bit ones come three to a 32 bit word, the first in the low
	bits, so a column is 96 samples wide. */

struct SandPicture {
	const uint8_t*	data;
	uint32_t		columnStride;	// bytes from one column to the next
	uint32_t		chromaOffset;	// of the chroma rows in a column
	uint32_t		left;			// the part wanted, in luma samples
	uint32_t		top;
	uint32_t		width;
	uint32_t		height;
};

/*!	Eight bit, to three planes; the chroma planes have half the width and
	height, rounded up. Strides are in bytes. */
void sand8_to_i420(const SandPicture& picture, uint8_t* luma,
	uint32_t lumaStride, uint8_t* cb, uint8_t* cr, uint32_t chromaStride);

/*!	Eight bit, to a plane of luma and one of Cb and Cr in pairs. */
void sand8_to_nv12(const SandPicture& picture, uint8_t* luma,
	uint32_t lumaStride, uint8_t* chroma, uint32_t chromaStride);

/*!	Ten bit, to sixteen bit samples (the value times 64): a plane of luma
	and one of Cb and Cr in pairs. */
void sand30_to_p010(const SandPicture& picture, uint8_t* luma,
	uint32_t lumaStride, uint8_t* chroma, uint32_t chromaStride);

/*!	The half size variants: each output sample the average of the 2x2 input
	samples, the output half as wide and high as the input, rounded up.
	What a player on a 1080p screen asks of a 4K film: a quarter of the
	pixels to make, and a quarter for the scaler and tone mapper to walk. */
void sand8_to_i420_half(const SandPicture& picture, uint8_t* luma,
	uint32_t lumaStride, uint8_t* cb, uint8_t* cr, uint32_t chromaStride,
	uint8_t* scratch, size_t scratchSize);

/*!	Eight bit, to a plane of luma and one of Cb and Cr in pairs. */
void sand8_to_nv12_half(const SandPicture& picture, uint8_t* luma,
	uint32_t lumaStride, uint8_t* chroma, uint32_t chromaStride,
	uint8_t* scratch, size_t scratchSize);

/*!	Ten bit, to sixteen bit samples (the value times 64). */
void sand30_to_p010_half(const SandPicture& picture, uint8_t* luma,
	uint32_t lumaStride, uint8_t* chroma, uint32_t chromaStride,
	uint8_t* scratch, size_t scratchSize);

#endif	// SAND_CONVERT_H
