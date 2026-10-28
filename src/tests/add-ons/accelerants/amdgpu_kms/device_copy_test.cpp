/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	device_copy_test [width height rounds]: the amdgpu_kms accelerant's copies
	to the screen by the GPU (device_copy.cpp), against a frame buffer of its
	own on the render node instead of the screen's, so it runs beside
	app_server without touching the display. Checks every pixel of the
	"frame buffer" after copies of whole frames, of many rectangles (more
	than one command submission holds), of small ones (which the CPU
	copies), of rectangles reaching past the source, and after the frame
	buffer was replaced as by a mode change, and times a full frame and a
	small rectangle against memcpy() into it.
*/


#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "accelerant.h"
#include "amdgpu_drm.h"


accelerant_info* gInfo;


static int sFailures;
static bool sDumbBuffers;


static bool
create_dumb_frame_buffer(uint32 width, uint32 height, Kms::Buffer& buffer)
{
	status_t status = Kms::CreateBuffer(gInfo->device, width, height,
		DRM_FORMAT_XRGB8888, buffer, false);
	if (status != B_OK)
		printf("dumb buffer: %s\n", strerror(status));
	return status == B_OK;
}


static bool
create_frame_buffer(uint32 width, uint32 height, Kms::Buffer& buffer)
{
	if (sDumbBuffers)
		return create_dumb_frame_buffer(width, height, buffer);
	memset(&buffer, 0, sizeof(buffer));
	buffer.width = width;
	buffer.height = height;
	buffer.pitch = width * 4;
	buffer.size = (uint64)buffer.pitch * height;
	buffer.area = -1;

	uint64 size = (buffer.size + B_PAGE_SIZE - 1) & ~(uint64)(B_PAGE_SIZE - 1);
	drm_amdgpu_gem_create create = {};
	create.in.bo_size = size;
	create.in.alignment = B_PAGE_SIZE;
	create.in.domains = AMDGPU_GEM_DOMAIN_VRAM;
	create.in.domain_flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
	status_t status = Kms::Ioctl(gInfo->device, DRM_IOCTL_AMDGPU_GEM_CREATE,
		&create);
	if (status != B_OK) {
		printf("GEM_CREATE: %s\n", strerror(status));
		return false;
	}
	buffer.handle = create.out.handle;

	drm_amdgpu_gem_mmap mmap = {};
	mmap.in.handle = buffer.handle;
	status = Kms::Ioctl(gInfo->device, DRM_IOCTL_AMDGPU_GEM_MMAP, &mmap);
	if (status != B_OK) {
		printf("GEM_MMAP: %s\n", strerror(status));
		return false;
	}
	drm_haiku_map map = {};
	map.offset = mmap.out.addr_ptr;
	map.size = size;
	map.flags = DRM_HAIKU_MAP_WRITE;
	status = Kms::Ioctl(gInfo->device, DRM_HAIKU_IOCTL_MAP, &map);
	if (status != B_OK) {
		printf("DRM_HAIKU_IOCTL_MAP: %s\n", strerror(status));
		return false;
	}
	buffer.address = (void*)(addr_t)map.address;
	buffer.area = map.area;
	return true;
}


static void
destroy_frame_buffer(Kms::Buffer& buffer)
{
	if (sDumbBuffers) {
		Kms::DestroyBuffer(gInfo->device, buffer);
		return;
	}
	drm_haiku_unmap unmap = {};
	unmap.address = (addr_t)buffer.address;
	Kms::Ioctl(gInfo->device, DRM_HAIKU_IOCTL_UNMAP, &unmap);
	drm_gem_close close = {};
	close.handle = buffer.handle;
	Kms::Ioctl(gInfo->device, DRM_IOCTL_GEM_CLOSE, &close);
}


static const uint32 kBackground = 0x12345678;


static void
clear(const Kms::Buffer& target)
{
	uint32* bits = (uint32*)target.address;
	for (uint64 i = 0; i < target.size / 4; i++)
		bits[i] = kBackground;
}


static uint32
test_pattern(uint32 x, uint32 y, uint32 seed)
{
	return 0xff000000 | ((x * 2654435761u + y * 40503u + seed) >> 8);
}


