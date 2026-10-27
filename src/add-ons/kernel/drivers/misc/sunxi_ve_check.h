/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SUNXI_VE_CHECK_H
#define SUNXI_VE_CHECK_H


/*	What sunxi_ve lets a slice's register writes do: the engine reads and
	writes memory at the bus addresses the program puts in its registers and
	SRAM, so every one of them has to lie in the program's own buffers, with
	room for what the engine does there at the picture size the same writes
	set. Only the registers of the H.264 and HEVC engines that the sunxi_cedar
	add-on uses may be written, the engine may only be started by the driver,
	and its scale-down output stays off.

	Header only, so that the add-on's host test can run the same check. */


#include <sunxi_ve.h>


struct ve_range {
	uint32	start;		// bus addresses
	uint32	end;		// exclusive
};


struct ve_check {
	const ve_range*	ranges;
	uint32			rangeCount;
	const char*		problem;
	uint32			index;		// of the op with the problem
};


namespace ve_check_private {


enum {
	kModeH264		= 1,
	kModeHevc		= 4,
	kModeNone		= 7,
	kSramLimit		= 0x1000,
	kH264Frames		= 0x400,	// SRAM bytes: 18 entries of 8 words
	kH264FrameSlots	= 18,
	kHevcFrames		= 0x400,	// 17 entries of 8 words
	kHevcFrameSlots	= 17
};


struct geometry {
	int			mode;
	int32		outputSlot;
	uint32		width;
	uint32		height;			// coded, 16 aligned
	uint32		stride;
	uint32		chromaHalf;
	uint32		mvcolSize;		// a picture's motion data
	// ten bits: the low two bits after each picture
	uint32		twoBitOffset;
	uint32		twoBitStride;
	bool		valid;
};


static inline uint32
align(uint32 value, uint32 alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}


/*!	The range \a start lies in, or NULL. */
static inline const ve_range*
range_of(const ve_check& check, uint64 start)
{
	for (uint32 i = 0; i < check.rangeCount; i++) {
		if (start >= check.ranges[i].start && start < check.ranges[i].end)
			return &check.ranges[i];
	}
	return NULL;
}


static inline bool
fits(const ve_check& check, uint64 start, uint64 size)
{
	const ve_range* range = range_of(check, start);
	return range != NULL && start + size <= range->end;
}


static inline bool
fail(ve_check& check, uint32 index, const char* problem)
{
	check.problem = problem;
	check.index = index;
	return false;
}


static bool
writable(uint16 reg)
{
	switch (reg) {
		// top: mode, line buffers, output format
		case 0x000: case 0x050: case 0x054: case 0x058: case 0x0c4:
		case 0x0c8: case 0x0e8: case 0x0ec:
			return true;
	}
	// H.264: SPS to SDROT, output index, extra buffers, SRAM port
	if ((reg >= 0x200 && reg <= 0x240) || reg == 0x24c || reg == 0x250
		|| reg == 0x254 || reg == 0x2e0 || reg == 0x2e4) {
		return true;
	}
	// HEVC: headers to tiles, scaling DC, the two-bit planes, SRAM port
	// (not the secondary output: on the A733 it stops the engine)
	if ((reg >= 0x500 && reg <= 0x54c) || (reg >= 0x55c && reg <= 0x56c)
		|| reg == 0x578 || reg == 0x57c || reg == 0x584 || reg == 0x58c
		|| reg == 0x5e0 || reg == 0x5e4) {
		return true;
	}
	return false;
}


/*!	The picture size and layout the writes set (the last value of each). */
static geometry
find_geometry(const sunxi_ve_op* ops, uint32 count)
{
	geometry result = {};
	result.outputSlot = -1;
	uint32 h264Sps = 0, hevcSize = 0, hevcSps = 0;
	bool haveH264Sps = false, haveHevcSize = false, haveHevcSps = false;
	bool haveStride = false;
	for (uint32 i = 0; i < count; i++) {
		if (ops[i].type != SUNXI_VE_OP_WRITE)
			continue;
		uint32 value = ops[i].value;
		switch (ops[i].reg) {
			case 0x000:
				result.mode = value & 0xf;
				break;
			case 0x0c4:
				result.chromaHalf = value & 0x0fffffff;
				break;
			case 0x24c:
			case 0x55c:
				result.outputSlot = value;
				break;
			case 0x584:
				result.twoBitOffset = value;
				break;
			case 0x58c:
				result.twoBitStride = value & 0x7ff;
				break;
			case 0x0c8:
				result.stride = value & 0xffff;
				haveStride = true;
				break;
			case 0x200:
				h264Sps = value;
				haveH264Sps = true;
				break;
			case 0x504:
				hevcSps = value;
				haveHevcSps = true;
				break;
			case 0x508:
				hevcSize = value;
				haveHevcSize = true;
				break;
		}
	}

	if (result.mode == kModeH264 && haveH264Sps) {
		uint32 widthMbs = ((h264Sps >> 8) & 0xff) + 1;
		uint32 mapUnits = (h264Sps & 0xff) + 1;
		bool frameMbsOnly = (h264Sps & (1u << 18)) != 0;
		bool direct8x8 = (h264Sps & (1u << 16)) != 0;
		result.width = widthMbs * 16;
		result.height = mapUnits * 16 * (frameMbsOnly ? 1 : 2);
		uint32 field = widthMbs * (result.height / 16) * 16;
		if (!direct8x8)
			field *= 2;
		if (!frameMbsOnly)
			field *= 2;
		result.mvcolSize = field * 2;
		result.valid = haveStride;
	} else if (result.mode == kModeHevc && haveHevcSize && haveHevcSps) {
		result.width = hevcSize & 0xffff;
		result.height = align(hevcSize >> 16, 16);
		uint32 minCb = ((hevcSps >> 9) & 3) + 3;
		uint32 ctb = 1u << (minCb + ((hevcSps >> 11) & 3));
		uint32 widthCtbs = (result.width + ctb - 1) / ctb;
		uint32 heightCtbs = ((hevcSize >> 16) + ctb - 1) / ctb;
		result.mvcolSize = widthCtbs * heightCtbs * 160 + 1024;
		result.valid = haveStride;
	}
	if (result.valid && (result.width == 0 || result.height == 0
			|| result.width > 4096 || result.height > 4096
			|| result.stride < result.width || result.stride > 8192)) {
		result.valid = false;
	}
	return result;
}


/*!	A frame list entry's addresses (bytes, as the engine sees them). */
static bool
check_picture(ve_check& check, uint32 index, const geometry& geometry,
	int word, uint64 address, bool hevc)
{
	uint64 lumaSize = (uint64)geometry.stride * geometry.height;
	uint64 chromaSize = lumaSize / 2;
	if ((uint64)geometry.chromaHalf * 2 > chromaSize)
		chromaSize = (uint64)geometry.chromaHalf * 2;
	bool ok;
	switch (word) {
		case 0:		// luma, and the low two bits behind the picture
		{
			uint64 size = lumaSize;
			if (geometry.twoBitStride != 0) {
				uint64 twoBitEnd = (uint64)geometry.twoBitOffset
					+ (uint64)geometry.twoBitStride * geometry.height * 3 / 2;
				if (twoBitEnd > size)
					size = twoBitEnd;
			}
			ok = fits(check, address, size);
			break;
		}
		case 1:		// chroma
			ok = fits(check, address, chromaSize);
			break;
		default:	// motion data: H.264 has a half for each field
			ok = fits(check, address,
				hevc ? geometry.mvcolSize : geometry.mvcolSize / 2);
			break;
	}
	return ok ? true : fail(check, index, "picture address outside the "
		"buffers");
}


}	// namespace ve_check_private


