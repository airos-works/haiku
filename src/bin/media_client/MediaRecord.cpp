/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "MediaRecord.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <MediaRecorder.h>
#include <MediaRoster.h>


struct RecordStatistics {
	media_raw_audio_format	format;
	uint64					bytes;
	uint32					buffers;
	float					peak[8];
};


/*!	Each buffer from the audio input: its size and the peak of each
	channel, as a fraction of full scale. */
static void
record_hook(void* cookie, bigtime_t timestamp, void* data, size_t size,
	const media_format& format)
{
	RecordStatistics& statistics = *(RecordStatistics*)cookie;
	const media_raw_audio_format& raw = format.u.raw_audio;
	statistics.format = raw;
	statistics.bytes += size;
	statistics.buffers++;
	uint32 channels = raw.channel_count;
	if (channels == 0 || channels > 8)
		return;
	if (raw.format == media_raw_audio_format::B_AUDIO_SHORT) {
		const int16* samples = (const int16*)data;
		for (size_t i = 0; i < size / sizeof(int16); i++) {
			float value = fabsf(samples[i] / 32768.0f);
			if (value > statistics.peak[i % channels])
				statistics.peak[i % channels] = value;
		}
	} else if (raw.format == media_raw_audio_format::B_AUDIO_FLOAT) {
		const float* samples = (const float*)data;
		for (size_t i = 0; i < size / sizeof(float); i++) {
			float value = fabsf(samples[i]);
			if (value > statistics.peak[i % channels])
				statistics.peak[i % channels] = value;
		}
	}
}


/*!	Records \a duration from the system's audio input, as Sound Recorder
	connects to it, and says what came: the format, how many frames, each
	channel's peak. */
int
media_record(bigtime_t duration)
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node input;
	status_t status = roster->GetAudioInput(&input);
	if (status != B_OK) {
		printf("No audio input: %s\n", strerror(status));
		return 2;
	}
	media_output output;
	int32 count = 0;
	status = roster->GetFreeOutputsFor(input, &output, 1, &count,
		B_MEDIA_RAW_AUDIO);
	if (status != B_OK || count < 1) {
		printf("The audio input has no free output: %s\n",
			strerror(status != B_OK ? status : B_BUSY));
		roster->ReleaseNode(input);
		return 2;
	}

	BMediaRecorder recorder("media_client", B_MEDIA_RAW_AUDIO);
	if (recorder.InitCheck() != B_OK) {
		printf("No recorder: %s\n", strerror(recorder.InitCheck()));
		roster->ReleaseNode(input);
		return 2;
	}
	RecordStatistics statistics;
	memset(&statistics, 0, sizeof(statistics));
	media_format format;
	format.type = B_MEDIA_RAW_AUDIO;
	format.u.raw_audio = output.format.u.raw_audio;
	recorder.SetHooks(record_hook, NULL, &statistics);
	status = recorder.Connect(input, &output, &format);
	if (status == B_OK)
		status = recorder.Start();
	if (status != B_OK) {
		printf("Recording does not start: %s\n", strerror(status));
		recorder.SetHooks(NULL, NULL, NULL);
		roster->ReleaseNode(input);
		return 2;
	}
	printf("Recording from \"%s\" for %g s...\n", input.node != 0
		? output.name : "?", duration / 1000000.0);
	snooze(duration);
	recorder.Stop();
	recorder.Disconnect();
	recorder.SetHooks(NULL, NULL, NULL);
	roster->ReleaseNode(input);

	const media_raw_audio_format& raw = statistics.format;
	uint32 frameSize = raw.channel_count
		* (raw.format & media_raw_audio_format::B_AUDIO_SIZE_MASK);
	printf("%" B_PRIu32 " buffers, %" B_PRIu64 " frames at %g Hz, %" B_PRIu32
		" channels, format %#" B_PRIx32 "; peaks:", statistics.buffers,
		frameSize != 0 ? statistics.bytes / frameSize : 0, raw.frame_rate,
		raw.channel_count, raw.format);
	for (uint32 channel = 0; channel < raw.channel_count && channel < 8;
			channel++)
		printf(" %.4f", statistics.peak[channel]);
	printf("\n");
	return statistics.buffers > 0 ? 0 : 1;
}
