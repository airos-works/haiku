/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include <amdgpu_haiku.h>
#include <OS.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>


static bool
Read(int fd, amdgpu_display_snapshot& snapshot)
{
	memset(&snapshot, 0, sizeof(snapshot));
	snapshot.version = AMDGPU_HAIKU_ABI_VERSION;
	snapshot.size = sizeof(snapshot);
	if (ioctl(fd, AMDGPU_DISPLAY_SNAPSHOT, &snapshot, sizeof(snapshot)) != 0) {
		perror("AMDGPU_DISPLAY_SNAPSHOT");
		return false;
	}
	return snapshot.version == AMDGPU_HAIKU_ABI_VERSION
		&& snapshot.size == sizeof(snapshot)
		&& snapshot.head_count == AMDGPU_DCE_HEAD_COUNT
		&& snapshot.reserved == 0 && snapshot.started_us >= 0
		&& snapshot.finished_us >= snapshot.started_us;
}


static bool
Reject(int fd, void* data, size_t bytes, status_t expected)
{
	errno = 0;
	int result = ioctl(fd, AMDGPU_DISPLAY_SNAPSHOT, data, bytes);
	if (result == -1 && errno == expected)
		return true;
	fprintf(stderr, "display request: result %d errno %d, expected %d\n",
		result, errno, (int)expected);
	return false;
}


