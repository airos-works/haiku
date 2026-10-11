/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Compares app_server's own copy of the screen (its "back buffer", which
	it draws into and copies to the screen, and composites the cursor from)
	with the frame buffer the display shows, in one rectangle, a number of
	times: how much of app_server's copy lags behind what is on the screen,
	for programs that put pixels on the screen themselves (Summit's direct
	present).

	screencheck <x> <y> <width> <height> [samples [interval-ms]]

	Each sample compares the rectangle row by row (each row of the screen
	right before the same row of the copy, so that a frame landing during the
	scan changes few rows) and prints the share of differing pixels (colour
	only), and whether the copy changed since the previous sample. app_server
	shares its copy with direct windows only: one must have connected since
	app_server started.
*/


#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include <sunxi_display.h>


static area_id
clone_back_buffer(uint8** _bits, size_t* _size)
{
	// app_server's MallocBuffer; app_server makes it cloneable when it hands
	// it to a direct window
	team_info team;
	int32 cookie = 0;
	while (get_next_team_info(&cookie, &team) == B_OK) {
		if (strstr(team.args, "app_server") == NULL)
			continue;
		area_info info;
		ssize_t areaCookie = 0;
		while (get_next_area_info(team.team, &areaCookie, &info) == B_OK) {
			if (strcmp(info.name, "back buffer") != 0)
				continue;
			void* address = NULL;
			area_id clone = clone_area("screencheck back buffer", &address,
				B_ANY_ADDRESS, B_READ_AREA, info.area);
			if (clone < 0) {
				fprintf(stderr, "app_server's back buffer (area %" B_PRId32
					") cannot be cloned: %s (no direct window yet?)\n",
					info.area, strerror(clone));
				return clone;
			}
			*_bits = (uint8*)address;
			*_size = info.size;
			return clone;
		}
	}
	fprintf(stderr, "no app_server back buffer found\n");
	return B_ENTRY_NOT_FOUND;
}


int
main(int argc, char** argv)
{
	if (argc < 5) {
		fprintf(stderr, "usage: %s <x> <y> <width> <height> [samples"
			" [interval-ms]]\n", argv[0]);
		return 2;
	}
	int32 x = atoi(argv[1]), y = atoi(argv[2]);
	int32 width = atoi(argv[3]), height = atoi(argv[4]);
	int32 samples = argc > 5 ? atoi(argv[5]) : 20;
	bigtime_t interval = (argc > 6 ? atoi(argv[6]) : 100) * 1000LL;

	int fd = open("/dev/" SUNXI_DISPLAY_DEVICE, O_RDWR);
	if (fd < 0) {
		perror("/dev/" SUNXI_DISPLAY_DEVICE);
		return 1;
	}
	sunxi_display_shared_info state;
	if (ioctl(fd, SUNXI_DISPLAY_GET_STATE, &state, sizeof(state)) != 0) {
		perror("SUNXI_DISPLAY_GET_STATE");
		return 1;
	}
	area_info front;
	if (ioctl(fd, SUNXI_DISPLAY_CLONE_FRAME_BUFFER, &front, sizeof(front))
			!= 0) {
		perror("SUNXI_DISPLAY_CLONE_FRAME_BUFFER");
		return 1;
	}
	close(fd);

	uint8* back = NULL;
	size_t backSize = 0;
	if (clone_back_buffer(&back, &backSize) < 0)
		return 1;
	// the copy is B_RGBA32 at the screen's (logical) size
	size_t backBytesPerRow = (size_t)state.width * 4;
	if (x < 0 || y < 0 || width <= 0 || height <= 0
		|| (uint32)(x + width) > state.width
		|| (uint32)(y + height) > state.height
		|| backBytesPerRow * state.height > backSize) {
		fprintf(stderr, "rectangle outside the %" B_PRIu32 "x%" B_PRIu32
			" screen, or the copy is not its size\n", state.width,
			state.height);
		return 1;
	}
	printf("screen %" B_PRIu32 "x%" B_PRIu32 ", %" B_PRIu32 " bytes per"
		" row; rectangle %" B_PRId32 ",%" B_PRId32 " %" B_PRId32 "x%" B_PRId32
		"\n", state.width, state.height, state.bytes_per_row, x, y, width,
		height);

	const uint8* screen = (const uint8*)front.address;
	uint32* frontRow = new uint32[width];
	uint64 previousHash = 0;
	int32 differentSamples = 0;
	double totalShare = 0;
	for (int32 sample = 0; sample < samples; sample++) {
		uint64 different = 0;
		uint64 hash = 1469598103934665603ULL;
		for (int32 row = y; row < y + height; row++) {
			// the screen's row first (uncached memory), then the copy's
			memcpy(frontRow, screen + (size_t)row * state.bytes_per_row
				+ (size_t)x * 4, (size_t)width * 4);
			const uint32* backRow = (const uint32*)(back
				+ (size_t)row * backBytesPerRow + (size_t)x * 4);
			for (int32 column = 0; column < width; column++) {
				uint32 pixel = backRow[column] & 0xffffff;
				if ((frontRow[column] & 0xffffff) != pixel)
					different++;
				hash = (hash ^ pixel) * 1099511628211ULL;
			}
		}
		double share = 100.0 * different / ((double)width * height);
		totalShare += share;
		if (different != 0)
			differentSamples++;
		printf("sample %2" B_PRId32 ": %6.2f %% of the copy differs from the"
			" screen, copy %s\n", sample, share,
			sample == 0 ? "-" : hash != previousHash ? "changed" : "unchanged");
		previousHash = hash;
		snooze(interval);
	}
	printf("%" B_PRId32 " of %" B_PRId32 " samples differ, %.2f %% of the"
		" pixels on average\n", differentSamples, samples,
		totalShare / samples);
	delete[] frontRow;
	return 0;
}
