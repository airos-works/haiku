/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RPI_HEVC_GEOMETRY_H
#define RPI_HEVC_GEOMETRY_H


/*	What the HEVC block reads and writes for a request of either phase, in
	bytes from the start of each buffer: the block is told where the
	buffers start, but not where they end. The layout is the one the Linux
	drivers (rpivid, hevc_dec) and the rpi_hevc media add-on use:
	- a picture is made of columns 128 bytes wide (128 samples of eight
	  bits, or 96 of ten), \c frame_stride apart; each holds the luma rows,
	  and the chroma rows (4:2:0, half as many) from \c chroma_offset on;
	- motion vectors take \c mv_stride bytes for each 16 rows of the
	  picture, rounded up to 64 rows;
	- phase one leaves \c pu_stride and \c coeff_stride bytes for each row
	  of coding tree blocks, \c rows of them.
	The block takes strides and addresses in units of 64 bytes.

	Header only, so that a host test can run it
	(src/tests/add-ons/kernel/drivers/rpi_hevc). Whoever includes it has
	<rpi_hevc.h>. */


struct hevc_phase1_needs {
	uint64	pu;
	uint64	coeff;
};


struct hevc_phase2_needs {
	uint64	picture;	// of the frame, and of each reference
	uint64	pu;
	uint64	coeff;
	uint64	mv;			// of the motion vector buffers, if any
};


static inline uint64
hevc_round_up(uint64 value, uint64 unit)
{
	return (value + unit - 1) / unit * unit;
}


/*!	What the PU and coefficient buffers of phase one's \a request must
	hold when its command list names \a rows rows of coding tree blocks:
	the block writes a row's data from the row's start to at most its
	stride (and reports a full buffer, RPI_HEVC_PHASE1_PU_FULL or
	_COEFF_FULL, when a row needs more). A stride of nothing would put all
	rows in one place.
*/
static status_t
get_phase1_needs(const rpi_hevc_phase1& request, uint32 rows,
	hevc_phase1_needs& needs)
{
	if (request.pu_stride == 0 || request.coeff_stride == 0)
		return B_BAD_VALUE;

	needs.pu = (uint64)rows * hevc_round_up(request.pu_stride, 64);
	needs.coeff = (uint64)rows * hevc_round_up(request.coeff_stride, 64);
	return B_OK;
}


/*!	Works out what the buffers of \a request must hold. Fails for a
	geometry the block cannot be given: no picture, a size of coding tree
	block that HEVC does not have, a number of rows that is not the
	picture's, motion vectors to keep without a buffer for them.
*/
static status_t
get_phase2_needs(const rpi_hevc_phase2& request,
	hevc_phase2_needs& needs)
{
	uint32 width = request.frame_size & 0xffff;
	uint32 height = request.frame_size >> 16;
	if (width == 0 || height == 0)
		return B_BAD_VALUE;

	uint32 log2CtbSize = (request.config >> 10) & 7;
	if (log2CtbSize < 4 || log2CtbSize > 6)
		return B_BAD_VALUE;
	uint32 ctbSize = 1u << log2CtbSize;
	// More rows would be written below the picture.
	if (request.rows != (height + ctbSize - 1) / ctbSize)
		return B_BAD_VALUE;

	// More than eight bits in either plane: 96 samples to a column. The
	// smaller number of the two makes more columns.
	bool wide = (request.config & ((1 << 8) | (1 << 9))) != 0
		|| (request.config & 0xf) > 8 || ((request.config >> 4) & 0xf) > 8;
	uint64 columns = (width + (wide ? 95 : 127)) / (wide ? 96 : 128);

	// Pictures are a whole number of the smallest coding blocks high (8
	// rows at least); allow the block to write rows in units of 16.
	uint64 rows = hevc_round_up(height, 16);
	uint64 stride = hevc_round_up(request.frame_stride, 64);
	uint64 luma = rows * 128;
	uint64 chroma = (request.chroma_offset & ~(uint64)63) + rows / 2 * 128;
	needs.picture = (columns - 1) * stride + (luma > chroma ? luma : chroma);

	needs.pu = request.rows * hevc_round_up(request.pu_stride, 64);
	needs.coeff = request.rows * hevc_round_up(request.coeff_stride, 64);

	// Each row of motion vectors is as long as a picture row (16 bytes for
	// each 16 samples): a shorter stride would not keep them apart.
	uint64 mvStride = hevc_round_up(request.mv_stride, 64);
	if ((request.mv != RPI_HEVC_NO_BUFFER
			|| request.collocated != RPI_HEVC_NO_BUFFER)
		&& mvStride < hevc_round_up(width, 64)) {
		return B_BAD_VALUE;
	}
	needs.mv = mvStride * (hevc_round_up(height, 64) >> 4);

	// The block writes the motion vectors when the configuration says to
	// keep them, at address 0 without a buffer.
	if ((request.config & (1 << 15)) != 0 && request.mv == RPI_HEVC_NO_BUFFER)
		return B_BAD_VALUE;

	return B_OK;
}


#endif	// RPI_HEVC_GEOMETRY_H
