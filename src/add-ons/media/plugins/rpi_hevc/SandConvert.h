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

/*!	As sand8_to_i420() and sand30_to_p010(), half as wide and high: each
	sample the mean of a 2x2 block, an odd chroma row the mean of the
	one above it. The width must be a multiple of four, the height even. */
void sand8_to_i420_half(const SandPicture& picture, uint8_t* luma,
	uint32_t lumaStride, uint8_t* cb, uint8_t* cr, uint32_t chromaStride);
void sand30_to_p010_half(const SandPicture& picture, uint8_t* luma,
	uint32_t lumaStride, uint8_t* chroma, uint32_t chromaStride);

#endif	// SAND_CONVERT_H