static void
fill(const frame_buffer_source& source, uint32 seed)
{
	for (uint32 y = 0; y < source.height; y++) {
		uint32* row = (uint32*)((uint8*)source.bits + y * source.bytes_per_row);
		for (uint32 x = 0; x < source.width; x++)
			row[x] = test_pattern(x, y, seed);
	}
}


/*!	Every pixel of the target: inside one of the rectangles and the source
	the pattern, else the background. */
static void
check(const char* name, const Kms::Buffer& target,
	const frame_buffer_source& source, const fill_rect_params* rects,
	uint32 count, uint32 seed)
{
	uint8* inside = (uint8*)calloc(target.width, target.height);
	for (uint32 i = 0; i < count; i++) {
		for (uint32 y = rects[i].top; y <= rects[i].bottom; y++) {
			for (uint32 x = rects[i].left; x <= rects[i].right; x++) {
				if (x < source.width && y < source.height
					&& x < target.width && y < target.height)
					inside[y * target.width + x] = 1;
			}
		}
	}
	int wrong = 0;
	for (uint32 y = 0; y < target.height; y++) {
		const uint32* row = (const uint32*)((uint8*)target.address
			+ y * target.pitch);
		for (uint32 x = 0; x < target.width; x++) {
			uint32 want = inside[y * target.width + x]
				? test_pattern(x, y, seed) : kBackground;
			if (row[x] != want && wrong++ < 3) {
				printf("  (%" B_PRIu32 ",%" B_PRIu32 "): %#" B_PRIx32
					", want %#" B_PRIx32 "\n", x, y, row[x], want);
			}
		}
	}
	free(inside);
	printf("%-28s %s (%d wrong pixels)\n", name, wrong == 0 ? "ok" : "FAILED",
		wrong);
	if (wrong != 0)
		sFailures++;
}


static void
copy_and_check(const char* name, const Kms::Buffer& target,
	const frame_buffer_source& source, const fill_rect_params* rects,
	uint32 count, uint32 seed)
{
	clear(target);
	fill(source, seed);
	status_t status = amdgpu_copy_to_frame_buffer(source.id, rects, count);
	if (status != B_OK) {
		printf("%-28s FAILED: %s\n", name, strerror(status));
		sFailures++;
		return;
	}
	check(name, target, source, rects, count, seed);
}


