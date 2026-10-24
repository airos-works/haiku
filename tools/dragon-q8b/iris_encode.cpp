/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab test of the qcom_video add-on's encoder: pictures (decoded on the
	codec from an elementary stream, or I420 from a file) encoded on the
	codec, the stream written out, decoded again and compared with what
	went in (PSNR).

	iris_encode [--hevc] [--bitrate kbit/s] [--key n] [--cbr]
		(--stream in.h264 | --yuv WxH in.yuv) out.h264|out.h265 */


#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <OS.h>

#include "IrisDecoder.h"
#include "IrisEncoder.h"


struct Picture {
	std::vector<uint8>	data;		// NV12, width bytes a row
	int64				pts;
};


static uint32 sWidth, sHeight;


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


//!	The access units of an Annex B stream (as iris_decode splits them).
static void
split_access_units(const std::vector<uint8>& data, bool hevc,
	std::vector<std::pair<size_t, size_t> >& units)
{
	std::vector<size_t> starts;
	for (size_t i = 0; i + 3 <= data.size(); i++) {
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
			starts.push_back(i > 0 && data[i - 1] == 0 ? i - 1 : i);
			i += 2;
		}
	}
	size_t unitStart = starts.empty() ? 0 : starts[0];
	bool havePicture = false;
	for (size_t s = 0; s < starts.size(); s++) {
		size_t nal = starts[s] + (data[starts[s] + 2] == 1 ? 3 : 4);
		if (nal + 2 >= data.size())
			break;
		bool vcl, begins;
		if (hevc) {
			int type = (data[nal] >> 1) & 0x3f;
			vcl = type < 32;
			begins = vcl ? (data[nal + 2] & 0x80) != 0
				: (type >= 32 && type <= 40) || (type >= 41 && type <= 44);
		} else {
			int type = data[nal] & 0x1f;
			vcl = type >= 1 && type <= 5;
			begins = vcl ? (data[nal + 1] & 0x80) != 0
				: type == 6 || type == 7 || type == 8 || type == 9;
		}
		if (begins && havePicture) {
			units.push_back(std::make_pair(unitStart, starts[s] - unitStart));
			unitStart = starts[s];
			havePicture = false;
		}
		if (vcl)
			havePicture = true;
	}
	if (unitStart < data.size())
		units.push_back(std::make_pair(unitStart, data.size() - unitStart));
}


