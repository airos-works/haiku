/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab tool: decodes an H.264 or HEVC Annex B stream with the sunxi_cedar
	add-on's decoder on the Cubie A7S's video engine, and prints an MD5 of
	every picture in output order, cropped and as yuv420p: the hashes of
	`ffmpeg -i X -f framemd5 -pix_fmt yuv420p`.

	cedar_decode [-c expected] [-m] [-M] [-n] [-o out.yuv] [-r times]
		[-2 out] [-p 422|rgb32] [-k pictures] h264|hevc stream

	-c	compares with the "<picture> <md5>" lines of a file (vecheck's and
		the vectors' .framemd5; ten bit pictures are hashed as P010, so
		.p010.framemd5) and says PASS or FAIL
	-m	"<picture> <md5>" for every picture
	-n	no copying out (the engine's speed alone)
	-o	the pictures as yuv420p to a file
	-r	decodes the stream that many times (for timing)
	-2	the low two bits of ten bit pictures, as the engine left them, to a
		file
	-p	hashes the pictures as the Media Kit gets them: 422 (B_YCbCr422) or
		rgb32 (B_RGB32)
	-k	stops after that many pictures
	-M	the MD5 of all the pictures together (what the JCT-VC streams'
		_md5.txt files hold) */


#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include <md5.h>

#include "CedarDecoder.h"
#include "bits.h"


static void
usage(const char* name)
{
	fprintf(stderr, "usage: %s [-c expected] [-m] [-M] [-n] [-o out.yuv]"
		" [-r times] [-2 out] [-p 422|rgb32] [-k pictures] h264|hevc stream\n",
		name);
	exit(1);
}


struct Output {
	char		(*expected)[33];
	uint32		expectedCount;
	uint32		mismatches;
	bool		printMd5;
	bool		copy;
	FILE*		file;
	FILE*		twoBitFile;
	color_space	packed;
	bool		wholeMd5;
	MD5_CTX		whole;
	uint8*		buffer;
	size_t		bufferSize;
	uint32		pictures;
	uint32		corrupt;
};


/*!	The "<picture> <md5>" lines of a file. */
static bool
read_expected(const char* path, Output& output)
{
	FILE* file = fopen(path, "r");
	if (file == NULL) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return false;
	}
	char line[256];
	uint32 allocated = 0;
	while (fgets(line, sizeof(line), file) != NULL) {
		unsigned index;
		char hex[33];
		if (line[0] == '#' || sscanf(line, "%u %32s", &index, hex) != 2)
			continue;
		if (index >= allocated) {
			allocated = index + 64;
			output.expected = (char(*)[33])realloc(output.expected,
				allocated * 33);
		}
		while (output.expectedCount <= index)
			output.expected[output.expectedCount++][0] = '\0';
		strlcpy(output.expected[index], hex, 33);
	}
	fclose(file);
	return true;
}


static uint8*
read_file(const char* path, size_t& size)
{
	FILE* file = fopen(path, "rb");
	if (file == NULL) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return NULL;
	}
	fseek(file, 0, SEEK_END);
	long length = ftell(file);
	fseek(file, 0, SEEK_SET);
	uint8* data = (uint8*)malloc(length + 16);
	if (data == NULL || fread(data, 1, length, file) != (size_t)length) {
		fprintf(stderr, "%s: cannot read it\n", path);
		fclose(file);
		free(data);
		return NULL;
	}
	fclose(file);
	size = length;
	return data;
}




static void
take_pictures(CedarDecoder& decoder, Output& output)
{
	CedarDecoder::Picture picture;
	while (decoder.NextPicture(picture)) {
		if (picture.corrupt)
			output.corrupt++;
		if (output.copy) {
			uint32 width = picture.width;
			uint32 height = picture.height;
			// yuv420p, or p010le for ten bits (what FFmpeg hashes), or what
			// the Media Kit gets
			bool tenBit = picture.bitDepth > 8;
			uint32 stride = tenBit ? width * 2 : width;
			size_t size = tenBit
				? (size_t)stride * height + (size_t)stride * ((height + 1) / 2)
				: (size_t)width * height + 2 * (size_t)(width / 2) * (height / 2);
			if (output.packed != B_NO_COLOR_SPACE) {
				stride = output.packed == B_RGB32 ? width * 4 : width * 2;
				size = (size_t)stride * height;
			}
			if (size > output.bufferSize) {
				free(output.buffer);
				output.buffer = (uint8*)malloc(size);
				output.bufferSize = size;
			}
			if (output.packed != B_NO_COLOR_SPACE) {
				decoder.CopyPacked(picture, output.buffer, stride,
					output.packed);
			} else
				decoder.CopyPlanes(picture, output.buffer, stride, tenBit);
			if (output.file != NULL)
				fwrite(output.buffer, 1, size, output.file);
			if (output.wholeMd5)
				MD5_Update(&output.whole, output.buffer, size);
			if (output.printMd5 || output.expected != NULL) {
				MD5_CTX context;
				uint8 digest[16];
				MD5_Init(&context);
				MD5_Update(&context, output.buffer, size);
				MD5_Final(digest, &context);
				char hex[33];
				for (int i = 0; i < 16; i++)
					sprintf(hex + 2 * i, "%02x", digest[i]);
				if (output.printMd5)
					printf("%" B_PRIu32 " %s\n", output.pictures, hex);
				if (output.expected != NULL) {
					// (with -r, the stream's pictures again and again)
					uint32 index = output.expectedCount > 0
						? output.pictures % output.expectedCount : 0;
					if (output.expectedCount == 0
						|| strcmp(output.expected[index], hex) != 0) {
						if (output.mismatches++ == 0) {
							printf("picture %" B_PRIu32 ": %s, expected %s\n",
								output.pictures, hex, output.expectedCount > 0
									? output.expected[index] : "none");
						}
					}
				}
			}
		}
		if (output.twoBitFile != NULL) {
			uint8* twoBit = (uint8*)malloc(64 << 20);
			size_t size = decoder.CopyTwoBit(picture, twoBit, 64 << 20);
			fwrite(twoBit, 1, size, output.twoBitFile);
			free(twoBit);
		}
		decoder.ReleasePicture(picture);
		output.pictures++;
	}
}


