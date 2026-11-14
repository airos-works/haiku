/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// rpi4_hevc_decode <file.hevc> [output | -]
// Decodes an HEVC Annex B byte stream on the Raspberry Pi 4's or 5's decoder
// block
// and writes the pictures (the visible part; eight bit as I420, ten bit as
// P010) to the output file or, with "-", to the standard output, to compare
// with another decoder's:
//   rpi4_hevc_decode film.hevc - | md5sum
//   ffmpeg -i film.hevc -pix_fmt yuv420p -f rawvideo - | md5sum
//   (ten bit: -pix_fmt p010le)
// RPI4_HEVC_FRAME_MD5: a line with each picture's own md5 instead, as
//   ffmpeg -i film.hevc -pix_fmt yuv420p -f framemd5 -


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include <OS.h>

#include "HevcDecoder.h"


static FILE* sOutput;
static FILE* sReport;
static uint32 sPictures;
static bigtime_t sCopyTime;
static std::vector<uint8> sPlanes;


static void
write_pictures(HevcDecoder& decoder)
{
	HevcDecoder::Picture picture;
	while (decoder.NextPicture(picture)) {
		uint32 bytes = picture.bitDepth > 8 ? 2 : 1;
		uint32 stride = ((picture.width + 1) & ~1u) * bytes;
		size_t size = (size_t)stride * picture.height
			+ (size_t)stride * ((picture.height + 1) / 2);
		sPlanes.resize(size);

		bigtime_t start = system_time();
		decoder.CopyPlanes(picture, sPlanes.data(), stride, false);
		sCopyTime += system_time() - start;
		decoder.ReleasePicture(picture);

		if (getenv("RPI4_HEVC_LIST") != NULL) {
			fprintf(sReport, "picture %" B_PRIu32 ": order %" B_PRId32
				", time %" B_PRId64 "%s\n", sPictures, picture.poc,
				picture.pts, picture.corrupt ? ", damaged" : "");
		}
		if (sOutput != NULL)
			fwrite(sPlanes.data(), 1, size, sOutput);
		sPictures++;
	}
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <file.hevc> [output | -]\n", argv[0]);
		return 1;
	}

	FILE* input = fopen(argv[1], "rb");
	if (input == NULL) {
		perror(argv[1]);
		return 1;
	}
	std::vector<uint8> stream;
	uint8 block[65536];
	size_t got;
	while ((got = fread(block, 1, sizeof(block), input)) > 0)
		stream.insert(stream.end(), block, block + got);
	fclose(input);

	if (argc > 2)
		sOutput = strcmp(argv[2], "-") == 0 ? stdout : fopen(argv[2], "wb");
	// what is said goes where the pictures do not
	sReport = sOutput == stdout ? stderr : stdout;

	HevcDecoder decoder;
	BString error;
	if (decoder.Open(&error) != B_OK) {
		fprintf(stderr, "%s\n", error.String());
		return 1;
	}

	bigtime_t start = system_time();

	// NAL units between start codes
	size_t size = stream.size();
	size_t position = 0;
	int64 count = 0;
	while (position + 3 <= size) {
		// the next start code
		size_t begin = position;
		while (begin + 3 <= size && !(stream[begin] == 0
				&& stream[begin + 1] == 0 && stream[begin + 2] == 1)) {
			begin++;
		}
		if (begin + 3 > size)
			break;
		begin += 3;
		size_t end = begin;
		while (end + 3 <= size && !(stream[end] == 0 && stream[end + 1] == 0
				&& (stream[end + 2] == 1 || stream[end + 2] == 0))) {
			end++;
		}
		if (end + 3 > size)
			end = size;

		status_t status = decoder.PutNal(&stream[begin], end - begin,
			count++);
		if (status != B_OK) {
			fprintf(stderr, "%s\n", decoder.Error());
			return 1;
		}
		write_pictures(decoder);
		position = end;
	}
	decoder.Drain();
	write_pictures(decoder);

	bigtime_t elapsed = system_time() - start;
	fprintf(sReport, "%" B_PRIu32 " pictures out, %" B_PRIu32 " decoded, in "
		"%.2f s: %.1f per second; %.1f ms per picture making planes\n",
		sPictures, decoder.PicturesDecoded(), elapsed / 1e6,
		sPictures * 1e6 / elapsed,
		sPictures > 0 ? sCopyTime / 1000.0 / sPictures : 0.0);
	return 0;
}
