/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Exercises the V3D render device without Mesa: parameters, buffers and
	their mappings, and one real job. The job has the texture formatting
	unit convert a 64x64 RGBA8 raster image to the "lineartile" layout
	(4x4-pixel tiles), which the program checks -- so a pass means the MMU,
	the interrupt and the executor work. The job is encoded for the core the
	device reports: V3D 4.2 (Pi 4) or 7.1 (Pi 5), whose TFU fields moved
	(Mesa: v3dx_tfu.c, broadcom/common/v3d_tfu.h). */


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include <graphics/v3d/v3d_drm.h>
#include <graphics/v3d/v3d_haiku.h>


#define TFU_IOA_FORMAT_SHIFT		3
#define TFU_IOA_FORMAT_LINEARTILE	3
#define TFU_ICFG_TTYPE_SHIFT		9
#define TFU_ICFG_FORMAT_SHIFT		18
#define TFU_ICFG_FORMAT_RASTER		0
// 7.1: the output format is in the output configuration (v71.ioc)
#define TFU71_IOC_FORMAT_SHIFT		12
#define TFU71_IOC_FORMAT_LINEARTILE	3
#define TFU71_IOC_FORMAT_UIF_NO_XOR	6
#define TFU71_IOC_STRIDE_SHIFT		16
#define TFU71_ICFG_OTYPE_SHIFT		16
#define TFU71_ICFG_IFORMAT_SHIFT	23
#define TEXTURE_FORMAT_RGBA8		4

static const uint32 kSize = 64;


static int sDevice;
static bool sCacheable;


static bool
prepare(uint32 handle, bool readOnly = false)
{
	if (!sCacheable)
		return true;
	v3d_haiku_handle request = {handle,
		readOnly ? (uint32)V3D_HAIKU_CPU_READ_ONLY : 0};
	return ioctl(sDevice, V3D_HAIKU_CPU_PREPARE, &request, sizeof(request)) == 0;
}


static bool
check(bool ok, const char* what)
{
	printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
	return ok;
}


static uint32*
create_buffer(uint32 size, uint32& handle, uint32& offset)
{
	drm_v3d_create_bo create = {};
	create.size = size;
	create.flags = sCacheable ? V3D_HAIKU_BO_CACHEABLE : 0;
	if (ioctl(sDevice, V3D_HAIKU_CREATE_BO, &create, sizeof(create)) != 0)
		return NULL;
	handle = create.handle;
	offset = create.offset;

	drm_v3d_mmap_bo map = {};
	map.handle = handle;
	if (ioctl(sDevice, V3D_HAIKU_MMAP_BO, &map, sizeof(map)) != 0)
		return NULL;
	return (uint32*)(addr_t)map.offset;
}


static int
compare(const void* a, const void* b)
{
	uint32 left = *(const uint32*)a;
	uint32 right = *(const uint32*)b;
	return left < right ? -1 : left > right ? 1 : 0;
}