int
main(int argc, char** argv)
{
	Output output = {};
	output.copy = true;
	int repeat = 1;
	uint32 limit = 0;
	const char* outputPath = NULL;
	const char* expectedPath = NULL;

	int option;
	while ((option = getopt(argc, argv, "2:c:k:mMno:p:r:")) != -1) {
		switch (option) {
			case 'M':
				output.wholeMd5 = true;
				MD5_Init(&output.whole);
				break;
			case 'k':
				limit = atoi(optarg);
				break;
			case 'p':
				output.packed = strcmp(optarg, "rgb32") == 0
					? B_RGB32 : B_YCbCr422;
				break;
			case '2':
				output.twoBitFile = fopen(optarg, "wb");
				break;
			case 'c':
				expectedPath = optarg;
				break;
			case 'm':
				output.printMd5 = true;
				break;
			case 'n':
				output.copy = false;
				break;
			case 'o':
				outputPath = optarg;
				break;
			case 'r':
				repeat = atoi(optarg);
				break;
			default:
				usage(argv[0]);
		}
	}
	if (argc - optind != 2)
		usage(argv[0]);

	CedarDecoder::codec codec;
	if (strcmp(argv[optind], "h264") == 0)
		codec = CedarDecoder::H264;
	else if (strcmp(argv[optind], "hevc") == 0)
		codec = CedarDecoder::HEVC;
	else
		usage(argv[0]);

	const char* streamPath = argv[optind + 1];
	size_t size;
	uint8* data = read_file(streamPath, size);
	if (data == NULL)
		return 1;
	if (expectedPath != NULL && !read_expected(expectedPath, output))
		return 1;
	if (outputPath != NULL) {
		output.file = fopen(outputPath, "wb");
		if (output.file == NULL) {
			fprintf(stderr, "%s: %s\n", outputPath, strerror(errno));
			return 1;
		}
	}

	CedarDecoder decoder(codec);
	BString error;
	status_t status = decoder.Open(&error);
	if (status != B_OK) {
		fprintf(stderr, "cedar_decode: %s\n", error.String());
		return 1;
	}
	const sunxi_ve_info& info = decoder.Info();
	fprintf(stderr, "cedar_decode: decoder IP %#" B_PRIx32 ", clock %" B_PRIu32
		" Hz\n", info.decoder_ip, info.clock);

	bigtime_t start = system_time();
	thread_info thread;
	get_thread_info(find_thread(NULL), &thread);
	bigtime_t cpuStart = thread.user_time + thread.kernel_time;

	int result = 0;
	for (int pass = 0; pass < repeat && result == 0; pass++) {
		decoder.Reset();
		size_t position = 0;
		const uint8_t* nal;
		size_t nalSize;
		int64 units = 0;
		while (annexb_next(data, size, &position, &nal, &nalSize)) {
			status = decoder.PutNal(nal, nalSize, units++);
			if (status != B_OK) {
				fprintf(stderr, "cedar_decode: %s\n", decoder.Error());
				result = 1;
				break;
			}
			take_pictures(decoder, output);
			if (limit != 0 && output.pictures >= limit)
				break;
		}
		if (limit == 0 || output.pictures < limit) {
			decoder.Drain();
			take_pictures(decoder, output);
		}
	}

	bigtime_t elapsed = system_time() - start;
	get_thread_info(find_thread(NULL), &thread);
	bigtime_t cpu = thread.user_time + thread.kernel_time - cpuStart;
	if (output.file != NULL)
		fclose(output.file);
	if (output.twoBitFile != NULL)
		fclose(output.twoBitFile);

	fprintf(stderr, "cedar_decode: %" B_PRIu32 " pictures (%" B_PRIu32
		" corrupt), %" B_PRIu32 " slices in %.3f s: %.1f pictures/s; engine"
		" %.3f s, CPU %.3f s\n", output.pictures, output.corrupt,
		decoder.SlicesDecoded(), elapsed / 1000000.0,
		elapsed > 0 ? output.pictures * 1000000.0 / elapsed : 0.0,
		decoder.EngineTime() / 1000000.0, cpu / 1000000.0);
	if (output.wholeMd5) {
		uint8 digest[16];
		MD5_Final(digest, &output.whole);
		printf("all pictures: ");
		for (int i = 0; i < 16; i++)
			printf("%02x", digest[i]);
		printf("\n");
	}
	if (expectedPath != NULL) {
		uint32 wanted = output.expectedCount * repeat;
		bool pass = result == 0 && output.mismatches == 0
			&& output.corrupt == 0 && output.pictures == wanted;
		const char* name = strrchr(streamPath, '/');
		printf("%s %s: %" B_PRIu32 " of %" B_PRIu32 " pictures, %" B_PRIu32
			" differ\n", pass ? "PASS" : "FAIL",
			name != NULL ? name + 1 : streamPath, output.pictures, wanted,
			output.mismatches);
		if (!pass)
			result = 1;
	}
	free(output.expected);
	free(output.buffer);
	free(data);
	return result != 0 || output.corrupt != 0 ? 1 : 0;
}
