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


int
main()
{
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

	drm_gem_close close_ = {};
	close_.handle = gem.handle;
	call(ADRENO_HAIKU_NR_GEM_CLOSE, &close_, sizeof(close_));
	delete_area(map.area);
	close(sFd);
	return words == NULL || error != 0 ? 1 : 0;
}
