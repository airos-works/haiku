/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <amdgpu_haiku.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void Require(bool ok, const char* why)
{
	if (!ok) { fprintf(stderr, "FAIL: %s (%s)\n", why, strerror(errno)); exit(1); }
}

int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc != 3 && argc != 4) {
		fprintf(stderr, "usage: amdgpu_uvd polaris10_uvd.bin output-prefix [rounds: 1..64]\n");
		return 2;
	}
	char* end = NULL;
	long rounds = argc == 4 ? strtol(argv[3], &end, 10) : 4;
	Require(rounds >= 1 && rounds <= 64 && (argc != 4 || (end != argv[3] && *end == 0)),
		"round count");
	FILE* file = fopen(argv[1], "rb");
	Require(file != NULL && fseek(file, 0, SEEK_END) == 0, "open firmware");
	long size = ftell(file);
	Require(size >= 32 && size <= 1024 * 1024, "firmware size");
	rewind(file);
	uint8* image = (uint8*)malloc(size);
	uint8* output = (uint8*)malloc(AMDGPU_UVD_TEST_OUTPUT_BYTES);
	Require(image != NULL && output != NULL, "allocate input/output");
	Require(fread(image, 1, size, file) == (size_t)size, "read firmware");
	fclose(file);
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	Require(fd >= 0, "open amdgpu");
	amdgpu_uvd_test request = {};
	request.version = AMDGPU_HAIKU_ABI_VERSION;
	request.size = sizeof(request);
	request.firmware = (addr_t)image;
	request.firmware_size = size;
	request.output = (addr_t)output;
	request.output_capacity = AMDGPU_UVD_TEST_OUTPUT_BYTES;
	Require(ioctl(fd, AMDGPU_UVD_TEST, NULL, sizeof(request)) == -1
		&& errno == B_BAD_ADDRESS, "null request rejected");
	auto bad = request;
	bad.firmware = 0;
	Require(ioctl(fd, AMDGPU_UVD_TEST, &bad, sizeof(bad)) == 0
		&& bad.stage == 0 && bad.status == B_BAD_ADDRESS, "invalid firmware pointer rejected");
	uint8 saved = image[12]; image[12] = 0xff;
	bad = request;
	Require(ioctl(fd, AMDGPU_UVD_TEST, &bad, sizeof(bad)) == 0
		&& bad.stage == 0 && bad.status == B_BAD_DATA, "wrong UVD firmware IP rejected");
	image[12] = saved;
	puts("PASS: invalid UVD firmware rejected before hardware access");
	for (uint32 round = 0; round < (uint32)rounds; round++) {
		auto result = request;
		Require(ioctl(fd, AMDGPU_UVD_TEST, &result, sizeof(result)) == 0, "UVD ioctl");
		printf("UVD stage %u status %#x (%s) firmware %#x seq %u fence %u\n",
			(unsigned)result.stage, (unsigned)result.status, strerror(result.status),
			(unsigned)result.firmware_version, (unsigned)result.sequence, (unsigned)result.fence);
		printf("engine %#x power %#x ring %#x ptr %u/%u context %#x VM %#x page %#x\n",
			(unsigned)result.uvd_status, (unsigned)result.power_status, (unsigned)result.ring_control,
			(unsigned)result.rptr, (unsigned)result.wptr, (unsigned)result.context,
			(unsigned)result.vm_fault_status, (unsigned)result.vm_fault_address);
		printf("output %u bytes checksum %#x guard mismatches %u feedback",
			(unsigned)result.checked_bytes, (unsigned)result.checksum, (unsigned)result.guard_mismatches);
		for (uint32 value : result.feedback) printf(" %#x", (unsigned)value);
		puts("");
		if (result.checked_bytes == AMDGPU_UVD_TEST_OUTPUT_BYTES) {
			char path[1024];
			Require(snprintf(path, sizeof(path), "%s.%u.nv12", argv[2], (unsigned)round)
				< (int)sizeof(path), "output path length");
			file = fopen(path, "wb");
			Require(file != NULL, "open output file");
			Require(fwrite(output, 1, result.checked_bytes, file) == result.checked_bytes,
				"save decoded output");
			Require(fclose(file) == 0, "close decoded output");
		}
		errno = result.status;
		Require(result.status == B_OK && result.stage == 7
			&& result.checked_bytes == AMDGPU_UVD_TEST_OUTPUT_BYTES
			&& result.guard_mismatches == 0 && result.checksum == 0x20345d8,
			"UVD create/decode/destroy, fence, guards and VM faults");
	}
	close(fd); free(image); free(output);
	printf("PASS: %ld fixed UVD H.264 decodes; saved output still requires full software-reference comparison\n", rounds);
	return 0;
}
