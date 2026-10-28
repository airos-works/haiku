/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	scanout_compare [rounds [left,top,right,bottom]...]: compares the frame buffer the display scans out
	(DRM_HAIKU_SCANOUT_GET on the render node) with app_server's back buffer
	(its "back buffer" area, cloned), pixel for pixel: whatever copies one to
	the other (the CPU, or the GPU through B_COPY_TO_FRAME_BUFFER), the two
	must be the same where nothing changed in between. At a scale other than
	100% BScreen::GetBitmap() gives logical pixels, so it cannot tell. Prints
	the pixels that differ and their bounds, leaving out the rectangles given
	(parts of the frame buffer no monitor shows, which app_server does not
	copy); the cursor is a plane of its own and in neither.
*/


#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Application.h>
#include <OS.h>
#include <Screen.h>

#include "amdgpu_drm.h"
#include "kms.h"


int
main(int argc, char** argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 3;
	clipping_rect ignored[16];
	int ignoredCount = 0;
	for (int i = 2; i < argc && ignoredCount < 16; i++) {
		clipping_rect& r = ignored[ignoredCount];
		if (sscanf(argv[i], "%" B_SCNd32 ",%" B_SCNd32 ",%" B_SCNd32 ",%"
				B_SCNd32, &r.left, &r.top, &r.right, &r.bottom) == 4)
			ignoredCount++;
	}
	BApplication app("application/x-vnd.airos-scanout_compare");
	team_id appServer = -1;
	int32 cookie = 0;
	team_info team;
	while (get_next_team_info(&cookie, &team) == B_OK) {
		const char* name = strrchr(team.args, '/');
		name = name != NULL ? name + 1 : team.args;
		if (strncmp(name, "app_server", 10) == 0)
			appServer = team.team;
	}
	area_info area;
	area_id backBuffer = -1;
	ssize_t areaCookie = 0;
	while (appServer >= 0
		&& get_next_area_info(appServer, &areaCookie, &area) == B_OK) {
		if (strcmp(area.name, "back buffer") == 0)
			backBuffer = area.area;
	}
	void* backBits = NULL;
	area_id clone = backBuffer >= 0 ? clone_area("back buffer", &backBits,
		B_ANY_ADDRESS, B_READ_AREA, backBuffer) : -1;
	if (clone < 0) {
		printf("cannot clone app_server's back buffer: %s\n",
			strerror(clone));
		return 1;
	}

	int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		printf("no render node\n");
		return 1;
	}
	drm_haiku_scanout scanout = {};
	scanout.op = DRM_HAIKU_SCANOUT_GET;
	status_t status = Kms::Ioctl(fd, DRM_HAIKU_IOCTL_SCANOUT, &scanout);
	if (status != B_OK) {
		printf("no scanout buffer: %s\n", strerror(status));
		return 1;
	}
	// the render node has no dumb buffer ioctls
	drm_amdgpu_gem_mmap mmap = {};
	mmap.in.handle = scanout.handle;
	drm_haiku_map map = {};
	status = Kms::Ioctl(fd, DRM_IOCTL_AMDGPU_GEM_MMAP, &mmap);
	if (status == B_OK) {
		map.offset = mmap.out.addr_ptr;
		map.size = (scanout.size + B_PAGE_SIZE - 1)
			& ~(uint64)(B_PAGE_SIZE - 1);
		status = Kms::Ioctl(fd, DRM_HAIKU_IOCTL_MAP, &map);
	}
	if (status != B_OK) {
		printf("cannot map the scanout buffer: %s\n", strerror(status));
		return 1;
	}
	const uint8* frameBuffer = (const uint8*)(addr_t)map.address;
	printf("scanout %" B_PRIu32 "x%" B_PRIu32 ", %" B_PRIu32 " bytes per "
		"row\n", scanout.width, scanout.height, scanout.pitch);

	int failures = 0;
	for (int round = 0; round < rounds; round++) {
		// the back buffer is as wide as the screen, rows unpadded
		BScreen screen;
		BRect frame = screen.Frame();
		display_mode mode;
		if (screen.GetMode(&mode) != B_OK) {
			printf("no display mode\n");
			return 1;
		}
		uint32 width = min_c((uint32)mode.virtual_width, scanout.width);
		uint32 height = min_c((uint32)mode.virtual_height, scanout.height);
		uint32 backBytesPerRow = mode.virtual_width * 4;
		if (round == 0) {
			printf("back buffer %" B_PRIu32 "x%" B_PRIu32 " (%.0fx%.0f "
				"logical)\n", (uint32)mode.virtual_width,
				(uint32)mode.virtual_height, frame.Width() + 1,
				frame.Height() + 1);
		}
		uint64 wrong = 0;
		uint64 lastColumn = 0;
		int sampled = 0;
		uint32 left = width, top = height, right = 0, bottom = 0;
		for (uint32 y = 0; y < height; y++) {
			const uint32* screenRow = (const uint32*)((const uint8*)backBits
				+ y * backBytesPerRow);
			const uint32* scanRow = (const uint32*)(frameBuffer
				+ y * scanout.pitch);
			for (uint32 x = 0; x < width; x++) {
				if (((screenRow[x] ^ scanRow[x]) & 0xffffff) == 0)
					continue;
				bool skip = false;
				for (int i = 0; i < ignoredCount; i++) {
					if ((int32)x >= ignored[i].left
						&& (int32)x <= ignored[i].right
						&& (int32)y >= ignored[i].top
						&& (int32)y <= ignored[i].bottom)
						skip = true;
				}
				if (skip)
					continue;
				if (getenv("SAMPLES") != NULL && x != width - 1
					&& sampled++ < 8) {
					printf("  (%" B_PRIu32 ",%" B_PRIu32 "): back %#" B_PRIx32
						", scanout %#" B_PRIx32 "\n", x, y, screenRow[x],
						scanRow[x]);
				}
				wrong++;
				if (x == width - 1)
					lastColumn++;
				left = min_c(left, x);
				right = max_c(right, x);
				top = min_c(top, y);
				bottom = max_c(bottom, y);
			}
		}
		if (wrong == 0) {
			printf("round %d: %" B_PRIu32 "x%" B_PRIu32 " the same\n", round,
				width, height);
		} else {
			printf("round %d: %" B_PRIu64 " pixels differ in %" B_PRIu32 ",%"
				B_PRIu32 "-%" B_PRIu32 ",%" B_PRIu32 ", %" B_PRIu64 " of them "
				"in the last column\n", round, wrong, left, top, right, bottom,
				lastColumn);
			failures++;
		}
		snooze(500000);
	}
	return failures == 0 ? 0 : 1;
}
