/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Checks and times the accelerant's copies by the GPU (device_copy.cpp) on
	a DRM file of its own, beside app_server: a source in system memory as
	app_server's back buffer, shadows of it in video and in system memory,
	and a buffer in video memory in place of the frame buffer.

	usage: amdcopytest [width height [device]]
*/


#include "accelerant.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>

#include "amdgpu_drm.h"


accelerant_info* gInfo;

static int sFailures;


static uint32
pixel_pattern(uint32 seed, uint32 x, uint32 y)
{
	uint32 v = seed * 0x9e3779b1u ^ x * 0x85ebca77u ^ y * 0xc2b2ae3du;
	v ^= v >> 15;
	v *= 0x2c1b3c6du;
	return (v ^ v >> 12) | 0xff000000;
}


static void
fill(const frame_buffer_source& source, uint32 seed, uint32 left = 0,
	uint32 top = 0, uint32 right = UINT32_MAX, uint32 bottom = UINT32_MAX)
{
	right = std::min<uint32>(right, source.width - 1);
	bottom = std::min<uint32>(bottom, source.height - 1);
	for (uint32 y = top; y <= bottom; y++) {
		uint32* row = (uint32*)((uint8*)source.bits + y * source.bytes_per_row);
		for (uint32 x = left; x <= right; x++)
			row[x] = pixel_pattern(seed, x, y);
	}
}


static status_t
create_frame_buffer(uint32 width, uint32 height)
{
	Kms::Buffer& buffer = gInfo->frameBuffer;
	buffer.pitch = (width * 4 + 255) & ~255u;
	buffer.size = ((uint64)buffer.pitch * height + B_PAGE_SIZE - 1)
		& ~(uint64)(B_PAGE_SIZE - 1);
	buffer.width = width;
	buffer.height = height;

	drm_amdgpu_gem_create create = {};
	create.in.bo_size = buffer.size;
	create.in.alignment = B_PAGE_SIZE;
	create.in.domains = AMDGPU_GEM_DOMAIN_VRAM;
	create.in.domain_flags = AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
	status_t status = Kms::Ioctl(gInfo->device, DRM_IOCTL_AMDGPU_GEM_CREATE,
		&create);
	if (status != B_OK)
		return status;
	buffer.handle = create.out.handle;

	drm_amdgpu_gem_mmap mmap = {};
	mmap.in.handle = buffer.handle;
	status = Kms::Ioctl(gInfo->device, DRM_IOCTL_AMDGPU_GEM_MMAP, &mmap);
	if (status != B_OK)
		return status;
	drm_haiku_map map = {};
	map.offset = mmap.out.addr_ptr;
	map.size = buffer.size;
	map.flags = DRM_HAIKU_MAP_WRITE;
	status = Kms::Ioctl(gInfo->device, DRM_HAIKU_IOCTL_MAP, &map);
	if (status != B_OK)
		return status;
	buffer.address = (void*)(addr_t)map.address;
	gInfo->frameBufferGeneration = 1;
	return B_OK;
}


/*!	Compares rows of the frame buffer with the back buffer: every 61st row
	(video memory is slow for the CPU to read), and all of those of \a focus
	(a rectangle that was copied). */
static void
check_screen(const char* what, const frame_buffer_source& back,
	const fill_rect_params* focus = NULL)
{
	const Kms::Buffer& screen = gInfo->frameBuffer;
	uint32 wrong = 0, checked = 0;
	uint32 firstX = 0, firstY = 0, want = 0, got = 0;
	for (uint32 y = 0; y < back.height; y++) {
		if (y % 61 != 0 && y + 1 != back.height && (focus == NULL
				|| y < (uint32)focus->top || y > (uint32)focus->bottom))
			continue;
		const uint32* from = (const uint32*)((const uint8*)back.bits
			+ y * back.bytes_per_row);
		const uint32* to = (const uint32*)((const uint8*)screen.address
			+ y * screen.pitch);
		for (uint32 x = 0; x < back.width; x++) {
			checked++;
			if ((to[x] & 0xffffff) != (from[x] & 0xffffff)) {
				if (wrong++ == 0) {
					firstX = x;
					firstY = y;
					want = from[x];
					got = to[x];
				}
			}
		}
	}
	if (wrong == 0) {
		printf("ok   %s (%u pixels compared)\n", what, checked);
		return;
	}
	printf("FAIL %s: %u of %u pixels, first %u,%u want %08x got %08x\n", what,
		wrong, checked, firstX, firstY, want, got);
	sFailures++;
}


