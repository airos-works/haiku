/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	cubie_audio_test: drives a multi_audio (hmulti) driver directly, without
	the Media Kit, the way the hmulti media add-on does: description, format,
	buffers, then buffer exchanges for a few seconds. It plays a sine on every
	output channel, reads the input channels back, and reports how fast the
	buffers really came: the sample rate the device consumes and produces.

	cubie_audio_test [-d device] [-r rate] [-s seconds] [-f frequency]

	The device defaults to the first one in /dev/audio/hmulti/usb. The driver
	takes one opener: stop the media services first (Media preferences, or
	kill media_addon_server media_server). Exits 1 when the device fails or
	the measured rate is more than 1% off.
*/

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include <hmulti_audio.h>


static const struct {
	uint32	id;
	uint32	rate;
} kRates[] = {
	{ B_SR_8000, 8000 }, { B_SR_11025, 11025 }, { B_SR_12000, 12000 },
	{ B_SR_16000, 16000 }, { B_SR_22050, 22050 }, { B_SR_24000, 24000 },
	{ B_SR_32000, 32000 }, { B_SR_44100, 44100 }, { B_SR_48000, 48000 },
	{ B_SR_64000, 64000 }, { B_SR_88200, 88200 }, { B_SR_96000, 96000 },
	{ B_SR_176400, 176400 }, { B_SR_192000, 192000 },
	{ B_SR_384000, 384000 },
};

static const int32 kMaxChannels = 32;
static const int32 kMaxBuffers = 8;


static uint32
rate_of(uint32 id)
{
	for (size_t i = 0; i < B_COUNT_OF(kRates); i++) {
		if (kRates[i].id == id)
			return kRates[i].rate;
	}
	return 0;
}


static uint32
id_of(uint32 rate)
{
	for (size_t i = 0; i < B_COUNT_OF(kRates); i++) {
		if (kRates[i].rate == rate)
			return kRates[i].id;
	}
	return 0;
}


static uint32
highest_rate(uint32 ids)
{
	for (size_t i = B_COUNT_OF(kRates); i-- > 0;) {
		if ((ids & kRates[i].id) != 0)
			return kRates[i].id;
	}
	return 0;
}


static void
print_rates(const char* what, uint32 ids)
{
	printf("%s rates:", what);
	for (size_t i = 0; i < B_COUNT_OF(kRates); i++) {
		if ((ids & kRates[i].id) != 0)
			printf(" %" B_PRIu32, kRates[i].rate);
	}
	printf("%s\n", ids == 0 ? " none" : "");
}


static uint32
pick_format(uint32 formats)
{
	// what the hmulti add-on would pick, among those written here
	static const uint32 kFormats[] = { B_FMT_32BIT, B_FMT_24BIT, B_FMT_20BIT,
		B_FMT_18BIT, B_FMT_16BIT, B_FMT_8BIT_S, B_FMT_8BIT_U, B_FMT_FLOAT };
	for (size_t i = 0; i < B_COUNT_OF(kFormats); i++) {
		if ((formats & kFormats[i]) != 0)
			return kFormats[i];
	}
	return 0;
}


static const char*
format_name(uint32 format)
{
	switch (format) {
		case B_FMT_8BIT_S: return "8 bit signed";
		case B_FMT_8BIT_U: return "8 bit unsigned";
		case B_FMT_16BIT: return "16 bit";
		case B_FMT_18BIT: return "18 bit";
		case B_FMT_20BIT: return "20 bit";
		case B_FMT_24BIT: return "24 bit";
		case B_FMT_32BIT: return "32 bit";
		case B_FMT_FLOAT: return "float";
	}
	return "?";
}


