/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	fsync() writes the pages someone else has busy at that moment, too
	(airos-works/haiku#223).

	It skipped them: a page that a read() was copying to user memory, or
	that the page writer was writing, stayed behind, and fsync() returned
	success while the data was only in memory.

	The test writes a file of 64 pages, each with a marker, syncs it and
	finds where each page is on the volume's raw device. Then, while
	threads read the file in a loop, it rewrites the file again and again,
	each time with new markers, calls fsync() and reads the pages from the
	device at once: each must have the new marker.

	fsync_busy_pages_test DEVICE DIRECTORY [ROUNDS]
		DEVICE the raw device of the volume DIRECTORY is on, at most 1 GiB
		(it is read in full once).
*/


#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>


static const int32 kPages = 64;
static const size_t kPageSize = 4096;
static const size_t kSectorSize = 512;
	// (file blocks need not be aligned to pages on the device)
static const off_t kMaxDeviceSize = 1024LL * 1024 * 1024;
static const int32 kReaders = 3;
static const bigtime_t kDeadline = 60 * 1000 * 1000;
static const uint32 kMagic = 'fsbp';


struct page_marker {
	uint32	magic;
	uint32	index;
	uint64	generation;
};


static const char* sPath;
static int32 sStop;


static void
fill_page(uint8* page, int32 index, uint64 generation)
{
	memset(page, (int)(generation & 0xff), kPageSize);
	page_marker marker = { kMagic, (uint32)index, generation };
	memcpy(page, &marker, sizeof(marker));
}


static bool
write_file(int fd, uint64 generation)
{
	static uint8 buffer[kPages * kPageSize];
	for (int32 i = 0; i < kPages; i++)
		fill_page(buffer + i * kPageSize, i, generation);

	return pwrite(fd, buffer, sizeof(buffer), 0) == (ssize_t)sizeof(buffer);
}


static void*
reader(void*)
{
	int fd = open(sPath, O_RDONLY);
	if (fd < 0)
		return NULL;

	const size_t size = kPages * kPageSize;
	uint8* buffer = (uint8*)malloc(size);
	while (buffer != NULL && atomic_get(&sStop) == 0)
		pread(fd, buffer, size, 0);

	free(buffer);
	close(fd);
	return NULL;
}


int
main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s DEVICE DIRECTORY [ROUNDS]\n", argv[0]);
		return 2;
	}
	const char* device = argv[1];
	int32 rounds = argc > 3 ? atoi(argv[3]) : 300;

	char path[B_PATH_NAME_LENGTH];
	snprintf(path, sizeof(path), "%s/fsync_busy_pages_test.file", argv[2]);
	sPath = path;

	int deviceFD = open(device, O_RDONLY);
	if (deviceFD < 0) {
		fprintf(stderr, "opening %s: %s\n", device, strerror(errno));
		return 1;
	}
	off_t deviceSize = lseek(deviceFD, 0, SEEK_END);
	if (deviceSize > kMaxDeviceSize) {
		fprintf(stderr, "%s is larger than 1 GiB: use a smaller volume\n",
			device);
		return 2;
	}

	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, "creating %s: %s\n", path, strerror(errno));
		return 1;
	}

	// generation 1, synced: where is each page on the device?
	uint64 base = (uint64)real_time_clock_usecs() << 20;
	if (!write_file(fd, base + 1) || fsync(fd) != 0) {
		fprintf(stderr, "writing %s: %s\n", path, strerror(errno));
		return 1;
	}
	sync();

	off_t offsets[kPages];
	int32 located = 0;
	for (int32 i = 0; i < kPages; i++)
		offsets[i] = -1;
	{
		static uint8 buffer[1024 * 1024];
		off_t offset = 0;
		while (offset < deviceSize && located < kPages) {
			size_t size = sizeof(buffer);
			if ((off_t)size > deviceSize - offset)
				size = deviceSize - offset;
			ssize_t bytesRead = pread(deviceFD, buffer, size, offset);
			if (bytesRead <= 0)
				break;
			for (ssize_t i = 0;
					i + (ssize_t)sizeof(page_marker) <= bytesRead;
					i += kSectorSize) {
				page_marker* marker = (page_marker*)(buffer + i);
				if (marker->magic == kMagic && marker->generation == base + 1
					&& marker->index < (uint32)kPages
					&& offsets[marker->index] < 0) {
					offsets[marker->index] = offset + i;
					located++;
				}
			}
			offset += bytesRead;
		}
	}
	if (located != kPages) {
		fprintf(stderr, "only %" B_PRId32 " of %" B_PRId32 " pages found on "
			"%s after sync(): is it the device of %s?\n", located, kPages,
			device, argv[2]);
		unlink(path);
		return 1;
	}
	printf("file pages located on %s; %" B_PRId32 " rounds with %" B_PRId32
		" readers\n", device, rounds, kReaders);

	pthread_t readers[kReaders];
	for (int32 i = 0; i < kReaders; i++)
		pthread_create(&readers[i], NULL, &reader, NULL);

	bigtime_t start = system_time();
	int32 staleRounds = 0;
	int32 stalePages = 0;
	int32 failedSyncs = 0;
	int32 round = 0;
	for (; round < rounds && system_time() - start < kDeadline; round++) {
		uint64 generation = base + 2 + round;
		if (!write_file(fd, generation)) {
			fprintf(stderr, "writing %s: %s\n", path, strerror(errno));
			break;
		}
		if (fsync(fd) != 0) {
			failedSyncs++;
			continue;
		}

		int32 stale = 0;
		for (int32 i = 0; i < kPages; i++) {
			page_marker marker;
			if (pread(deviceFD, &marker, sizeof(marker), offsets[i])
					!= (ssize_t)sizeof(marker)
				|| marker.generation != generation)
				stale++;
		}
		if (stale > 0) {
			staleRounds++;
			stalePages += stale;
		}
	}

	atomic_set(&sStop, 1);
	for (int32 i = 0; i < kReaders; i++)
		pthread_join(readers[i], NULL);

	close(fd);
	close(deviceFD);
	unlink(path);

	printf("%" B_PRId32 " rounds in %.1f s: %" B_PRId32 " with pages not on "
		"the device after fsync() (%" B_PRId32 " pages), %" B_PRId32
		" failed fsync()\n", round, (system_time() - start) / 1e6,
		staleRounds, stalePages, failedSyncs);

	if (staleRounds > 0 || failedSyncs > 0) {
		printf("FAIL\n");
		return 1;
	}
	printf("PASS\n");
	return 0;
}