static bigtime_t
thread_cpu_time()
{
	thread_info info;
	get_thread_info(find_thread(NULL), &info);
	return info.user_time + info.kernel_time;
}


// as app_server copies an area into a shadow: bands of 512K pixels
static status_t
copy_bands(uint32 from, uint32 to, uint32 left, uint32 top, uint32 right,
	uint32 bottom)
{
	uint32 width = right - left + 1;
	uint32 bandRows = std::max<uint32>(16, 512 * 1024 / width);
	for (uint32 y = top; y <= bottom; y += bandRows) {
		fill_rect_params rect;
		rect.left = left;
		rect.top = y;
		rect.right = right;
		rect.bottom = std::min<uint32>(y + bandRows - 1, bottom);
		status_t status = amdgpu_copy_frame_buffer_source(from, to, &rect, 1);
		if (status != B_OK)
			return status;
	}
	return B_OK;
}


struct Timing {
	bigtime_t	best;
	bigtime_t	median;
	bigtime_t	cpu;		// the caller's, per run
};


template<typename Function>
static Timing
time_it(Function function, int runs = 15)
{
	bigtime_t times[64];
	bigtime_t cpuStart = thread_cpu_time();
	for (int i = 0; i < runs; i++) {
		bigtime_t start = system_time();
		if (function() != B_OK) {
			printf("FAIL a timed copy failed\n");
			sFailures++;
			return Timing{0, 0, 0};
		}
		times[i] = system_time() - start;
	}
	bigtime_t cpu = (thread_cpu_time() - cpuStart) / runs;
	std::sort(times, times + runs);
	return Timing{times[0], times[runs / 2], cpu};
}


static void
report(const char* what, const Timing& timing, uint64 pixels)
{
	printf("time %-44s %8.3f ms (median %.3f), %5.2f GB/s, caller CPU "
		"%.3f ms\n", what, timing.best / 1000.0, timing.median / 1000.0,
		timing.best > 0 ? pixels * 4.0 / timing.best / 1000.0 : 0.0,
		timing.cpu / 1000.0);
}


struct ThreadArgs {
	uint32		from;
	uint32		to;
	bool		toScreen;
	bigtime_t	until;
	int			errors;
	int			copies;
	uint32		width;
	uint32		height;
};


static status_t
copy_thread(void* data)
{
	ThreadArgs& args = *(ThreadArgs*)data;
	uint32 i = 0;
	while (system_time() < args.until) {
		fill_rect_params rect;
		// alternately big and small rectangles over the whole surface
		uint32 w = (i % 3 == 0) ? args.width : 64 + (i * 37) % 400;
		uint32 h = (i % 3 == 0) ? args.height / 2 : 16 + (i * 11) % 200;
		rect.left = (i * 101) % (args.width - w + 1);
		rect.top = (i * 53) % (args.height - h + 1);
		rect.right = rect.left + w - 1;
		rect.bottom = rect.top + h - 1;
		status_t status = args.toScreen
			? amdgpu_copy_to_frame_buffer(args.from, &rect, 1)
			: amdgpu_copy_frame_buffer_source(args.from, args.to, &rect, 1);
		if (status != B_OK)
			args.errors++;
		args.copies++;
		i++;
	}
	return B_OK;
}


