/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	cedar_venc: encodes test pictures (or NV12 from a file) with
	CedarEncoder and writes the H.264 or H.265 stream, and the pictures it
	encoded when asked, to compare them with the decoded stream:

		cedar_venc [-c h264|h265] [-s WxH] [-n pictures] [-q QP] [-b kbit/s]
			[-k interval] [-i in.nv12] [-y pattern.nv12] out
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include "../CedarEncoder.h"


static void
pattern(uint8* y, uint8* uv, uint32 width, uint32 height, uint32 index)
{
	uint32 boxX = 64 + index * 8, boxY = height / 4 + index * 2;
	for (uint32 j = 0; j < height; j++) {
		for (uint32 i = 0; i < width; i++) {
			uint32 v = 32 + i * 160 / width + j * 40 / height;
			if (i >= boxX && i < boxX + width / 6 && j >= boxY
				&& j < boxY + height / 4)
				v = ((i / 16 + j / 16) & 1) ? 220 : 40;
			else if (j > height * 3 / 4)
				v = ((i * 8 / width) & 1) ? 190 : 60;
			y[(size_t)j * width + i] = (uint8)v;
		}
	}
	for (uint32 j = 0; j < height / 2; j++) {
		for (uint32 i = 0; i < width / 2; i++) {
			uv[(size_t)j * width + 2 * i] = (uint8)(96 + i * 64 / (width / 2));
			uv[(size_t)j * width + 2 * i + 1]
				= (uint8)(96 + j * 64 / (height / 2));
		}
	}
}


int
main(int argc, char** argv)
{
	CedarEncoder::Params params;
	params.width = 1920;
	params.height = 1080;
	params.keyInterval = 30;
	uint32 pictures = 30;
	const char* input = NULL;
	const char* patternOut = NULL;

	int option;
	while ((option = getopt(argc, argv, "c:s:n:q:b:k:i:y:")) != -1) {
		switch (option) {
			case 's':
				sscanf(optarg, "%" B_SCNu32 "x%" B_SCNu32, &params.width,
					&params.height);
				break;
			case 'n': pictures = atoi(optarg); break;
			case 'q': params.qp = atoi(optarg); break;
			case 'b': params.bitrate = atoi(optarg) * 1000; break;
			case 'k': params.keyInterval = atoi(optarg); break;
			case 'c':
				params.codec = strcmp(optarg, "h265") == 0
					|| strcmp(optarg, "hevc") == 0
					? CedarEncoder::kHEVC : CedarEncoder::kH264;
				break;
			case 'i': input = optarg; break;
			case 'y': patternOut = optarg; break;
			default:
				return 1;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "usage: %s [-c h264|h265] [-s WxH] [-n pictures] "
			"[-q QP] [-b kbit/s] [-k interval] [-i in.nv12] "
			"[-y pattern.nv12] out\n", argv[0]);
		return 1;
	}

	char reason[256];
	if (!CedarEncoder::IsAvailable(reason, sizeof(reason))) {
		fprintf(stderr, "%s\n", reason);
		return 1;
	}
	CedarEncoder encoder;
	if (encoder.Open(params) != B_OK) {
		fprintf(stderr, "open: %s\n", encoder.Error());
		return 1;
	}
	printf("%s\n", encoder.Description());

	FILE* out = fopen(argv[optind], "wb");
	FILE* in = input != NULL ? fopen(input, "rb") : NULL;
	FILE* reference = patternOut != NULL ? fopen(patternOut, "wb") : NULL;
	if (out == NULL || (input != NULL && in == NULL)) {
		perror("fopen");
		return 1;
	}
	size_t lumaSize = (size_t)params.width * params.height;
	uint8* picture = (uint8*)malloc(lumaSize * 3 / 2);

	bigtime_t start = system_time();
	uint64 engine = 0, bytes = 0;
	uint32 done = 0;
	for (; done < pictures; done++) {
		if (in != NULL) {
			if (fread(picture, 1, lumaSize * 3 / 2, in) != lumaSize * 3 / 2)
				break;
		} else
			pattern(picture, picture + lumaSize, params.width, params.height,
				done);
		if (reference != NULL)
			fwrite(picture, 1, lumaSize * 3 / 2, reference);
		const uint8* data;
		size_t size;
		bool key;
		status_t status = encoder.Encode(picture, params.width,
			picture + lumaSize, params.width, false, &data, &size, &key);
		if (status != B_OK) {
			fprintf(stderr, "picture %" B_PRIu32 ": %s\n", done,
				encoder.Error());
			return 1;
		}
		engine += encoder.EngineTime();
		bytes += size;
		fwrite(data, 1, size, out);
	}
	bigtime_t elapsed = system_time() - start;
	printf("%" B_PRIu32 " pictures, %" B_PRIu64 " bytes, %.2f ms engine and "
		"%.2f ms in all a picture (%.1f fps)\n", done, bytes,
		done ? engine / 1000.0 / done : 0.0,
		done ? elapsed / 1000.0 / done : 0.0,
		elapsed > 0 ? done * 1e6 / elapsed : 0.0);
	fclose(out);
	if (in != NULL)
		fclose(in);
	if (reference != NULL)
		fclose(reference);
	free(picture);
	return 0;
}
