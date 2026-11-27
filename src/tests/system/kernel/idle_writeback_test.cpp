/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	File data written without sync() or fsync() reaches the disk within
	seconds, also when there is little of it (airos-works/haiku#219).

	A disk's page writer wrote only while 256 or more modified pages were
	queued for it: fewer stayed in memory for as long as the machine ran, and
	a power cut lost them.

	The test writes a small file (100 pages, each with a marker of its own),
	closes it, and then reads the volume's raw device once a second until
	every page is on it, for 30 s at most. It does not sync.

	idle_writeback_test DEVICE DIRECTORY
		DEVICE the raw device of the volume DIRECTORY is on, at most 1 GiB
		(it is read in full each time). Nothing else should write to the
		volume while the test runs: another write could be what wakes the
		page writer.
*/


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>


static const int32 kPages = 100;
static const size_t kPageSize = 4096;
static const size_t kSectorSize = 512;
	// (file blocks need not be aligned to pages on the device)
static const off_t kMaxDeviceSize = 1024LL * 1024 * 1024;
static const bigtime_t kDeadline = 30 * 1000 * 1000;
static const uint32 kMagic = 'iwbt';


struct page_marker {
	uint32	magic;
	uint32	index;
	uint64	run;
};


static int32
count_pages_on_device(const char* device, off_t deviceSize, uint64 run,
	uint8* found)
{
	int fd = open(device, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "opening %s: %s\n", device, strerror(errno));
		exit(1);
	}

	static uint8 buffer[1024 * 1024];
	int32 count = 0;
	off_t offset = 0;
	while (offset < deviceSize) {
		size_t size = sizeof(buffer);
		if ((off_t)size > deviceSize - offset)
			size = deviceSize - offset;
		ssize_t bytesRead = pread(fd, buffer, size, offset);
		if (bytesRead <= 0)
			break;
		for (ssize_t i = 0; i + (ssize_t)sizeof(page_marker) <= bytesRead;
				i += kSectorSize) {
			page_marker* marker = (page_marker*)(buffer + i);
			if (marker->magic == kMagic && marker->run == run
				&& marker->index < (uint32)kPages && !found[marker->index]) {
				found[marker->index] = 1;
				count++;
			}
		}
		offset += bytesRead;
	}

	close(fd);
	return count;
}


int
main(int argc, char** argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s DEVICE DIRECTORY\n", argv[0]);
		return 2;
	}
	const char* device = argv[1];

	int deviceFD = open(device, O_RDONLY);
	if (deviceFD < 0) {
		fprintf(stderr, "opening %s: %s\n", device, strerror(errno));
		return 1;
	}
	off_t deviceSize = lseek(deviceFD, 0, SEEK_END);
	close(deviceFD);
	if (deviceSize > kMaxDeviceSize) {
		fprintf(stderr, "%s is larger than 1 GiB: use a smaller volume\n",
			device);
		return 2;
	}

	char path[B_PATH_NAME_LENGTH];
	snprintf(path, sizeof(path), "%s/idle_writeback_test.file", argv[2]);

	// a marker no earlier run left on the device
	uint64 run = ((uint64)real_time_clock_usecs() << 16) ^ getpid();

	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, "creating %s: %s\n", path, strerror(errno));
		return 1;
	}
	static uint8 page[kPageSize];
	for (int32 i = 0; i < kPages; i++) {
		memset(page, 'w', sizeof(page));
		page_marker marker = { kMagic, (uint32)i, run };
		memcpy(page, &marker, sizeof(marker));
		if (write(fd, page, sizeof(page)) != (ssize_t)sizeof(page)) {
			fprintf(stderr, "writing %s: %s\n", path, strerror(errno));
			return 1;
		}
	}
	close(fd);

	bigtime_t start = system_time();
	printf("wrote %" B_PRId32 " pages to %s, no sync; reading %s\n", kPages,
		path, device);

	uint8 found[kPages] = {};
	int32 onDevice = 0;
	while (true) {
		onDevice += count_pages_on_device(device, deviceSize, run, found);
		bigtime_t elapsed = system_time() - start;
		if (onDevice == kPages) {
			printf("PASS: all %" B_PRId32 " pages on the device after "
				"%.1f s\n", kPages, elapsed / 1e6);
			unlink(path);
			return 0;
		}
		if (elapsed > kDeadline) {
			printf("FAIL: %" B_PRId32 " of %" B_PRId32 " pages on the "
				"device after %.1f s\n", onDevice, kPages, elapsed / 1e6);
			unlink(path);
			return 1;
		}
		snooze(1000000);
	}
}
