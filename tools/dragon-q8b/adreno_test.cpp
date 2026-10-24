/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab test of the Adreno driver's userland interface (adreno_haiku.h):
	version and parameters, a buffer placed and mapped, a command buffer
	with CP_MEM_WRITE submitted with an out sync object, the fence and the
	sync object waited for, the word checked. */


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include <graphics/adreno/adreno_haiku.h>
#include <graphics/adreno/drm.h>
#include <graphics/adreno/msm_drm.h>


static int sFd;


static int
call(uint32 nr, void* data, size_t size)
{
	if (ioctl(sFd, ADRENO_HAIKU_OP(nr), data, size) != 0)
		return errno;
	return 0;
}


static int
msm(uint32 command, void* data, size_t size)
{
	return call(DRM_COMMAND_BASE + command, data, size);
}


static uint64
param(uint32 which)
{
	drm_msm_param p = {};
	p.pipe = MSM_PIPE_3D0;
	p.param = which;
	int error = msm(DRM_MSM_GET_PARAM, &p, sizeof(p));
	if (error != 0)
		printf("GET_PARAM %#x: %s\n", which, strerror(error));
	return p.value;
}


static uint32
pkt7(uint32 opcode, uint32 count)
{
	auto parity = [](uint32 v) {
		return (0x9669 >> (0xf & (v ^ (v >> 4) ^ (v >> 8) ^ (v >> 12)
			^ (v >> 16) ^ (v >> 20) ^ (v >> 24) ^ (v >> 28)))) & 1;
	};
	return 7u << 28 | count | parity(count) << 15 | (opcode & 0x7f) << 16
		| parity(opcode) << 23;
}


/*!	--hang: an indirect buffer of zeroes (a CP opcode error) loses the GPU;
	the wait for it must fail at once, and a new open must be refused.
*/
static int
hang_test(uint32* words, uint64 iova, uint32 handle, uint32 queue)
{
	for (int i = 0; i < 16; i++)
		words[i] = 0;
	__sync_synchronize();
	drm_msm_gem_submit_bo bo = {};
	bo.flags = MSM_SUBMIT_BO_READ;
	bo.handle = handle;
	drm_msm_gem_submit_cmd cmd = {};
	cmd.type = MSM_SUBMIT_CMD_BUF;
	cmd.size = 64;
	drm_msm_gem_submit submit = {};
	submit.flags = MSM_PIPE_3D0;
	submit.nr_bos = 1;
	submit.nr_cmds = 1;
	submit.bos = (addr_t)&bo;
	submit.cmds = (addr_t)&cmd;
	submit.queueid = queue;
	int error = msm(DRM_MSM_GEM_SUBMIT, &submit, sizeof(submit));
	printf("hang: submit %s\n", strerror(error));
	drm_msm_wait_fence wait = {};
	wait.fence = submit.fence;
	wait.queueid = queue;
	bigtime_t start = system_time();
	bigtime_t deadline = start + 5000000;
	wait.timeout.tv_sec = deadline / 1000000;
	wait.timeout.tv_nsec = (deadline % 1000000) * 1000;
	error = msm(DRM_MSM_WAIT_FENCE, &wait, sizeof(wait));
	printf("hang: wait %s after %lld us\n", strerror(error),
		(long long)(system_time() - start));
	error = msm(DRM_MSM_GEM_SUBMIT, &submit, sizeof(submit));
	printf("hang: next submit %s\n", strerror(error));
	const char* path = getenv(ADRENO_HAIKU_DEVICE_ENV);
	int fd = open(path != NULL ? path : ADRENO_HAIKU_DEVICE_PATH, O_RDWR);
	printf("hang: new open %s\n", fd < 0 ? strerror(errno) : "allowed");
	return 0;
}


