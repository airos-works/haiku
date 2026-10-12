/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "H264Stream.h"
#include "UvdH264.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __HAIKU__
#include <amdgpu_haiku.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

static void Require(bool ok, const char* why)
{
	if (!ok) { fprintf(stderr, "FAIL: %s (%s)\n", why, strerror(errno)); exit(1); }
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	bool parseOnly = argc == 3 && strcmp(argv[1], "--parse") == 0;
	if (!parseOnly && (argc < 3 || argc > 4)) {
		fprintf(stderr, "usage: amdgpu_video_stream input.au output.frames [--unprivileged]\n"
			"       amdgpu_video_stream --parse input.au\n"); return 2;
	}
#ifndef __HAIKU__
	Require(parseOnly, "host build supports --parse only");
#endif
	FILE* input = fopen(argv[parseOnly ? 2 : 1], "rb");
	FILE* output = parseOnly ? NULL : fopen(argv[2], "wb");
	Require(input != NULL && (parseOnly || output != NULL), "open input/output");
#ifdef __HAIKU__
	if (argc == 4) Require(strcmp(argv[3], "--unprivileged") == 0
		&& setgid(65534) == 0 && setuid(65534) == 0, "drop privileges");
	int fd = parseOnly ? -1 : open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	Require(parseOnly || fd >= 0, "open AMD device");
	amdgpu_video_create session = {};
#endif
	H264Stream stream;
	unsigned frames = 0;
	for (;;) {
		uint8_t length[4];
		size_t got = fread(length, 1, 4, input);
		if (got == 0) { Require(feof(input), "read access-unit size"); break; }
		Require(got == 4, "complete access-unit size");
		uint32_t bytes = length[0] | (uint32_t)length[1] << 8 | (uint32_t)length[2] << 16
			| (uint32_t)length[3] << 24;
		Require(bytes > 0 && bytes <= AMDGPU_VIDEO_MAX_BITSTREAM, "access-unit bounds");
		std::vector<uint8_t> data(bytes);
		Require(fread(data.data(), 1, bytes, input) == bytes, "complete access unit");
		if (!stream.Prepare(data.data(), bytes)) {
			fprintf(stderr, "frame %u: %s\n", frames, stream.Error()); return 1;
		}
		amdgpu::UvdH264Layout layout;
		Require(amdgpu::UvdH264Size(stream.config, layout)
			&& amdgpu::UvdH264Validate(stream.config, stream.picture, stream.bitstream.size())
			&& amdgpu::UvdH264Bitstream(stream.bitstream.data(), stream.bitstream.size(),
				(stream.picture.flags & AMDGPU_H264_IDR) != 0), "kernel metadata contract");
#ifdef __HAIKU__
		if (!parseOnly) {
			if (session.handle == 0 || memcmp(&session.config, &stream.config, sizeof(stream.config)) != 0) {
				Require((stream.picture.flags & AMDGPU_H264_IDR) != 0, "format change requires IDR");
				if (session.handle != 0) {
					amdgpu_video_destroy d = {AMDGPU_HAIKU_ABI_VERSION, sizeof(d), session.handle};
					Require(ioctl(fd, AMDGPU_VIDEO_DESTROY, &d, sizeof(d)) == 0, "destroy old format");
				}
				session = {}; session.version = AMDGPU_HAIKU_ABI_VERSION; session.size = sizeof(session);
				session.config = stream.config;
				Require(ioctl(fd, AMDGPU_VIDEO_CREATE, &session, sizeof(session)) == 0, "create video session");
			}
			std::vector<uint8_t> pixels(session.output_bytes);
			amdgpu_video_decode d = {};
			d.version = AMDGPU_HAIKU_ABI_VERSION; d.size = sizeof(d); d.handle = session.handle;
			d.bitstream = (addr_t)stream.bitstream.data(); d.bitstream_bytes = stream.bitstream.size();
			d.output = (addr_t)pixels.data(); d.output_capacity = pixels.size(); d.picture = stream.picture;
			Require(ioctl(fd, AMDGPU_VIDEO_DECODE, &d, sizeof(d)) == 0, "decode access unit");
			Require(d.sequence == d.fence && d.rptr == d.wptr && !d.guard_mismatches
				&& !(d.vm_fault_status & 0xff), "GPU completion");
			uint32_t header[] = {0x55445631, frames, (uint32_t)stream.poc, stream.sequence,
				stream.config.width, stream.config.height, session.pitch, session.output_bytes,
				(uint32_t)stream.cropLeft, (uint32_t)stream.cropTop, (uint32_t)stream.width, (uint32_t)stream.height};
			Require(fwrite(header, 1, sizeof(header), output) == sizeof(header)
				&& fwrite(pixels.data(), 1, pixels.size(), output) == pixels.size(), "save frame record");
			printf("frame %u sequence %u POC %d %ux%u fence %u ring %u/%u\n",
				frames, stream.sequence, stream.poc, stream.config.width, stream.config.height,
				d.fence, d.rptr, d.wptr);
		}
#endif
		if (!stream.Commit()) Require(false, stream.Error());
		frames++;
	}
	Require(frames != 0, "at least one picture");
#ifdef __HAIKU__
	if (!parseOnly) {
		amdgpu_video_destroy d = {AMDGPU_HAIKU_ABI_VERSION, sizeof(d), session.handle};
		Require(ioctl(fd, AMDGPU_VIDEO_DESTROY, &d, sizeof(d)) == 0, "destroy final session");
		close(fd);
	}
#endif
	fclose(input);
	if (output != NULL) Require(fclose(output) == 0, "finish output");
	printf("PASS: %u access units %s; native pixels require independent comparison\n",
		frames, parseOnly ? "parsed and validated" : "decoded");
}