static void
write_sample(char* to, uint32 format, double value)
{
	switch (format) {
		case B_FMT_8BIT_S:
			*(int8*)to = (int8)(value * 127);
			break;
		case B_FMT_8BIT_U:
			*(uint8*)to = (uint8)(value * 127 + 128);
			break;
		case B_FMT_16BIT:
			*(int16*)to = (int16)(value * 32767);
			break;
		case B_FMT_18BIT:
		case B_FMT_20BIT:
		case B_FMT_24BIT:
		case B_FMT_32BIT:
			*(int32*)to = (int32)(value * 2147483647.0);
			break;
		case B_FMT_FLOAT:
			*(float*)to = (float)value;
			break;
	}
}


static double
read_sample(const char* from, uint32 format)
{
	switch (format) {
		case B_FMT_8BIT_S:
			return *(const int8*)from / 128.0;
		case B_FMT_8BIT_U:
			return (*(const uint8*)from - 128) / 128.0;
		case B_FMT_16BIT:
			return *(const int16*)from / 32768.0;
		case B_FMT_18BIT:
		case B_FMT_20BIT:
		case B_FMT_24BIT:
		case B_FMT_32BIT:
			return *(const int32*)from / 2147483648.0;
		case B_FMT_FLOAT:
			return *(const float*)from;
	}
	return 0;
}


static bool
first_device(char* path, size_t size)
{
	const char* base = "/dev/audio/hmulti/usb";
	DIR* dir = opendir(base);
	if (dir == NULL)
		return false;

	bool found = false;
	while (dirent* entry = readdir(dir)) {
		if (entry->d_name[0] == '.')
			continue;
		snprintf(path, size, "%s/%s", base, entry->d_name);
		found = true;
		break;
	}
	closedir(dir);
	return found;
}


static status_t
control(int device, uint32 op, void* data, size_t size, const char* what)
{
	if (ioctl(device, op, data, size) < 0) {
		fprintf(stderr, "%s failed: %s\n", what, strerror(errno));
		return errno;
	}
	return B_OK;
}


struct StreamStats {
	int64		buffers;
	bigtime_t	firstTime;
	bigtime_t	lastTime;
	int64		firstFrames;
	int64		lastFrames;
	bigtime_t	longestGap;
	double		peak;
	double		sumSquares;
	int64		samples;

	StreamStats() { memset(this, 0, sizeof(*this)); }

	void Exchanged(bigtime_t when, int64 frames)
	{
		if (buffers == 0) {
			firstTime = when;
			firstFrames = frames;
		} else if (when - lastTime > longestGap)
			longestGap = when - lastTime;
		lastTime = when;
		lastFrames = frames;
		buffers++;
	}

	double Rate() const
	{
		if (lastTime <= firstTime)
			return 0;
		return (lastFrames - firstFrames) * 1000000.0
			/ (lastTime - firstTime);
	}
};


static bool
report(const char* what, const StreamStats& stats, uint32 rate)
{
	double measured = stats.Rate();
	double error = rate != 0 ? (measured - rate) * 100.0 / rate : 0;
	printf("%s: %" B_PRId64 " buffers, %" B_PRId64 " frames in %.3f s: "
		"%.1f frames/s (%+.2f%% of %" B_PRIu32 "), longest gap %.1f ms",
		what, stats.buffers, stats.lastFrames - stats.firstFrames,
		(stats.lastTime - stats.firstTime) / 1000000.0, measured, error, rate,
		stats.longestGap / 1000.0);
	if (stats.samples > 0) {
		printf(", input peak %.4f rms %.4f", stats.peak,
			sqrt(stats.sumSquares / stats.samples));
	}
	printf("\n");
	return stats.buffers >= 3 && fabs(error) <= 1.0;
}


