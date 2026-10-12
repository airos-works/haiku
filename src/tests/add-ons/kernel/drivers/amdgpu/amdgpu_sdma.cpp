/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <amdgpu_haiku.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static bool
CheckRejections(int fd, const amdgpu_sdma_test_result& valid)
{
	amdgpu_sdma_test_result bad = valid;
	if (ioctl(fd, AMDGPU_SDMA_TEST, &bad, sizeof(bad) - 1) != -1
		|| errno != B_BAD_VALUE)
		return false;
	if (ioctl(fd, AMDGPU_SDMA_TEST, NULL, sizeof(bad)) != -1
		|| errno != B_BAD_ADDRESS)
		return false;
	for (unsigned field = 0; field < 5; field++) {
		bad = valid;
		switch (field) {
			case 0: bad.version++; break;
			case 1: bad.size--; break;
			case 2: bad.reserved = 1; break;
			case 3: bad.firmware_size = 51; break;
			case 4: bad.firmware_size = 65537; break;
		}
		if (ioctl(fd, AMDGPU_SDMA_TEST, &bad, sizeof(bad)) != -1
			|| errno != B_BAD_VALUE)
			return false;
	}
	bad = valid;
	bad.firmware = 0;
	if (ioctl(fd, AMDGPU_SDMA_TEST, &bad, sizeof(bad)) != 0
		|| bad.status != B_BAD_ADDRESS || bad.stage != 0)
		return false;
	uint8 corrupt[52] = {};
	bad = valid;
	bad.firmware = (uint64)(addr_t)corrupt;
	bad.firmware_size = sizeof(corrupt);
	if (ioctl(fd, AMDGPU_SDMA_TEST, &bad, sizeof(bad)) != 0
		|| bad.status != B_BAD_DATA || bad.stage != 0)
		return false;
	puts("PASS: malformed requests and firmware rejected before engine access");
	return true;
}


int
main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: amdgpu_sdma polaris10_sdma.bin\n");
		return 2;
	}
	FILE* file = fopen(argv[1], "rb");
	if (file == NULL) {
		perror(argv[1]);
		return 1;
	}
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	if (size < 52 || size > 65536) {
		fclose(file);
		return 1;
	}
	rewind(file);
	void* data = malloc(size);
	if (data == NULL) {
		fclose(file);
		return 1;
	}
	bool read = fread(data, 1, size, file) == (size_t)size;
	fclose(file);
	if (!read) {
		free(data);
		return 1;
	}
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	if (fd < 0) {
		perror("open amdgpu");
		free(data);
		return 1;
	}
	amdgpu_sdma_test_result result = {};
	result.version = AMDGPU_HAIKU_ABI_VERSION;
	result.size = sizeof(result);
	result.firmware = (uint64)(addr_t)data;
	result.firmware_size = size;
	if (!CheckRejections(fd, result)) {
		fprintf(stderr, "FAIL: unsafe request accepted\n");
		free(data);
		close(fd);
		return 1;
	}
	int status = ioctl(fd, AMDGPU_SDMA_TEST, &result, sizeof(result));
	free(data);
	close(fd);
	if (status != 0) {
		perror("AMDGPU_SDMA_TEST");
		return 1;
	}
	printf("SDMA stage %" B_PRIu32 ", status %" B_PRId32 " (%s)\n",
		result.stage, result.status, strerror(result.status));
	printf("firmware %" B_PRIu32 ", scratch GPU %#" B_PRIx64
		", fence %#" B_PRIx32 ", ring %" B_PRIu32 "/%" B_PRIu32
		", engine %#" B_PRIx32 ", halted %" B_PRIu32 "\n",
		result.firmware_version, result.scratch_gpu, result.fence,
		result.rptr, result.wptr, result.engine_status, result.halted);
	printf("%" B_PRIu32 " bytes checked, %" B_PRIu32
		" mismatches, %" B_PRIu64 " us\n", result.checked_bytes,
		result.mismatches, result.elapsed_us);
	bool pass = result.status == B_OK && result.stage == 5
		&& result.fence == 0xdeadbeef && result.halted == 1
		&& result.checked_bytes == 1024 * 1024 - 4096 && result.mismatches == 0;
	puts(pass ? "PASS: GPU copy, fill, fence and scratch guards" : "FAIL: SDMA test");
	return pass ? 0 : 1;
}
