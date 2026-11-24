/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	A trim of an SD card must leave the sector after the range alone
	(airos-works/haiku#172: mmc_disk gave the card the block after the range
	as the last block to erase).

	sd_trim_test --erase DEVICE [SECTOR]

	DESTROYS what is in the 17 sectors from SECTOR (default 2048) of DEVICE,
	e.g. /dev/disk/mmc/0/raw: only for a scratch card. It writes a pattern to
	the 17 sectors, trims the first 8 and reads all of them again. The 9
	sectors behind the range must still hold the pattern. What the trimmed
	sectors read as is up to the card (a discard is a hint) and only printed;
	QEMU's SD card fills them with 0xff, which is what makes a virtual
	machine with `-device sdhci-pci -device sd-card,drive=...` a test of this.
*/


#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Drivers.h>


static const int kSectorSize = 512;
static const int kSectors = 17;
static const int kTrimmed = 8;


static void
fill(uint8_t* sector, off_t number)
{
	for (int i = 0; i < kSectorSize; i += 8) {
		uint64_t value = 0x7472696d00000000ULL + number * 64 + i / 8;
		memcpy(sector + i, &value, 8);
	}
}


int
main(int argc, char** argv)
{
	if (argc < 3 || strcmp(argv[1], "--erase") != 0) {
		fprintf(stderr, "usage: sd_trim_test --erase DEVICE [SECTOR]\n"
			"Overwrites and trims sectors of DEVICE: a scratch card only!\n");
		return 2;
	}
	const off_t first = argc > 3 ? atoll(argv[3]) : 2048;

	int fd = open(argv[2], O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", argv[2], strerror(errno));
		return 2;
	}

	static uint8_t written[kSectors * kSectorSize];
	static uint8_t read[kSectors * kSectorSize];
	for (int i = 0; i < kSectors; i++)
		fill(written + i * kSectorSize, first + i);

	if (pwrite(fd, written, sizeof(written), first * kSectorSize)
			!= (ssize_t)sizeof(written)) {
		fprintf(stderr, "writing the pattern: %s\n", strerror(errno));
		return 2;
	}
	if (pread(fd, read, sizeof(read), first * kSectorSize)
			!= (ssize_t)sizeof(read)
		|| memcmp(read, written, sizeof(read)) != 0) {
		fprintf(stderr, "the pattern does not read back\n");
		return 2;
	}

	fs_trim_data trim;
	trim.range_count = 1;
	trim.trimmed_size = 0;
	trim.ranges[0].offset = first * kSectorSize;
	trim.ranges[0].size = kTrimmed * kSectorSize;
	if (ioctl(fd, B_TRIM_DEVICE, &trim, sizeof(trim)) != 0) {
		// (eMMC, or a device that does not trim: nothing to test)
		printf("trim of sectors %lld to %lld: %s\nSKIPPED\n", (long long)first,
			(long long)first + kTrimmed - 1, strerror(errno));
		return 0;
	}
	printf("trimmed sectors %lld to %lld (%llu bytes reported)\n",
		(long long)first, (long long)first + kTrimmed - 1,
		(unsigned long long)trim.trimmed_size);

	if (pread(fd, read, sizeof(read), first * kSectorSize)
			!= (ssize_t)sizeof(read)) {
		fprintf(stderr, "reading after the trim: %s\n", strerror(errno));
		return 2;
	}
	close(fd);

	int kept = 0;
	for (int i = 0; i < kTrimmed; i++) {
		if (memcmp(read + i * kSectorSize, written + i * kSectorSize,
				kSectorSize) == 0)
			kept++;
	}
	printf("     of the %d trimmed sectors %d still hold the pattern (the "
		"first starts with %02x %02x %02x %02x)\n", kTrimmed, kept, read[0],
		read[1], read[2], read[3]);

	int failures = 0;
	for (int i = kTrimmed; i < kSectors; i++) {
		const uint8_t* sector = read + i * kSectorSize;
		if (memcmp(sector, written + i * kSectorSize, kSectorSize) != 0) {
			printf("FAIL sector %lld, %d after the trimmed range, lost its "
				"data (starts with %02x %02x %02x %02x)\n",
				(long long)first + i, i - kTrimmed + 1, sector[0], sector[1],
				sector[2], sector[3]);
			failures++;
		}
	}
	if (failures == 0) {
		printf("ok   the %d sectors after the trimmed range hold their data\n",
			kSectors - kTrimmed);
	}
	if (trim.trimmed_size != (uint64_t)kTrimmed * kSectorSize) {
		printf("FAIL %llu bytes reported as trimmed, expected %d\n",
			(unsigned long long)trim.trimmed_size, kTrimmed * kSectorSize);
		failures++;
	}

	printf(failures == 0 ? "PASS\n" : "FAILED\n");
	return failures == 0 ? 0 : 1;
}
