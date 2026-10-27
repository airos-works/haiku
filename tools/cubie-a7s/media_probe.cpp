/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab tool: decodes the first video track of a file through the Media Kit
	(BMediaFile, as MediaPlayer does) and says which decoder did it, how fast
	and at what processor cost.

	media_probe [-n pictures] [-f 422|rgb32|nv12|i420] file */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Entry.h>
#include <MediaFile.h>
#include <MediaTrack.h>
#include <OS.h>


static bigtime_t
team_cpu_time()
{
	team_usage_info usage;
	if (get_team_usage_info(B_CURRENT_TEAM, B_TEAM_USAGE_SELF, &usage)
			!= B_OK) {
		return 0;
	}
	return usage.user_time + usage.kernel_time;
}


int
main(int argc, char** argv)
{
	int64 wanted = 300;
	color_space space = B_YCbCr422;
	int option;
	while ((option = getopt(argc, argv, "n:f:")) != -1) {
		switch (option) {
			case 'n':
				wanted = atoll(optarg);
				break;
			case 'f':
				if (strcmp(optarg, "rgb32") == 0)
					space = B_RGB32;
				else if (strcmp(optarg, "nv12") == 0)
					space = (color_space)0x4e563132;
				else if (strcmp(optarg, "i420") == 0)
					space = (color_space)0x49343230;
				else
					space = B_YCbCr422;
				break;
			default:
				fprintf(stderr, "usage: %s [-n pictures] "
					"[-f 422|rgb32|nv12|i420] file\n", argv[0]);
				return 1;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "usage: %s [-n pictures] [-f 422|rgb32|nv12|i420] "
			"file\n", argv[0]);
		return 1;
	}

	entry_ref ref;
	if (get_ref_for_path(argv[optind], &ref) != B_OK) {
		fprintf(stderr, "%s: not found\n", argv[optind]);
		return 1;
	}
	BMediaFile file(&ref);
	if (file.InitCheck() != B_OK) {
		fprintf(stderr, "%s: %s\n", argv[optind], strerror(file.InitCheck()));
		return 1;
	}

	for (int32 i = 0; i < file.CountTracks(); i++) {
		BMediaTrack* track = file.TrackAt(i);
		media_format encoded;
		if (track == NULL || track->EncodedFormat(&encoded) != B_OK
			|| encoded.type != B_MEDIA_ENCODED_VIDEO) {
			if (track != NULL)
				file.ReleaseTrack(track);
			continue;
		}

		uint32 width = encoded.u.encoded_video.output.display.line_width;
		uint32 height = encoded.u.encoded_video.output.display.line_count;
		media_format decoded;
		decoded.type = B_MEDIA_RAW_VIDEO;
		decoded.u.raw_video = media_raw_video_format::wildcard;
		decoded.u.raw_video.display.format = space;
		decoded.u.raw_video.display.line_width = width;
		decoded.u.raw_video.display.line_count = height;
		status_t status = track->DecodedFormat(&decoded);
		media_codec_info info;
		memset(&info, 0, sizeof(info));
		track->GetCodecInfo(&info);
		printf("track %" B_PRId32 ": %" B_PRIu32 "x%" B_PRIu32 ", decoder "
			"\"%s\" (%s), output %#x, %" B_PRIu32 " bytes a row: %s\n", i,
			width, height, info.pretty_name, info.short_name,
			decoded.u.raw_video.display.format,
			decoded.u.raw_video.display.bytes_per_row, strerror(status));
		if (status != B_OK)
			return 1;

		size_t size = (size_t)decoded.u.raw_video.display.bytes_per_row
			* height * 2;
		uint8* buffer = (uint8*)malloc(size);
		bigtime_t start = system_time();
		bigtime_t cpuStart = team_cpu_time();
		int64 pictures = 0;
		while (pictures < wanted) {
			int64 count = 0;
			media_header header;
			status = track->ReadFrames(buffer, &count, &header);
			if (status != B_OK)
				break;
			pictures += count;
		}
		bigtime_t elapsed = system_time() - start;
		bigtime_t cpu = team_cpu_time() - cpuStart;
		printf("%" B_PRId64 " pictures in %.3f s: %.1f pictures/s, processor "
			"%.3f s (%.0f %% of one core)%s%s\n", pictures,
			elapsed / 1000000.0, pictures * 1000000.0 / elapsed,
			cpu / 1000000.0, 100.0 * cpu / elapsed,
			status != B_OK ? ", ended: " : "",
			status != B_OK ? strerror(status) : "");
		free(buffer);
		file.ReleaseTrack(track);
		return 0;
	}
	fprintf(stderr, "%s: no video track\n", argv[optind]);
	return 1;
}