int
main(int argc, char** argv)
{
	char path[B_PATH_NAME_LENGTH] = "";
	uint32 wantedRate = 0;
	double seconds = 5;
	double frequency = 1000;

	int option;
	while ((option = getopt(argc, argv, "d:r:s:f:h")) != -1) {
		switch (option) {
			case 'd':
				strlcpy(path, optarg, sizeof(path));
				break;
			case 'r':
				wantedRate = strtoul(optarg, NULL, 0);
				break;
			case 's':
				seconds = atof(optarg);
				break;
			case 'f':
				frequency = atof(optarg);
				break;
			default:
				fprintf(stderr, "usage: %s [-d device] [-r rate] [-s seconds] "
					"[-f frequency]\n", argv[0]);
				return 2;
		}
	}

	if (path[0] == '\0' && !first_device(path, sizeof(path))) {
		fprintf(stderr, "no device in /dev/audio/hmulti/usb\n");
		return 1;
	}

	int device = open(path, O_RDWR);
	if (device < 0) {
		fprintf(stderr, "%s: %s (the media services keep it open: stop them "
			"first)\n", path, strerror(errno));
		return 1;
	}

	multi_channel_info channels[kMaxChannels];
	multi_description description;
	memset(&description, 0, sizeof(description));
	description.info_size = sizeof(description);
	description.request_channel_count = kMaxChannels;
	description.channels = channels;
	if (control(device, B_MULTI_GET_DESCRIPTION, &description,
			sizeof(description), "B_MULTI_GET_DESCRIPTION") != B_OK) {
		return 1;
	}

	printf("%s: \"%s\" (%s), %" B_PRId32 " outputs, %" B_PRId32 " inputs\n",
		path, description.friendly_name, description.vendor_info,
		description.output_channel_count, description.input_channel_count);
	print_rates("output", description.output_rates);
	print_rates("input", description.input_rates);

	int32 channelCount = description.output_channel_count
		+ description.input_channel_count;
	uint32 enabled = channelCount >= 32 ? 0xffffffff : (1u << channelCount) - 1;
	multi_channel_enable enable;
	memset(&enable, 0, sizeof(enable));
	enable.info_size = sizeof(enable);
	enable.enable_bits = (uchar*)&enabled;
	enable.lock_source = B_MULTI_LOCK_INTERNAL;
	control(device, B_MULTI_SET_ENABLED_CHANNELS, &enable, sizeof(enable),
		"B_MULTI_SET_ENABLED_CHANNELS");

	multi_format_info format;
	memset(&format, 0, sizeof(format));
	format.info_size = sizeof(format);
	format.output.rate = wantedRate != 0 ? id_of(wantedRate)
		: highest_rate(description.output_rates);
	format.output.format = pick_format(description.output_formats);
	format.input.rate = wantedRate != 0 ? id_of(wantedRate)
		: highest_rate(description.input_rates);
	format.input.format = pick_format(description.input_formats);
	if ((wantedRate != 0 && format.output.rate == 0)
		|| (description.output_channel_count > 0
			&& (description.output_rates & format.output.rate) == 0)) {
		fprintf(stderr, "the device does not play at %" B_PRIu32 " Hz\n",
			wantedRate);
		return 1;
	}
	control(device, B_MULTI_SET_GLOBAL_FORMAT, &format, sizeof(format),
		"B_MULTI_SET_GLOBAL_FORMAT");
	if (control(device, B_MULTI_GET_GLOBAL_FORMAT, &format, sizeof(format),
			"B_MULTI_GET_GLOBAL_FORMAT") != B_OK) {
		return 1;
	}
	uint32 outputRate = rate_of(format.output.rate);
	uint32 inputRate = rate_of(format.input.rate);
	printf("format: output %" B_PRIu32 " Hz %s, input %" B_PRIu32 " Hz %s\n",
		outputRate, format_name(format.output.format), inputRate,
		format_name(format.input.format));

	buffer_desc playBuffers[kMaxBuffers][kMaxChannels];
	buffer_desc recordBuffers[kMaxBuffers][kMaxChannels];
	buffer_desc* playList[kMaxBuffers];
	buffer_desc* recordList[kMaxBuffers];
	for (int32 i = 0; i < kMaxBuffers; i++) {
		playList[i] = playBuffers[i];
		recordList[i] = recordBuffers[i];
	}

	multi_buffer_list buffers;
	memset(&buffers, 0, sizeof(buffers));
	buffers.info_size = sizeof(buffers);
	buffers.request_playback_buffers = kMaxBuffers;
	buffers.request_playback_channels = description.output_channel_count;
	buffers.playback_buffers = playList;
	buffers.request_record_buffers = kMaxBuffers;
	buffers.request_record_channels = description.input_channel_count;
	buffers.record_buffers = recordList;
	if (control(device, B_MULTI_GET_BUFFERS, &buffers, sizeof(buffers),
			"B_MULTI_GET_BUFFERS") != B_OK) {
		return 1;
	}
	printf("buffers: playback %" B_PRId32 " x %" B_PRId32 " frames, %" B_PRId32
		" channels; record %" B_PRId32 " x %" B_PRId32 " frames, %" B_PRId32
		" channels\n", buffers.return_playback_buffers,
		buffers.return_playback_buffer_size, buffers.return_playback_channels,
		buffers.return_record_buffers, buffers.return_record_buffer_size,
		buffers.return_record_channels);

	multi_buffer_info info;
	memset(&info, 0, sizeof(info));
	info.info_size = sizeof(info);
	info.playback_buffer_cycle = -1;
	info.record_buffer_cycle = -1;

	StreamStats played;
	StreamStats recorded;
	int64 phase = 0;
	int64 lastPlayed = -1;
	int64 lastRecorded = -1;
	int32 failures = 0;
	bigtime_t end = system_time() + (bigtime_t)(seconds * 1000000);

	while (system_time() < end && failures < 10) {
		if (ioctl(device, B_MULTI_BUFFER_EXCHANGE, &info, sizeof(info)) < 0) {
			fprintf(stderr, "B_MULTI_BUFFER_EXCHANGE failed: %s\n",
				strerror(errno));
			failures++;
			continue;
		}

		// usb_audio reports one stream per exchange, and counts its
		// frames on in the struct passed back to it
		if (buffers.return_playback_channels > 0
			&& info.played_frames_count != lastPlayed) {
			lastPlayed = info.played_frames_count;
			played.Exchanged(info.played_real_time, info.played_frames_count);

			int32 cycle = info.playback_buffer_cycle;
			if (cycle >= 0 && cycle < buffers.return_playback_buffers) {
				for (uint32 frame = 0;
						frame < buffers.return_playback_buffer_size; frame++) {
					double value = 0.5 * sin(2 * M_PI * frequency
						* (phase + frame) / (outputRate != 0 ? outputRate : 1));
					for (int32 channel = 0;
							channel < buffers.return_playback_channels;
							channel++) {
						buffer_desc& buffer = playList[cycle][channel];
						write_sample(buffer.base + frame * buffer.stride,
							format.output.format, value);
					}
				}
				phase += buffers.return_playback_buffer_size;
			}
		}

		if (buffers.return_record_channels > 0
			&& info.recorded_frames_count != lastRecorded) {
			lastRecorded = info.recorded_frames_count;
			recorded.Exchanged(info.recorded_real_time,
				info.recorded_frames_count);

			int32 cycle = info.record_buffer_cycle;
			if (cycle >= 0 && cycle < buffers.return_record_buffers) {
				for (uint32 frame = 0;
						frame < buffers.return_record_buffer_size; frame++) {
					for (int32 channel = 0;
							channel < buffers.return_record_channels;
							channel++) {
						buffer_desc& buffer = recordList[cycle][channel];
						double value = read_sample(
							buffer.base + frame * buffer.stride,
							format.input.format);
						if (fabs(value) > recorded.peak)
							recorded.peak = fabs(value);
						recorded.sumSquares += value * value;
						recorded.samples++;
					}
				}
			}
		}
	}

	control(device, B_MULTI_BUFFER_FORCE_STOP, NULL, 0,
		"B_MULTI_BUFFER_FORCE_STOP");
	close(device);

	bool ok = failures == 0;
	if (buffers.return_playback_channels > 0)
		ok = report("playback", played, outputRate) && ok;
	if (buffers.return_record_channels > 0)
		ok = report("record", recorded, inputRate) && ok;
	printf("%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