/*!	Whether the writes of one slice may run. \a hevc says which engine the
	driver triggers.
*/
static bool
ve_check_ops(ve_check& check, const sunxi_ve_op* ops, uint32 count,
	bool hevc)
{
	using namespace ve_check_private;

	check.problem = NULL;
	check.index = 0;
	geometry geometry = find_geometry(ops, count);
	if (!geometry.valid)
		return fail(check, 0, "no picture size and layout");
	if (geometry.mode != (hevc ? kModeHevc : kModeH264))
		return fail(check, 0, "another engine than the one triggered");

	uint32 h264Sram = kSramLimit, hevcSram = kSramLimit;
	int32 outputSlot = geometry.outputSlot;
	bool outputSlotWritten[2] = { false, false };
		// luma and chroma of the output entry
	uint32 bitstream = 0, bitstreamEnd = 0;
	uint32 bitOffset = 0, bitLength = 0;

	for (uint32 i = 0; i < count; i++) {
		const sunxi_ve_op& op = ops[i];
		if (op.type == SUNXI_VE_OP_POLL_CLEAR || op.type == SUNXI_VE_OP_WRITE_BACK) {
			if (op.reg != 0x228 && op.reg != 0x538)
				return fail(check, i, "polls or clears a register that is no status");
			continue;
		}
		if (op.type != SUNXI_VE_OP_WRITE)
			return fail(check, i, "unknown op");
		if (!writable(op.reg))
			return fail(check, i, "register not writable");

		uint32 value = op.value;
		switch (op.reg) {
			case 0x000:
				if ((value & 0xf) != (uint32)(hevc ? kModeHevc : kModeH264)
					&& (value & 0xf) != kModeNone) {
					return fail(check, i, "another engine");
				}
				break;

			// commands of the bit reader only; the driver starts decoding
			case 0x224:
			case 0x534:
			{
				uint32 command = value & 0xff;
				if (command != 1 && command != 3 && command != 7)
					return fail(check, i, "engine command");
				break;
			}

			case 0x240:		// H.264 scale-down and rotation
				if (value != 0)
					return fail(check, i, "scale-down output");
				break;
			case 0x220:		// no secondary (scaled or rotated) output
			case 0x530:
				if ((value & (1u << 9)) != 0)
					return fail(check, i, "secondary output");
				break;
			case 0x58c:		// the two-bit stride only
				if ((value & ~0x7ffu) != 0)
					return fail(check, i, "ten bit output mode");
				break;

			case 0x24c:
				if (value < 1 || value >= kH264FrameSlots)
					return fail(check, i, "output position");
				break;
			case 0x55c:
				if (value >= kHevcFrameSlots)
					return fail(check, i, "output slot");
				break;

			// the bit stream: start and end in one buffer
			case 0x230:
				bitstream = (value & 0x0ffffff0) | ((value & 0xf) << 28);
				break;
			case 0x234:
			case 0x544:
				bitOffset = value;
				break;
			case 0x238:
			case 0x548:
				bitLength = value;
				break;
			case 0x23c:
				bitstreamEnd = value;
				break;
			case 0x540:
				bitstream = (value & 0x00ffffff) << 8;
				break;
			case 0x54c:
				bitstreamEnd = value << 8;
				break;

			// the engine's own buffers, as large as it needs them
			case 0x250:
			{
				uint64 size = (uint64)kH264FrameSlots
						* (geometry.width > 2048 ? 0x4000 : 0x1000)
					+ (uint64)geometry.height * 2 * 64;
				if (size < 130 * 1024)
					size = 130 * 1024;
				if (!fits(check, value, size))
					return fail(check, i, "picture information outside the buffers");
				break;
			}
			case 0x254:
				if (!fits(check, value, 32 * 1024))
					return fail(check, i, "neighbour information outside the buffers");
				break;
			case 0x054:
				if (!fits(check, value, align(geometry.width, 32) * 12))
					return fail(check, i, "deblocking buffer outside the buffers");
				break;
			case 0x058:
				if (!fits(check, value, align(geometry.width, 64) * 5 * 2))
					return fail(check, i, "intra prediction buffer outside the buffers");
				break;
			case 0x560:
				if (!fits(check, (uint64)value << 8, 794 * 1024))
					return fail(check, i, "neighbour information outside the buffers");
				break;
			case 0x564:
				if (range_of(check, (uint64)value << 8) == NULL)
					return fail(check, i, "entry points outside the buffers");
				break;

			// SRAM: the frame lists hold addresses
			case 0x2e0:
				h264Sram = value;
				if (h264Sram >= kSramLimit || (h264Sram & 3) != 0)
					return fail(check, i, "SRAM offset");
				break;
			case 0x2e4:
			{
				if (h264Sram >= kSramLimit)
					return fail(check, i, "SRAM offset");
				uint32 offset = h264Sram;
				h264Sram += 4;
				if (offset < kH264Frames
					|| offset >= kH264Frames + kH264FrameSlots * 32) {
					break;
				}
				uint32 slot = (offset - kH264Frames) / 32;
				int word = ((offset - kH264Frames) / 4) % 8;
				if (word < 3 || word > 6)
					break;
				if (value == 0)
					break;		// a position nothing is in
				if (!check_picture(check, i, geometry, word - 3, value, false))
					return false;
				if ((int32)slot == outputSlot && word <= 4)
					outputSlotWritten[word - 3] = true;
				break;
			}
			case 0x5e0:
				hevcSram = value;
				if (hevcSram >= kSramLimit || (hevcSram & 3) != 0)
					return fail(check, i, "SRAM offset");
				break;
			case 0x5e4:
			{
				if (hevcSram >= kSramLimit)
					return fail(check, i, "SRAM offset");
				uint32 offset = hevcSram;
				hevcSram += 4;
				if (offset < kHevcFrames
					|| offset >= kHevcFrames + kHevcFrameSlots * 32) {
					break;
				}
				uint32 slot = (offset - kHevcFrames) / 32;
				int word = ((offset - kHevcFrames) / 4) % 8;
				if (word < 2 || word > 5)
					break;
				if (value == 0)
					break;
				// motion data twice, then luma, then chroma, in 256 bytes
				int kind = word >= 4 ? word - 4 : 2;
				if (!check_picture(check, i, geometry, kind, (uint64)value << 8,
						true)) {
					return false;
				}
				if ((int32)slot == outputSlot && kind <= 1)
					outputSlotWritten[kind] = true;
				break;
			}
		}
	}

	// the picture the engine writes must be one of the program's
	if (outputSlot < 0 || !outputSlotWritten[0] || !outputSlotWritten[1])
		return fail(check, count, "no output picture");

	// the stream, and what the bit reader is told to read of it, in one
	// buffer (the registers are written in any order)
	if (bitstreamEnd == 0 || bitstreamEnd < bitstream
		|| !fits(check, bitstream, (uint64)bitstreamEnd - bitstream)
		|| !fits(check, bitstream, ((uint64)bitOffset + bitLength + 7) / 8)) {
		return fail(check, count, "bit stream outside the buffers");
	}
	return true;
}


#endif	// SUNXI_VE_CHECK_H
