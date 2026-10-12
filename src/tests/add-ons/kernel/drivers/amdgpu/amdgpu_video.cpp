/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <amdgpu_haiku.h>
#include "UvdFixture.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

static void Require(bool value, const char* why)
{
	if (!value) { fprintf(stderr, "FAIL: %s: %s\n", why, strerror(errno)); exit(1); }
}
template<typename T> static T Request()
{
	T result = {}; result.version = AMDGPU_HAIKU_ABI_VERSION; result.size = sizeof(T); return result;
}
static amdgpu_video_create Create(int fd)
{
	auto c = Request<amdgpu_video_create>();
	c.config = {864, 480, 100, 30, 2, 0};
	Require(ioctl(fd, AMDGPU_VIDEO_CREATE, &c, sizeof(c)) == 0, "create private video session");
	Require(c.handle != 0 && c.pitch == 1024 && c.output_bytes == 737280, "linear NV12 layout");
	printf("session %llu pitch %u output %u private bytes %llu\n",
		(unsigned long long)c.handle, (unsigned)c.pitch, (unsigned)c.output_bytes,
		(unsigned long long)c.allocated_bytes);
	return c;
}
static amdgpu_video_decode DecodeRequest(const amdgpu_video_create& c, void* output)
{
	auto d = Request<amdgpu_video_decode>();
	d.handle = c.handle; d.bitstream = (addr_t)uvd_bitstream; d.bitstream_bytes = sizeof(uvd_bitstream);
	d.output = (addr_t)output; d.output_capacity = c.output_bytes;
	auto& p = d.picture;
	p.flags = AMDGPU_H264_IDR; p.sps_flags = 5; p.pps_flags = 0x88;
	p.log2_frame_num_minus4 = 1; p.log2_poc_lsb_minus4 = 3; p.initial_qp_minus26 = 2;
	memset(p.scaling4x4, 16, sizeof(p.scaling4x4)); memset(p.scaling8x8, 16, sizeof(p.scaling8x8));
	return d;
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--unprivileged") != 0)) {
		fprintf(stderr, "usage: amdgpu_video output-prefix [--unprivileged]\n"); return 2;
	}
	FILE* files[8];
	for (unsigned i = 0; i < 8; i++) {
		char path[1024];
		Require(snprintf(path, sizeof(path), "%s.%u.nv12", argv[1], i) < (int)sizeof(path), "path");
		files[i] = fopen(path, "wb"); Require(files[i] != NULL, "output file");
	}
	if (argc == 3) Require(setgid(65534) == 0 && setuid(65534) == 0, "drop privileges");
	printf("video client UID %u\n", (unsigned)geteuid());
	int a = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	int b = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	int ro = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY);
	Require(a >= 0 && b >= 0 && ro >= 0, "open clients");
	auto c = Request<amdgpu_video_create>(); c.config = {864, 480, 100, 30, 2, 0};
	Require(ioctl(ro, AMDGPU_VIDEO_CREATE, &c, sizeof(c)) == -1 && errno == B_NOT_ALLOWED,
		"read-only create denied"); close(ro);
	c.version++;
	Require(ioctl(a, AMDGPU_VIDEO_CREATE, &c, sizeof(c)) == -1 && errno == B_BAD_VALUE, "version");
	c.version--; c.config.width = 0xffffffff;
	Require(ioctl(a, AMDGPU_VIDEO_CREATE, &c, sizeof(c)) == -1 && errno == B_BAD_VALUE, "extent");
	Require(ioctl(a, AMDGPU_VIDEO_CREATE, NULL, sizeof(c)) == -1 && errno == B_BAD_ADDRESS, "null");
	auto ca = Create(a), cb = Create(b);
	Require(ca.handle != cb.handle, "distinct sessions");
	Require(ioctl(a, AMDGPU_VIDEO_CREATE, &ca, sizeof(ca)) == -1 && errno == B_BUSY,
		"second session on same file rejected");
	std::vector<uint8> output(ca.output_bytes);
	auto da = DecodeRequest(ca, output.data()), db = DecodeRequest(cb, output.data());
	Require(ioctl(b, AMDGPU_VIDEO_DECODE, &da, sizeof(da)) == -1 && errno == B_BAD_VALUE,
		"cross-file handle rejected");
	auto bad = da; bad.picture.log2_frame_num_minus4 = 255;
	Require(ioctl(a, AMDGPU_VIDEO_DECODE, &bad, sizeof(bad)) == -1 && errno == B_BAD_VALUE,
		"unbounded picture fields rejected");
	bad = da; bad.bitstream = 1;
	Require(ioctl(a, AMDGPU_VIDEO_DECODE, &bad, sizeof(bad)) == -1 && errno == B_BAD_ADDRESS,
		"bad input pointer rejected");
	uint8 invalid[] = {0, 0, 1, 0x67, 0x80}; bad = da;
	bad.bitstream = (addr_t)invalid; bad.bitstream_bytes = sizeof(invalid);
	Require(ioctl(a, AMDGPU_VIDEO_DECODE, &bad, sizeof(bad)) == -1 && errno == B_BAD_VALUE,
		"parameter sets cannot enter firmware bitstream");
	uint32 lastSequence = 0;
	for (unsigned i = 0; i < 8; i++) {
		auto d = (i & 1) ? db : da;
		if (i != 0) {
			for (uint64 address : {0xfffffffffffff000ULL, 0x7fffffffffffULL,
					0xfffffffffffffe00ULL}) {
				for (uint32 which = 0; which < 2; which++) {
					auto invalidPointer = da;
					if (which == 0) invalidPointer.bitstream = address;
					else invalidPointer.output = address;
					Require(ioctl(a, AMDGPU_VIDEO_DECODE, &invalidPointer,
						sizeof(invalidPointer)) == -1 && errno == B_BAD_ADDRESS,
						"kernel/cross-boundary/wrapping video pointer rejected");
				}
			}
		}
		Require(ioctl((i & 1) ? b : a, AMDGPU_VIDEO_DECODE, &d, sizeof(d)) == 0, "decode");
		printf("frame %u fence %u/%u ring %u/%u guards %u VM %#x feedback", i,
			d.sequence, d.fence, d.rptr, d.wptr, d.guard_mismatches, d.vm_fault_status);
		for (uint32 value : d.feedback) printf(" %#x", value);
		puts("");
		Require(d.sequence != 0 && d.fence == d.sequence && d.rptr == d.wptr
			&& d.guard_mismatches == 0 && (d.vm_fault_status & 0xff) == 0, "GPU completion");
		Require(i == 0 || d.sequence == lastSequence + 1,
			"rejected pointers never submit firmware work");
		lastSequence = d.sequence;
		Require(fwrite(output.data(), 1, output.size(), files[i]) == output.size(), "save output");
		Require(fclose(files[i]) == 0, "close output");
	}
	auto destroy = Request<amdgpu_video_destroy>(); destroy.handle = ca.handle;
	Require(ioctl(a, AMDGPU_VIDEO_DESTROY, &destroy, sizeof(destroy)) == 0, "destroy");
	Require(ioctl(a, AMDGPU_VIDEO_DECODE, &da, sizeof(da)) == -1 && errno == B_BAD_VALUE, "stale handle");
	close(b); // implicit destruction must retire before the allocation is reusable
	auto info = Request<amdgpu_memory_info>();
	Require(ioctl(a, AMDGPU_MEMORY_INFO, &info, sizeof(info)) == 0 && info.client_bytes == 0
		&& info.allocated_bytes == 0 && !info.faulted, "all private video memory reclaimed");
	auto ram = Request<amdgpu_gart_info>();
	Require(ioctl(a, AMDGPU_GART_INFO, &ram, sizeof(ram)) == 0 && ram.client_bytes == 0
		&& ram.allocated_bytes == 0 && !(ram.vm_fault_status & 0xff), "private readback RAM reclaimed");
	c = Create(a); destroy.handle = c.handle;
	Require(ioctl(a, AMDGPU_VIDEO_DESTROY, &destroy, sizeof(destroy)) == 0, "reopen session");
	// Exercise the global firmware-handle limit with independent file owners.
	// These tiny sessions also wrap the ring while all 32 contexts are alive.
	int owners[33];
	for (unsigned i = 0; i < 33; i++) {
		owners[i] = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
		Require(owners[i] >= 0, "open limit client");
		auto small = Request<amdgpu_video_create>();
		small.config = {16, 16, 66, 10, 0, 0};
		int status = ioctl(owners[i], AMDGPU_VIDEO_CREATE, &small, sizeof(small));
		Require(i < 32 ? status == 0 : status == -1 && errno == B_NO_MEMORY,
			"bounded firmware session count");
	}
	for (unsigned i = 33; i > 0; i--) close(owners[i - 1]);
	Require(ioctl(a, AMDGPU_MEMORY_INFO, &info, sizeof(info)) == 0
		&& info.allocated_bytes == 0 && !info.faulted, "all 32 sessions reclaimed");
	Require(ioctl(a, AMDGPU_GART_INFO, &ram, sizeof(ram)) == 0 && ram.client_bytes == 0
		&& ram.allocated_bytes == 0 && !(ram.vm_fault_status & 0xff), "all 32 RAM mappings reclaimed");
	puts("PASS: 32 simultaneous private sessions, excess session rejected, ring wrap and close cleanup");
	close(a);
	puts("PASS: automatic UVD, private sessions, validation, alternating decode, destroy/close/reuse");
	puts("PASS: 42 embedded pointer rejections before decode, consecutive firmware sequences");
	puts("Saved linear NV12 output requires full independent reference comparison.");
}
