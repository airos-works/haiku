/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab test of the qcom_iris media plugin's decoder: an H.264 or HEVC
	elementary stream (Annex B) decoded on the codec, each picture compared
	with a reference (I420, as "ffmpeg -pix_fmt yuv420p" writes it) and
	optionally written out the same way.

	iris_decode [--hevc] [--hashes] [--size WxH] stream
		[reference.yuv | reference.hashes] [out.yuv]

	--hashes prints each picture's FNV-1a hash (of the I420 picture). Ten bit
	pictures are compared and written as P010 without padding (as
	"-pix_fmt p010le"). */


#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <OS.h>

#include "IrisDecoder.h"


static bool sHevc;
static bool sHashes;


//!	FNV-1a, to compare pictures with a decode elsewhere.
static uint64
hash(const uint8* data, size_t size)
{
	uint64 value = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < size; i++) {
		value ^= data[i];
		value *= 0x100000001b3ULL;
	}
	return value;
}


static bool
read_file(const char* path, std::vector<uint8>& data)
{
	FILE* file = fopen(path, "rb");
	if (file == NULL)
		return false;
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	data.resize(size);
	bool ok = fread(data.data(), 1, size, file) == (size_t)size;
	fclose(file);
	return ok;
}


/*!	The start code at or after \a offset; data.size() when there is none.
	\a _length is its length (3 or 4). */
static size_t
next_start_code(const std::vector<uint8>& data, size_t offset,
	size_t* _length)
{
	for (size_t i = offset; i + 3 <= data.size(); i++) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
			if (i > offset && data[i - 1] == 0) {
				*_length = 4;
				return i - 1;
			}
			*_length = 3;
			return i;
		}
	}
	*_length = 0;
	return data.size();
}


//!	Whether the NAL unit at \a nal begins an access unit after a picture.
static bool
begins_access_unit(const uint8* nal, size_t size, bool* _vcl)
{
	if (sHevc) {
		if (size < 3)
			return false;
		int type = (nal[0] >> 1) & 0x3f;
		*_vcl = type < 32;
		if (type < 32)
			return (nal[2] & 0x80) != 0;
		return (type >= 32 && type <= 40) || (type >= 41 && type <= 44)
			|| (type >= 48 && type <= 55);
	}
	if (size < 2)
		return false;
	int type = nal[0] & 0x1f;
	*_vcl = type >= 1 && type <= 5;
	if (*_vcl)
		return (nal[1] & 0x80) != 0;	// first_mb_in_slice 0
	return type == 6 || type == 7 || type == 8 || type == 9
		|| (type >= 14 && type <= 18);
}


static void
split_access_units(const std::vector<uint8>& data,
	std::vector<std::pair<size_t, size_t> >& units)
{
	size_t length;
	size_t start = next_start_code(data, 0, &length);
	size_t unitStart = start;
	bool havePicture = false;
	while (start < data.size()) {
		size_t nal = start + length;
		size_t nextLength;
		size_t next = next_start_code(data, nal, &nextLength);
		bool vcl = false;
		if (begins_access_unit(&data[nal], next - nal, &vcl) && havePicture) {
			units.push_back(std::make_pair(unitStart, start - unitStart));
			unitStart = start;
			havePicture = false;
		}
		if (vcl)
			havePicture = true;
		start = next;
		length = nextLength;
	}
	if (unitStart < data.size())
		units.push_back(std::make_pair(unitStart, data.size() - unitStart));
}