int
main(int argc, char** argv)
{
	bool hang = argc > 1 && strcmp(argv[1], "--hang") == 0;
	const char* path = getenv(ADRENO_HAIKU_DEVICE_ENV);
	sFd = open(path != NULL ? path : ADRENO_HAIKU_DEVICE_PATH, O_RDWR);
	if (sFd < 0) {
		printf("open: %s\n", strerror(errno));
		return 1;
	}

	char name[32] = {};
	drm_version version = {};
	version.name = name;
	version.name_len = sizeof(name) - 1;
	if (call(ADRENO_HAIKU_NR_VERSION, &version, sizeof(version)) == 0) {
		printf("driver %s %d.%d\n", name, version.version_major,
			version.version_minor);
	}
	printf("GPU %llu, chip %#llx, GMEM %llu KiB at %#llx, VA %#llx+%#llx, "
		"timestamp %llu\n", (unsigned long long)param(MSM_PARAM_GPU_ID),
		(unsigned long long)param(MSM_PARAM_CHIP_ID),
		(unsigned long long)param(MSM_PARAM_GMEM_SIZE) / 1024,
		(unsigned long long)param(MSM_PARAM_GMEM_BASE),
		(unsigned long long)param(MSM_PARAM_VA_START),
		(unsigned long long)param(MSM_PARAM_VA_SIZE),
		(unsigned long long)param(MSM_PARAM_TIMESTAMP));

	drm_msm_gem_new gem = {};
	gem.size = 64 * 1024;
	gem.flags = MSM_BO_CACHED_COHERENT;
	int error = msm(DRM_MSM_GEM_NEW, &gem, sizeof(gem));
	if (error != 0) {
		printf("GEM_NEW: %s\n", strerror(error));
		return 1;
	}
	uint64 iova = param(MSM_PARAM_VA_START) + 0x100000;
	drm_msm_gem_info info = {};
	info.handle = gem.handle;
	info.info = MSM_INFO_SET_IOVA;
	info.value = iova;
	error = msm(DRM_MSM_GEM_INFO, &info, sizeof(info));
	if (error != 0) {
		printf("SET_IOVA: %s\n", strerror(error));
		return 1;
	}

	adreno_haiku_map_bo map = {};
	map.handle = gem.handle;
	error = call(ADRENO_HAIKU_NR_MAP_BO, &map, sizeof(map));
	if (error != 0) {
		printf("MAP_BO: %s\n", strerror(error));
		return 1;
	}
	uint32* words = (uint32*)(addr_t)map.address;
	printf("buffer %u at GPU %#llx, mapped at %p (%llu bytes)\n", gem.handle,
		(unsigned long long)iova, words, (unsigned long long)map.size);

	if (hang) {
		drm_msm_submitqueue hangQueue = {};
		msm(DRM_MSM_SUBMITQUEUE_NEW, &hangQueue, sizeof(hangQueue));
		return hang_test(words, iova, gem.handle, hangQueue.id);
	}

	// commands at 0, the target word at 0x8000
	uint64 target = iova + 0x8000;
	uint32 n = 0;
	words[n++] = pkt7(0x3d, 3);		// CP_MEM_WRITE
	words[n++] = (uint32)target;
	words[n++] = (uint32)(target >> 32);
	words[n++] = 0x0690c0de;
	words[0x8000 / 4] = 0;
	__sync_synchronize();

	drm_syncobj_create create = {};
	call(ADRENO_HAIKU_NR_SYNCOBJ_CREATE, &create, sizeof(create));

	drm_msm_gem_submit_bo bo = {};
	bo.flags = MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE;
	bo.handle = gem.handle;
	drm_msm_gem_submit_cmd cmd = {};
	cmd.type = MSM_SUBMIT_CMD_BUF;
	cmd.submit_idx = 0;
	cmd.submit_offset = 0;
	cmd.size = n * 4;
	drm_msm_syncobj out = {};
	out.handle = create.handle;
	drm_msm_submitqueue queue = {};
	msm(DRM_MSM_SUBMITQUEUE_NEW, &queue, sizeof(queue));

	drm_msm_gem_submit submit = {};
	submit.flags = MSM_PIPE_3D0 | MSM_SUBMIT_SYNCOBJ_OUT;
	submit.nr_bos = 1;
	submit.nr_cmds = 1;
	submit.bos = (addr_t)&bo;
	submit.cmds = (addr_t)&cmd;
	submit.queueid = queue.id;
	submit.out_syncobjs = (addr_t)&out;
	submit.nr_out_syncobjs = 1;
	submit.syncobj_stride = sizeof(out);
	bigtime_t start = system_time();
	error = msm(DRM_MSM_GEM_SUBMIT, &submit, sizeof(submit));
	if (error != 0) {
		printf("GEM_SUBMIT: %s\n", strerror(error));
		return 1;
	}

	drm_msm_wait_fence wait = {};
	wait.fence = submit.fence;
	wait.queueid = queue.id;
	bigtime_t deadline = system_time() + 1000000;
	wait.timeout.tv_sec = deadline / 1000000;
	wait.timeout.tv_nsec = (deadline % 1000000) * 1000;
	error = msm(DRM_MSM_WAIT_FENCE, &wait, sizeof(wait));
	bigtime_t done = system_time();

	uint32 handles[1] = { create.handle };
	drm_syncobj_wait syncWait = {};
	syncWait.handles = (addr_t)handles;
	syncWait.count_handles = 1;
	syncWait.timeout_nsec = (system_time() + 1000000) * 1000;
	int syncError = call(ADRENO_HAIKU_NR_SYNCOBJ_WAIT, &syncWait,
		sizeof(syncWait));

	printf("fence %u: %s after %lld us, syncobj: %s; word %#x (%s)\n",
		submit.fence, strerror(error), (long long)(done - start),
		strerror(syncError), words[0x8000 / 4],
		words[0x8000 / 4] == 0x0690c0de ? "right" : "wrong");

	// many submissions, for the fence rate
	start = system_time();
	for (int i = 0; i < 1000; i++)
		msm(DRM_MSM_GEM_SUBMIT, &submit, sizeof(submit));
	wait.fence = submit.fence;
	deadline = system_time() + 5000000;
	wait.timeout.tv_sec = deadline / 1000000;
	wait.timeout.tv_nsec = (deadline % 1000000) * 1000;
	error = msm(DRM_MSM_WAIT_FENCE, &wait, sizeof(wait));
	printf("1000 submissions: %s, %lld us\n", strerror(error),
		(long long)(system_time() - start));

	// coherency: every submission a new command buffer, written by the CPU
	// just before, at a new place (CP_MEM_WRITE of a new value into a word
	// the CPU cleared); what the CP fetches must be what the CPU wrote
	{
		drm_msm_gem_new big = {};
		big.size = 4 * 1024 * 1024;
		big.flags = MSM_BO_CACHED_COHERENT;
		uint64 bigIova = iova + 0x1000000;
		drm_msm_gem_info bigInfo = {};
		adreno_haiku_map_bo bigMap = {};
		if (msm(DRM_MSM_GEM_NEW, &big, sizeof(big)) == 0) {
			bigInfo.handle = big.handle;
			bigInfo.info = MSM_INFO_SET_IOVA;
			bigInfo.value = bigIova;
			msm(DRM_MSM_GEM_INFO, &bigInfo, sizeof(bigInfo));
			bigMap.handle = big.handle;
			call(ADRENO_HAIKU_NR_MAP_BO, &bigMap, sizeof(bigMap));
		}
		uint32* bigWords = (uint32*)(addr_t)bigMap.address;
		drm_msm_gem_submit_bo bigBo = {};
		bigBo.flags = MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE;
		bigBo.handle = big.handle;
		uint32 wrong = 0, failed = 0, runs = 20000;
		start = system_time();
		for (uint32 run = 0; bigWords != NULL && run < runs; run++) {
			// a 64-byte command buffer in the first 3 MiB, the result word
			// in the last MiB
			uint32 slot = (run * 7919) % (3 * 1024 * 1024 / 64);
			uint32* commands = bigWords + slot * 16;
			uint32 result = 3 * 1024 * 1024 / 4 + (run % (256 * 1024));
			uint64 target = bigIova + result * 4;
			uint32 value = 0x5a000000u ^ run;
			bigWords[result] = 0;
			commands[0] = pkt7(0x3d, 3);
			commands[1] = (uint32)target;
			commands[2] = (uint32)(target >> 32);
			commands[3] = value;
			drm_msm_gem_submit_cmd bigCmd = {};
			bigCmd.type = MSM_SUBMIT_CMD_BUF;
			bigCmd.submit_offset = slot * 64;
			bigCmd.size = 16;
			drm_msm_gem_submit bigSubmit = {};
			bigSubmit.flags = MSM_PIPE_3D0;
			bigSubmit.nr_bos = 1;
			bigSubmit.nr_cmds = 1;
			bigSubmit.bos = (addr_t)&bigBo;
			bigSubmit.cmds = (addr_t)&bigCmd;
			bigSubmit.queueid = queue.id;
			if (msm(DRM_MSM_GEM_SUBMIT, &bigSubmit, sizeof(bigSubmit)) != 0) {
				failed++;
				continue;
			}
			drm_msm_wait_fence bigWait = {};
			bigWait.fence = bigSubmit.fence;
			bigWait.queueid = queue.id;
			bigtime_t bigDeadline = system_time() + 1000000;
			bigWait.timeout.tv_sec = bigDeadline / 1000000;
			bigWait.timeout.tv_nsec = (bigDeadline % 1000000) * 1000;
			if (msm(DRM_MSM_WAIT_FENCE, &bigWait, sizeof(bigWait)) != 0) {
				failed++;
				break;
			}
			if (bigWords[result] != value) {
				if (wrong++ < 8) {
					printf("run %" B_PRIu32 ": slot %" B_PRIu32 " word %#"
						B_PRIx32 ", not %#" B_PRIx32 "\n", run, slot,
						bigWords[result], value);
				}
			}
		}
		printf("coherency: %" B_PRIu32 " runs, %" B_PRIu32 " wrong, %" B_PRIu32
			" failed, %lld us\n", runs, wrong, failed,
			(long long)(system_time() - start));
		if (wrong != 0 || failed != 0)
			error = B_ERROR;
		drm_gem_close bigClose = {};
		bigClose.handle = big.handle;
		call(ADRENO_HAIKU_NR_GEM_CLOSE, &bigClose, sizeof(bigClose));
		delete_area(bigMap.area);
	}

	drm_gem_close close_ = {};
	close_.handle = gem.handle;
	call(ADRENO_HAIKU_NR_GEM_CLOSE, &close_, sizeof(close_));
	delete_area(map.area);
	close(sFd);
	return words == NULL || error != 0 ? 1 : 0;
}
