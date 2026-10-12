/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <amdgpu_haiku.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

int
main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: amdgpu_smc polaris10_smc.bin\n");
		return 2;
	}
	FILE* file = fopen(argv[1], "rb");
	if (file == NULL) {
		perror(argv[1]);
		return 1;
	}
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	if (size < 36 || size > 0x20100) {
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
	amdgpu_smc_bootstrap result = {};
	result.version = AMDGPU_HAIKU_ABI_VERSION;
	result.size = sizeof(result);
	result.firmware = (uint64)(addr_t)data;
	result.firmware_size = size;
	bool rejected = ioctl(fd, AMDGPU_SMC_BOOTSTRAP, NULL, sizeof(result)) == -1
		&& errno == B_BAD_ADDRESS;
	amdgpu_smc_bootstrap bad = result;
	bad.firmware = 0;
	rejected &= ioctl(fd, AMDGPU_SMC_BOOTSTRAP, &bad, sizeof(bad)) == 0
		&& bad.stage == 0 && bad.status == B_BAD_ADDRESS;
	uint8 corrupt[36] = {};
	bad = result;
	bad.firmware = (uint64)(addr_t)corrupt;
	bad.firmware_size = sizeof(corrupt);
	rejected &= ioctl(fd, AMDGPU_SMC_BOOTSTRAP, &bad, sizeof(bad)) == 0
		&& bad.stage == 0 && bad.status == B_BAD_DATA;
	if (!rejected) {
		fprintf(stderr, "FAIL: invalid SMC input accepted\n");
		free(data);
		close(fd);
		return 1;
	}
	puts("PASS: invalid SMC input rejected before hardware access");
	int status = ioctl(fd, AMDGPU_SMC_BOOTSTRAP, &result, sizeof(result));
	free(data);
	close(fd);
	if (status != 0) {
		perror("AMDGPU_SMC_BOOTSTRAP");
		return 1;
	}
	printf("SMC stage %" B_PRIu32 ", status %" B_PRId32 " (%s), firmware %#"
		B_PRIx32 "\n", result.stage, result.status, strerror(result.status),
		result.firmware_version);
	printf("clock %#" B_PRIx32 ", PC %#" B_PRIx32 ", security %#" B_PRIx32
		", auth %#" B_PRIx32 ", response %#" B_PRIx32 ", soft registers %#"
		B_PRIx32 "\n", result.clock, result.pc, result.security,
		result.smu_status, result.response, result.soft_registers);
	bool pass = result.status == B_OK && result.stage == 5
		&& result.pc >= 0x20100 && result.pc < 0x40000
		&& (result.smu_status & 3) == 3 && (result.clock & 1) == 0;
	puts(pass ? "PASS: authenticated SMC RAM firmware running"
		: "FAIL: SMC firmware startup; cold boot before retry");
	return pass ? 0 : 1;
}