int
main(int argc, char** argv)
{
	uint32 width = 0, height = 0;
	int argument = 1;
	while (argument < argc && argv[argument][0] == '-') {
		if (strcmp(argv[argument], "--hevc") == 0)
			sHevc = true;
		else if (strcmp(argv[argument], "--hashes") == 0)
			sHashes = true;
		else if (strcmp(argv[argument], "--size") == 0
			&& argument + 1 < argc) {
			sscanf(argv[++argument], "%" B_SCNu32 "x%" B_SCNu32, &width,
				&height);
		}
		argument++;
	}
	if (argument >= argc) {
		fprintf(stderr, "usage: %s [--hevc] [--hashes] [--size WxH] stream "
			"[reference.yuv] [out.yuv]\n", argv[0]);
		return 1;
	}
	const char* streamPath = argv[argument];
	const char* referencePath = argument + 1 < argc ? argv[argument + 1]
		: NULL;
	const char* outPath = argument + 2 < argc ? argv[argument + 2] : NULL;
	if (strstr(streamPath, "265") != NULL || strstr(streamPath, ".hevc"))
		sHevc = true;

	std::vector<uint8> stream, reference;
	std::vector<uint64> referenceHashes;
	if (!read_file(streamPath, stream)) {
		fprintf(stderr, "cannot read %s\n", streamPath);
		return 1;
	}
	if (referencePath != NULL && strstr(referencePath, ".hashes") != NULL) {
		// "hash <picture> <FNV-1a in hex>" lines, from another decoder
		FILE* file = fopen(referencePath, "r");
		char line[128];
		while (file != NULL && fgets(line, sizeof(line), file) != NULL) {
			unsigned index;
			unsigned long long value;
			if (sscanf(line, "hash %u %llx", &index, &value) == 2)
				referenceHashes.push_back(value);
		}
		if (file != NULL)
			fclose(file);
		else {
			fprintf(stderr, "cannot read %s\n", referencePath);
			return 1;
		}
	} else if (referencePath != NULL && strcmp(referencePath, "-") != 0
		&& !read_file(referencePath, reference)) {
		fprintf(stderr, "cannot read %s\n", referencePath);
		return 1;
	}
	FILE* out = outPath != NULL ? fopen(outPath, "wb") : NULL;

	std::vector<std::pair<size_t, size_t> > units;
	split_access_units(stream, units);
	printf("%s: %zu bytes, %zu access units (%s)\n", streamPath,
		stream.size(), units.size(), sHevc ? "HEVC" : "H.264");

	IrisDecoder decoder(sHevc ? IrisDecoder::HEVC : IrisDecoder::H264);
	BString error;
	bigtime_t start = system_time();
	if (decoder.Open(width, height, &error) != B_OK) {
		printf("open: %s\n", error.String());
		return 1;
	}
	printf("firmware %s, open in %" B_PRId64 " us\n",
		decoder.FirmwareVersion(), system_time() - start);

	uint32 pictures = 0, matching = 0, compared = 0;
	double worstPsnr = 1000;
	std::vector<uint8> frame;
	bigtime_t firstPicture = 0;
	status_t status = B_OK;

	auto take = [&](bigtime_t timeout) -> status_t {
		IrisDecoder::Picture picture;
		status_t result = decoder.NextPicture(picture, timeout);
		if (result != B_OK)
			return result;
		if (firstPicture == 0)
			firstPicture = system_time();
		size_t frameSize = (size_t)picture.width * picture.height * 3 / 2;
		if (picture.tenBit) {
			frameSize *= 2;
			frame.resize(frameSize);
			decoder.CopyP010(picture, frame.data(), picture.width * 2, false);
		} else {
			frame.resize(frameSize);
			decoder.CopyPlanes(picture, frame.data(), picture.width, false);
		}
		if (out != NULL)
			fwrite(frame.data(), 1, frameSize, out);
		if (sHashes) {
			printf("hash %" B_PRIu32 " %016" B_PRIx64 "\n", pictures,
				hash(frame.data(), frameSize));
		}

		const char* verdict = "";
		char buffer[64] = "";
		size_t referenceOffset = (size_t)pictures * frameSize;
		if (pictures < referenceHashes.size()) {
			compared++;
			if (hash(frame.data(), frameSize) == referenceHashes[pictures]) {
				matching++;
				verdict = " identical";
			} else
				verdict = " differs";
		} else if (!reference.empty() && referenceOffset + frameSize
				<= reference.size()) {
			compared++;
			const uint8* expected = &reference[referenceOffset];
			if (memcmp(expected, frame.data(), frameSize) == 0) {
				matching++;
				verdict = " identical";
			} else {
				double error = 0;
				size_t differ = 0;
				for (size_t i = 0; i < frameSize; i++) {
					// (a P010 picture: the high bytes and low ones apart)
					int d = (int)expected[i] - frame[i];
					error += d * d;
					if (d != 0)
						differ++;
				}
				double psnr = error == 0 ? 99
					: 10 * log10(255.0 * 255.0 * frameSize / error);
				if (psnr < worstPsnr)
					worstPsnr = psnr;
				snprintf(buffer, sizeof(buffer), " differs: %zu samples, "
					"PSNR %.2f dB", differ, psnr);
				verdict = buffer;
			}
		}
		if (pictures < 8 || (verdict[0] == ' ' && verdict[1] == 'd')) {
			printf("picture %" B_PRIu32 ": %" B_PRIu32 "x%" B_PRIu32
				" pts %" B_PRId64 "%s%s\n", pictures, picture.width,
				picture.height, picture.pts, picture.corrupt ? " corrupt" : "",
				verdict);
		}
		pictures++;
		decoder.ReleasePicture(picture);
		return B_OK;
	};

	bigtime_t feedStart = system_time();
	for (size_t i = 0; i < units.size() && status == B_OK; i++) {
		while (true) {
			status = decoder.PutAccessUnit(&stream[units[i].first],
				units[i].second, i * 1000, 2000000);
			if (status != B_WOULD_BLOCK)
				break;
			status_t taken = take(0);
			if (taken != B_OK && taken != B_WOULD_BLOCK) {
				status = taken;
				break;
			}
			if (taken == B_WOULD_BLOCK) {
				printf("access unit %zu: no input buffer comes free\n", i);
				status = B_TIMED_OUT;
				break;
			}
		}
		while (status == B_OK && take(0) == B_OK)
			;
	}
	if (status == B_OK)
		status = decoder.EndOfStream();
	while (status == B_OK) {
		status_t taken = take(2000000);
		if (taken == B_LAST_BUFFER_ERROR)
			break;
		if (taken != B_OK) {
			printf("waiting for pictures: %s\n", strerror(taken));
			status = taken;
		}
	}
	bigtime_t end = system_time();
	if (status != B_OK)
		printf("error: %s (%s)\n", strerror(status), decoder.Error());

	printf("%" B_PRIu32 " pictures of %zu access units in %" B_PRId64
		" ms (%.1f/s after the first)\n", pictures, units.size(),
		(end - feedStart) / 1000,
		pictures > 1 && end > firstPicture
			? (pictures - 1) * 1000000.0 / (end - firstPicture) : 0.0);
	if (compared > 0) {
		printf("%" B_PRIu32 " of %" B_PRIu32 " compared pictures identical",
			matching, compared);
		if (matching != compared)
			printf(", worst PSNR %.2f dB", worstPsnr);
		printf("\n");
	}
	if (out != NULL)
		fclose(out);
	decoder.Close();
	return status == B_OK && matching == compared ? 0 : 1;
}