int
main(int argc, char** argv)
{
	sCacheable = argc == 2 && strcmp(argv[1], "--cached") == 0;
	sDevice = open(V3D_HAIKU_DEVICE_PATH, O_RDWR);
	if (sDevice < 0) {
		printf("FAIL: open %s: %s\n", V3D_HAIKU_DEVICE_PATH, strerror(errno));
		return 1;
	}

	bool ok = true;
	if (sCacheable) {
		drm_v3d_get_param param = {};
		param.param = V3D_HAIKU_PARAM_CACHEABLE_BO;
		if (!check(ioctl(sDevice, V3D_HAIKU_GET_PARAM, &param, sizeof(param)) == 0
				&& param.value >= 2, "cacheable buffer ownership capability")) {
			return 1;
		}
	}

	static const struct {
		uint32 param;
		const char* name;
	} kParams[] = {
		{DRM_V3D_PARAM_V3D_HUB_IDENT1, "hub ident 1"},
		{DRM_V3D_PARAM_V3D_HUB_IDENT2, "hub ident 2"},
		{DRM_V3D_PARAM_V3D_HUB_IDENT3, "hub ident 3"},
		{DRM_V3D_PARAM_V3D_CORE0_IDENT0, "core ident 0"},
		{DRM_V3D_PARAM_V3D_CORE0_IDENT1, "core ident 1"},
		{DRM_V3D_PARAM_V3D_CORE0_IDENT2, "core ident 2"},
		{DRM_V3D_PARAM_SUPPORTS_TFU, "TFU"},
		{DRM_V3D_PARAM_SUPPORTS_CSD, "CSD"},
	};
	for (size_t i = 0; i < sizeof(kParams) / sizeof(kParams[0]); i++) {
		drm_v3d_get_param param = {};
		param.param = kParams[i].param;
		if (ioctl(sDevice, V3D_HAIKU_GET_PARAM, &param, sizeof(param)) != 0) {
			ok = check(false, kParams[i].name);
			continue;
		}
		printf("%s: %#llx\n", kParams[i].name, (unsigned long long)param.value);
	}

	// the version as Mesa's v3d_get_device_info() computes it
	uint32 version = 0;
	{
		drm_v3d_get_param ident0 = {}, ident1 = {};
		ident0.param = DRM_V3D_PARAM_V3D_CORE0_IDENT0;
		ident1.param = DRM_V3D_PARAM_V3D_CORE0_IDENT1;
		if (ioctl(sDevice, V3D_HAIKU_GET_PARAM, &ident0, sizeof(ident0)) == 0
			&& ioctl(sDevice, V3D_HAIKU_GET_PARAM, &ident1, sizeof(ident1))
				== 0) {
			version = (ident0.value >> 24 & 0xff) * 10 + (ident1.value & 0xf);
		}
	}
	printf("V3D %u.%u\n", version / 10, version % 10);
	ok &= check(version == 42 || version == 71, "a V3D version Mesa knows");

	uint32 sourceHandle, sourceOffset, targetHandle, targetOffset;
	uint32 bytes = kSize * kSize * 4;
	// a page more than the image: the TFU reads ahead of what it needs
	uint32* source = create_buffer(bytes + 4096, sourceHandle, sourceOffset);
	uint32* target = create_buffer(bytes + 4096, targetHandle, targetOffset);
	if (!check(source != NULL && target != NULL, "create and map two buffers"))
		return 1;
	printf("buffers at GPU %#x and %#x, CPU %p and %p\n", sourceOffset,
		targetOffset, source, target);
	ok &= check(sourceOffset != 0 && targetOffset != 0
		&& sourceOffset != targetOffset, "distinct GPU addresses");

	if (!check(prepare(sourceHandle) && prepare(targetHandle), "CPU ownership"))
		return 1;
	for (uint32 i = 0; i < kSize * kSize; i++) {
		source[i] = 0xff000000 | i * 2654435761u >> 8;
		target[i] = 0xdeadbeef;
	}
	ok &= check(prepare(sourceHandle) && prepare(targetHandle),
		"repeated CPU ownership preserves writes");
	bool readback = true;
	for (uint32 i = 0; i < kSize * kSize; i++)
		readback &= source[i] == (0xff000000 | i * 2654435761u >> 8);
	ok &= check(readback, "buffer memory holds what was written");

	v3d_haiku_handle sync = {};
	ok &= check(ioctl(sDevice, V3D_HAIKU_SYNC_CREATE, &sync, sizeof(sync)) == 0
		&& sync.handle != 0, "create a sync object");

	drm_v3d_submit_tfu tfu = {};
	tfu.iia = sourceOffset;
	tfu.iis = kSize;
	if (version >= 71) {
		tfu.icfg = TFU_ICFG_FORMAT_RASTER << TFU71_ICFG_IFORMAT_SHIFT
			| TEXTURE_FORMAT_RGBA8 << TFU71_ICFG_OTYPE_SHIFT;
		tfu.ioa = targetOffset;
		// The 7.1 TFU writes a linear-tile image in 16 pixel wide columns
		// without a stride (each one over the last): Mesa takes it for
		// small images only. A UIF image (no XOR) has its column stride:
		// the padded height in pairs of 4 row utiles.
		tfu.v71.ioc = TFU71_IOC_FORMAT_UIF_NO_XOR << TFU71_IOC_FORMAT_SHIFT
			| (kSize / 8) << TFU71_IOC_STRIDE_SHIFT;
	} else {
		tfu.icfg = TFU_ICFG_FORMAT_RASTER << TFU_ICFG_FORMAT_SHIFT
			| TEXTURE_FORMAT_RGBA8 << TFU_ICFG_TTYPE_SHIFT;
		tfu.ioa = targetOffset
			| TFU_IOA_FORMAT_LINEARTILE << TFU_IOA_FORMAT_SHIFT;
	}
	tfu.ios = kSize << 16 | kSize;
	tfu.bo_handles[0] = targetHandle;
	tfu.bo_handles[1] = sourceHandle;
	tfu.out_sync = sync.handle;

	bigtime_t start = system_time();
	if (!check(ioctl(sDevice, V3D_HAIKU_SUBMIT_TFU, &tfu, sizeof(tfu)) == 0,
			"submit a TFU job")) {
		printf("  %s\n", strerror(errno));
		return 1;
	}

	v3d_haiku_sync wait = {};
	wait.handle = sync.handle;
	wait.timeout_ns = 5000000000LL;
	int result = ioctl(sDevice, V3D_HAIKU_SYNC_WAIT, &wait, sizeof(wait));
	printf("job done after %lld us\n", (long long)(system_time() - start));
	ok &= check(result == 0, "wait for the job through its sync object");

	drm_v3d_wait_bo waitBuffer = {};
	waitBuffer.handle = targetHandle;
	waitBuffer.timeout_ns = 1000000000LL;
	ok &= check(ioctl(sDevice, V3D_HAIKU_WAIT_BO, &waitBuffer,
		sizeof(waitBuffer)) == 0, "wait for the target buffer");
	if (!check(prepare(sourceHandle, true) && prepare(targetHandle, true),
			"GPU to CPU ownership")) {
		return 1;
	}

	// The tiled layout moves the pixels around in 4x4 blocks: the first
	// block must be the image's top left corner, and (nearly) all of the
	// target must hold pixels of the source. Where exactly each block goes
	// is the hardware's business.
	bool firstBlock = true;
	for (uint32 i = 0; i < 16; i++)
		firstBlock &= target[i] == source[(i / 4) * kSize + i % 4];
	ok &= check(firstBlock, "the first tile is the image's top left corner");

	uint32* sorted = (uint32*)malloc(bytes);
	memcpy(sorted, source, bytes);
	qsort(sorted, kSize * kSize, 4, compare);
	uint32 found = 0;
	uint32 untouched = 0;
	for (uint32 i = 0; i < kSize * kSize; i++) {
		if (target[i] == 0xdeadbeef)
			untouched++;
		else if (bsearch(&target[i], sorted, kSize * kSize, 4, compare) != NULL)
			found++;
	}
	printf("%u of %u target words are source pixels, %u untouched\n", found,
		kSize * kSize, untouched);
	if (found + untouched != kSize * kSize || found < kSize * kSize * 15 / 16) {
		// where the written words are, and which source pixels they hold
		// (as x,y; "-" untouched, "?" no source pixel)
		for (uint32 i = 0; i < kSize * kSize; i += 4) {
			if (i >= 128 && i % 1024 != 0)
				continue;
			printf("  %4u:", i);
			for (uint32 j = i; j < i + 4; j++) {
				int32 index = -1;
				for (uint32 k = 0; k < kSize * kSize; k++) {
					if (source[k] == target[j]) {
						index = k;
						break;
					}
				}
				if (target[j] == 0xdeadbeef)
					printf("      -");
				else if (index < 0)
					printf(" %08x", target[j]);
				else
					printf("  %2d,%2d", index % kSize, index / kSize);
			}
			printf("\n");
		}
		uint32 firstWritten = kSize * kSize, lastWritten = 0;
		for (uint32 i = 0; i < kSize * kSize; i++) {
			if (target[i] != 0xdeadbeef) {
				if (firstWritten > i)
					firstWritten = i;
				lastWritten = i;
			}
		}
		printf("  written words %u to %u\n", firstWritten, lastWritten);
	}
	ok &= check(found + untouched == kSize * kSize
		&& found >= kSize * kSize * 15 / 16,
		"the TFU wrote the image in a tiled layout");

	if (sCacheable) {
		bool repeated = true;
		for (int round = 0; round < 64 && repeated; round++) {
			repeated = prepare(sourceHandle);
			if (!repeated)
				break;
			source[0] ^= 0x112233;
			uint32 expected = source[0];
			repeated = ioctl(sDevice, V3D_HAIKU_SUBMIT_TFU, &tfu, sizeof(tfu)) == 0
				&& prepare(targetHandle, true) && target[0] == expected;
			// Source is GPU-owned too, even though the job only reads it.
			repeated &= prepare(sourceHandle, true);
		}
		ok &= check(repeated, "64 read-only / write / GPU ownership cycles");
	}
	free(sorted);

	v3d_haiku_handle close = {sourceHandle, 0};
	ok &= check(ioctl(sDevice, V3D_HAIKU_CLOSE_BO, &close, sizeof(close)) == 0,
		"close a buffer");
	delete_area(area_for(source));
	delete_area(area_for(target));

	printf("%s\n", ok ? "V3D PROBE PASSED" : "V3D PROBE FAILED");
	return ok ? 0 : 1;
}