int
main(int argc, char** argv)
{
	uint32 width = argc > 2 ? atoi(argv[1]) : 3840;
	uint32 height = argc > 2 ? atoi(argv[2]) : 2160;
	int rounds = argc > 3 ? atoi(argv[3]) : 30;

	gInfo = (accelerant_info*)calloc(1, sizeof(accelerant_info));
	// DEVICE_COPY_NODE=/dev/dri/card0: the screen's node, and dumb buffers
	// as the accelerant's frame buffer is, but not the screen's
	const char* node = getenv("DEVICE_COPY_NODE");
	if (node == NULL)
		node = "/dev/dri/renderD128";
	sDumbBuffers = strstr(node, "render") == NULL;
	gInfo->device = open(node, O_RDWR | O_CLOEXEC);
	if (gInfo->device < 0) {
		printf("cannot open %s\n", node);
		return 1;
	}
	if (!create_frame_buffer(width, height, gInfo->frameBuffer)) {
		printf("no frame buffer\n");
		return 1;
	}
	Kms::Buffer& target = gInfo->frameBuffer;

	frame_buffer_source source;
	status_t status = amdgpu_create_frame_buffer_source(width, height,
		B_RGBA32, &source);
	if (status != B_OK) {
		printf("no source: %s\n", strerror(status));
		return 1;
	}
	printf("%" B_PRIu32 "x%" B_PRIu32 ": source %" B_PRIu32 " bytes per row\n",
		width, height, source.bytes_per_row);

	fill_rect_params full = { 0, 0, (uint16)(width - 1), (uint16)(height - 1) };
	copy_and_check("full frame", target, source, &full, 1, 1);

	fill_rect_params some[] = {
		{ 10, 20, 309, 219 },
		{ (uint16)(width - 500), (uint16)(height - 400), (uint16)(width - 1),
			(uint16)(height - 1) },
		{ 1001, 7, 1001, 7 },
		{ 0, (uint16)(height / 2), (uint16)(width - 1),
			(uint16)(height / 2 + 2) },
	};
	copy_and_check("four rects", target, source, some, B_COUNT_OF(some), 2);

	// small enough for the CPU to copy
	fill_rect_params typed[] = {
		{ 400, 300, 519, 319 },
		{ 7, 9, 7, 9 },
	};
	copy_and_check("two small rects (CPU)", target, source, typed,
		B_COUNT_OF(typed), 6);

	// more rectangles than one submission takes (a glyph-sized grid)
	const uint32 kMany = 1000;
	fill_rect_params* many = new fill_rect_params[kMany];
	for (uint32 i = 0; i < kMany; i++) {
		uint16 x = (i % 40) * (width / 40) + 3;
		uint16 y = (i / 40) * (height / 25) + 5;
		many[i] = { x, y, (uint16)(x + 7 + i % 5), (uint16)(y + 12) };
	}
	copy_and_check("1000 rects", target, source, many, kMany, 3);
	delete[] many;

	// a source smaller than the frame buffer: only its part is copied
	frame_buffer_source small;
	status = amdgpu_create_frame_buffer_source(width / 2 + 13, height / 2,
		B_RGB32, &small);
	if (status != B_OK) {
		printf("no small source: %s\n", strerror(status));
		return 1;
	}
	copy_and_check("small source, full rect", target, small, &full, 1, 4);
	amdgpu_delete_frame_buffer_source(small.id);
	if (amdgpu_copy_to_frame_buffer(small.id, &full, 1) == B_OK) {
		printf("copy from a deleted source succeeded\n");
		sFailures++;
	}

	// the frame buffer replaced, as by a mode change
	Kms::Buffer old = target;
	Kms::Buffer replacement;
	if (!create_frame_buffer(width, height, replacement)) {
		printf("no second frame buffer\n");
		return 1;
	}
	lock_device_copies();
	gInfo->frameBuffer = replacement;
	gInfo->frameBufferGeneration++;
	unlock_device_copies();
	destroy_frame_buffer(old);
	copy_and_check("after a mode change", target, source, some,
		B_COUNT_OF(some), 5);

	// timing: a full frame, and a word typed (one 120x20 rectangle)
	fill_rect_params word = { 400, 300, 519, 319 };
	struct {
		const char*				name;
		const fill_rect_params*	rect;
	} timings[] = {
		{ "full frame", &full },
		{ "120x20 rect", &word },
	};
	for (uint32 t = 0; t < B_COUNT_OF(timings); t++) {
		const fill_rect_params& rect = *timings[t].rect;
		bigtime_t total = 0, worst = 0;
		for (int i = 0; i < rounds; i++) {
			bigtime_t start = system_time();
			if (amdgpu_copy_to_frame_buffer(source.id, &rect, 1) != B_OK) {
				printf("round %d failed\n", i);
				return 1;
			}
			bigtime_t took = system_time() - start;
			total += took;
			if (took > worst)
				worst = took;
		}
		uint32 rowBytes = (rect.right - rect.left + 1) * 4;
		bigtime_t start = system_time();
		for (int i = 0; i < rounds; i++) {
			for (uint32 y = rect.top; y <= rect.bottom; y++) {
				memcpy((uint8*)target.address + y * target.pitch
						+ rect.left * 4,
					(uint8*)source.bits + y * source.bytes_per_row
						+ rect.left * 4, rowBytes);
			}
		}
		bigtime_t cpu = system_time() - start;
		printf("%s copied: mean %.3f ms, worst %.3f ms; memcpy() "
			"%.3f ms\n", timings[t].name, total / 1000.0 / rounds,
			worst / 1000.0, cpu / 1000.0 / rounds);
	}

	amdgpu_delete_frame_buffer_source(source.id);
	uninit_device_copies();
	printf("%s\n", sFailures == 0 ? "PASSED" : "FAILED");
	return sFailures == 0 ? 0 : 1;
}
