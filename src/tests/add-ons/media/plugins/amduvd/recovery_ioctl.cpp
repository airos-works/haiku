/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
// TEST ONLY. The normal amduvd target never links this file or reads these
// variables. The kernel still enforces root and exact decoder ownership.
#include "recovery_ioctl.h"
#include <amdgpu_haiku.h>
#include <OS.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
struct Configuration {
	int32 after = -1;
	uint32 stage = AMDGPU_VIDEO_SDMA_READBACK;
	Configuration()
	{
		const char* value = getenv("AMDUVD_TEST_GPU_TIMEOUT_AFTER");
		if (value == NULL) return;
		char* end;
		long n = strtol(value, &end, 10);
		if (end == value || *end || n < 0 || n > 100000) {
			fprintf(stderr, "amduvd TEST: invalid timeout picture count; disabled\n"); return;
		}
		const char* boundary = getenv("AMDUVD_TEST_GPU_TIMEOUT_STAGE");
		if (boundary != NULL && strcmp(boundary, "clear") == 0) stage = AMDGPU_VIDEO_SDMA_CLEAR;
		else if (boundary != NULL && strcmp(boundary, "readback") != 0) {
			fprintf(stderr, "amduvd TEST: invalid timeout boundary; disabled\n"); return;
		}
		after = n;
		fprintf(stderr, "amduvd TEST: configured GPU timeout stage %u after %ld decoded pictures\n",
			(unsigned)stage, (long)after);
	}
};
int32 sPictures = 0, sAttempted = 0;
}

int
AmduvdRecoveryIoctl(int fd, unsigned long op, void* data, size_t size)
{
	const bool decode = op == AMDGPU_VIDEO_DECODE || op == AMDGPU_HEVC_DECODE;
	static const Configuration config;
	if (decode && config.after >= 0 && atomic_get(&sPictures) >= config.after
		&& atomic_test_and_set(&sAttempted, 1, 0) == 0) {
		amdgpu_video_sdma_timeout_test arm = {};
		arm.version = AMDGPU_HAIKU_ABI_VERSION; arm.size = sizeof(arm); arm.stage = config.stage;
		arm.handle = op == AMDGPU_VIDEO_DECODE
			? ((amdgpu_video_decode*)data)->handle : ((amdgpu_hevc_decode*)data)->handle;
		if (ioctl(fd, AMDGPU_VIDEO_SDMA_TIMEOUT_TEST, &arm, sizeof(arm)) != 0) {
			// A busy compositor has not armed or faulted the GPU. Defer the
			// diagnostic to a later decode, never retry a hardware failure.
			if (errno == B_BUSY) atomic_set(&sAttempted, 0);
			else {
				status_t error = errno;
				fprintf(stderr, "amduvd TEST: arm refused 0x%x\n", (unsigned)error);
				errno = error; return -1;
			}
		} else {
			fprintf(stderr, "amduvd TEST: armed real GPU timeout stage %u after %ld decoded pictures, session %llu\n",
				(unsigned)config.stage, (long)atomic_get(&sPictures), (unsigned long long)arm.handle);
		}
	}
	int result = ioctl(fd, op, data, size);
	status_t error = errno;
	if (decode && result == 0) atomic_add(&sPictures, 1);
	if (decode && result < 0 && atomic_get(&sAttempted)) {
		fprintf(stderr, "amduvd TEST: real decode returned 0x%x after timeout arm\n", (unsigned)error);
	}
	errno = error;
	return result;
}