int
main(int argc, char** argv)
{
	bool absent = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--expect-no-device") == 0)
			absent = true;
		else if (strcmp(argv[i], "--unprivileged") == 0) {
			if (setgid(65534) != 0 || setuid(65534) != 0) {
				perror("drop privileges");
				return 1;
			}
		} else {
			fprintf(stderr, "usage: amdgpu_display [--expect-no-device] [--unprivileged]\n");
			return 2;
		}
	}
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY);
	if (fd < 0) {
		if (absent && (errno == ENOENT || errno == ENODEV)) {
			puts("PASS: no AMD display device exposed");
			return 0;
		}
		perror("open amdgpu");
		return 1;
	}
	if (absent) {
		fputs("FAIL: unexpected AMD device\n", stderr);
		close(fd);
		return 1;
	}
	printf("DCE read-only observation, UID %u\n", (unsigned)geteuid());
	amdgpu_info info = {};
	info.version = AMDGPU_HAIKU_ABI_VERSION;
	info.size = sizeof(info);
	if (ioctl(fd, AMDGPU_GET_INFO, &info, sizeof(info)) != 0) {
		perror("AMDGPU_GET_INFO");
		return 1;
	}
	amdgpu_display_snapshot first;
	if (!Read(fd, first))
		return 1;
	bool pass = Reject(fd, &first, sizeof(first) - 1, B_BAD_VALUE);
	first.version++;
	pass &= Reject(fd, &first, sizeof(first), B_BAD_VALUE);
	first.version--;
	first.size--;
	pass &= Reject(fd, &first, sizeof(first), B_BAD_VALUE);
	first.size++;
	first.reserved = 1;
	pass &= Reject(fd, &first, sizeof(first), B_BAD_VALUE);
	first.reserved = 0;
	pass &= Reject(fd, NULL, sizeof(first), B_BAD_ADDRESS);
	if (!Read(fd, first))
		return 1;

	uint32 positionChanges[AMDGPU_DCE_HEAD_COUNT] = {};
	uint32 blankSamples[AMDGPU_DCE_HEAD_COUNT] = {};
	uint32 active = 0;
	uint64 bootGPU = 0;
	bool bootValid = info.boot_framebuffer >= info.bar_address[0]
		&& info.boot_framebuffer - info.bar_address[0] < info.bar_size[0];
	if (bootValid)
		bootGPU = info.vram_gpu_base + info.boot_framebuffer - info.bar_address[0];
	for (uint32 head = 0; head < first.head_count; head++) {
		const uint32* r = first.heads[head].registers;
		printf("head %u base %#x HPD[%u] %#x registers", (unsigned)head,
			(unsigned)first.heads[head].register_base, (unsigned)head,
			(unsigned)first.hpd_status[head]);
		for (uint32 i = 0; i < AMDGPU_DCE_REGISTER_COUNT; i++)
			printf(" %08x", (unsigned)r[i]);
		putchar('\n');
		if ((r[AMDGPU_DCE_CRTC_CONTROL] & 1) == 0)
			continue;
		active++;
		uint64 surface = (uint64)r[AMDGPU_DCE_PRIMARY_HIGH] << 32
			| (r[AMDGPU_DCE_PRIMARY_LOW] & ~0xffu);
		uint32 viewport = r[AMDGPU_DCE_VIEWPORT_SIZE];
		printf("active head %u: surface %#" B_PRIx64 " pitch %u, viewport %ux%u"
			", totals %ux%u, cursor %#x\n", (unsigned)head, surface,
			(unsigned)r[AMDGPU_DCE_PITCH], (unsigned)((viewport >> 16) & 0x3fff),
			(unsigned)(viewport & 0x3fff), (unsigned)((r[AMDGPU_DCE_H_TOTAL] & 0x3fff) + 1),
			(unsigned)((r[AMDGPU_DCE_V_TOTAL] & 0x3fff) + 1),
			(unsigned)r[AMDGPU_DCE_CURSOR_CONTROL]);
		// This acceptance fixture is the existing firmware's linear RGB32
		// scanout. A different configuration must be investigated, not claimed.
		pass &= bootValid && surface == bootGPU
			&& (uint64)r[AMDGPU_DCE_PITCH] * 4 == info.boot_stride
			&& (r[AMDGPU_DCE_GRPH_ENABLE] & 1) != 0
			&& (r[AMDGPU_DCE_GRPH_CONTROL] & 0xe00003) == 2
			&& (viewport & 0x3fff) == info.boot_height
			&& ((viewport >> 16) & 0x3fff) == info.boot_width;
	}
	pass &= active != 0;
	amdgpu_display_snapshot previous = first, last = first;
	uint32 samples = 0;
	while (system_time() - first.finished_us < 500000) {
		snooze(500);
		if (!Read(fd, last))
			return 1;
		pass &= last.started_us >= previous.finished_us;
		for (uint32 head = 0; head < last.head_count; head++) {
			const uint32* r = last.heads[head].registers;
			const uint32* old = previous.heads[head].registers;
			positionChanges[head] += r[AMDGPU_DCE_POSITION] != old[AMDGPU_DCE_POSITION];
			blankSamples[head] += (r[AMDGPU_DCE_CRTC_STATUS] & 1) != 0;
			for (uint32 i = 0; i < AMDGPU_DCE_REGISTER_COUNT; i++) {
				if (i != AMDGPU_DCE_CRTC_STATUS && i != AMDGPU_DCE_POSITION
					&& i != AMDGPU_DCE_FRAME_COUNT)
					pass &= r[i] == first.heads[head].registers[i];
			}
		}
		previous = last;
		samples++;
	}
	for (uint32 head = 0; head < last.head_count; head++) {
		if ((first.heads[head].registers[AMDGPU_DCE_CRTC_CONTROL] & 1) == 0)
			continue;
		uint32 frames = (last.heads[head].registers[AMDGPU_DCE_FRAME_COUNT]
			- first.heads[head].registers[AMDGPU_DCE_FRAME_COUNT]) & 0xffffff;
		printf("head %u: %u frames, %u position changes, %u blank samples"
			" in %u reads / %" B_PRId64 " us\n", (unsigned)head,
			(unsigned)frames, (unsigned)positionChanges[head], (unsigned)blankSamples[head],
			(unsigned)samples, last.finished_us - first.finished_us);
		pass &= frames > 0 && positionChanges[head] > 0;
	}
	int second = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY);
	close(fd);
	pass &= second >= 0 && Read(second, last);
	if (second >= 0)
		close(second);
	puts(pass ? "PASS: display query validation, firmware scanout match, live timing and shared open"
		: "FAIL: display observation; no display state was changed");
	return pass ? 0 : 1;
}
