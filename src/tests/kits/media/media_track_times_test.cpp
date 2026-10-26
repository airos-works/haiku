/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	media_track_times_test <file>: decodes the first frames of each track and
	checks that media_header::start_time is in microseconds (frames so far /
	rate). The ffmpeg plugin used to report it in the stream's time base.
*/


#include <MediaFile.h>
#include <MediaTrack.h>
#include <Entry.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv)
{
	entry_ref ref;
	if (argc < 2 || get_ref_for_path(argv[1], &ref) != B_OK)
		return 2;
	BMediaFile file(&ref);
	if (file.InitCheck() != B_OK) {
		printf("BMediaFile: %s\n", strerror(file.InitCheck()));
		return 1;
	}
	int bad = 0;
	for (int32 t = 0; t < file.CountTracks(); t++) {
		BMediaTrack* track = file.TrackAt(t);
		media_format format;
		track->EncodedFormat(&format);
		bool video = format.IsVideo();
		media_format decoded;
		if (video) {
			decoded.type = B_MEDIA_RAW_VIDEO;
			decoded.u.raw_video = media_raw_video_format::wildcard;
			decoded.u.raw_video.display.format = B_RGB32;
		} else {
			decoded.type = B_MEDIA_RAW_AUDIO;
			decoded.u.raw_audio = media_raw_audio_format::wildcard;
		}
		track->DecodedFormat(&decoded);
		size_t size = video ? decoded.u.raw_video.display.line_width
				* decoded.u.raw_video.display.line_count * 4
			: decoded.u.raw_audio.buffer_size;
		char* buffer = (char*)malloc(size > 0 ? size : 1 << 20);
		double rate = video ? decoded.u.raw_video.field_rate
			: decoded.u.raw_audio.frame_rate;
		int64 framesSoFar = 0;
		for (int i = 0; i < 8; i++) {
			int64 count = 0;
			media_header header;
			if (track->ReadFrames(buffer, &count, &header) != B_OK)
				break;
			bigtime_t expected = (bigtime_t)(framesSoFar * 1000000.0 / rate);
			printf("%s %d: start_time %lld expected %lld, CurrentTime %lld\n",
				video ? "video" : "audio", i, (long long)header.start_time,
				(long long)expected, (long long)track->CurrentTime());
			if (header.start_time > expected + 50000
				|| header.start_time < expected - 50000)
				bad++;
			framesSoFar += count;
		}
		file.ReleaseTrack(track);
		free(buffer);
	}
	printf("%s\n", bad ? "WRONG start times" : "start times ok");
	return bad != 0;
}