int
main(int argc, char** argv)
{
	uint32 width = argc > 2 ? atoi(argv[1]) : 3840;
	uint32 height = argc > 2 ? atoi(argv[2]) : 2160;
	const char* path = argc > 3 ? argv[3] : "/dev/dri/renderD128";

	gInfo = (accelerant_info*)calloc(1, sizeof(accelerant_info));
	gInfo->device = open(path, O_RDWR);
	if (gInfo->device < 0) {
		fprintf(stderr, "opening %s: %s\n", path, strerror(errno));
		return 2;
	}
	status_t status = create_frame_buffer(width, height);
	if (status != B_OK) {
		fprintf(stderr, "creating a %ux%u buffer in video memory: %s\n",
			width, height, strerror(status));
		return 2;
	}
	printf("     %ux%u, frame buffer stand-in in video memory, pitch %u\n",
		width, height, gInfo->frameBuffer.pitch);
	uint64 pixels = (uint64)width * height;

	frame_buffer_source back, video, system;
	status = amdgpu_create_frame_buffer_source(width, height, B_RGBA32, &back);
	if (status != B_OK) {
		fprintf(stderr, "no back buffer source: %s\n", strerror(status));
		return 2;
	}
	uint32 defaultDomain
		= device_copy_set_shadow_domain(AMDGPU_GEM_DOMAIN_VRAM);
	printf("     shadows go to %s memory on this GPU\n",
		defaultDomain == AMDGPU_GEM_DOMAIN_GTT ? "system" : "video");
	status = amdgpu_create_frame_buffer_shadow(width, height, B_RGBA32,
		&video);
	if (status != B_OK) {
		fprintf(stderr, "no shadow: %s\n", strerror(status));
		return 2;
	}
	device_copy_set_shadow_domain(AMDGPU_GEM_DOMAIN_GTT);
	status = amdgpu_create_frame_buffer_shadow(width, height, B_RGBA32,
		&system);
	device_copy_set_shadow_domain(defaultDomain);
	if (status != B_OK) {
		fprintf(stderr, "no shadow in system memory: %s\n", strerror(status));
		return 2;
	}
	printf("     back buffer bits %p, shadows' bits %p %p, domains %u %u "
		"(4 video, 2 system)\n", back.bits, video.bits, system.bits,
		device_copy_source_domain(video.id),
		device_copy_source_domain(system.id));
	if (video.bits != NULL || system.bits != NULL || back.bits == NULL) {
		printf("FAIL shadows must have no bits, the back buffer must\n");
		sFailures++;
	}

	fill_rect_params all = {0, 0, (uint16)(width - 1), (uint16)(height - 1)};

	// correctness: through each shadow to the screen
	const struct {
		const char* name;
		frame_buffer_source* shadow;
	} shadows[] = {
		{"video memory shadow", &video},
		{"system memory shadow", &system},
	};
	uint32 seed = 1;
	for (const auto& entry : shadows) {
		char what[128];
		fill(back, ++seed);
		snprintf(what, sizeof(what), "whole screen through the %s",
			entry.name);
		if (copy_bands(back.id, entry.shadow->id, 0, 0, width - 1, height - 1)
					!= B_OK
				|| amdgpu_copy_to_frame_buffer(entry.shadow->id, &all, 1)
					!= B_OK) {
			printf("FAIL %s: a copy failed\n", what);
			sFailures++;
		} else
			check_screen(what, back);

		// small (CPU, where it reaches both) and large rectangles: only
		// they change
		const fill_rect_params rects[] = {
			{17, 9, 136, 28},						// 120x20
			{(uint16)(width / 3), (uint16)(height / 4),
				(uint16)(width / 3 + 899), (uint16)(height / 4 + 499)},
			{(uint16)(width - 300), (uint16)(height - 70),
				(uint16)(width - 1), (uint16)(height - 1)},
		};
		for (const fill_rect_params& rect : rects) {
			uint32 before = ++seed;
			fill(back, before);
			copy_bands(back.id, entry.shadow->id, 0, 0, width - 1, height - 1);
			amdgpu_copy_to_frame_buffer(entry.shadow->id, &all, 1);
			fill(back, ++seed, rect.left, rect.top, rect.right, rect.bottom);
			snprintf(what, sizeof(what), "%ux%u at %u,%u through the %s",
				rect.right - rect.left + 1, rect.bottom - rect.top + 1,
				rect.left, rect.top, entry.name);
			if (amdgpu_copy_frame_buffer_source(back.id, entry.shadow->id,
						&rect, 1) != B_OK
					|| amdgpu_copy_to_frame_buffer(entry.shadow->id, &rect, 1)
						!= B_OK) {
				printf("FAIL %s: a copy failed\n", what);
				sFailures++;
				continue;
			}
			// around the rectangle the old picture
			check_screen(what, back, &rect);
		}
	}

	// timings
	void* heap = malloc((size_t)back.bytes_per_row * height);
	report("CPU memcpy, back buffer to heap (the old way)", time_it([&]() {
		memcpy(heap, back.bits, (size_t)back.bytes_per_row * height);
		return B_OK;
	}), pixels);
	free(heap);
	report("back buffer to the screen (today's flush)", time_it([&]() {
		return amdgpu_copy_to_frame_buffer(back.id, &all, 1);
	}), pixels);
	report("back buffer to video memory shadow, bands", time_it([&]() {
		return copy_bands(back.id, video.id, 0, 0, width - 1, height - 1);
	}), pixels);
	report("back buffer to video memory shadow, one copy", time_it([&]() {
		return amdgpu_copy_frame_buffer_source(back.id, video.id, &all, 1);
	}), pixels);
	report("back buffer to system memory shadow, bands", time_it([&]() {
		return copy_bands(back.id, system.id, 0, 0, width - 1, height - 1);
	}), pixels);
	report("video memory shadow to the screen", time_it([&]() {
		return amdgpu_copy_to_frame_buffer(video.id, &all, 1);
	}), pixels);
	report("system memory shadow to the screen", time_it([&]() {
		return amdgpu_copy_to_frame_buffer(system.id, &all, 1);
	}), pixels);
	fill_rect_params band = {0, 0, (uint16)(width - 1), 127};
	report("128 rows, back buffer to video memory shadow", time_it([&]() {
		return amdgpu_copy_frame_buffer_source(back.id, video.id, &band, 1);
	}), (uint64)width * 128);
	fill_rect_params small = {17, 9, 136, 28};
	report("120x20, back buffer to video memory shadow", time_it([&]() {
		return amdgpu_copy_frame_buffer_source(back.id, video.id, &small, 1);
	}, 50), 120 * 20);
	report("120x20, video memory shadow to the screen", time_it([&]() {
		return amdgpu_copy_to_frame_buffer(video.id, &small, 1);
	}, 50), 120 * 20);
	report("120x20, back buffer to system memory shadow", time_it([&]() {
		return amdgpu_copy_frame_buffer_source(back.id, system.id, &small, 1);
	}, 50), 120 * 20);

	// two threads at once, as app_server's drawing and flush threads
	fill(back, ++seed);
	ThreadArgs fillArgs = {back.id, video.id, false,
		system_time() + 3000000, 0, 0, width, height};
	ThreadArgs flushArgs = {video.id, 0, true, fillArgs.until, 0, 0, width,
		height};
	thread_id threads[2] = {
		spawn_thread(copy_thread, "fill", B_NORMAL_PRIORITY, &fillArgs),
		spawn_thread(copy_thread, "flush", B_NORMAL_PRIORITY, &flushArgs),
	};
	resume_thread(threads[0]);
	resume_thread(threads[1]);
	status_t result;
	wait_for_thread(threads[0], &result);
	wait_for_thread(threads[1], &result);
	printf("%s two threads for 3 s: %d fills, %d flushes, %d errors\n",
		fillArgs.errors + flushArgs.errors == 0 ? "ok  " : "FAIL",
		fillArgs.copies, flushArgs.copies, fillArgs.errors + flushArgs.errors);
	if (fillArgs.errors + flushArgs.errors != 0)
		sFailures++;
	copy_bands(back.id, video.id, 0, 0, width - 1, height - 1);
	amdgpu_copy_to_frame_buffer(video.id, &all, 1);
	check_screen("whole screen after the two threads", back);

	amdgpu_delete_frame_buffer_source(system.id);
	amdgpu_delete_frame_buffer_source(video.id);
	amdgpu_delete_frame_buffer_source(back.id);
	uninit_device_copies();

	printf("\n%d failures\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