//!	Decodes \a stream on the codec into NV12 pictures.
static status_t
decode(const std::vector<uint8>& stream, bool hevc,
	std::vector<Picture>& pictures)
{
	std::vector<std::pair<size_t, size_t> > units;
	split_access_units(stream, hevc, units);
	IrisDecoder decoder(hevc ? IrisDecoder::HEVC : IrisDecoder::H264);
	BString error;
	status_t status = decoder.Open(sWidth, sHeight, &error);
	if (status != B_OK) {
		printf("decoder: %s\n", error.String());
		return status;
	}
	auto take = [&](bigtime_t timeout) -> status_t {
		IrisDecoder::Picture picture;
		status_t result = decoder.NextPicture(picture, timeout);
		if (result != B_OK)
			return result;
		Picture out;
		out.data.resize((size_t)picture.width * picture.height * 3 / 2);
		decoder.CopyPlanes(picture, out.data.data(), picture.width, true);
		out.pts = picture.pts;
		sWidth = picture.width;
		sHeight = picture.height;
		pictures.push_back(out);
		decoder.ReleasePicture(picture);
		return B_OK;
	};
	for (size_t i = 0; i < units.size() && status == B_OK; i++) {
		while ((status = decoder.PutAccessUnit(&stream[units[i].first],
				units[i].second, i * 1000, 2000000)) == B_WOULD_BLOCK) {
			if (take(0) != B_OK) {
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
		if (taken != B_OK)
			status = taken;
	}
	if (status != B_OK)
		printf("decoding: %s (%s)\n", strerror(status), decoder.Error());
	return status;
}


static double
psnr(const uint8* a, const uint8* b, size_t size)
{
	double error = 0;
	for (size_t i = 0; i < size; i++) {
		int d = (int)a[i] - b[i];
		error += d * d;
	}
	return error == 0 ? 99 : 10 * log10(255.0 * 255.0 * size / error);
}


int
main(int argc, char** argv)
{
	IrisEncoder::Params params;
	const char* streamPath = NULL;
	const char* yuvPath = NULL;
	bool sourceHevc = false;
	int argument = 1;
	for (; argument < argc && argv[argument][0] == '-'; argument++) {
		const char* option = argv[argument];
		if (strcmp(option, "--hevc") == 0)
			params.codec = IrisEncoder::HEVC;
		else if (strcmp(option, "--cbr") == 0)
			params.constantBitrate = true;
		else if (strcmp(option, "--bitrate") == 0 && argument + 1 < argc)
			params.bitrate = atoi(argv[++argument]) * 1000;
		else if (strcmp(option, "--key") == 0 && argument + 1 < argc)
			params.keyInterval = atoi(argv[++argument]);
		else if (strcmp(option, "--fps") == 0 && argument + 1 < argc)
			params.framesPerSecond = atoi(argv[++argument]);
		else if (strcmp(option, "--stream") == 0 && argument + 1 < argc)
			streamPath = argv[++argument];
		else if (strcmp(option, "--yuv") == 0 && argument + 2 < argc) {
			sscanf(argv[++argument], "%" B_SCNu32 "x%" B_SCNu32, &sWidth,
				&sHeight);
			yuvPath = argv[++argument];
		}
	}
	if (argument >= argc || (streamPath == NULL && yuvPath == NULL)) {
		fprintf(stderr, "usage: %s [--hevc] [--bitrate kbit/s] [--key n] "
			"[--cbr] [--fps n] (--stream in | --yuv WxH in.yuv) out\n",
			argv[0]);
		return 1;
	}
	const char* outPath = argv[argument];

	// the pictures
	std::vector<Picture> source;
	std::vector<uint8> data;
	const char* input = streamPath != NULL ? streamPath : yuvPath;
	if (!read_file(input, data)) {
		fprintf(stderr, "cannot read %s\n", input);
		return 1;
	}
	if (streamPath != NULL) {
		sourceHevc = strstr(streamPath, "265") != NULL;
		if (decode(data, sourceHevc, source) != B_OK)
			return 1;
	} else {
		size_t frameSize = (size_t)sWidth * sHeight * 3 / 2;
		for (size_t offset = 0; offset + frameSize <= data.size();
				offset += frameSize) {
			Picture picture;
			picture.data.resize(frameSize);
			const uint8* in = &data[offset];
			memcpy(picture.data.data(), in, (size_t)sWidth * sHeight);
			const uint8* cb = in + (size_t)sWidth * sHeight;
			const uint8* cr = cb + (size_t)sWidth * sHeight / 4;
			uint8* pairs = picture.data.data() + (size_t)sWidth * sHeight;
			for (size_t i = 0; i < (size_t)sWidth * sHeight / 4; i++) {
				pairs[2 * i] = cb[i];
				pairs[2 * i + 1] = cr[i];
			}
			picture.pts = source.size() * 1000;
			source.push_back(picture);
		}
	}
	// in the order they are shown, a picture time apart: the codec drops
	// pictures whose time goes back
	for (size_t i = 0; i < source.size(); i++)
		source[i].pts = (int64)i * 1000000 / params.framesPerSecond;
	printf("%zu pictures of %" B_PRIu32 "x%" B_PRIu32 " from %s\n",
		source.size(), sWidth, sHeight, input);
	if (source.empty())
		return 1;

	// encoded
	params.width = sWidth;
	params.height = sHeight;
	IrisEncoder encoder;
	BString error;
	if (encoder.Open(params, &error) != B_OK) {
		printf("encoder: %s\n", error.String());
		return 1;
	}
	printf("encoder: firmware %s, %s, %" B_PRIu32 " kbit/s %s, key every %"
		B_PRIu32 "\n", encoder.FirmwareVersion(),
		params.codec == IrisEncoder::HEVC ? "HEVC Main" : "H.264 High",
		params.bitrate / 1000, params.constantBitrate ? "CBR" : "VBR",
		params.keyInterval);

	std::vector<uint8> stream;
	uint32 packets = 0, keys = 0;
	int64 lastPts = -1;
	bool ordered = true;
	auto take = [&](bigtime_t timeout) -> status_t {
		IrisEncoder::Packet packet;
		status_t result = encoder.NextPacket(packet, timeout);
		if (result != B_OK)
			return result;
		if (packets < 4 || packet.key) {
			printf("  packet %" B_PRIu32 ": %zu bytes, pts %" B_PRId64 "%s\n",
				packets, packet.size, packet.pts, packet.key ? ", key" : "");
		}
		if (packet.pts <= lastPts)
			ordered = false;
		lastPts = packet.pts;
		stream.insert(stream.end(), packet.data, packet.data + packet.size);
		packets++;
		if (packet.key)
			keys++;
		encoder.ReleasePacket(packet);
		return B_OK;
	};

	bigtime_t start = system_time();
	status_t status = B_OK;
	for (size_t i = 0; i < source.size() && status == B_OK; i++) {
		const uint8* luma = source[i].data.data();
		const uint8* chroma = luma + (size_t)sWidth * sHeight;
		while ((status = encoder.PutPicture(luma, sWidth, chroma, sWidth,
				source[i].pts, 2000000)) == B_WOULD_BLOCK) {
			if (take(0) != B_OK) {
				printf("picture %zu: the codec takes no more\n", i);
				status = B_TIMED_OUT;
				break;
			}
		}
		while (status == B_OK && take(0) == B_OK)
			;
	}
	if (status == B_OK)
		status = encoder.EndOfStream();
	while (status == B_OK) {
		status_t taken = take(2000000);
		if (taken == B_LAST_BUFFER_ERROR)
			break;
		if (taken != B_OK) {
			printf("waiting for the stream: %s\n", strerror(taken));
			status = taken;
		}
	}
	bigtime_t elapsed = system_time() - start;
	if (status != B_OK)
		printf("encoding: %s (%s)\n", strerror(status), encoder.Error());
	BString parameterSets;
	parameterSets.SetToFormat("%zu bytes of parameter sets",
		encoder.ParameterSets().size());
	encoder.Close();

	double seconds = source.size() / (double)params.framesPerSecond;
	printf("%" B_PRIu32 " packets (%" B_PRIu32 " key) of %zu pictures in %"
		B_PRId64 " ms, %.1f pictures/s; %zu bytes, %.0f kbit/s at %" B_PRIu32
		" pictures/s; %s; %s order\n", packets, keys, source.size(),
		elapsed / 1000, source.size() * 1000000.0 / elapsed, stream.size(),
		stream.size() * 8 / seconds / 1000, params.framesPerSecond,
		parameterSets.String(), ordered ? "input" : "NOT input");

	FILE* out = fopen(outPath, "wb");
	if (out != NULL) {
		fwrite(stream.data(), 1, stream.size(), out);
		fclose(out);
	}
	if (status != B_OK || stream.empty())
		return 1;

	// decoded again
	std::vector<Picture> decoded;
	if (decode(stream, params.codec == IrisEncoder::HEVC, decoded) != B_OK)
		return 1;
	size_t count = decoded.size() < source.size() ? decoded.size()
		: source.size();
	double total = 0, worst = 1000;
	size_t lumaSize = (size_t)sWidth * sHeight;
	for (size_t i = 0; i < count; i++) {
		double value = psnr(source[i].data.data(), decoded[i].data.data(),
			lumaSize);
		total += value;
		if (value < worst)
			worst = value;
	}
	printf("decoded again: %zu pictures; luma PSNR %.2f dB on average, %.2f "
		"dB the worst\n", decoded.size(), count > 0 ? total / count : 0.0,
		worst);
	return decoded.size() == source.size() && worst > 30 ? 0 : 1;
}
