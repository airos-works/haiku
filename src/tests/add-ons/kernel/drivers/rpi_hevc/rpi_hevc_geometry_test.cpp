/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Tests how the rpi_hevc driver works out what the buffers of a phase two
	request must hold (get_phase2_needs(), #202): every geometry the
	rpi_hevc media add-on makes (HevcDecoder::_Configure() and
	_RunHardware(), worked out here the same way) must fit its buffers, and
	a picture larger than its buffer, too many rows, a short motion vector
	stride or motion vectors without a buffer must not.
	Builds for Haiku (jam rpi_hevc_geometry_test) and on any host:
	  g++ -std=c++17 -g -fsanitize=address,undefined \
	      -I<haiku>/headers/os/kernel -I<haiku>/headers/os/drivers \
	      -I<haiku>/headers/private/drivers \
	      -I<haiku>/src/add-ons/kernel/drivers/misc \
	      rpi_hevc_geometry_test.cpp -o rpi_hevc_geometry_test \
	      && ./rpi_hevc_geometry_test
*/


#include <stdio.h>
#include <string.h>

#ifndef __HAIKU__
// what the headers under test use of OS.h and Drivers.h (found, and left
// out by their guards)
#	define _OS_H
#	define _DRIVERS_DRIVERS_H
typedef unsigned char uint8;
typedef unsigned int uint32;
typedef unsigned long long uint64;
typedef int int32;
typedef int status_t;
typedef int32 area_id;
enum {
	B_OK = 0,
	B_BAD_VALUE = -1000
};
#	define B_DEVICE_OP_CODES_END 9999
#else
#	include <OS.h>
#endif

#include <rpi_hevc.h>
#include "rpi_hevc_geometry.h"


static const uint64 kPageSize = 4096;

static int sFailures = 0;
static int sChecks = 0;

#define CHECK(condition, what...) \
	do { \
		sChecks++; \
		if (!(condition)) { \
			sFailures++; \
			fprintf(stderr, "%s:%d: %s failed: ", __FILE__, __LINE__, \
				#condition); \
			fprintf(stderr, what); \
			fputc('\n', stderr); \
		} \
	} while (0)


// The buffers of a picture as the media add-on sizes them, each rounded up
// to pages as the driver does.
struct Picture {
	rpi_hevc_phase2	request;
	uint64			frame;
	uint64			mv;
	uint64			pu;
	uint64			coeff;
};


static uint64
pages(uint64 size)
{
	return hevc_round_up(size, kPageSize);
}


static Picture
decoder_picture(uint32 width, uint32 height, uint32 bitDepth,
	uint32 log2CtbSize)
{
	Picture picture;
	memset(&picture, 0, sizeof(picture));

	// HevcDecoder::_Configure()
	uint32 alignedHeight = hevc_round_up(height, 16);
	uint32 columns = bitDepth == 8 ? hevc_round_up(width, 128) / 128
		: hevc_round_up((width + 2) / 3, 32) / 32;
	uint32 columnStride = alignedHeight * 3 / 2 * 128;
	uint32 chromaOffset = alignedHeight * 128;
	uint32 mvStride = hevc_round_up(width, 64);
	picture.frame = pages((uint64)columnStride * columns);
	picture.mv = pages((uint64)mvStride * (hevc_round_up(height, 64) >> 4));
	uint64 pixels = (uint64)width * height;
	picture.coeff = pages(pixels);
	picture.pu = pages(pixels / 4);

	// HevcDecoder::_RunHardware()
	uint32 ctbSize = 1u << log2CtbSize;
	uint32 ctbHeight = (height + ctbSize - 1) / ctbSize;
	rpi_hevc_phase2& request = picture.request;
	request.pu = 1;
	request.pu_stride = (uint32)(picture.pu / ctbHeight) & ~63u;
	request.coeff = 2;
	request.coeff_stride = (uint32)(picture.coeff / ctbHeight) & ~63u;
	request.frame = 3;
	request.chroma_offset = chromaOffset;
	request.frame_stride = columnStride;
	for (int32 i = 0; i < 16; i++)
		request.references[i] = RPI_HEVC_NO_BUFFER;
	request.config = bitDepth | (bitDepth << 4) | (log2CtbSize << 10)
		| (1 << 15);
	if (bitDepth > 8)
		request.config |= (1 << 8) | (1 << 9);
	request.frame_size = (height << 16) | width;
	request.rows = ctbHeight;
	request.mv_stride = mvStride;
	request.mv = 4;
	request.collocated = 5;
	return picture;
}


static bool
fits(const Picture& picture)
{
	hevc_phase2_needs needs;
	return get_phase2_needs(picture.request, needs) == B_OK
		&& needs.picture <= picture.frame && needs.mv <= picture.mv
		&& needs.pu <= picture.pu && needs.coeff <= picture.coeff;
}


int
main()
{
	// Every picture size the add-on makes fits its own buffers: sizes the
	// HEVC levels allow, in steps of the smallest coding block (8).
	int sizes = 0;
	for (uint32 bitDepth = 8; bitDepth <= 10; bitDepth += 2) {
		for (uint32 log2CtbSize = 4; log2CtbSize <= 6; log2CtbSize++) {
			for (uint32 width = 8; width <= 4096; width += 8) {
				for (uint32 height = 8; height <= 2304; height += 8) {
					if (width * 9 / 16 != height && width != 4096
						&& height != 8 && width != 8
						&& (width + height) % 136 != 0) {
						continue;
					}
					Picture picture = decoder_picture(width, height,
						bitDepth, log2CtbSize);
					sizes++;
					CHECK(fits(picture), "%ux%u, %u bit, CTB %u",
						width, height, bitDepth, 1u << log2CtbSize);
				}
			}
		}
	}
	printf("%d picture sizes of the media add-on checked\n", sizes);

	// The case of the issue: a 1920x1080 picture into a 4 KiB buffer. The
	// old check (only chroma_offset < size) let it through.
	{
		Picture picture = decoder_picture(1920, 1080, 8, 6);
		picture.request.chroma_offset = 2048;
		picture.frame = 4096;
		CHECK(picture.request.chroma_offset < picture.frame,
			"the old check accepts it");
		CHECK(!fits(picture), "1080p into 4 KiB");
	}

	// A buffer one page short of the picture
	{
		Picture picture = decoder_picture(1920, 1080, 8, 6);
		picture.frame -= kPageSize;
		CHECK(!fits(picture), "1080p, a page short");
		picture = decoder_picture(3840, 2160, 10, 6);
		picture.frame -= kPageSize;
		CHECK(!fits(picture), "2160p 10 bit, a page short");
	}

	// The geometry of a larger picture with the buffers of a smaller one
	// (a stream that grows without the pool following)
	{
		Picture small = decoder_picture(1280, 720, 8, 6);
		Picture large = decoder_picture(1920, 1080, 8, 6);
		large.frame = small.frame;
		CHECK(!fits(large), "1080p in a 720p frame buffer");
		large = decoder_picture(1920, 1080, 8, 6);
		large.mv = small.mv;
		CHECK(!fits(large), "1080p in a 720p motion vector buffer");
		large = decoder_picture(1920, 1080, 8, 6);
		large.pu = small.pu;
		CHECK(!fits(large), "1080p in a 720p PU buffer");
		large = decoder_picture(1920, 1080, 8, 6);
		large.coeff = small.coeff;
		CHECK(!fits(large), "1080p in a 720p coefficient buffer");
		// 8-bit columns are wider than 10-bit ones
		large = decoder_picture(1920, 1080, 10, 6);
		large.frame = decoder_picture(1920, 1080, 8, 6).frame;
		CHECK(!fits(large), "10 bit in an 8 bit frame buffer");
	}

	// Geometries the block must not be given
	{
		hevc_phase2_needs needs;
		Picture picture = decoder_picture(1920, 1080, 8, 6);
		picture.request.rows++;
		CHECK(get_phase2_needs(picture.request, needs) == B_BAD_VALUE,
			"a row more than the picture has");
		picture = decoder_picture(1920, 1080, 8, 6);
		picture.request.rows = 0xffffffff;
		CHECK(get_phase2_needs(picture.request, needs) == B_BAD_VALUE,
			"2^32-1 rows");
		picture = decoder_picture(1920, 1080, 8, 6);
		picture.request.frame_size = 1080 << 16;
		CHECK(get_phase2_needs(picture.request, needs) == B_BAD_VALUE,
			"no width");
		picture = decoder_picture(1920, 1080, 8, 6);
		picture.request.config &= ~(7 << 10);
		picture.request.config |= 7 << 10;
		CHECK(get_phase2_needs(picture.request, needs) == B_BAD_VALUE,
			"coding tree blocks of 128");
		picture = decoder_picture(1920, 1080, 8, 6);
		picture.request.mv_stride = 64;
		CHECK(get_phase2_needs(picture.request, needs) == B_BAD_VALUE,
			"a motion vector stride shorter than a row");
		picture = decoder_picture(1920, 1080, 8, 6);
		picture.request.mv = RPI_HEVC_NO_BUFFER;
		CHECK(get_phase2_needs(picture.request, needs) == B_BAD_VALUE,
			"motion vectors to keep, without a buffer");
		picture.request.config &= ~(1 << 15);
		CHECK(get_phase2_needs(picture.request, needs) == B_OK,
			"motion vectors not kept, no buffer");

		// the largest numbers the request can hold do not wrap around
		picture = decoder_picture(1920, 1080, 8, 4);
		picture.request.frame_size = 0xffffffff;
		picture.request.rows = (0xffff + 15) / 16;
		picture.request.frame_stride = 0xffffffff;
		picture.request.chroma_offset = 0xffffffff;
		picture.request.pu_stride = 0xffffffff;
		picture.request.coeff_stride = 0xffffffff;
		picture.request.mv_stride = 0xffffffff;
		CHECK(get_phase2_needs(picture.request, needs) == B_OK,
			"the largest geometry");
		CHECK(needs.picture > 0xffffffffull && needs.pu > 0xffffffffull
			&& needs.mv > 0xffffffffull, "the largest geometry's needs");
	}

	printf("rpi_hevc_geometry_test: %d checks, %d failed\n", sChecks,
		sFailures);
	return sFailures == 0 ? 0 : 1;
}
